/*==============================================================================
 *  bsp_uart.c  --  UART1 驱动（8N1，Timer2 波特率，中断收 + 轮询发）
 *  波特率公式（Timer2，1T）：baud = FOSC / 4 / (65536 - RL)
 *      11.0592MHz / 115200 -> RL = 65536 - 24 = 0xFFE8
 *  TODO(核对手册) STC32G 的 UART1 SFR 是否为 SCON/SBUF；若为 S1CON/S1BUF，
 *                 本文件顶部一改即可（其余逻辑不变）。
 *============================================================================*/
#include "bsp_uart.h"
#include "bsp_clock.h"
#include "ringbuf.h"

static u8 xdata s_rx_mem[BSP_UART_RX_SIZE];   /* 1KB：必须 xdata，data 段只有 128B */
static ring_t  xdata s_rx;
static u8      xdata s_ready = 0;

/* ★ volatile 不能省：这个变量在"主循环 while(!s_tx_idle)"里轮询，
 *   由 ISR 改。不加 volatile 时优化等级 7 会把读操作提出循环外
 *   （循环体为空、无副作用），结果是"发完第一个字节就永远卡住"，
 *   症状恰好是串口只出现 1 个字符然后彻底安静。 */
static volatile u8 s_tx_idle = 1;             /* 1 = 发送空闲，可由主循环写 SBUF */

/*------------------------------------------------------------------------------
 * 引脚模式：这一步不能省
 *
 *   ★ STC 的准双向口（PxM1:PxM0 = 00）输出高电平靠的是**内部弱上拉**，
 *     驱动能力只有几百 µA 级。板上 P3.1 只要挂了点负载（指示灯、下拉电阻、
 *     保护电阻、电平转换），高电平就会被拽下去，接收端看到的是一条恒低的线
 *     —— 现象就是"**能下载，但用户程序一个字节都发不出来**"。
 *
 *     为什么 ISP 下载却完全正常？因为片内 ISP 监控程序**自己会把 P3.1 配成推挽**。
 *     所以"下载成功"只能证明线通，**不能证明用户代码的引脚模式是对的**。
 *     这正是本地最容易被漏掉的一环。
 *
 *   官方推荐：UART 的 TXD 用推挽输出，RXD 用准双向（输入态即可）。
 *----------------------------------------------------------------------------*/
#ifndef BSP_UART1_ON_P3
#define BSP_UART1_ON_P3   1      /* 1 = UART1 用默认引脚 P3.0/P3.1 */
#endif

u8 bsp_uart_init(u32 baud)
{
    u16 rl;

    if (baud == 0) return 0;
    if (!ring_init(&s_rx, s_rx_mem, BSP_UART_RX_SIZE)) return 0;

#if (BSP_UART1_ON_P3 != 0)
    /* 换过 P_SW1 的话，这两行必须同步换成新引脚，否则 TXD 还是弱上拉 */
    P3M1 &= (u8)~0x02;  P3M0 |= (u8)0x02;      /* P3.1 / TXD1 -> 推挽  */
    P3M1 &= (u8)~0x01;  P3M0 &= (u8)~0x01;     /* P3.0 / RXD1 -> 准双向 */
    P3   |= (u8)0x02;                          /* TXD 空闲应保持高电平  */
#endif

    /* 波特率重装值：RL = 65536 - FOSC/(4*baud)
     * ★ 公式对 Timer1 / Timer2 完全相同（STC 官方库 STC32G_UART.c 里两条分支
     *   用的是同一个 j），所以换波特率源不会改变实际波特率。 */
    rl = (u16)(65536UL - (BSP_FOSC_HZ / 4UL / baud));

    SCON  = 0x50;          /* 模式1：8 位可变波特率，REN=1 允许接收 */

    /*--------------------------------------------------------------------------
     * ★★ 日志串口的波特率源必须是 Timer1，不能是 Timer2（v0.47 改）
     *
     *   STC32G 的 UART2（模组口）**固定**用 Timer2 做波特率发生器，没有第二个
     *   选项。而 UART1 可以二选一：S1BRT=0 → Timer1，S1BRT=1 → Timer2。
     *
     *   原来这里写的是 Timer2（AUXR|=0x01）。一开始没问题，因为模组也是
     *   115200，两个口共用一个重装值恰好一致。但**自适应波特率**一旦把模组
     *   定到 9600/230400，Timer2 的重装值就跟着变 —— 日志会一起变成那个波特率，
     *   现场看到的就是"检测成功后整段乱码"，比不检测还糟。
     *
     *   改成 Timer1 后：日志永远 115200（Timer1），模组随便什么档（Timer2），
     *   两条链路彻底解耦。Timer1 在本工程里没有别的用途（Timer0 是 1ms 时基）。
     *   官方库两分支用的是同一个重装值公式，切换不改变实际波特率。
     *------------------------------------------------------------------------*/
    TR1   = 0;             /* 先停 Timer1 再改重装值                */
    S1BRT = 0;             /* S1 波特率源 = Timer1                  */
    T1_CT = 0;             /* Timer1 作定时器（不是计数器）          */
    TMOD &= (u8)~0x30;     /* Timer1：16 位自动重装（模式 0）        */
    T1x12 = 1;             /* Timer1 工作在 1T                       */
    TH1   = (u8)(rl >> 8);
    TL1   = (u8)rl;
    TR1   = 1;             /* 启动 Timer1                            */

    ES    = 1;             /* 允许 UART1 中断（EA 由 main 打开） */
    s_tx_idle = 1;
    s_ready = 1;
    return 1;
}

/*------------------------------------------------------------------------------
 * 发送：必须区分"中断在跑"与"中断没跑"两种情形
 *
 *   老写法 `SBUF=c; while(!TI);TI=0;` 在 EA=1 时是**必卡死**的：
 *   字节一发送完 TI 立刻置位，中断比主循环先抢到，ISR 里 TI=0 清掉，
 *   主循环醒来时看到的永远是 TI==0 —— 死等。BL 一开机就连打十几行日志，
 *   这一步不修，串口会停在第 1 个字符上。
 *
 *   修法：中断模式下 TI 完全交给 ISR 管，主循环只读写 s_tx_idle 这个标志；
 *         无中断模式下（EA=0）才允许主循环直接轮询 TI。
 *
 *   ★ 两条都不许"死等到底"：EA=1 但中断没真正进来（比如向量没挂对、ES 被误清）时，
 *     s_tx_idle 永远回不到 1，日志第一句就把整个 BL 挂死 —— 现场表现是"串口一片安静"。
 *     所以两个等待都带一个自旋上限，等不到就往下走（宁可丢字节，也不能锁死）。
 *----------------------------------------------------------------------------*/
#define UART_TX_SPIN_MAX   30000U

void bsp_uart_putc(u8 c)
{
    /* ★ static 而非局部：局部 xdata 进 _XDATA_GROUP_ overlay 池，
     *   会与 log_printf 的 varargs 参数区抢地址（见 COMMON/log.c 文件头实测）。
     *   本函数不可重入（发送权由 s_tx_idle 串行化），改 static 安全。 */
    static u16 xdata s_guard;

    if (!s_ready) return;

    if (EA)
    {
        /* 等上一字节退出移位寄存器 */
        s_guard = UART_TX_SPIN_MAX;
        while (!s_tx_idle && --s_guard) { }

        /*==================================================================
         * ★★ 临界区只做 3 件事，几个机器周期（2026-10-25 实测丢字符的根源）
         *
         *   v0.5 及以前这里是：
         *       while (!s_tx_idle) {}        <- 等上一字节
         *       s_tx_idle = 0;
         *       SBUF = c;                    <- 写完就返回，不等本字节
         *
         *   两个缺口：
         *
         *   (1) **没清陈旧 TI**。上一字节发完会把 TI 置位；如果 ISR 还排在
         *       时基中断后面没跑，此时 TI=1、s_tx_idle=0。本函数把 s_tx_idle
         *       清 0 之后 ISR 才进来，它看到 TI 就把 s_tx_idle 又置回 1 ——
         *       可我们的字节**正在移位**，标志却说"空闲"。下一次调用立刻
         *       覆盖 SBUF，当前字节被拦腰截断。
         *       现场表现：日志里随机丢单个字符。
         *           实测样本 `STAGING` -> `STAG NG`、`IAP` -> `AP`、
         *                    `selftest` -> `selftes`、`NOT yet` -> `NO  yet`、
         *                    `p3=3b` -> `p3=3` / `p3=13` / `p3=bb`。
         *       注意它**不是格式化问题**：被丢的字符根本没进 SBUF。
         *
         *   (2) **不等本字节发完就返回**。等于把覆盖窗口留给下一次调用，
         *       效果同上。
         *
         *   修法：清 TI -> 置忙 -> 写 SBUF -> 等 s_tx_idle 回来。
         *   即"一个字节一个字节同步发"，与原来总耗时完全相同（串口速率
         *   是瓶颈，本来也快不了），但发送权判定变成无竞态的。
         *   ES 只关 3 条指令的时间，不影响接收。
         *================================================================*/
        ES = 0;
        if (TI) TI = 0;                        /* (1) 清掉"已完成但中断未跑"的陈旧标志 */
        s_tx_idle = 0;                         /* 置忙：此后 TI 归 ISR 处理 */
        ES = 1;

        SBUF = c;

        /* (2) 同步等本字节真正移出去。
         *     UART_TX_SPIN_MAX 约 13 ms 的上限，57600 下 1 字节约 174 us，
         *     即使被时基中断挤后也不会误判超时。 */
        s_guard = UART_TX_SPIN_MAX;
        while (!s_tx_idle && --s_guard) { }
    }
    else
    {
        /* EA=0：没有中断，TI 完全归本函数
         * ★ 必须先清 TI 再写 SBUF。反过来的话，若上一字节的 TI 还站着，
         *   while(!TI) 立刻通过，本字节刚进移位寄存器就被判成"发完了"，
         *   下一次调用直接覆盖 —— 同样是丢字符。 */
        if (TI) TI = 0;
        SBUF = c;
        s_guard = UART_TX_SPIN_MAX;
        while (!TI && --s_guard) { }
        TI = 0;
    }
}

void bsp_uart_write(const u8 *p, u16 n)
{
    static u16 xdata s_i;
    if (p == 0) return;
    for (s_i = 0; s_i < n; s_i++) bsp_uart_putc(p[s_i]);
}

void bsp_uart_puts(const char *s)
{
    if (s == 0) return;
    while (*s) bsp_uart_putc((u8)*s++);
}

void bsp_uart_tx_wait_idle(void)
{
    static u16 xdata s_guard;

    if (EA)
    {
        s_guard = UART_TX_SPIN_MAX;
        while (!s_tx_idle && --s_guard) { }
    }
    else
    {
        /* EA=0 时 putc 是同步的（写完就等 TI），走到这里必然已经空了。
         * 只把可能残留的 TI 清掉，**绝不能再 while(!TI)** —— 没有字节在发时
         * TI 永远是 0，那样会白等满整个自旋上限（约 13 ms），
         * 而本函数正好在跳转前的最后一步被调用。 */
        if (TI) TI = 0;
    }
}

/*------------------------------------------------------------------------------
 * 中断服务：与 bsp_timer.c 同一套规则
 *   APP 工程（OTA_BUILD_APP 已定义）不声明 interrupt —— 向量在 BL 那边，
 *   APP 这份只有被 ota_isr_dispatch(4) 调到才有意义。
 *----------------------------------------------------------------------------*/
#if defined(OTA_BUILD_APP)
void bsp_uart_isr(void)
#else
void bsp_uart_isr(void) interrupt 4
#endif
{
    if (RI)
    {
        RI = 0;
        ring_push(&s_rx, SBUF);        /* 满则计入 ovf，不阻塞中断 */
    }
    if (TI)
    {
        TI = 0;
        s_tx_idle = 1;                 /* 交还发送权 */
    }
}

u8 bsp_uart_rx_pop(u8 *p)
{
    return ring_pop(&s_rx, p);
}

u16 bsp_uart_rx_avail(void)
{
    return ring_avail(&s_rx);
}

u16 bsp_uart_rx_flush(void)
{
    return ring_flush(&s_rx);
}

u16 bsp_uart_rx_overflow(void)
{
    u16 v = s_rx.ovf;
    s_rx.ovf = 0;
    return v;
}
