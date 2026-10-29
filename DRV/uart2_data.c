/*==============================================================================
 *  uart2_data.c  --  串口2 数据口：32 位定长帧的接收与解析
 *  Project : Project/APP/STC32G_OTA.uvproj    CODE = 0xFF4000
 *  Platform: STC32G12K128 (Keil C251, Huge)
 *
 *  帧格式、波特率约束、为什么只能轮询 —— 全部写在 uart2_data.h 顶部的注释里。
 *  本文件只负责"把线变成 10 个 u32"，不解释数据含义、不碰网络。
 *
 *  ── 纪律（本工程踩过的坑，逐条对应）──────────────────────────────────
 *   ① 热路径只用 SFR 直读（DTA_RI/DTA_BUF），不进任何函数调用。
 *      v0.6.13 的定案：一字节 87 us，中间多几个函数调用就是静默丢字节。
 *   ② 所有缓冲都是 `static xdata`，且在 data_init() 里**显式清零**
 *      （铁律 13：static xdata 的初值与隐式零初始化都不生效，上电是 0xFF）。
 *   ③ CRC 覆盖帧头。只覆盖数据的话，"AA 55"错位一次就会把后面的字节
 *      当成合法数据写进模板 —— 那种错误在现场几乎不可能发现。
 *============================================================================*/
#include "ota_mcu.h"

#include "log.h"
#include "uart_at.h"          /* 只为 AT_UART_PORT：守卫"两个口不能同一个串口" */
#include "uart2_data.h"

#define TAG     "DTA"

#if (AT_UART_PORT == 2)
  #error "AT 通道被配到 UART2（P1.0/P1.1）：数据口与 AT 口会抢同一个串口，改 AT_UART_PORT"
#endif

/* 硬件一律走 SFR 直读：不进函数、不过指针 */
#define DTA_RI      S2RI
#define DTA_BUF     S2BUF

/*------------------------------------------------------------------------------
 * 状态（全部 xdata + 显式初始化）
 *------------------------------------------------------------------------*/
static u8  xdata s_state;                       /* 0=等AA 1=等55 2=收剩余   */
static u8  xdata s_n;                           /* 当前已收字节数           */
static u8  xdata s_buf[DATA_FRAME_LEN];         /* 帧装配缓冲               */
static u32 xdata s_val[DATA_SLOT_N];            /* 10 个模板的当前值        */
static u16 xdata s_mask;                        /* bit0..bit9 = D1..D10 有效 */
static u16 xdata s_bytes;
static u16 xdata s_frames;
static u16 xdata s_bad_crc;
static u16 xdata s_bad_seq;
static u8  xdata s_init;                        /* 0xA5 = 已初始化          */

/*==============================================================================
 * CRC-16/CCITT-FALSE：init 0xFFFF，poly 0x1021，不反转、不异或输出
 *   逐位算，7 字节 56 次循环 —— 一帧几微秒，不值得为它开 512 B 的表。
 *   只算 s_buf[0..6]（帧头 + 序号 + 4 字节数据）。
 *============================================================================*/
static u16 data_crc(void)
{
    static u16 xdata c;
    static u8  xdata i;
    static u8  xdata b;

    c = 0xFFFFU;
    for (i = 0U; i < 7U; i++)
    {
        c = (u16)(c ^ (u16)((u16)s_buf[i] << 8));
        for (b = 0U; b < 8U; b++)
        {
            if ((c & 0x8000U) != 0U) c = (u16)((u16)(c << 1) ^ 0x1021U);
            else                     c = (u16)(c << 1);
        }
    }
    return c;
}

/* 一整帧到手：校验 CRC → 校验序号 → 落槽位 */
static void data_frame(void)
{
    static u16 xdata crc;
    static u16 xdata want;
    static u8  xdata idx;

    crc  = data_crc();
    want = (u16)(((u16)s_buf[7] << 8) | (u16)s_buf[8]);

    if (crc != want) { s_bad_crc++; return; }

    idx = s_buf[2];
    if ((idx < 1U) || (idx > DATA_SLOT_N)) { s_bad_seq++; return; }

    s_val[(u8)(idx - 1U)] =
        ((u32)s_buf[3] << 24) | ((u32)s_buf[4] << 16) |
        ((u32)s_buf[5] << 8)  | ((u32)s_buf[6]);

    s_mask = (u16)(s_mask | (u16)(1U << (u8)(idx - 1U)));
    s_frames++;
}

/*==============================================================================
 * 初始化
 *============================================================================*/
void data_init(void)
{
    static u8 xdata i;

    /* P1.1/TXD2 推挽，P1.0/RXD2 准双向（与 AT 通道同一套做法：锁存器必须先写 1，
     * 写 0 会把引脚强行拉低，读回来永远是 0）。S2_S=0 → 用 P1.0/P1.1。 */
    P1M1 &= (u8)~0x02;  P1M0 |= (u8)0x02;
    P1M1 &= (u8)~0x01;  P1M0 &= (u8)~0x01;
    P1   |= (u8)0x03;
    P_SW2 &= (u8)~0x01;

    /* 模式1（8 位可变）+ 允许接收。波特率源是 Timer2，**本函数不许碰 T2**：
     * 那是 at_uart_init() 的地盘，而且 AT 通道和本口共用同一个重装值。 */
    S2CON = 0x50U;
    DTA_RI = 0;
    (void)DTA_BUF;

    s_state   = 0U;
    s_n       = 0U;
    s_mask    = 0U;
    s_bytes   = 0U;
    s_frames  = 0U;
    s_bad_crc = 0U;
    s_bad_seq = 0U;
    for (i = 0U; i < DATA_SLOT_N; i++) s_val[i] = 0UL;

    s_init = 0xA5U;

    /* 打一行自报：端口、引脚、波特率来源。现场"数据不动"时先看这一行对不对。 */
    LOG_I1(TAG, "uart2 data P1.0/P1.1, baud=%lu (shared T2)", at_get_baud());
}

/*==============================================================================
 * 轮询：把在途字节读干净。绝不等待。
 *============================================================================*/
void data_poll(void)
{
    static u8 xdata c;

    if (s_init != 0xA5U) return;

    while (DTA_RI)
    {
        DTA_RI = 0;
        c = DTA_BUF;
        s_bytes++;

        if (s_state == 0U)
        {
            if (c == 0xAAU) { s_buf[0] = c; s_n = 1U; s_state = 1U; }
        }
        else if (s_state == 1U)
        {
            if (c == 0x55U)      { s_buf[1] = c; s_n = 2U; s_state = 2U; }
            else if (c == 0xAAU) { s_n = 1U; }             /* AA AA 55：重来 */
            else                 { s_state = 0U; s_n = 0U; }
        }
        else
        {
            if (s_n < DATA_FRAME_LEN) s_buf[s_n++] = c;
            if (s_n >= DATA_FRAME_LEN)
            {
                s_state = 0U;
                s_n     = 0U;
                data_frame();
            }
        }
    }
}

/*==============================================================================
 * 查询接口（零指针：序号进、值出）
 *============================================================================*/
u8 data_have(u8 idx)
{
    if ((idx < 1U) || (idx > DATA_SLOT_N)) return 0U;
    return (u8)((s_mask >> (u8)(idx - 1U)) & 1U);
}

u32 data_value(u8 idx)
{
    if ((idx < 1U) || (idx > DATA_SLOT_N)) return 0UL;
    return s_val[(u8)(idx - 1U)];
}

u16 data_mask(void)  { return s_mask;    }
u16 data_bytes(void) { return s_bytes;   }
u16 data_frames(void){ return s_frames;  }
u16 data_bad_crc(void){ return s_bad_crc; }
u16 data_bad_seq(void){ return s_bad_seq; }
