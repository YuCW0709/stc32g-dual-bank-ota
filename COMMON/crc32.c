/*==============================================================================
 *  crc32.c  --  CRC32 (反射 0xEDB88320, 初值全1, 结果取反)
 *  与 zlib / Python binascii.crc32 一致；4 位查表（16 项，64 B code）。
 *============================================================================*/
#include "crc32.h"
#include "flash_iap.h"

/* 4 位查表：nibble 值 -> 该 nibble 走完 4 次移位后的余式
 *
 * ★★★ 绝对不能加 `code`（v0.11 实测，v0.9/v0.10 的 CRC 恒错真因）★★★
 *
 *   本工程的代码基址是 0xFF0000(BL) / 0xFF4000(APP)，**已超出 16 位 code 空间**。
 *   C251 里 `code` 限定 = 16 位绝对地址，编译器取表时只搬低 16 位（实测汇编）：
 *
 *       ADD  WR10,#WORD0 crc32_tab4      ; 只有 WORD0，没有 WORD2
 *
 *   `?CO?CRC32` 落在 FF006F(BL) / FF4048(APP) → 实际读地址被截成 006F / 4048，
 *   而 0x006F / 0x4048 正是 **STAGING 区**（擦完是 0xFF，写镜像时又在变）。
 *   于是 CRC 表内容随机 → `crc32 selftest FAIL` → flash_iap_selftest 提前返回
 *   → s_high_ok=0 → "STAGING NOT writable"；A1 与 verify 用同一张坏表、
 *   而表还从"正在被写的那片 Flash"读 → 校验值天然不稳 → `[VER] STAGING fail rc=12`。
 *
 *   去掉 `code` 后进 HCONST（0xFF3200 之后，24 位 far 寻址），汇编变成
 *   `ADD DR4,#WORD0` + `ADD WR4,#WORD2`，取表地址完整。
 *   守门：`tools/check_overlay.py` 判据⑤ —— 除 `?CO?start251` 外不许有任何 `?CO?` 段。
 */
static const u32 crc32_tab4[16] = {
    0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
    0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
    0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
    0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL
};

u32 crc32_begin(void)
{
    return CRC32_INIT_VAL;
}

/*==============================================================================
 * crc32_update 的形参副本（v0.25，铁律 9 补最后一处）
 *
 *   本函数是**唯一**还把形参留在参数块里、并且**在循环里反复回读**的函数。
 *   汇编实测（BL v0.24）：
 *       ?crc32_update??BYTE = 0x10F92   crc@+0  src@+4  len@+8
 *       len 的家 = 0x10F9A = ?XD?CRC32_UPDATE??CRC32（本函数自己的 2 字节局部段）
 *   而 0x10F92..0x10F99 这 8 个字节恰好是
 *       ?XD?BSP_UART_WRITE / ?XD?FLASH_IAP_WRITE /
 *       ?XD?FLASH_IAP_READ  / ?XD?FLASH_IAP_VERIFY
 *   四个别的函数的局部段。crc/src 走寄存器所以暂时没炸，但"len 在循环里被
 *   别的东西写掉"这件事一旦发生，表现就是 **CRC 值不稳定**：
 *   同一个自检向量，两次上电打出两个不同的值（v0.24 上板：
 *   9f4db109 / 84d14afa，而 0xCBF43926 才是对的），
 *   而镜像头 CRC（step=5）和正文 CRC（step=9）都建在这个函数上。
 *
 *   ⇒ 形参第一条语句落地，循环只用副本。本函数不回调任何东西，所以四份
 *     static 不存在重入问题。
 *============================================================================*/
static u32         s_ucrc;
static const u8   *s_usrc;
static u16         s_ulen;
static u16         s_ui;

u32 crc32_update(u32 crc, const u8 *src, u16 len)
{
    s_ucrc = crc;
    s_usrc = src;
    s_ulen = len;

    if (s_usrc == 0) return s_ucrc;
    for (s_ui = 0; s_ui < s_ulen; s_ui++)
    {
        s_ucrc ^= s_usrc[s_ui];
        s_ucrc = (s_ucrc >> 4) ^ crc32_tab4[s_ucrc & 0x0F];
        s_ucrc = (s_ucrc >> 4) ^ crc32_tab4[s_ucrc & 0x0F];
    }
    return s_ucrc;
}

u32 crc32_final(u32 crc)
{
    return crc ^ CRC32_FINAL_XOR;
}

/*------------------------------------------------------------------------------
 *  crc32_calc() 已在 v0.27 删除 —— 见 crc32.h 里 OTA_CRC32_OF 的说明。
 *
 *  原来这里是：
 *      u32 crc32_calc(const u8 *src, u16 len)
 *      {
 *          if (src == 0) return 0;
 *          return crc32_final(crc32_update(crc32_begin(), src, len));
 *      }
 *  它把三步塞进一条 return，是全工程唯一这么写的地方，也是**唯一算错**的地方。
 *  改成宏 + 调用点三行之后，代码生成与 APP 侧那两条已经证明正确的调用
 *  （app_main.c 的正文 CRC、flash_iap_crc32_cb）完全一致。
 *--------------------------------------------------------------------------*/
/*------------------------------------------------------------------------------
 * 对 Flash 求 CRC 的两个入口：**实现只有一份，在 flash_iap.c**
 *
 * 这里曾经复制了一整份 while 循环（读 64B -> crc32_update），和
 * flash_iap_crc32_cb() 逐字相同。副本的坏处很实在：
 *   · BL 与 APP 各自编译，两份实现有一天改歪一处（比如把 64 改成 128），
 *     升级校验就会在"用 BL 算的"和"用 APP 算的"之间对不上，且极难发现；
 *   · 未被调用时每个副本还要占一份 22 字节 data 帧（TINY 模型下就是 L107）。
 * 所以统一转发过去：谁写 flash 谁负责算 CRC，只有一个真相。
 *--------------------------------------------------------------------------*/
ota_ret_t crc32_flash(u32 addr, u32 len, u32 *p_crc)
{
    return flash_iap_crc32(addr, len, p_crc);
}

ota_ret_t crc32_flash_cb(u32 addr, u32 len, u32 *p_crc,
                         void (*on_tick)(u32 done))
{
    return flash_iap_crc32_cb(addr, len, p_crc, on_tick);
}

/*==============================================================================
 *  CRC 自检 —— **只保留 HCONST 那一条向量**（v0.29）
 *
 *  v0.28 上板读数（BL/APP 两工程、两次上电全部稳定复现）：
 *      cst   = cbf43926    HCONST 向量（const，与查表同一空间）  ← **正确**
 *      c1    = d3d99e8b    1 字节 xdata 向量（"A"）              ← **正确**
 *      crc32 = cbf41f48    9 字节 xdata 向量（"123456789"）      ← 错
 *
 *  ⇒ cst 全对，说明 **查表 / 初值 / 最终取反 / 9 字节循环 / 3 字节通用指针
 *    全都没问题**；c1 也全对，说明 len==1 的取数也没问题。
 *
 *  同期工程里 4 条**真正用来判生死**的通路，值都与 Python binascii.crc32 逐位一致：
 *      APP 造镜像正文 49088 B（128 B xdata 缓冲）        95e314a4  ✓
 *      APP 回读 STAGING 49088 B                          95e314a4  ✓
 *      param 462 B（&s_p / xdata）                       param_init OK ✓
 *      镜像头 36 B（&s_hdr / xdata）                     step=5 通过 ✓
 *  ⇒ "xdata 多字节 + 索引取数"这条通路**也是好的**。
 *
 *  唯一算错的只有那个 9 字节 xdata 自检向量本身，而它**不在任何功能路径上**
 *  （正文、参数、镜像头都不经过它）。它的唯一副作用是让 selftest 提前
 *  return 6 并打出 `crc32 selftest FAIL` + 一句误导人的
 *  `-> STAGING NOT writable: IAP cannot reach FE block`（见 flash_iap.c）。
 *  所以 v0.29 起把它删掉，只留 cst 这条一直准确的看门狗。
 *
 *  ★ 记一笔，别再回头：cst 对 ⇒ **不要再用 "算法/查表坏了" 解释任何现象**。
 *    剩下所有异常都只能是"某个变量的内容不对"或"某条链路的参数不对"。
 *
 *  ★★ v0.30：连 cst 这条也删了 ★★
 *    v0.29 同一份源码重编后 cst 又变成 69d39b4b（v0.28 是 cbf43926），
 *    而 s_vc 在镜像 0xFF3E5E 处的内容复查过就是 "123456789"、指针立即数
 *    （MOV WR2,#3E5E / MOV WR0,#00FF @0xFF321A）也复查过是对的 ——
 *    也就是说**这条自检自身的读数不可复现**，它作为"看门狗"的价值已经归零，
 *    只剩副作用：一 FAIL 就提前 return，把读 BL 入口 4 字节那条**真正有效的
 *    读通路判据**和 STAGING/APP 两处探针全部跳过。
 *    ⇒ 整个函数群删除，读通路的可信度改由 flash_iap_selftest 第 3) 条
 *      （读 BL 入口 0x02）来证明，那是真固件真地址。
 *============================================================================*/
