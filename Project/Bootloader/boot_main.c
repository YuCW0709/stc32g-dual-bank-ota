/*==============================================================================
 *  boot_main.c  --  Bootloader 入口（P0 探针 / P1 启动流程）
 *  Project : Project/Bootloader/Bootloader.uvproj   CODE = 0xFF0000
 *  Platform: STC32G12K128 (Keil C251, CodeRomSize = Huge, HEX-386)
 *
 *  本文件当前实现 P0/P1：
 *      P0  flash_iap_init(128) + flash_iap_selftest()  -> 串口日志定开关
 *      P1  param_init() 读参数、判断是否有待生效固件、跳 APP
 *      P1-A1  待安装镜像**只读**校验（ota_verify.c，不搬不擦不写）
 *  未实现（等 P1-A2/A3）：
 *      搬运（ota_apply_staging）、回滚计数判定、向量 stub 的动态绑定
 *
 *  ── P1-A 的推进顺序（不可跳步）──────────────────────────────────────────
 *      A1  只读：BL 能读懂 STAGING 的镜像头并校验正文 CRC        ← 当前在这
 *      A2  擦写：BL 把 STAGING 搬回 APP 区（先做"自己搬自己"）
 *      A3  提交：APP 启动后写 committed，失败则回滚
 *  为什么必须 A1 先做完：写不可逆。如果一上来就打开擦写，"校验不过"这件事
 *  会同时有"读错了"和"镜像真的坏了"两种解释，而这两种的修法完全相反。
 *============================================================================*/
#include "ota_mcu.h"
#include "bsp_clock.h"
#include "bsp_gpio.h"
#include "bsp_uart.h"
#include "bsp_timer.h"
#include "bsp_wdt.h"
#include "flash_iap.h"
#include "crc32.h"
#include "log.h"
#include "ota_partition.h"
#include "ota_fsm.h"      /* OTA_ST_PENDING_REBOOT / ota_verify_staging() */

/*------------------------------------------------------------------------------
 * ★★ 单一真相源：A1 强制校验开关（v0.42）
 *
 *   0 = 生产语义：**只有 ota_state == PENDING_REBOOT 才校验并搬运 STAGING**。
 *   1 = 诊断语义：每次上电都校验并搬运（会让重新烧进去的 APP 一次都跑不起来，
 *       见下面第 3 节的长注释）。
 *
 *   ★ 必须定义在 BL_VERSION 之前 —— 版本串要引用它。写在 559 行那种
 *     "先引用后定义"的写法会让 #if 把未定义宏当 0，版本串永远谎报 A1FORCE=0。
 *
 *   ★★ 现场事故（2026-10-29，就是 FORCE=1 造成的，记下来别重犯）：
 *     某次 OTA 把一份 0.7.4 装进 APP 之后，**STAGING 里那份镜像一直留着**
 *     —— 关键前提：STC-ISP 烧 hex 只覆盖 IAP 0x10000 以上，
 *     **从不碰 STAGING（0x0000 起）与 PARAM（0xC000 起）**。
 *     FORCE=1 让 BL 每次上电都无条件校验 STAGING，而那份 0.7.4 是完好的、
 *     校验必然通过 ⇒ 每轮上电都执行 A2 搬运 ⇒ 重新烧进去的 APP **一次都跑不起来**，
 *     上电直接"自动搬运成 0.7.4"，连 MQTT 都进不去。
 *     表现极像"烧录没生效"，实际是 BL 主动把它覆盖掉了。
 *     上一次没暴露，只是因为当时搬进去的恰好还是同一份镜像。
 *     纪律：① 临时开关必须有到期条件，定位完当轮改回；
 *           ② BL 的任何"无条件动作"都会掩盖 APP 侧的修复。
 *------------------------------------------------------------------------------*/
#define BL_OTA_A1_FORCE     0

#if (BL_OTA_A1_FORCE != 0)
#define BL_VERSION      "BL v0.2 (A1FORCE=1!)"
#else
#define BL_VERSION      "BL v0.2 (A1FORCE=0)"
#endif

/*==============================================================================
 * P0 阶段看门狗策略（★ 不是简单的"关掉"）
 *
 *   STC-ISP 硬件选项里「上电复位时由硬件自动启动看门狗」是勾着的 ——
 *   这颗芯片**上电就在跑狗，跟我们代码有没有初始化它无关**。
 *   所以：
 *     · 不能关喂狗：一关就是复位循环（灯偶尔闪、串口一个字节都出不来）；
 *     · 但要关"初始化"：bsp_wdt_init() 会把 PS 改成 32 分频，
 *       超时可能比选项字节里的 256 分频还短，是在给自己找麻烦。
 *   → BL_INIT_WDT = 0 表示"不主动配置/不主动使能，但照常喂（CLR_WDT）"。
 *     P1 恢复启动流程时再置 1，并按手册实测超时。
 *============================================================================*/
#define BL_INIT_WDT     0


/*------------------------------------------------------------------------------
 * P0 裸通路诊断（闪灯计数 + 引脚读回）
 *   1 = 保留 bl_tx_probe() / bl_pin_probe() 及其在 main 里的闪灯播报
 *   0 = 关掉（P1 默认）
 *   P0 阶段它把"UART 发不出来"和"PC 收不到"劈成了两半，功劳很大；
 *   但 P1 每次上电再闪那几下没有新信息，还白占约 250 B code + 1 s 启动时间。
 *   将来怀疑 UART 链路时改回 1 即可。
 *----------------------------------------------------------------------------*/
#define BL_P0_DIAG      0


/*==============================================================================
 * 编译开关
 *   BL_PROBE_ONLY = 1 : 只跑 P0 探针（IAP 自检）然后死循环，不跳 APP
 *   BL_PROBE_ONLY = 0 : 走 P1 启动流程（读参数 -> 校验 -> 跳 APP）
 *
 * 下面两个开关决定"要不要把对应模块编进工程"。P0 阶段关掉它们有两个好处：
 *   1) 这两个模块在 P0 里根本用不到，白占 code（评估版 2KB 很紧张）；
 *   2) ★ 关键：ota_jump.c 里的 ota_jump_to_app() 通过函数指针间接调用，
 *      链接器遇到间接调用会**放弃 overlay 分析**，导致所有函数的 data 帧累加，
 *      数据段很快撑爆 128 字节。P0 阶段去掉它，overlay 恢复正常。
 * 注意：置 0 时要把对应 .c 从工程里移除，否则只是白编。
 *
 * ★ 2026-10-25：P0 探针已通过（stg 区写-回读成功、BL 未被误擦），
 *   现正式切到 P1 启动流程 —— 这是"第二步"的前提：
 *   若仍为 1，合并镜像烧进去 BL 只会停在诊断循环，APP 永远不会被执行。
 *============================================================================*/
#define BL_PROBE_ONLY   0

#if (BL_PROBE_ONLY != 0)
  #define OTA_BL_USE_JUMP    0
  #define OTA_BL_USE_PARAM   0
#else
  #define OTA_BL_USE_JUMP    1
  #define OTA_BL_USE_PARAM   1
#endif

#if (OTA_BL_USE_PARAM != 0)
  #include "param.h"
#endif
#if (OTA_BL_USE_JUMP != 0)
  #include "ota_jump.h"
#endif

/*------------------------------------------------------------------------------
 * 纯空转延时（不依赖 Timer0 / 不依赖中断）
 *
 *   开机第一次闪灯绝对不能用 bsp_timer_delay_ms()：那个函数靠 Timer0 的中断
 *   累加 s_ms，中断没跑起来就是**死等**，而且死在"第一句日志之前"，
 *   串口一个字节都看不到 —— 这正是最难查的一种失败形态。
 *   这里用 volatile 自减保证不会被优化掉，时长不精确但"肉眼看得见"足够。
 *----------------------------------------------------------------------------*/
static void bl_blink(u8 times, u16 outer)
{
    volatile u16 i, j;
    u8 k;

    for (k = 0; k < times; k++)
    {
        bsp_led_on();
        for (i = 0; i < outer; i++)
            for (j = 0; j < 600; j++) { }
        bsp_led_off();
        for (i = 0; i < outer; i++)
            for (j = 0; j < 600; j++) { }
    }
}

/* LED 灭着空转，用来在两次闪灯之间做分隔（同样不依赖任何外设） */
static void bl_idle(u16 outer)
{
    volatile u16 i, j;
    for (i = 0; i < outer; i++)
        for (j = 0; j < 600; j++) { }
}

/*------------------------------------------------------------------------------
 * ★ 串口发字节能力自检：返回"真正移出去"的字节数
 *   判断依据是 **TI 标志是否被硬件置位**：
 *     · TI 置位  -> 移位寄存器把整个字节移完了，UART 发送链路是活的，
 *                   那么 PC 收不到就只能是下游（引脚模式/线/CH340/上位机）；
 *     · TI 始终不置位 -> 波特率发生器或 SCON 配置就不对，字节根本没动。
 *
 *   这条把"MCU 发不出来"和"PC 收不到"彻底劈开，而且**不需要电脑参与**，
 *   结果直接用灯闪出来（闪几下 = 成功几个字节）。
 *
 *   ★ P1 起由 BL_P0_DIAG 关闭（默认 0）：这两个探针在 P0 阶段已经把
 *     "UART 能不能发" 定死了，P1 每一次上电再闪一遍纯属浪费 ——
 *     实测它们合计约 250 B code + 约 1 秒启动延时，
 *     而 BL 的 16 KB 槽要给 A2 的搬运逻辑腾地方（见 docs §8.13）。
 *----------------------------------------------------------------------------*/
#if (BL_P0_DIAG != 0)
static u8 bl_tx_probe(u8 n)
{
    u16 xdata g;
    u8  ok = 0;
    u8  i;

    for (i = 0; i < n; i++)
    {
        SBUF = 'U';
        g = 20000;
        while (!TI && --g) { }
        if (TI) { TI = 0; ok++; }
    }
    return ok;
}

/*------------------------------------------------------------------------------
 * 引脚自检：把 P3.1 配成推挽并输出高，然后**读回引脚电平**
 *
 *   返回 1 = 引脚确实被拉到高（线路正常）
 *   返回 0 = 写完高电平、读回来还是低 —— 板子上有东西在拽这根线：
 *            下拉电阻、指示灯、跳线帽接错、或者它根本不是 TXD 那根脚。
 *   这是"能下载但发不出数据"的另一个典型成因，和"准双向驱动不够"是两回事。
 *   注意：只有在 1T/推挽模式下读 Px 才反映真实引脚电平。
 *----------------------------------------------------------------------------*/
static u8 bl_pin_probe(void)
{
    P3M1 &= (u8)~0x02;
    P3M0 |= (u8)0x02;                  /* P3.1 推挽 */
    P3   |= (u8)0x02;                  /* 输出高   */
    bl_idle(2);
    return (u8)((P3 & (u8)0x02) ? 1U : 0U);
}
#endif  /* BL_P0_DIAG */


/*------------------------------------------------------------------------------
 * P0：IAP 自检（这段跑一次就能定死 OTA_IAP_CAN_REACH_CODE）
 *----------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------
 * 自检结果留档：主循环要周期重播，否则串口打开晚了就什么都看不到
 *----------------------------------------------------------------------------*/
static u8 s_stl_rc = 0xFF;      /* flash_iap_selftest() 返回码；0xFF = 还没跑 */
static u8 s_init_rc = 0xFF;     /* flash_iap_init() 返回码              */

static void bl_iap_check(void)
{
    ota_ret_t rc;
    u32       fosc;

    /* ★ fosc 先取到局部变量再传进 LOG
     *   规矩：**LOG 的参数列表里只放变量和常量，不放函数调用。**
     *   被调函数的局部 xdata 变量仍可能与调用方的活跃变量共用 overlay 地址，
     *   "先算到变量、再传变量"永远是最稳的写法。
     *   （varargs 全局参数块那个更严重的坑已在 v0.7 彻底移除，见 log.h 文件头。） */
    fosc = bsp_clock_fosc();

    LOG_I1("BL", "==== %s ====", BL_VERSION);
    LOG_I1("BL", "fosc=%luHz", fosc);

    /* 先用已知值验证"日志本身没骗人"，再相信后面的诊断结论。
     * ★ LOG_SELFTEST=0 时不编译：BL 的 16K 槽已经只剩一百多字节，
     *   这 5 行值约 200 B code，优先让位给真正的测量。 */
#if (LOG_SELFTEST != 0)
    log_selftest();
#endif

    rc = flash_iap_init(128);              /* 必须与 STC-ISP 的 EEPROM=128K 一致 */
    s_init_rc = (u8)rc;
    LOG_I1("BL", "flash_iap_init(128) -> %u", (u16)rc);
    if (rc != OTA_OK)
    {
        LOG_E0("BL", "IAP init failed");
        return;
    }

    rc = flash_iap_selftest();
    s_stl_rc = (u8)rc;
    if (rc == OTA_OK)         LOG_I0("BL", "selftest OK");
    else if (rc == OTA_ERR_SUPPORT)
                              /* 措辞纠正（v0.15）：BL_STG_PROBE=0 时 STAGING 只能是
                               * UNTESTED，绝不能写成 "NOT writable" —— 那句话把人
                               * 往"硬件到不了 FE 块"带，白烧过一版。真实结论看
                               * bl_summary 里的 `-> STAGING UNTESTED` 那一行。 */
                              LOG_W0("BL", "selftest: STAGING untested (probe off), not a fault");
    else                      LOG_E1("BL", "selftest FAIL rc=%u", (u16)rc);
}

/*------------------------------------------------------------------------------
 * v0.24：`bl_iap_alias()`（v0.23 的 bit16 决定性实验）已删除 —— 它一次答完了：
 *
 *       alias e0 w0            擦除 + 单字节编程都成功
 *       alias t0=165 r1=0      写 0xA5 到 STAGING[0]，读回 165 ⇒ 0~64K 读写都通
 *       alias v1=2   v2=117    raw 直读 0x10000 = 2（BL 入口 LJMP）、
 *                              read_byte(0x14000) = 0x75（APP 入口首字节）
 *
 *   ⇒ **IAP 读通路完全正常，bit16（IAP_ADDRE）完全生效，0x10000 没有别名到 0x0000。**
 *     三种解释一次性排除 (A)(B)(C)，"查 EEPROM 选项 / 换头文件"那条路彻底关闭。
 *
 *   ⇒ 那 `bl head: ff ff ff ff` 是什么？**是 selftest 自己把结果存错了地方**：
 *     汇编实测它把 b0..b3 放在 R15/R12/R13/R11，而 read_byte 那条调用链不保存
 *     R12/R11，于是 4 次连读之后 b0 已不是 0x02 ⇒ 提前 return 6 ⇒ 两处探针全被跳过。
 *     v0.24 已把 selftest 的这 5 个字节改成 static（见 flash_iap.c 顶部注释）。
 *
 *   ⇒ 本探针把 STAGING 首字节写成 0xA5，导致上一版 `[VER] step=2`（魔数不对）。
 *     step=1（读头）能过就证明 BL 读 STAGING 的链路是通的 —— 这一步已达成。
 *     删掉探针后 STAGING 恢复干净，A1 才能真正被判。
 *------------------------------------------------------------------------------*/


#if (BL_PROBE_ONLY != 0)
static void bl_probe(void)
{
    bl_iap_check();
    LOG_I0("BL", "--- P0 done, see the summary line below ---");
}
#endif

/* 探针结果转字符串（flash_iap.c 里那个是 static，跨文件不可用） */
static const char *bl_pstr(u8 r)
{
    if (r == FLASH_PROBE_WRITABLE)     return "W";
    if (r == FLASH_PROBE_NOT_WRITABLE) return "NA";
    return "untested";
}

/*------------------------------------------------------------------------------
 * ★ 诊断摘要重播
 *
 *   背景：板载 CH340 + USB 供电的最小系统板，**没法做到"先开串口再上电"** ——
 *   插线就是上电；而下载时 STC-ISP 又独占 COM 口，下载一完成芯片立刻运行，
 *   开机那几十毫秒的日志必然错过。用户只能"下载完 -> 关掉 ISP -> 开串口助手"，
 *   等他能看到串口时，程序早就跑进主循环了。
 *
 *   所以关键结论一个字都不能只看一次：把探针结果存下来（flash_iap_probe_result），
 *   主循环每 8 秒重播一遍。这样任何时刻打开串口，最多等 8 秒就能看全。
 *
 *   ★★ 结论只允许来自**实测**的探针结果 ★★
 *   不能用 "selftest 返回 0 就算通过" 这种推断 —— 曾经就因为 APP 区非空时
 *   跳过探针却硬设 writable=1，打出了 "OTA arch (a) is valid" 的假结论。
 *----------------------------------------------------------------------------*/
static void bl_summary(void)
{
    u8 rs;      /* STAGING 区探针结果 */
    u8 ra;      /* APP 区探针结果     */

    if (s_init_rc == 0xFF) return;         /* 没跑过 selftest（P1 启动流程） */

    rs = flash_iap_probe_result(0);
    ra = flash_iap_probe_result(1);

    LOG_I4("BL", "== summary: init=%u selftest=%u stg=%s app=%s ==", (u16)s_init_rc, (u16)s_stl_rc, bl_pstr(rs), bl_pstr(ra));

    if (s_init_rc != 0)
    {
        LOG_E0("BL", "-> IAP init failed, cannot write anything");
        return;
    }
    /* ★ 必须把"未测"和"不可写"分开说 ★
     *   BL_STG_PROBE=0 时自检只读不写，s_r_stg 只能是 UNTESTED。
     *   旧文案在这两种情况下都打 "NOT writable"，把人往"硬件到不了 FE 块"
     *   的方向带 —— v0.13 实测就误读了一轮。
     *
     * ★ v0.33：本函数在 `rs == UNTESTED` 之后就 return，而现在 s_r_stg 恒为
     *   UNTESTED（两处探针已删），所以下面那些 "writable / NOT writable"
     *   分支已成死代码，连同它们的字符串一起删除腾 16 K 槽。
     *   真要恢复，把 BL_STG_PROBE 改成 1 并恢复 flash_iap_selftest 的探针调用。 */
    if (rs == FLASH_PROBE_UNTESTED)
    {
        return;
    }
}

/*------------------------------------------------------------------------------
 * P1：启动流程（读参数 -> 判待生效 -> 跳 APP）
 * 用宏整体包住：P0 阶段不参与编译，避免 C174 unreferenced 警告
 *----------------------------------------------------------------------------*/
#if (BL_PROBE_ONLY == 0)
/*------------------------------------------------------------------------------
 * 每次上电是否做一次 IAP 自检（含 STAGING 写-回读探针）
 *   1 = P1 调试期：每次上电实测"这版固件还能不能写 flash"，多花约 0.5 s。
 *       OTA 的全部前提就是这一条，调试期不能省。
 *   0 = 量产：跳过，开机直接跳 APP。
 *   TODO(P2)：改成"param 里记录的 selftest 结果在 N 次启动内有效"，
 *             避免每次上电都擦一遍 STAGING 首扇区（会毁掉断点续传数据）。
 *----------------------------------------------------------------------------*/
#define BL_BOOT_IAP_CHECK   1

/*------------------------------------------------------------------------------
 * P1-A1：待安装镜像的**只读**检查
 *   1 = 当 param.ota_state == PENDING_REBOOT 时，读 STAGING 的镜像头 + 算正文
 *       CRC32，把结论打到串口；**全程不擦、不写、不搬**。
 *   0 = 完全不看，直接跳 APP（等于回到 P0 行为）。
 *
 *   打开它的唯一目的：在动用"擦掉 APP 区"这个不可逆动作之前，
 *   先拿到"BL 能正确读懂 STAGING"这个结论。
 *
 *   ★ 唯一会写 Flash 的地方是校验失败后把 state 改回 IDLE 并 param_save(1)，
 *     写的是参数区（双备份，与 STAGING/APP 无关）。不写的话每次上电都会
 *     重试一次校验，白等好几秒。
 *----------------------------------------------------------------------------*/
#define BL_OTA_A1_CHECK     1

/*------------------------------------------------------------------------------
 * ★★★ P1-A2：搬运生效（v0.33 第一版）★★★
 *   1 = 校验通过后，把 STAGING 里的镜像**擦写进 APP 区**，再回读校验，
 *       成功后才跳转（等于真正完成一次本地 OTA）。
 *   0 = 退回 A1 的只读行为（校验完只打一行，不擦不写）。
 *
 *   ★ A2 是**第一条会让固件自己被替换**的路径，所以设计上做了三件事：
 *     ① 幂等：APP 区正文 CRC 已等于镜像头值就直接算成功，不擦不写。
 *        —— BL 的 param_save 目前是坏的（APP 侧实测 rc=6，下次上电 BL 就报
 *           param empty/corrupt），**"装好了"这个状态没法持久化**；不幂等的话
 *           每一轮上电都会重擦重写 48 K。
 *     ② 自愈：先擦后搬，源永远在 STAGING（只读）⇒ 中途掉电/复位后，
 *        下一次上电必然发现 CRC 不符，自动整段重来。
 *     ③ 可回退：把本开关改回 0 重烧，就回到 A1 的"只看不动"。
 *----------------------------------------------------------------------------*/
#define BL_OTA_A2_APPLY     1

/*------------------------------------------------------------------------------
 * ★★ A1 探针（临时诊断；BL_A1_PROBE = 1 时启用，问题定位后请改回 0）★★
 *
 *   v0.11 上板实测：APP 侧 A1 造镜像成功 —— 正文 383 块、
 *   "写 + 整块回读校验"全部返回 OTA_OK，最后还打了
 *   `A1 image ready len=49088 crc=f7589f23`。
 *   但断电后 BL 报 `[VER] STAGING fail rc=12 step=0`。
 *
 *   rc=12 就是 OTA_ERR_VERIFY(0x0C)，而 step=0 对应 ota_verify.c:101 那句
 *       if (s_hdr.magic == 0xFFFFFFFFUL) return OTA_ERR_VERIFY;
 *   即 BL 从 STAGING+0 读到的 4 个字节全是 0xFF。
 *
 *   ★ 关键：`flash_iap_read()` 的实现是
 *       for (i...) { buf[i] = 0xFF; flash_iap_raw(READ, addr+i, &buf[i]); }
 *     —— **先把目标缓冲填 0xFF 再让 IAP 覆盖**。
 *     所以"读到全 FF"有两种完全不同的含义，必须分开：
 *         (a) Flash 里真的是 FF（没写进去 / 被擦了）
 *         (b) 读根本没生效，缓冲停在预填值
 *
 *   探针三条通路，一次问清：
 *     ① 逐字节读 STAGING[0..3]      → 走 flash_iap_read_byte，与 `bl head` 同路
 *     ② 批量读 STAGING[0..3]        → 走 flash_iap_read + xdata 目标，
 *                                     正是 ver_hdr 用的那条路
 *     ③ 逐字节读 STAGING[0x40..0x43]→ 正文首字节，A1 一定写过，
 *                                     应为 APP 入口 `75 84 1 7e`（对照）
 *
 *   判读表：
 *     ①=49 41 54 4f                → 头在！是批量读有问题（改 ver_hdr 读法）
 *     ①=ff ff 且 ③=75 84          → 头没写进去，正文在（问题在 APP 侧）
 *     ①=ff ff 且 ③=ff ff          → STAGING 整个是空的，什么都没落地
 *     ①②不一致                     → 两条读通路行为不同，锁定批量读
 *----------------------------------------------------------------------------*/
/* v0.27：关掉。它的三个问题都已回答完（见下面 v0.13 的注释与 RULES_DETAIL），
 * 而 param 有效时 g_pdg[] / g_iap_dbg 根本不会被写，打出来的就是**未初始化的
 * xdata 垃圾**（v0.26 第二次上电实测 ps1 e11302 i97 / wdbg a=1386217503）
 * —— 留着只会误导判读。同时腾出 ~200 B 给 A2（16K 槽本来就只剩 286 B）。 */
#define BL_A1_PROBE         0

#if (BL_A1_PROBE != 0)
/* --- v0.13：探针改为"查 param 为什么读不到"（A1 头/正文已确认在位）-----------
 *
 *  v0.12 探针实测（已是定论，不用再测）：
 *      pg hdr  byte = 4F 54 41 49      ← magic 在（"OTAI"）
 *      pg hdr  bulk = 4F 54 41 49      ← 批量读通路也正常
 *      pg body byte = 75 84 01 7E      ← 正文在（APP 入口）
 *  ⇒ STAGING 里镜像完整，`flash_iap_read(0)` 没问题。
 *
 *  但同一个上电周期里：BL `[698] param empty/corrupt, defaults created`、state=0，
 *  紧接着 APP `param_init OK`、`seq=2`、`ota_state=3`。
 *  两边的 param.c 目标码逐字节相同（SRC 反汇编 diff 只有 8 行注释差异）。
 *  ⇒ 槽里 magic/len/fmt 全对，只有 CRC 不符。该行校验和自洽（原文反算 = 57）。
 *  ⇒ 结论：**槽里那 462 字节正文 ≠ 保存时算 CRC 用的那 462 字节。**
 *  ⇒ 但 param_save 的返回码一直被 (void) 丢掉，四种可能还没分开：
 *       ① 擦除被门禁拒绝   ② 擦不干净（残留位把正文 AND 掉）
 *       ③ 写失败/写不全     ④ flash_iap_write 的写后校验有洞
 *
 *  所以 v0.14 不再"读槽看结果"，而是让 param_save() 自己汇报四个动作的结果
 *  （g_pdg[] 见 param.h）。判读：
 *     psv e=0 i=0  → 擦不干净，第一个嫌疑人
 *     psv w!=0     → 写后校验当场发现差异
 *     psv d=0 且 CRC 仍不符 → 只剩"CRC 计算本身有问题"一种可能
 *--------------------------------------------------------------------------*/
static void bl_param_probe(void)
{
    /* ★ v0.24：这两个中转必须 static。
     *   v0.23 把三次 read_byte 直接写在 LOG 的参数表里，实测打出
     *   `pra2 255 255 0` —— 而**同一上电**里 `ps2 w0 d0`（466 B 读回零差异）、
     *   `wdbg a=49152 l=466 nf=0`（写函数拿到的正是 param_save 该传的值）。
     *   两者不可能同时为真 ⇒ 是参数求值互相踩：C251 在调用点复用参数块/寄存器，
     *   后一次 read_byte 会改掉前一次的返回值槽。
     *   ⇒ 先落 static，再只读 static（铁律 5/9 的同一个道理）。 */
    static u8 s_pa, s_pb;
    /* v0.21：cb/ca 已经证明过 g_p 在写窗口里稳定（v0.20 两者都是 c8b22b74），
     * 这条线关闭，把这一行换成 param_save 内部**四个动作各自的返回码**：
     *      e = 擦除 rc，i = 擦完是否真的全 FF（1=干净），
     *      w = 写入 rc（含整块回读校验），d = 读回与 g_p 不同的字节数。
     * 这样一行的信息量 = "擦对了没 / 擦净了没 / 写进去没 / 差多少"，
     * 而 pra2 那行补上 A/B 槽首字节和读回自身的 rc —— 整条链路没有盲区。
     *
     * ★ v0.23 拆成两行各 2 个参数：v0.22 实测这一行在上板日志里被截成 "ps"
     *   （行尾校验和不符）。串口丢字节是常态，一行 4 个 %u 太长；拆短降低
     *   单行被截断的概率，也便于用和值逐行判定哪一半可信。 */
    LOG_I2("BL", "ps1 e%u i%u",
           (u16)(g_pdg[0] >> 8), (u16)(g_pdg[0] & 0xFFUL));
    LOG_I2("BL", "ps2 w%u d%u",
           (u16)g_pdg[1], (u16)(g_pdg[4] >> 16));

    /* ★ 判"param 到底写进哪个槽、有没有落地"的唯一直接证据。
     *   param_t 的首字节是 magic 的低 8 位 = 'A' = 0x41 = 65。
     *     65 ff → 写在 A 槽且落地；ff 65 → 写在 B 槽且落地；
     *     ff ff → 两个槽都没有 magic ⇒ 写根本没落地。
     *
     * ★ v0.21 改十进制（原来用 %x）：v0.20 实测这一格打出 `pra2 ff 800ff`，
     *   5 位十六进制 = 参数高 16 位漏进来了（(u32)(u16) 的截断没生效）。
     *   %u 走的是另一条转换通路（长除），同样的漏位会显示成一个大数而不是
     *   悄悄混进十六进制串里，一眼能认出是"参数脏"而不是"值就是 800ff"。
     *   第三个值 rr = g_pdg_rc = 读回本身的 rc（0 = 读通路正常）。 */
    s_pa = flash_iap_read_byte(OTA_PARAM_A_BASE);
    s_pb = flash_iap_read_byte(OTA_PARAM_B_BASE);
    LOG_I3("BL", "pra2 %u %u %u", (u16)s_pa, (u16)s_pb, (u16)g_pdg_rc);

    /* ★ v0.22 唯一新增判据：直接念出 flash_iap_write 内部拿到的三个数。
     *      wdbg a=49152 l=466 nf=0    → 写真的跑了、466 次 PROGRAM 全部被接受
     *                                   ⇒ 数据仍不落地 = 硬件/时序（或读回不对）
     *      wdbg a=49152 l=466 nf=466  → 每一次 PROGRAM 都被门禁拒了
     *                                   ⇒ s_eeprom_size / ota_part_of 在窗口内变了
     *      wdbg a=49152 l=0   nf=0    → len 槽被踩（v0.21 的形态），修法未生效
     *      wdbg a=0         l=0       → addr 槽也被踩
     *   注：f 是粘滞的 CMD_FAIL，可能带着擦除那一步留下的值；
     *       "nf>0 且 f=0" 才是"被软件门禁拒"，"nf>0 且 f=1" 是硬件拒。 */
    LOG_I4("BL", "wdbg a=%u l=%u nf=%u f=%u",
           g_iap_dbg[0], (u16)g_iap_dbg[1], (u16)g_iap_dbg[3],
           (u16)flash_iap_last_fail());
}
#endif

/*------------------------------------------------------------------------------
 * v0.22：`bl_iap_probe()`（P1/P2 两次 A/B 擦写）已删除 —— 它的问题已经回答完了：
 *      P1 e0 r0 f0 b80   源 = g_p，  466 B 写 + 内部校验全过、首字节落地
 *      P2 e0 r0 f0 b90   源 = s_sw， 同上
 *   ⇒ 写通路本身无罪，"源缓冲是 g_p 还是静态数组"也不是变量
 *     （v0.20 的 w466 是 s_sw 过的，本版 P1 换成 g_p 同样过）。
 *   而同一上电里 param_save（隔着 is_erased、栈更深）写同址同长却一个字节都不落地。
 *
 *   ⇒ 剩下的唯一嫌疑就是**形参本身**：C251 的 ?func??BYTE 参数块按 7 字节步进，
 *     而 addr@+0 / len@+8 实际要 10 字节。v0.22 已把 flash_iap.c 里所有
 *     "形参在被调函数返回后还被读"的地方全部改成"入口即拷进模块级 static"。
 *   本轮判据只剩一行 `wdbg`（见 bl_param_probe），它直接念出写函数内部
 *   拿到的 addr / len / 被拒次数。
 *--------------------------------------------------------------------------*/


static void bl_boot(void)
{
    param_t *p;
#if (BL_OTA_A2_APPLY != 0)
    /* A2 需要镜像头的 img_len/img_crc32 —— 42 B，必须 static xdata
     *（BL 的栈只有 256 B，任何 >32 B 的对象都不许放栈上，铁律 6）。*/
    static ota_img_hdr_t xdata s_a2hdr;
#endif
    ota_ret_t rc;
    u8 app0;

    /* --- 1) IAP 自检 ---------------------------------------------------- */
#if (BL_BOOT_IAP_CHECK != 0)
    bl_iap_check();
    /* v0.24：bl_iap_alias() 已删除（问题答完，见上方说明）。
     * 它每次上电都往 STAGING[0] 写 0xA5，把 A1 镜像头变成"魔数错"，
     * 于是 `[VER]` 永远进 step=2 —— 探针反而挡住了要验的东西。 */
#else
    s_init_rc = 0xFF;   /* 0xFF = 未测；bl_summary 见它会直接跳过，不重播假结论 */
#endif

    /* --- 2) 参数区 ------------------------------------------------------ */
    rc = param_init();
    if (rc == OTA_ERR_CRC)
        LOG_W0("BL", "param empty/corrupt, defaults created");
    else if (rc != OTA_OK)
        LOG_E1("BL", "param_init rc=%u", (u16)rc);

    p = param_get();
    LOG_I4("BL", "seq=%lu boot_cnt=%lu boot_try=%u state=%u", p->seq, p->boot_cnt, (u16)p->boot_try, (u16)p->ota_state);

#if (BL_A1_PROBE != 0)
    /* 无条件执行（不受 state 影响）：state 若被重置成 IDLE 也照样能看到槽里有什么 */
    bl_param_probe();
#endif

    /* --- 3) 待安装镜像检查（P1-A1：只读，不搬、不擦、不写 STAGING/APP）---
     *
     *   APP 侧负责造镜像：把当前 APP 区 48 K 复制到 STAGING、写好镜像头、
     *   置 ota_state = PENDING_REBOOT，然后停机等断电。
     *   重新上电后由这里读出并校验。期望日志：
     *       [BL]  pending install -> verify staging (read-only)
     *       [VER] STAGING ok ver=0.1.0.1
     *       [VER] STAGING ok len=49152 crc=xxxxxxxx
     *       [BL]  staging verified: A1 read-only, apply NOT enabled
     *
     *   ★ 这里刻意只打 W 级日志、不打 I 级：A1 是过渡状态，
     *     A2 打开搬运后这一段会被整体替换，不要让人误以为已经能升级了。
     */
#if (BL_OTA_A1_CHECK != 0)
    /* ★ 临时开关（v0.13）：BL_OTA_A1_FORCE = 1 时不管 state 都校验一次。
     *
     *   理由：param 现在读不出来 → state 永远是 0 → 这个 if 进不去 →
     *   `[VER]` 一次都跑不到，等于白烧一版。强制跑一次的成本只有
     *   "每次上电多算 49 KB 的 CRC"，而收益是同一份日志里同时拿到：
     *       · 探针给出的 param 诊断
     *       · A1 的真实结论（镜像头 + 48 K 正文的 CRC）
     *   定位完成后必须改回 0，否则等于放弃"只有待安装时才校验"这个语义。
     *
     * ★ v0.39（2026-10-27）：已改回 0。理由：A1/A2 都已上板跑通
     *   （`STAGING ok len=49088 crc=b13ea4` → `APPLY OK` → `A2 DONE`），
     *   param 也早就能落盘（seq 一路 1→2→3→4），"state 永远是 0 所以进不去"
     *   这个当初打开它的理由已经完全消失。
     *   现在必须回到生产语义：**只有 state == PENDING_REBOOT 才校验**，
     *   否则 A3 的"提交后回到干净启动"根本走不到 —— 每轮上电都会重新搬一遍。
     *----------------------------------------------------------------------------*/
     /* ★ 开关本身已移到**文件开头**（BL_VERSION 之前），这里是它的判定点。
      *   v0.42：值改回 0，生产语义生效。事故记录见文件开头那条注释。 */
    if (p->ota_state == (u8)OTA_ST_PENDING_REBOOT || (BL_OTA_A1_FORCE != 0))
    {
        LOG_I0("BL", "pending install -> verify staging (read-only)");

#if (BL_OTA_A2_APPLY != 0)
        rc = ota_verify_staging(&s_a2hdr);   /* A2 要用头里的 img_len/img_crc32 */
#else
        rc = ota_verify_staging(0);      /* 传 0：A1 不需要头的内容，省一次拷贝 */
#endif

        if (rc == OTA_OK)
        {
#if (BL_OTA_A2_APPLY != 0)
            /* ★ A2：擦 + 搬 + 回读校验。幂等，见 ota_verify.c 的 ota_apply_staging()。
             *   APPLY OK 之后 APP 区已经是这份镜像 ⇒ 下面的跳转闸门必然放行。 */
            LOG_W0("BL", "A2: apply staging -> app (erase+copy+verify)");
            rc = ota_apply_staging(&s_a2hdr);
            if (rc != OTA_OK)
            {
                LOG_E1("BL", "apply FAIL rc=%u, stay in BL (re-flash to recover)", (u16)rc);
                /* 绝不能跳：APP 区此刻是被擦过/半截写的状态 */
                bl_summary();
                return;
            }

            /* ★★★ v0.39：状态从 IDLE 改成 COMMITTED —— A3 的交接点 ★★★
             *
             *   以前这里写 IDLE，结果"我装好了"这件事在参数里留不下任何痕迹：
             *   上板实测 APP 侧一直是
             *       running_ver = 0.0.0.0   pending_ver = 0.1.0.1
             *       dl offset=49088 total=49152
             *   即"新固件明明在跑，账本上却还是旧版本，而且留着一个假断点"。
             *   更实际的问题：`param_get_download()` 认为 offset>0 且 offset<=total
             *   就是**有效断点** ⇒ 以后真下载时会把上一次 A1 的残留当成断点续传起点。
             *
             *   改法（最小、可自愈、不需要新字段）：
             *     BL 搬完只负责把状态置成 COMMITTED（"镜像已落地，等 APP 确认"），
             *     APP 自检通过后自己把它变成 IDLE 并写 running_ver —— 见
             *     app_main.c 的 app_ota_commit()。
             *   掉电落在这两步之间也没关系：state 停在 COMMITTED，
             *   下一次上电 BL 不校验直接跳（COMMITTED != PENDING_REBOOT），
             *   APP 再提交一次，幂等。 */
            p->ota_state = (u8)OTA_ST_COMMITTED;
            p->boot_try   = 0U;
            param_save(1);

            LOG_I0("BL", "A2 DONE: app updated, booting new firmware");
#else
            LOG_W0("BL", "staging verified: A1 read-only, apply NOT enabled");
            /* ★ 刻意**不改 state**：让它停在 PENDING_REBOOT，
             *   下次上电再校验一遍。A1 的全部价值就在于"能重复观察" ——
             *   单次通过说明不了什么，要连续十几次上电都通过才算读通路可靠。
             *   A2 打开搬运后这一段会被整体替换。 */
#endif
        }
        else
        {
            /* 校验失败：回 IDLE 并**允许重试**，但不能无限重试 ——
             * 每轮重试 APP 都要擦写 48 KB，几十轮就把 Flash 寿命耗掉一截。
             * 这里借用 boot_try 当 A1 的重试计数（A1 阶段 boot_try 没有别的用途，
             * APP_CLEAR_BOOT_TRY 现在是 0，不会被 APP 清零）。
             *   · boot_try < 20：清 rollback_flag → APP 下次上电会重造镜像
             *   · boot_try = 20：保留标记 → 停止重试，要再来一次就重新烧录
             *
             * ★ v0.24 把上限从 3 提到 20。原因：**以前这个分支形同虚设** ——
             *   param_save 存不下来（形参块 + 假 6 双重阻断），boot_try 永远是 0，
             *   所以"3 次放弃"从来没触发过。v0.24 起 param_save 真能落地，
             *   boot_try 会真的累加，3 次上限立刻变成"只能试 3 次"。
             *   而 A1 的价值恰恰是"能重复观察"（见上面成功分支的注释），
             *   20 轮 = 20 次 APP 重造 48 K 镜像，对 Flash 寿命无影响。
             */
            LOG_E1("BL", "staging verify FAIL rc=%u", (u16)rc);

            p->ota_state = (u8)OTA_ST_IDLE;

            if (p->boot_try < 20U)
            {
                p->boot_try++;
                p->rollback_flag = 0U;
                LOG_W1("BL", "A1 retry %u/20 armed (APP will rebuild)", (u16)p->boot_try);
            }
            else
            {
                LOG_E0("BL", "A1 gave up after 20 tries; re-flash to retry");
            }

            param_save(1);
        }
    }
#endif

    /* --- 4) 重播探针结论（主循环里每 8 s 还会再播一次）------------------ */
    bl_summary();

    /* --- 5) 跳转前的最小镜像校验 ----------------------------------------
     *
     *   ★ APP 入口处放的是 C251 的 `?C_C51STARTUP`（启动代码），不是 LJMP。
     *     实测（2026-10-25，C251 V5.60）两处完全一致：
     *         BL  ?C_C51STARTUP @0xFF0027: 75 84 01 7E 44 04 1F ...
     *         APP ?C_C51STARTUP @0xFF4000: 75 84 01 7E 44 04 1F ...
     *     硬件复位是 FF:0000 的 LJMP 跳 startup；BL 跳 APP 则是**直达 startup**。
     *     所以判据不能是"首字节 = 0x02"，那是 BL 自己的 start251 特征。
     *
     *   只挡两种必死的情况：
     *     0xFF = 空片（还没烧 APP）
     *     0x00 = 全零（擦除失败 / 半截镜像留下的残留）
     *   其余放行并把前 4 字节打出来；完整镜像头 + CRC 校验等 ota_verify.c。
     */
    app0 = flash_iap_read_byte(OTA_APP_IAP_BASE);
    if (app0 == 0xFF || app0 == 0x00)
    {
        LOG_E1("BL", "APP head=%x -> refuse to jump", (u16)app0);
        if (app0 == 0x00)
        {
            LOG_E0("BL", "  head is 00: burned image damaged / half written");
            LOG_E0("BL", "  (old IAP_CMD bug could zero 0xFF0000 & 0xFF4000)");
        }
        LOG_E0("BL", "  flash the merged image (BL+APP), not BL-only");
        return;
    }
    /* --- 5.1) 读 APP 首 4 字节 ------------------------------------------
     *   ★ 先把 4 个字节读进变量，再整体传给 LOG。
     *
     *   曾经写成 LOG_I("BL","APP head=%x %x %x %x, jump",
     *                  flash_iap_read_byte(a), flash_iap_read_byte(a+1), ...)
     *   结果打出来是 `APP head=    1  , jump` —— 前三个字段全空。
     *   而同一份字节在 ota_jump.c 里用局部变量打成 `75 84 1 7e`，完全正确。
     *
     *   4 次调用各自会把 EA 拉低再恢复（IAP 临界区），参数区的存取与
     *   overlay 分配互相干扰。v0.7 起参数区换成了固定槽，但"先读变量、
     *   再传变量"这条规矩继续保留 —— 多读一次不花什么钱，一旦踩坑很贵。
     */
    /* ★ 这两行（APP head a / b）自 P0 起从未错过，而 APP 自己会打
     *   `read app[0..3] = 75 84 1 7e`，信息完全重复 —— 2026-10-26 删掉腾 16K 槽。
     *   只保留下面那行入口地址：它是"跳转闸门"唯一的外部证据。 */

    /* --- 6) 跳转 -------------------------------------------------------- */
    /* ★ 入口地址拆成 hi/lo 两个 u16 打印，不用 %lx。
     *   原因：%lx 在这条路径上实测打不出数字（v0.5 的
     *   `jump -> 0x` 后面直接断掉），而 %x 在同一个进程里从未出过错。
     *   期望：hi=ff lo=4000（即 0xFF4000）。 */
    LOG_I2("BL", "jump entry hi=%x lo=%x", (u16)((OTA_APP_CODE_BASE) >> 16), (u16)(OTA_APP_CODE_BASE));
    rc = ota_jump_to_app(OTA_APP_CODE_BASE);

    /* 正常情况下永远走不到这里（跳过去就不会回来） */
    LOG_W1("BL", "jump returned rc=%u, stay in BL", (u16)rc);
}
#endif  /* BL_PROBE_ONLY == 0 */

/*------------------------------------------------------------------------------
 * ★ 诊断宽限期：用纯轮询把身份信息反复播报约 9 秒
 *
 *   为什么必须有这一段：
 *   板载 CH340 + USB 供电的最小系统板，**插线就是上电**，中间没有"先开串口
 *   再上电"的操作空间；而下载时 STC-ISP 又独占 COM 口，下载一完成芯片立刻
 *   运行。用户唯一的操作序列只能是：
 *        点下载 -> 等完成 -> 关掉 STC-ISP -> 打开串口助手
 *   这一串动作最快也要好几秒，而原来的 BOOT-POLL 只发 3 遍（4 ms），
 *   必然全部错过 —— 现场表现就是"一个字节都没有"。
 *
 *   所以这里把窗口拉到约 9 秒，并且走 EA=0 的**纯轮询**路径，
 *   不依赖 Timer0、不依赖中断、不依赖日志模块。
 *   串口助手什么时候打开，都能在窗口内接住。
 *
 *   ★ P1 起 BL_DIAG_WINDOW = 0：正式启动流程不再白等 9 秒。
 *     日志不会因此丢掉 —— 跳转成功后由 APP 的心跳日志接着播
 *     （APP/app_main.c 主循环每 2 s 一行 alive），跳转失败则由 BL
 *     自己的主循环心跳接着播。两条路都有持续输出。
 *----------------------------------------------------------------------------*/
#define BL_DIAG_WINDOW   0          /* 遍数；每遍约 220 ms，40 遍 ≈ 9 s。0 = 关闭 */

static void bl_diag_window(void)
{
#if (BL_DIAG_WINDOW > 0)
    u8 k;

    for (k = 0; k < BL_DIAG_WINDOW; k++)
    {
        bsp_uart_puts("\r\n[BOOT-POLL] poll-mode ok, uart1 p3.1, fosc=11.0592MHz\r\n");
        bl_idle(1000);
    }
#endif
}

/*------------------------------------------------------------------------------*/
void main(void)
{
    u8 uart_ok;

    EA = 0;                                 /* 先关总中断，逐项初始化 */

    bsp_clock_init();
    bsp_gpio_init();

    /*==================================================================
     * ★ 第 0 段：裸通路自检（最重要的一段，出问题时全靠它定位）
     *
     *   此时 EA=0、Timer0 没开、日志模块没初始化 —— 发送走 bsp_uart_putc
     *   的"EA=0 纯轮询"分支，一个字节一个字节硬等 TI。
     *
     *   这一段能看到字符，就同时证明了四件事：
     *     · 程序确实跑到了 main（没卡在 ISP、没跑飞）
     *     · 主频（选项字节里的 IRC）是对的
     *     · P3.1 到 CH340 的线是通的
     *     · UART1 的寄存器配置（SCON/AUXR/T2）是对的
     *   看不到，就说明问题在这四者之一，且与日志/中断/定时器无关，
     *   排查范围直接砍掉一大半。
     *
     *   发 3 遍是因为串口助手常常在 MCU 已经开始发之后才打开，
     *   前一遍会丢。
     *===============================================================*/
    uart_ok = bsp_uart_init(115200);
    if (!uart_ok)
    {
        /* 串口初始化失败（几乎只可能是 ring_init 参数错）：快闪 2 下循环 */
        for (;;) bl_blink(2, 120);
    }

    /* 启动标记：慢闪 1 下，把"上电了"和后面的计数分开，避免误读 */
    bl_blink(1, 400);
    bl_idle(200);

#if (BL_P0_DIAG != 0)
    /* ★ 计数 ①：快闪 n 下 = 真正被硬件移出去的字节数（发 8 个，n 应为 8）
     *   n = 8  -> UART 发送链路活的，PC 收不到就是下游问题（引脚/CH340/上位机）
     *   n = 0  -> TI 从未置位，波特率发生器或 SCON 根本没生效
     */
    bl_blink(bl_tx_probe(8), 100);
    bl_idle(500);                           /* 灭 1.5 秒做分隔 */

    /* ★ 计数 ②：慢闪 1 下 = P3.1 能被拉高（线路正常）
     *            慢闪 3 下 = P3.1 被外部拽低（板上有负载/接错脚）      */
    bl_blink(bl_pin_probe() ? 1 : 3, 400);
    bl_idle(400);
#endif  /* BL_P0_DIAG */

    /* ★ 第 0.5 段：诊断宽限期（约 9 秒，纯轮询，任何时刻开串口都能接住）
     *   见 bl_diag_window() 上方注释。P1 恢复正常启动时 BL_DIAG_WINDOW 改 0。 */
    bl_diag_window();

    /* ★ 第 1 段：不依赖 Timer0 的目视心跳 —— 灯灭说明程序已经跑过这一行 */
    bl_blink(1, 400);

    bsp_timer_init_1ms();
#if (BL_INIT_WDT != 0)
    bsp_wdt_init(WDT_PS_32);                /* 约数百 ms ~ 数 s，按手册核 */
#endif

#if (OTA_BL_USE_JUMP != 0)
    ota_isr_table_init();                   /* 向量跳板必须先于 EA=1 */
#endif
    log_init(LOG_LVL_DBG);

    EA = 1;                                 /* 从这里开始串口/时基中断生效 */

    LOG_I2("BL", "boot, bl_code=0x%lx app_code=0x%lx", OTA_BL_CODE_BASE, OTA_APP_CODE_BASE);

#if (BL_PROBE_ONLY)
    bl_probe();
#else
    bl_boot();
#endif

    /*==================================================================
     * 主循环：喂狗 + 闪灯 + ★ 周期心跳日志
     *
     *   ★ 为什么要周期打日志（这是最容易误诊的一条）：
     *     开机那一段（BOOT-POLL ×3 + bl_probe 的 6 行）总共约 40 个字节，
     *     115200 下 4 ms 就发完了。串口助手如果是在**上电之后**才打开的，
     *     一个字节都收不到 —— 现场表现和"根本没发出去"完全一样。
     *     加了周期心跳后，任何时候打开串口都能立刻看到输出，
     *     这个假故障就再也不会出现。
     *
     *   ★ 它同时验证 EA=1 之后的中断发送路径：
     *     心跳能连续打出来，说明 TI 中断和 s_tx_idle 握手都是活的。
     *     如果只打出第一行就没了、灯也不闪了 -> 卡在 bsp_uart_putc。
     *===============================================================*/
    {
        u32 t    = bsp_timer_ms();
        u16 tick = 0;

        LOG_I0("BL", "loop alive, heartbeat 2s");
        for (;;)
        {
            bsp_wdt_feed();                 /* 无论有没有 init，都照喂 */
            if (bsp_timer_elapsed(t, 500))
            {
                u8 p3v;

                t = bsp_timer_ms();
                bsp_led_toggle();
                tick++;

                if ((tick & 0x03) == 0)     /* 每 4 个 500ms = 2 s */
                {
                    p3v = P3;               /* bit1 = P3.1 引脚实际电平 */
                    /* ★ 去掉原来的 `%s` + 三目字面量（`p3v&2 ? "" : " (txd low!)"`）。
                     *   一是有违"LOG 参数里不放三目/字面量指针"的铁律，
                     *   二是这行是心跳，越简单越可信：p3 稳定为 3b 就说明
                     *   P3.1 是高（TXD 空闲电平正确）。异常时这行本身就是证据。 */
                    LOG_I2("BL", "alive #%u p3=%x", (u16)(tick >> 2), (u16)p3v);
                }
                if ((tick & 0x0F) == 0)     /* 每 16 个 500ms = 8 s 重播摘要 */
                {
                    bl_summary();
                }
            }
        }
    }
}
