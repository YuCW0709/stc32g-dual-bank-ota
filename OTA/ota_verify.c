/*==============================================================================
 *  ota_verify.c  --  暂存区镜像校验（Bootloader / APP 共用同一份判据）
 *  Platform : STC32G12K128  (Keil C251, LARGE)
 *  Depends  : ota_types.h / ota_partition.h / ota_fsm.h / flash_iap.h / crc32.h
 *
 *  ── 本文件只读 Flash，一个字节都不写 ─────────────────────────────────────
 *  铁律（2026-10-25 那次事故换来的）：**写不可逆、读永远可逆 → 先读证，后写测。**
 *  所以 P1 的第一步（A1）只启用本文件：证明"BL 能正确读懂 STAGING 里的镜像"。
 *  在这一点被日志证实之前，绝不打开任何擦写分支。
 *
 *  ── 镜像头的物理布局（与上位机打包工具的唯一约定）───────────────────────
 *      STAGING + 0x00   magic(4) hdr_len(2) hdr_ver(2) ver(4) img_len(4)
 *                       img_crc32(4) entry(4) build_time(4) target(8)
 *                       hdr_crc32(4) reserved(2)                = 42 B
 *      STAGING + 0x40   固件正文起点（hdr_len 恒 = OTA_IMG_HDR_RESERVE = 64）
 *
 *      hdr_crc32 = CRC32(magic .. target[8])，固定 OTA_IMG_HDR_CRC_LEN = 36 B，
 *      **不含 hdr_crc32 自身与 reserved**。
 *
 *      ★ 不要用 "hdr_len - 4" 去算 CRC 范围：hdr_len 是**正文偏移(64)**，
 *        不是头的有效长度。二者混用会让 CRC 少算 28 字节的 0xFF 填充，
 *        而且失败时毫无提示（CRC 只是"对不上"）。这里用独立常量锁定。
 *
 *  ── 校验顺序（任一步失败立刻返回，后面一步都不算）──────────────────────
 *      1 读头  2 魔数  3 头长度  4 头格式版本  5 头 CRC
 *      6 正文长度  7 机型  8 入口地址  9 正文 CRC32
 *
 *  失败时 s_step 记住停在第几步，日志只打 "fail rc=? step=?"。
 *  **一份字符串覆盖 9 种失败** —— BL 只剩不到 2 KB，字符串是按字节算钱的。
 *      step=0  STAGING 是空的（全 FF / 从没下载过）→ 不是错误，是常态
 *      step=1  读 Flash 失败（IAP 层出错，rc 会是 OTA_ERR_ADDR/IO）
 *      step=2  魔数不对 → 不是镜像 / 被擦过 / 长度写错
 *      step=3  hdr_len 不是 64 → 打包工具与固件版本不匹配
 *      step=4  hdr_ver 不是 1 → 头格式升级了，BL 太旧
 *      step=5  头 CRC 不对 → 头本身被破坏（最常见的"下载没写全"）
 *      step=6  正文长度 0 或超容量 → 打包参数错
 *      step=7  机型不符 → 刷错设备
 *      step=8  entry 不是 0xFF4000 → 镜像不是给本分区布局打的
 *      step=9  正文 CRC 不对 → 数据损坏（下载断了 / 写入没写全）
 *============================================================================*/
#include "ota_mcu.h"
#include "ota_types.h"
#include "ota_partition.h"
#include "ota_fsm.h"
#include "flash_iap.h"
#include "crc32.h"
#include "bsp_wdt.h"
#include "log.h"

#define TAG "VER"

/*------------------------------------------------------------------------------
 * A2 调试开关（v0.39 起**已改回 0 = 生产行为**）
 *   1 = 跳过幂等判定，每次都强制"擦 + 搬 + 回读校验"
 *   0 = 正常行为（APP 区 CRC 已等于镜像头就直接跳过，不擦不写）
 *   v0.33~v0.38 一直是 1：A1 的镜像就是从 APP 区自身复制出来的，内容与现场
 *   完全相同 ⇒ 幂等判定必然命中 ⇒ 真正的擦写路径一次都跑不到。
 *   2026-10-27 上板已跑通完整擦写（`APPLY OK len=49088 crc=b13ea4`）⇒
 *   现在必须回到 0，让"已经是最新就不动 Flash"这条生产路径接受检验。
 *----------------------------------------------------------------------------*/
#ifndef BL_OTA_A2_FORCE_COPY
#define BL_OTA_A2_FORCE_COPY    0
#endif

/* 头部有效字节数：magic .. target[8]，即 hdr_crc32 的覆盖范围 */
#define OTA_IMG_HDR_CRC_LEN     36U
/* 结构体总长 = 36 + hdr_crc32(4) + reserved(2) */
#define OTA_IMG_HDR_STRUCT_LEN  42U
/* 头格式版本：本固件只认 1 */
#define OTA_IMG_HDR_VER         0x0001U
/* CRC 分块大小：同时是 flash_iap_read 的缓冲长度与喂狗节奏 */
#define OTA_CRC_CHUNK           128U

#if (OTA_IMG_HDR_CRC_LEN + 6U) != OTA_IMG_HDR_STRUCT_LEN
#error "镜像头长度约定自相矛盾：CRC 范围 + hdr_crc32 + reserved 必须等于结构体长"
#endif

/* 编译期钉死结构体尺寸：C251 对含 u32 的结构体做自然对齐，
 * 一旦有人往 ota_img_hdr_t 里插字段，这里立刻编不过（负长度数组），
 * 而不是等到上板时"CRC 永远对不上"却查不出原因。
 * 42 = magic(4)+hdr_len(2)+hdr_ver(2)+ver(4)+img_len(4)+img_crc32(4)
 *      +entry(4)+build_time(4)+target(8)+hdr_crc32(4)+reserved(2)，无填充。 */
typedef char ota_hdr_size_assert[
    (sizeof(ota_img_hdr_t) == OTA_IMG_HDR_STRUCT_LEN) ? 1 : -1];

/*------------------------------------------------------------------------------
 * ★★★ v0.39：apl_crc() 从"仅 BL"提升为**两工程共用且是本文件唯一的读循环** ★★★
 *
 *   A1 首趟正文 CRC 偶发算错（v0.39 前已收集 7 个互不相同的错值：
 *   121a9955 / bac835a5 / 34824ce1 / 5768849c / 110cb6f2 / ff1b951d / d6a47614），
 *   而**同一个上电周期内的第二趟（apl_crc）8 次全对**。
 *   两趟算的是同一段 Flash、同一个 CRC 引擎、同样的分块与喂狗节奏 ——
 *   唯一的差别只剩"调用点的形状"：
 *       首趟 ver_body()  自己写了一份循环，形参对象是 s_off / s_n
 *       第二趟 apl_crc() 形参落进 s_aoff / s_an
 *   本工程反复栽在"布局一挪、受害函数就换一个"上（铁律 9/14）：
 *   crc32_update(u32,u8*,u16) 合计 4+3+2 = 9 B > 8 B，**必然生成 ?BYTE 参数块**，
 *   而 L251 只按 6~7 B 步进 ⇒ 谁的局部段排在块尾，谁就被自己清掉。
 *   两份循环 = 两个调用点 = 只有一个会中招，这正好解释了"首趟错、二趟对"。
 *
 *   ⇒ 治法不是再猜哪一块坏了，而是**让首趟走那条已被证明正确的调用点**：
 *     删掉 ver_body() 自带的那份循环，ver_body() 与 ota_apply_staging() 都调 apl_crc()。
 *     代价是 APP 工程多编进 ~120 B（APP 有 48 K，可忽略），
 *     收益是"CRC 只有一条路"，RETRY 从"另一份实现"变成"同一条路的第二次尝试"。
 *
 *   ★★★ v0.40：上面这套"调用点形状"的解释**已被上板证伪** ★★★
 *
 *   v0.39 让首趟与第二趟走同一个调用点之后，首趟**依旧错**：
 *       [4724][VER] body crc now=b8b173ec want=571d1b2b   ← 第 8 个互不相同的错值
 *       [9270][VER] stgA crc=571d1b2b
 *       [13872][APL] app already current crc=571d1b2b     ← 同一周期第 3 次调用，一字不差
 *   同一个函数、同一段 Flash、同一个上电周期，第 1 次错、第 2 次起对。
 *   ⇒ 分界不是"哪个调用点"，而是**本上电周期的第几次调用**。
 *   所以 v0.40 不再猜，直接在首趟按 4 KB 打累计 CRC（见 apl_crc 里的探针），
 *   把"第一处偏离"夹出来。离线穷举已排除的结构性损坏清单见那段注释。
 *
 *   ★ 下面这些量必须声明在 ver_body() 之前 —— 它就在那儿调用 apl_crc()。
 *------------------------------------------------------------------------------*/
static u32  xdata s_abase;      /* apl_crc：段基址（形参落地处）*/
static u32  xdata s_alen;       /* apl_crc / 搬运：总长          */
static u32  xdata s_aoff;       /* 段内偏移                      */
static u32  xdata s_acrc;       /* apl_crc 的结果                */
static u16  xdata s_an;         /* 本块字节数                    */
static ota_ret_t xdata s_arc;   /* apl_crc / 搬运：最近一次返回码 */
#if !defined(OTA_BUILD_APP)
static u32  xdata s_p_len;      /* hdr->img_len 的副本（仅 A2）   */
static u32  xdata s_p_crc;      /* hdr->img_crc32 的副本（仅 A2） */
static u8   xdata s_a_try;      /* v0.38：本块重试计数           */
static u8   xdata s_a_rd;       /* v0.38：最近一次"源读"的失败码 */
#endif

/*------------------------------------------------------------------------------
 * 状态一律 static：本模块运行在 BL 的深调用链里，
 * 任何局部 xdata 数组都会进 overlay 复用池（v0.4~v0.7 反复翻车的老地方）。
 *----------------------------------------------------------------------------*/
static ota_img_hdr_t xdata s_hdr;                 /* 42 B，读完就一直放这儿 */
static u8   xdata s_buf[OTA_CRC_CHUNK];           /* 正文 CRC 的读缓冲       */
static u32  xdata s_crc;                          /* 算出来的正文 CRC32      */
static u32  xdata s_crc_run;                      /* 分块累加的中间值（apl_crc 用）*/
static u8   xdata s_step;                         /* 失败时停在第几步        */
static u32  xdata s_crc_hdr;                      /* v0.25 头 CRC 诊断中转    */
static u8   xdata s_i;                            /* 循环变量（u8，块内偏移 ≤128）*/
static u8   xdata s_rt;                           /* v0.42：单字节读重试计数     */
static u8   xdata s_ea;                           /* v0.43：外层临界区保存的 EA   */
static u32  xdata s_asz;                          /* v0.42：EEPROM 容量（一次性） */

/* 本机机型标识，必须与上位机打包工具写进 target[8] 的一致 */
static const u8 s_local_target[8] = { 'S', 'T', 'C', '3', '2', 'G', '1', '2' };

/*==============================================================================
 * 头校验（第 1~8 步）
 *============================================================================*/
static ota_ret_t ver_hdr(void)
{
    ota_ret_t rc;

    s_step = 1;
    rc = flash_iap_read(OTA_STAGE_BASE, (u8 *)&s_hdr, (u16)OTA_IMG_HDR_STRUCT_LEN);
    if (rc != OTA_OK) return rc;

    /* 全 FF = 空片。这不是错误，是"还没下载过"，BL 侧应照常启动旧 APP。 */
    s_step = 0;
    if (s_hdr.magic == 0xFFFFFFFFUL) return OTA_ERR_VERIFY;

    s_step = 2;
    if (s_hdr.magic != OTA_IMG_MAGIC) return OTA_ERR_VERIFY;

    s_step = 3;
    if (s_hdr.hdr_len != (u16)OTA_IMG_HDR_RESERVE) return OTA_ERR_VERIFY;

    s_step = 4;
    if (s_hdr.hdr_ver != (u16)OTA_IMG_HDR_VER) return OTA_ERR_VERIFY;

    /* ★ v0.25：这一步是 A1 现在唯一的卡点，必须把**两个值都念出来**。
     *
     *   前 8 个字节（magic / hdr_len / hdr_ver）都过了，说明头确实写进
     *   STAGING 了；那就只剩两种可能，而单看 "fail rc=7 step=5" 分不开：
     *     now  == aabc4c0e 而 want !=  → **APP 侧的 crc32 算错了**（写进头的
     *                                     CRC 是错的，BL 重算是对的）
     *     want == aabc4c0e 而 now  !=  → **BL 侧的 crc32 算错了**
     *     两者都不等于 aabc4c0e        → 头里的字段本身被写歪了
     *   aabc4c0e 是本地用 Python 按大端把这个头的 36 个字节算出来的期望值
     *   （magic=4F544149 hdr_len=64 hdr_ver=1 ver=0.1.0.1 img_len=49088
     *     img_crc32=b723d64c entry=FF4000 build_time=0 target="STC32G12"）。
     *   算进 static 再打：LOG 参数表里不许直接写函数调用（铁律 10）。 */
    s_step = 5;
    /* v0.27：不再调 crc32_calc（宏见 crc32.h），三步按语句写 */
    OTA_CRC32_OF(s_crc_hdr, (const u8 *)&s_hdr, OTA_IMG_HDR_CRC_LEN);
    if (s_crc_hdr != s_hdr.hdr_crc32)
    {
        LOG_I2(TAG, "hdr crc now=%lx want=%lx", s_crc_hdr, s_hdr.hdr_crc32);
        return OTA_ERR_CRC;
    }

    s_step = 6;
    if (s_hdr.img_len == 0UL || s_hdr.img_len > OTA_STAGE_PAYLOAD_MAX)
        return OTA_ERR_NOSPACE;

    s_step = 7;
    for (s_i = 0U; s_i < 8U; s_i++)
    {
        if (s_hdr.target[s_i] != s_local_target[s_i]) return OTA_ERR_VERIFY;
    }

    s_step = 8;
    if (s_hdr.entry != OTA_APP_CODE_BASE) return OTA_ERR_VERIFY;

    return OTA_OK;
}

/*==============================================================================
 * 算任意一段 IAP 空间的 CRC32（STAGING 正文 / APP 正文共用）
 *
 *   ★ 形参第一条语句就落地（铁律 9）：本函数在 BL 最深调用链上，
 *     而且它的 ?BYTE 参数块是 4+4 = 8 B —— 刚刚好在生成参数块的边界上，
 *     不留活口最安全。
 *   ★ v0.39 起它是本文件**唯一的读循环**：ver_body() 与 ota_apply_staging()
 *     都走这里。分段读 + 分段算 + 每段喂狗。
 *     不用 flash_iap_crc32() 的原因：它内部一次性跑完整个长度，中间没有喂狗点，
 *     48 KB 在 11.0592 MHz 下要好几秒，WDT（PS_32）会先复位。
 *============================================================================*/
static ota_ret_t apl_crc(u32 base, u32 len)
{
    s_abase   = base;
    s_alen    = len;
    s_aoff    = 0UL;
    s_crc_run = crc32_begin();

    /* v0.42：地址门禁一次性做完（铁律 15 的立即数比较），循环内不再判。
     *   读了 382 块都对、只有一块没落到 s_buf ⇒ 不是地址错，但既然不再经过
     *   flash_iap_read()（它原来替我们查容量），这条判据必须补上，
     *   免得哪天调用点写错把读指到 EEPROM 之外。 */
    s_asz = flash_iap_eeprom_size();
    if (s_alen == 0UL || s_abase >= s_asz || s_alen > (s_asz - s_abase))
    {
        return OTA_ERR_ADDR;
    }

    /* ★ v0.42：v0.40/v0.41 的逐窗口探针已全部撤除。
     *   它已经把该说的话说完了（定位到 0x2800 单块未刷新，见下面读块的注释），
     *   而在 BL 的深调用链上多打 13 行日志是**有代价的**：v0.41 打到第 9 行
     *   （3375 ms）就自己复位了，v0.40（无探针）却能跑完整段。
     *   ⇒ 这一版正常路径零打印；判据回到唯一一条：
     *      `STAGING ok` 之前**没有** `body crc now=` 才算真修好。 */

    while (s_aoff < s_alen)
    {
        s_an = (u16)(((s_alen - s_aoff) > (u32)OTA_CRC_CHUNK)
                        ? (u32)OTA_CRC_CHUNK
                        : (s_alen - s_aoff));

        /*======================================================================
         * ★★★ v0.42：读块改为**内联**，不再经 flash_iap_read() ★★★
         *
         *   v0.41 上板（同一份 hex 反复重启、9 行 `p1` 一字不差 ⇒ 确定性）
         *   用 4 KB 累计 CRC 把第一处偏离夹到了唯一解：
         *
         *       o=2000 c=cfe40c15   ← 与离线靶子一致
         *       o=3000 c=0b254f1b   ← 与离线靶子 3bcca8a8 不符
         *       tools/locate_bad_window.py 在 0x2000..0x3000 里穷举 ⇒
         *       **命中有且只有一条：偏移 0x2800 那一块 := 0x2780 那一块**
         *       用这条假设重算，o=3000…o=8000 **九个探针 9/9 逐位吻合**。
         *
         *   即：整段 49088 B 里，**只有第 80 块读回来的是前一块的内容**，
         *   其余 382 块全对。这不是"读到了错误的数据"，而是**那一块压根没写进
         *   s_buf** —— flash_iap_read() 里 `s_rbuf[s_i] = 0xFF` 是前置填充，
         *   只要循环跑过，s_buf 就不可能是"完整的上一块"。所以只能是
         *   `s_rlen` 或 `s_rbuf` 那一次是错的，而函数照样返回了 OTA_OK。
         *
         *   ⇒ 与铁律 14 三次验证（v0.30 读门禁 / v0.35 擦门禁 / v0.36 写门禁）
         *     完全同型：**形参 4+3+2 = 9 B > 8 B 的那个函数，就是出事的那个**。
         *     flash_iap_read() 是整条链上最后一个这样的中间层，而且它的三个
         *     `s_raddr / s_rlen / s_rbuf` 是**跨函数共享、不可重入**的静态量：
         *     首趟前面刚跑完 param 装载 + 8 步头校验（几十次 read），
         *     只要有一次把它仨写歪，整块就静默变成上一块。
         *
         *   ⇒ 治法同前三次：**这一条通路不再经过它**。读块就地内联，
         *     只用本模块自己的 s_i / s_an / s_arc，不共享任何别的函数的静态量。
         *     地址门禁按铁律 15 做一次性立即数比较，循环内不再判。
         *     重试语义与 v0.38 一致（读是幂等的，单字节最多 3 次）。
         *====================================================================*/
        /* ★★★ v0.43：整块读期间**外层再包一道临界区** ★★★
         *
         *   v0.42 上板：首趟 `body crc now=2afa96e9` 错，**第二趟 `stgA` 也错**
         *   （`e4db7301`），且连续两次上电四个错值全不同 ⇒ 随机。这打破了
         *   v0.38~v0.41 铁律「第二趟必对」，RETRY 网第一次被击穿。
         *
         *   根因：flash_iap_raw() 内部每次调用自己 enter/exit 一次临界区
         *   （读 EA→EA=0 … EA=恢复）。flash_iap_read() 之所以稳，是因为它
         *   **外层先 enter 一次**，于是整块 128 字节循环期间 EA 恒为 0，
         *   内层 raw 的 exit 只会"恢复成 0"，1ms 定时器中断全程插不进来。
         *   v0.42 的内联循环**漏了这个外层 enter** ⇒ 每读一个字节，raw 的
         *   exit 就把 EA 恢复成 1 ⇒ 下一字节之间 Timer0 中断插入，打断
         *   IAP 读时序 ⇒ 读回的 IAP_DATA 是随机错误值，且散布在多个字节。
         *   ⇒ 错值随机、第二趟也错、单块/单字节穷举全不命中（多字节散布）。
         *
         *   修法：恢复 flash_iap_read() 的语义 —— 整块一个外层临界区。
         *   内层 raw 的 enter 读到 EA=0、exit 恢复 EA=0，无副作用。 */
        s_ea = flash_iap_critical_enter();
        for (s_i = 0U; s_i < (u8)s_an; s_i++)
        {
            s_arc = OTA_ERR_IO;
            for (s_rt = 0U; s_rt < 3U; s_rt++)
            {
                if (flash_iap_raw(IAP_CMD_READ,
                                  s_abase + s_aoff + (u32)s_i,
                                  &s_buf[s_i]) == OTA_OK)
                {
                    s_arc = OTA_OK;
                    break;
                }
            }
            if (s_arc != OTA_OK)
            {
                /* 三次都没成 = 真读不出来（不是"读到旧值"）。先恢复 EA 再上抛，
                 * 上层会打 `body crc now=` / `copy fail`，够定位到块。 */
                flash_iap_critical_exit(s_ea);
                return OTA_ERR_IO;
            }
            if ((s_i & 31U) == 31U) bsp_wdt_feed();
        }
        flash_iap_critical_exit(s_ea);

        s_crc_run = crc32_update(s_crc_run, s_buf, s_an);
        s_aoff += (u32)s_an;

        bsp_wdt_feed();
    }

    s_acrc = crc32_final(s_crc_run);
    return OTA_OK;
}

/*==============================================================================
 * 正文 CRC（第 9 步）
 *
 *   ★ v0.39：本函数**不再自带读循环**，直接调 apl_crc()。
 *     理由见文件头部那段注释：首趟偶发算错、第二趟必对，两趟唯一的差别
 *     就是"调用点形状不同"，所以让它们变成同一个调用点。
 *     顺带把 BL 的 code 省下 ~90 B（16 K 槽只剩两百多字节）。
 *
 *   ★ 把两个值都打出来（v0.28 起）：
 *       now == want            → 通过
 *       now 完全不像            → 读/算错（但现在这条路已被证明可靠，
 *                                 再出现就是真·硬件偶发，看 RETRY 是否兜住）
 *     want 是镜像头里的 img_crc32，它被 header CRC 覆盖 ⇒ step=5 过了
 *     就说明这个字段本身是好的，可以直接当基准。
 *============================================================================*/
static ota_ret_t ver_body(void)
{
    ota_ret_t rc;

    s_step = 9;

    rc = apl_crc(OTA_STAGE_BASE + OTA_IMG_HDR_RESERVE, s_hdr.img_len);
    if (rc != OTA_OK) return rc;

    s_crc = s_acrc;

    if (s_crc != s_hdr.img_crc32)
    {
        LOG_E2("VER", "body crc now=%lx want=%lx", s_crc, s_hdr.img_crc32);
        return OTA_ERR_CRC;
    }
    return OTA_OK;
}

/*==============================================================================
 * 对外接口（声明见 ota_fsm.h）
 *============================================================================*/
ota_ret_t ota_verify_staging(ota_img_hdr_t *p_hdr)
{
    ota_ret_t rc;

    s_step = 0;
    s_crc  = 0UL;

    /* 结构体大小自检：哪天有人往头里加字段，这里会立刻报 PARAM 而不是静默错位。
     * sizeof 是编译期常量，比较会被优化掉，运行期零成本。 */
    if (sizeof(ota_img_hdr_t) != OTA_IMG_HDR_STRUCT_LEN) return OTA_ERR_PARAM;

    rc = ver_hdr();
    if (rc == OTA_OK) rc = ver_body();

#if !defined(OTA_BUILD_APP)
    /*==========================================================================
     * ★ v0.39：这一段现在只是**兜底**，不再是主力。
     *   ver_body() 已改成调 apl_crc()（与这里同一个函数）——
     *   如果"首趟错、二趟对"真是"两个调用点只有一个被参数块踩"，
     *   那么首趟从此不该再错；如果还错，说明是 IAP 时序，
     *   这时重跑同一段代码仍能救回来（8 次实测 8 次救回）。
     *   ★ 判据：日志里 `stgA crc=` 前面**没有** `body crc now=` 才是真进步。
     *
     * ★ v0.35：正文 CRC 失配时只留"第二趟"（v0.34 的三条探针已全部答完）
     *
     *   v0.34 上板（行尾和值逐行核对通过，都是真值）：
     *       [6771]  self crc=26c0ad90   ← 与离线预言一字不差（读自己的 16K 代码区）
     *       [6798]  ck38 crc=13320cd8   ← 与离线预言一字不差（读 0x1340 单块）
     *       [11744] stgA crc=25d6d29f   ← 第二趟全量，就是正确答案
     *   一次钉死三件事：
     *     ① 读通路 + CRC 引擎**按需调用时完全正确**（两个预言精确命中）；
     *     ② 首趟 now 错、第二趟 stgA 对 ⇒ 错的是"首趟这一趟"，不是某个地址；
     *     ③ v0.33 的 ck38 假说（那块读出第 37 块的内容）在 v0.34 不再复现
     *        ⇒ 受害块是**随机**的，与 v0.33 离线穷举（唯一解 chunk38:=chunk37、
     *        概率 1e-6）互相印证：每次上电随机坏一块，v0.32 恰好没中。
     *   ⇒ self / ck38 两条探针使命完成，删掉给 A2 腾 16K 槽。
     *
     *   保留"第二趟"有两个用处：
     *      · A1 的兜底：对上了就继续往下走（A2 照常），A1 判据依然成立；
     *      · now 与 stgA 两个值都留在日志上 ⇒ "两趟不一致"这件事藏不住。
     *========================================================================*/
    if (rc == OTA_ERR_CRC)
    {
        if (apl_crc(OTA_STAGE_BASE + OTA_IMG_HDR_RESERVE, s_hdr.img_len) == OTA_OK)
        {
            LOG_I1(TAG, "stgA crc=%lx", s_acrc);
            if (s_acrc == s_hdr.img_crc32)
            {
                s_crc = s_acrc;
                rc = OTA_OK;
                LOG_W1(TAG, "body RETRY ok crc=%lx", s_acrc);
            }
        }
    }
#endif

    if (rc == OTA_OK)
    {
        /* 手动逐字节复制：C251 对 42 B 结构体做整体赋值会生成一串
         * 逐字段搬运（实测约 300 B code），换成 8 位循环只要十几字节。
         * A1 阶段 BL 传的就是 NULL（只要结论不要内容），这条路径不会走，
         * 但 A2 要靠它拿头，接口必须完整。 */
        if (p_hdr != 0)
        {
            for (s_i = 0U; s_i < (u8)OTA_IMG_HDR_STRUCT_LEN; s_i++)
            {
                ((u8 *)p_hdr)[s_i] = ((const u8 *)&s_hdr)[s_i];
            }
        }

        /* 只打长度与 CRC：版本号 A1 用不到，多一条日志就多一份格式串。
         * A2 真正要看版本时再加回来（那时 BL 空间会重新分配）。 */
        LOG_I2(TAG, "STAGING ok len=%lu crc=%lx", s_hdr.img_len, s_crc);
    }
    else
    {
        /* step=0 表示 STAGING 空着，属正常，调用方不必当错误处理 */
        LOG_I2(TAG, "STAGING fail rc=%u step=%u", (u16)rc, (u16)s_step);

        /* ★ v0.29：rc=2（OTA_ERR_ADDR）时把地址门禁实际看到的量打出来。
         *
         *   v0.28 上板拿到 `rc=2 step=9`（正文第一块就被拒），而 ver_body →
         *   flash_iap_read → flash_iap_check_range 这条链的每一处参数传递、
         *   符号地址、段边界都逐条核过、全部正确。读代码已经到极限，
         *   于是让门禁自己把 [addr, len, eeprom_size] 留档后打出来：
         *       a >= sz      → 地址超容量（sz 是多少一眼就看到）
         *       l > sz - a   → 长度越界
         *       三个都正常   → rc=2 不是门禁给的，去查调用链本身
         *   off 是正文内偏移，能看出是"第一块就错"还是"读到某处才错"。 */
        if (rc == OTA_ERR_ADDR)
        {
            LOG_E2(TAG, "chk a=%lx l=%lx", g_cr[0], g_cr[1]);
            LOG_E2(TAG, "chk sz=%lx off=%lx", g_cr[2], s_aoff);
        }
    }

    return rc;
}

/*==============================================================================
 *                      A2：STAGING -> APP 搬运生效（仅 BL）
 *
 *  ── 为什么必须先做"幂等判定" ─────────────────────────────────────────────
 *  BL 侧 param_save 目前是坏的（APP 侧实测 `A1 param_save rc=6`，
 *  下一次上电 BL 立刻报 `param empty/corrupt`）⇒ **别指望"装好了"这个状态
 *  能被持久化**。如果搬运不幂等，每一轮上电都会：擦 96 个扇区 + 写 49 KB，
 *  等于把 20 次重试额度直接烧成 Flash 磨损。
 *
 *  所以第一步不是擦写，而是**先把 APP 区正文算一遍 CRC**：
 *      与镜像头里的 img_crc32 相等 → 已经是目标镜像，一个字节都不动，直接算成功。
 *  这条判据还有一个额外好处：搬运中途掉电/看门狗复位时，APP 区一定对不上，
 *  下一次上电自动重擦重写 —— **不需要任何"进度标记"就能自愈**。
 *
 *  ── 擦写顺序（不可逆，倒着来的代价是本项目最贵的一课）────────────────
 *      ① 擦：OTA_APP_IAP_BASE 起 ceil(img_len/512) 个扇区
 *      ② 搬：STAGING+0x40+off -> s_buf(128B) -> APP+off
 *      ③ 校：整段 APP 正文重算 CRC，必须等于 hdr->img_crc32
 *  ①②③ 全部靠 flash_iap_read/write 的唯一通路，每 128 B 喂一次狗。
 *  搬运源永远在 STAGING（BL 只读它），所以**任何一步失败都可以整段重来**。
 *
 *  ── 分支裁剪 ────────────────────────────────────────────────────────────
 *  APP 工程也编译本文件，但 APP 不需要（也不允许）搬运自己：
 *  被调者用 OTA_BUILD_APP 切掉，省的是 APP 的 48 K，不是 BL 的 16 K。
 *============================================================================*/
#if !defined(OTA_BUILD_APP)

/* ★ v0.39：apl_crc() 已上移到 ver_body() 之前（两工程共用、本文件唯一读循环）。
 *   这里只剩"搬运生效"这一个 BL 专属函数。 */

/* 声明见 ota_fsm.h:234。hdr 必须来自 ota_verify_staging() 成功后的输出。 */
ota_ret_t ota_apply_staging(const ota_img_hdr_t *hdr)
{
    /* 铁律 9：形参第一条语句就拷进模块 static，此后不再读形参 */
    s_p_len = hdr->img_len;
    s_p_crc = hdr->img_crc32;

    if (s_p_len == 0UL || s_p_len > OTA_APP_IAP_SIZE) return OTA_ERR_NOSPACE;

    /* v0.37 的 `pt a= w=` 探针已验证通过（上板 `pt a=5 w=1`），
     * v0.38 起删掉腾出 16K 槽空间；分区判定的守门仍由擦除门禁的
     * `erase g=/p=` 承担（p 就是 ota_part_of 的结果）。 */

#if (BL_OTA_A2_FORCE_COPY != 0)
    /* ★ 本轮专用：跳过幂等判定，强制走一遍"擦 + 搬 + 回读校验"。
     *   原因：A1 造的镜像就是**从 APP 区自己复制出来的**，内容必然与 APP 相同，
     *   幂等判定一定命中 ⇒ 第一次上板只会看到 "app already current"，
     *   真正要验证的擦写路径一次都跑不到（白烧一版）。
     *   镜像内容与现场一致，所以哪怕搬运写坏，代价也只是"APP 区被写成它自己的
     *   副本"，ISP 冷启动随时可救 —— 这是全项目里最安全的一次真写测试。
     *   ★ 搬运路径被证实之后，本开关必须改回 0。 */
    LOG_W0("APL", "FORCE copy: idempotent check skipped (debug build)");
#else
    /* ① 幂等判定：APP 区已经是目标镜像就什么都不做（见文件头说明） */
    s_arc = apl_crc(OTA_APP_IAP_BASE, s_p_len);
    if (s_arc != OTA_OK) return s_arc;
    if (s_acrc == s_p_crc)
    {
        LOG_I1("APL", "app already current crc=%lx", s_acrc);
        return OTA_OK;
    }
    LOG_I2("APL", "app crc now=%lx want=%lx -> erase+copy", s_acrc, s_p_crc);
#endif

    /* ② 擦：按扇区上取整。只擦镜像实际占用的部分，不整片擦 48 K。 */
    s_arc = flash_iap_erase_range(OTA_APP_IAP_BASE,
                (s_p_len + OTA_IAP_SECTOR_SIZE - 1UL) & ~(OTA_IAP_SECTOR_SIZE - 1UL));
    if (s_arc != OTA_OK)
    {
        LOG_E1("APL", "erase app fail rc=%u", (u16)s_arc);
        return s_arc;
    }

    /* ③ 搬：128 B 一块，读 STAGING -> 写 APP */
    s_aoff = 0UL;
    while (s_aoff < s_p_len)
    {
        s_an = (u16)(((s_p_len - s_aoff) > (u32)OTA_CRC_CHUNK)
                        ? (u32)OTA_CRC_CHUNK
                        : (s_p_len - s_aoff));

        /* v0.38：整块（源读 + 目标写 + 写后回读校验）失败就整块重来 3 次。
         *   底层已按单字节重试 3 次，能走到这里说明是"这一次块操作真没成"。
         *   重试整块是安全的：目标地址这一块刚刚写过的字节若已成功，
         *   再写同样的值是幂等的（Flash 只能 1→0，同值再写不会变）。 */
        for (s_a_try = 0; s_a_try < 3U; s_a_try++)
        {
            s_a_rd = 0U;
            s_arc = flash_iap_read(OTA_STAGE_BASE + OTA_IMG_HDR_RESERVE + s_aoff, s_buf, s_an);
            if (s_arc == OTA_OK)
            {
                s_arc = flash_iap_write(OTA_APP_IAP_BASE + s_aoff, s_buf, s_an);
            }
            else
            {
                s_a_rd = (u8)s_arc;         /* 源读就失败，写根本没跑 */
            }
            if (s_arc == OTA_OK) break;
        }
        if (s_arc != OTA_OK)
        {
            /* off 是唯一能定位"搬到哪儿断的"的量 —— 必须打；
             * 第二行把"源读失败"和"PROGRAM 被拒"分开（rd=0 且 rej>0 ⇒ 写被拒）。 */
            LOG_E2("APL", "copy fail off=%lx rc=%u", s_aoff, (u16)s_arc);
            LOG_I2("APL", "rd=%u rej=%lu", (u16)s_a_rd, g_iap_dbg[3]);
            return s_arc;
        }

        s_aoff += (u32)s_an;
        bsp_wdt_feed();

        /* 进度：每 8 KB 一行。这一段要连跑十几秒（擦 96 扇区 + 搬 49 KB +
         * 每块内部回读校验），串口上什么都不会动，很容易被当成死机。
         * 只打偏移（一个参数）—— 顺着最后一行 offset 就能看出断在哪。 */
        if ((s_aoff & 0x1FFFUL) == 0UL)
        {
            LOG_I1("APL", "copy off=%lx", s_aoff);
        }
    }

    /* ④ 回读校验：整段 APP 正文重算，必须等于镜像头里的值 */
    s_arc = apl_crc(OTA_APP_IAP_BASE, s_p_len);
    if (s_arc != OTA_OK) return s_arc;
    if (s_acrc != s_p_crc)
    {
        LOG_E2("APL", "app crc now=%lx want=%lx", s_acrc, s_p_crc);
        return OTA_ERR_VERIFY;
    }

    LOG_I2("APL", "APPLY OK len=%lu crc=%lx", s_p_len, s_acrc);
    return OTA_OK;
}

#endif  /* !OTA_BUILD_APP */
