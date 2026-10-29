/*==============================================================================
 *  uart_at.c  --  AT 命令引擎（轮询版，APP 全程不开总中断）
 *  Platform : STC32G12K128 (Keil C251, CodeRomSize = Huge, Memory Model = LARGE)
 *  Depends  : bsp_timer.h, log.h, ota_types.h, uart_at.h
 *
 *  ── 与中断版的区别（★ 本轮关键决策）──────────────────────────────────────
 *   APP 侧不接管中断向量（向量在 BL 的 FF:0003/000B/0023h，APP 收不到中断），
 *   所以本引擎**不依赖 UART 中断**：发送轮询 S2TI，接收轮询 S2RI。
 *   代价是收发期间 CPU 被占用（一字节约 87 us @115200），但对 OTA 这种
 *   "下载时本来就全速干这一件事"的场景完全够用，且**彻底绕开中断所有权问题**。
 *
 *  ── 物理层：UART2 ────────────────────────────────────────────────────────
 *   P1.0 = RXD2，P1.1 = TXD2（STC32G 默认引脚，无需 P_SW2 切换）。
 *   波特率源 Timer2。★ 日志 UART1 已改由 Timer1 供时基（v0.47），
 *   所以本文件独占 Timer2，扫描档位时不会把日志波特率一起改掉。
 *   寄存器：S2CON(0x9A) / S2BUF(0x9B)。
 *
 *  ── 目标模组：Y100P（银尔达 Core-Y100P，Air780 平台 + 移远 AT 固件）────
 *   ★ 型号沿革：Air780EP -> Y100E(判死) -> Air724UG(已放弃) -> **Y100P（当前）**
 *   查银尔达官方文档确认的硬事实，直接决定本文件写法：
 *     ① 自适应波特率，范围 **1200~115200**。主控在这段里任意一档发 "AT"
 *        模组都能对上 ⇒ 扫 10 档扫不到，只可能是硬件/供电/没开机。
 *     ② **上电/重启时模组主动发开机日志**，必须先过滤再发命令
 *        ⇒ 自适应期间收到的字节一律不解释（现场纪律），
 *          判据只认"整行恰好 OK"或"整行恰好 RDY"。
 *     ③ AT 固件是**移远（Quectel）风格**：PDP 用 `AT+QICSGP`/`AT+QIACT`，
 *        HTTP 用 `AT+QHTTP*`，**不是** `AT+HTTPINIT` 那一套。
 *        ⇒ http_ota.c 的 HTTP 命令组**必须改**，别再按合宙标准写。
 *     ④ TTL 串口兼容 3.3V/5V 电平（不像 Air724UG 的 1.8V 域）
 *        ⇒ MCU 直连即可，不需要电平转换。
 *     ⑤ 供电 5~12V（推荐 12V/1A，功率 >10W），不是 3.8V
 *        ⇒ 供的是核心板 VIN，不是裸模组 VBAT。
 *   另：本文件的行结束符是 **CRLF**（见 at_send_cmd 上方说明）。
 *
 *  ── 铁律遵从（照抄项目约定，不许违反）────────────────────────────────────
 *   1) 所有缓冲区 static；不许非 static 局部 xdata 数组（进 overlay 池被踩）。
 *   2) 形参一律"第一条语句拷进模块级 static"，此后不再读形参。
 *   3) 常量表裸 const，不加 code 限定。
 *   4) 日志用固定槽宏 LOG_{E,W,I,D,T}0..4，一行 ≤ 2 值优先。
 *============================================================================*/
#include "uart_at.h"
#include "bsp_clock.h"
#include "bsp_timer.h"
#include "bsp_wdt.h"            /* ★ 忙等必须喂狗，见 at_wdt_tick() 说明 */
#include "log.h"

/*==============================================================================
 * ★★ v0.5.0：通道寄存器别名 —— 换口只改 uart_at.h 的 AT_UART_PORT
 *
 *   为什么用别名而不是到处写 #if：
 *     收发逻辑本身（自旋上限、喂狗点、顺手捞接收字节）是**唯一被验证过**
 *     的部分，一个字符都不该动。把 S2CON/S2BUF/S2TI/S2RI 换成别名之后，
 *     下面 at_uart2_putc()/at_uart2_getc() 的代码保持逐字不变，
 *     "换口"退化成"换 4 个名字"，把引入新时序 bug 的可能性压到最低。
 *
 *   ★ 两个口的控制寄存器**位定义不同**，不能互换数值：
 *       S2CON.6 = S2SM1（=1 模式1）
 *       S3CON.6 = S3ST3（=1 改用 Timer3）  ← 照抄 0x50 就选错了波特率源
 *     因此 AT_CON 的取值也一并在下面按口给出（ATR_CON_INIT），
 *     不允许在别处再写死 0x50。
 *============================================================================*/
#if (AT_UART_PORT == 3)
  #define ATR_CON       S3CON
  #define ATR_BUF       S3BUF
  #define ATR_TI        S3TI
  #define ATR_RI        S3RI
  #define ATR_CON_INIT  0x10U   /* S3SM0=0 八位可变 + S3ST3=0 用T2 + REN=1  */
  #if (AT_UART3_PIN_P5 != 0)
    #define ATR_RXD()   ((u8)(P5 & 0x01U))
  #else
    #define ATR_RXD()   ((u8)(P0 & 0x01U))
  #endif
#else
  #define ATR_CON       S2CON
  #define ATR_BUF       S2BUF
  #define ATR_TI        S2TI
  #define ATR_RI        S2RI
  #define ATR_CON_INIT  0x50U   /* S2SM1=1 模式1 + S2REN=1                  */
  #define ATR_RXD()     ((u8)(P1 & 0x01U))
#endif

/* 端口整体寄存器（环回自检要直接驱动 TX 脚，必须有整字节访问） */
#if (AT_UART_PORT == 3)
  #if (AT_UART3_PIN_P5 != 0)
    #define ATR_PORT  P5
    #define ATR_PM0   P5M0
    #define ATR_PM1   P5M1
  #else
    #define ATR_PORT  P0
    #define ATR_PM0   P0M0
    #define ATR_PM1   P0M1
  #endif
#else
  #define ATR_PORT  P1
  #define ATR_PM0   P1M0
  #define ATR_PM1   P1M1
#endif

/*==============================================================================
 * 模块级状态（全部 static，物理上不可能被 overlay 复用地址）
 *============================================================================*/

/* 接收环形缓冲（4 KB，必须 2 的幂） */
static u8  xdata s_rxbuf[AT_RX_BUF_SIZE];
static u16 xdata s_head;          /* 写指针（本引擎轮询写入）           */
static u16 xdata s_tail;          /* 读指针（解析消费）                 */
static u16 xdata s_ovf;           /* 溢出丢弃计数                       */

/* 行解析缓冲（一行的临时存放，AT_LINE_MAX） */
static u8  xdata s_line[AT_LINE_MAX];
static u16 xdata s_line_len;

/* ★ v0.8.0 删除：s_locked / s_binary / URC 注册表（三张表合计 342 B xdata）
 *   它们只服务 at_poll / at_urc_register / at_binary_enter 这三条已经没有任何
 *   调用点的老通路（见本文件后面那两段删除说明）。通道独占是既成事实，
 *   二进制透传由 at_binary_arm/taken/disarm 直通，URC 由 at_pump 处理。 */

/* 中止标志 */
static u8 xdata s_abort;

/*==============================================================================
 * ★★ v0.6.0 宽松判据（丢字节容忍）
 *
 *   上板证据：桥接日志（USB-TTL 侧）里模组回的 "+CPIN: READY" / "OK" 完整无缺，
 *   而 MCU 自己的 rx 缓冲里同一批应答缺了约 40% 的字节，例如
 *       AT+CPIN? -> "AT+PN  +CP:EEA   O"      (OK 只剩一个 'O')
 *   形态是"字符都对、只是少几个" ⇒ 整字节丢，不是波特率错（波特率错是整片 '.'）。
 *   后果：line_is_ok() 要求恰好 2 字符 "OK"，丢一个就永远不成立，
 *         于是一条**实际成功**的命令被 rc=4 判死，整条 PDP/HTTP 链路卡在第一步。
 *
 *   判据设计（只能救真成功，不能放过真失败）：
 *     超时时看本次等待期间
 *       ① 出现过 'K'（来自 OK）            —— 丢字节不会凭空造出 K
 *       ② 从未出现过 "ER?R" 的形状（ERROR）—— 丢字节也只会让 ERROR 变形，
 *          不会把它变出 K 来；但真回了 ERROR 时必须判失败，所以单独记一笔
 *     两条都成立 ⇒ AT_RESP_PART
 *   ★ 默认关闭，只有 PDP / 下载这种"已知线路在丢字节"的场合才打开
 *     （见 http_ota.c hota_pdp_up() 与 app_main.c 的探针）。
 *============================================================================*/
static u8 xdata s_lenient;
static u8 xdata s_saw_ok;     /* 本次等待期间出现过相邻的 "OK"（字符串级，不等 '\n'） */
static u8 xdata s_saw_err;    /* 本次等待期间出现过 "ERR"                        */
static u8 xdata s_last_pr;    /* 本次等待期间最后一个**可打印**字节              */
static u8 xdata s_prev;       /* 上一字节（子串匹配用）                          */
static u8 xdata s_prev2;      /* 上上字节                                        */

void at_set_lenient(u8 on)
{
    s_lenient = (on != 0U) ? 1U : 0U;
}

/* 超时那一刻的判决（无形参 ⇒ 不生成参数块） */
static u8 at_lenient_verdict(void)
{
    if (s_lenient == 0U) return (u8)AT_RESP_TIMEOUT;

    /* ① 真回了 ERROR 就绝不放过。丢字节只会让 ERROR 变形，不会把它变出 O/K；
     *   反过来若把失败放过，后面会在一个没建立的承载上做下载，错得更晚更贵。 */
    if (s_saw_err != 0U) return (u8)AT_RESP_TIMEOUT;

    /* ② 见到完整的相邻 "OK"（不一定带 CR/LF） */
    if (s_saw_ok != 0U) return (u8)AT_RESP_PART;

    /* ③ ★★ v0.6.2 尾巴判据 —— 本轮上板数据直接推出来的
     *
     *   step56 实测（UART3，n 是 MCU 真正收到的字节数）：
     *     [AT+HTTPTERM] n=11  rx="ATHTTR   OK"   ← 19 B 的应答活下来 11 B，尾巴 OK 在
     *     [AT+CPIN?]    n=16  rx="ATCI?  PN ED   O"   ← 32 B 只活 16 B，K 没了
     *     [AT+SAPBR=2,1] n=25 rx="ATSPR21  AB:11"07.312  O "
     *   ★ 关键：所有 rx 里**一个 '.' 都没有** ⇒ 收到的每个字节都是完整合法的 ASCII
     *     ⇒ 帧没错、位没歪，就是**字节没来**（整字节丢）。
     *   ★ 第二个关键：活下来的字节随应答长度指数下降（10 B 的 "AT" 常成功、
     *     52 B 的 SAPBR 查询必失败）⇒ 减少线上字节数 = 直接提高成功率。
     *
     *   模组每条应答都以 "OK\r\n" 收尾，而 ERROR 以 'R' 收尾、"+CME ERROR: 3"
     *   以 '3' 收尾 ⇒ **最后一个可打印字节是 O 或 K，只可能是 OK 的残骸**。
     *   这是"用一个字符换一条命令的生死"，比要求两个字节都活着可靠一倍。
     */
    if (s_last_pr == (u8)'O' || s_last_pr == (u8)'K') return (u8)AT_RESP_PART;

    return (u8)AT_RESP_TIMEOUT;
}

/* 初始化标志 */
static u8 xdata s_ready;

/* 前置声明：putc 里要顺手收字节，而 ring_push 定义在后面 */
static void ring_push(u8 dat);

/*==============================================================================
 * ★★ 不依赖中断的 1ms 时基（本轮关键修正，不修则 AT 必卡死）
 *
 *   现象：P0 阶段 APP 全程 EA=0（见 app_main.c 第 487 行）。
 *         bsp_timer_ms() 返回的 s_ms 由 Timer0 中断自增，EA=0 时**永远不增**，
 *         于是 bsp_timer_elapsed() 恒返回 0 ⇒ 所有 AT 超时永不成立 ⇒ 死循环。
 *         （此前 uart_at.c 里写 bsp_timer_delay_ms(1) 就是直接踩这个坑。）
 *
 *   解法：Timer0 已在 bsp_timer_init_1ms() 里 TR0=1 启动（16 位自动重装 1ms）。
 *         中断不响应**不影响硬件**：TF0 照样每 1ms 置位、计数器照样重装。
 *         手动查 TF0 并清零 = 精准的、零中断依赖的毫秒时基，且与 bsp_timer
 *         那份 s_ms 互不干扰（各查各的，谁也不清谁）。
 *
 *   ★ 必须"每圈都查"：TF0 只是标志位，漏查期间溢出多次也只记 1 次。
 *     本文件收发循环一圈几 us（115200 每字节 87us），余量 20 倍以上。
 *   ★ 不许改回 bsp_timer_delay_ms / bsp_timer_elapsed（EA=0 下必卡死）。
 *============================================================================*/
static u32 xdata s_at_ms;

/* 当前生效的 UART2 波特率（0 = 还没确定）。
 * ★ 自适应扫描成功后才会被赋值；确认之前收到的字节一律丢弃，不参与解析。 */
static u32 xdata s_cur_baud;

/*==============================================================================
 * ★★★ v0.7.0：非阻塞 URC 通道（MQTT 订阅消息靠它，见 at_pump 的说明）
 *
 *   s_ul/s_ul_n   : 拼行缓冲（**只有 at_pump 用**，绝不与 s_line 共用，
 *                   否则阻塞命令一进来就把半行清了）
 *   s_urc_pend    : 1 = g_urc 里有一条还没被业务层取走的 URC
 *   s_at_busy     : 1 = at_wait_core 正在跑 ⇒ at_pump 必须让路，否则两边
 *                   同时读 ATR_BUF 会互相偷字节
 *============================================================================*/
static u8  xdata s_ul[AT_URC_MAX];
static u8  xdata s_ul_n;
static u8  xdata s_urc_pend;
static u8  xdata s_at_busy;

char xdata g_urc[AT_URC_MAX];
u16  xdata g_urcn;

/* ★ v0.7.5：URC 通路自证计数器（声明与判读表见 uart_at.h） */
u16 xdata g_pump_b;
u8  xdata g_pump_l;
u8  xdata g_pump_m;
u8  xdata g_urc_take_n;

/*==============================================================================
 * ★★ 线路诊断计数器（v0.48 新增）
 *
 *   为什么必须加：v0.47 上板的结果是"10 个档位全是 0 B"，而 0 B 这件事
 *   至少有三个完全不同的根因，光看串口分不开：
 *     ① 固件：UART2 时钟死（Timer2 没跑 / 重装值错）⇒ 连"发"都发不出去；
 *     ② 固件：接收通道死（S2REN/引脚模式错）⇒ 发得出去但一个字收不回；
 *     ③ 硬件：模组没上电 / 没启动 / TXD 没接过来 ⇒ 线上根本没有信号。
 *   三个计数器正好一一对应：
 *     s_tx_ok/s_tx_to → ①（S2TI 到底等没等到 = UART2 时钟活没活）
 *     s_edges/s_low_n → ③（引脚有没有跳变，与波特率无关）
 *     两者都正常而 UART 仍收不到 → ②
 *============================================================================*/
static u16 xdata s_tx_ok;      /* 发送成功字节数（S2TI 等到了）     */
static u16 xdata s_tx_to;      /* 发送超限次数（S2TI 始终没等来）    */
static u16 xdata s_edges;      /* 采样窗口内 P1.0 的跳变次数         */
static u16 xdata s_low_n;      /* 采样窗口内 P1.0 读到 0 的次数      */

static void at_t0_poll(void)
{
    if (TF0)
    {
        TF0 = 0;            /* 软件清溢出标志；中断没开，没人替我们清 */
        s_at_ms++;
    }
}

/*==============================================================================
 * ★★ 忙等里的喂狗（本轮第二个必改点，不修则 APP 每 ~1.1 s 复位一次）
 *
 *   病史：v0.45 上板，日志停在 `RXD2 idle level=1`，之后一个字都没有，
 *         紧接着又从 BL 重启，boot_try 1→2→3→4→5 一路涨。
 *   根因：原引擎的超时靠 bsp_timer_delay_ms(1)，而那个函数**每毫秒喂一次狗**
 *         （见 bsp_timer.c 第 60 行）。改成 TF0 轮询时基后不再调它，
 *         AT 等待期间（500 ms × 20 次）**全程没有一次 CLR_WDT**
 *         ⇒ WDT_PS_32 约 1.1 s 溢出 ⇒ 硬件复位。
 *         APP 启动本身（param_save 双槽 ~480 ms + 自检 + 日志）已吃掉大半配额，
 *         所以连第一条 AT 都等不完。
 *
 *   铁律化：**任何可能超过 100 ms 的忙等，循环体里必须有喂狗点。**
 *         这条同样适用于后续的 HTTP 下载等待与 flash 擦写。
 *============================================================================*/
static void at_wdt_tick(void)
{
    bsp_wdt_feed();
}

/*==============================================================================
 * 物理层：UART2 轮询收发
 *============================================================================*/

/*==============================================================================
 * 引脚模式（v0.5.0：按 AT_UART_PORT 选口）
 *
 *   UART2：P1.1/TXD2 推挽，P1.0/RXD2 准双向      —— S2_S=0（默认）
 *   UART3：P0.1/TXD3 推挽，P0.0/RXD3 准双向      —— S3_S=0（默认）
 *        或 P5.1/TXD3 推挽，P5.0/RXD3 准双向      —— S3_S=1
 *
 *   ★ 准双向口的锁存器必须先写 1：锁存器是 0 时引脚被强行拉低，
 *     读回来永远是 0 —— 那会让 at_listen() 判成"线上恒低"，
 *     与真实情况完全相反。
 *   ★ P_SW2 里还有 EAXFR(bit7) 等位，只能用 |= / &= 改目标位，
 *     整字节赋值会把扩展寄存器访问一起关掉。
 *============================================================================*/
static void at_uart2_pin_init(void)
{
#if (AT_UART_PORT == 3)
  #if (AT_UART3_PIN_P5 != 0)
    P5M1 &= (u8)~0x02;  P5M0 |= (u8)0x02;      /* P5.1 TXD3 -> 推挽   */
    P5M1 &= (u8)~0x01;  P5M0 &= (u8)~0x01;     /* P5.0 RXD3 -> 准双向 */
    P5   |= (u8)0x03;
    P_SW2 |= (u8)0x02;                         /* S3_S = 1 → P5.0/P5.1 */
  #else
    P0M1 &= (u8)~0x02;  P0M0 |= (u8)0x02;      /* P0.1 TXD3 -> 推挽   */
    P0M1 &= (u8)~0x01;  P0M0 &= (u8)~0x01;     /* P0.0 RXD3 -> 准双向 */
    P0   |= (u8)0x03;
    P_SW2 &= (u8)~0x02;                        /* S3_S = 0 → P0.0/P0.1 */
  #endif
#else
    P1M1 &= (u8)~0x02;  P1M0 |= (u8)0x02;      /* P1.1 TXD2 -> 推挽   */
    P1M1 &= (u8)~0x01;  P1M0 &= (u8)~0x01;     /* P1.0 RXD2 -> 准双向 */
    P1   |= (u8)0x03;
    P_SW2 &= (u8)~0x01;                        /* S2_S = 0 → P1.0/P1.1 */
#endif
}

/* 读当前通道 RX 引脚的实时电平（UART 空闲态恒为高）。
 * 读回 0 ⇒ 模组没上电 / 模组 TXD 没接过来 / 接到了 GND —— 一句话排除八成接线问题。 */
u8 at_uart2_rxd_level(void)
{
    return ATR_RXD();
}

/* 读**指定**通道的 RX 引脚电平（交叉监听用，与当前 AT 通道无关）
 *   port 2 → P1.0（RXD2）
 *   port 3 → P0.0 或 P5.0（RXD3，按 AT_UART3_PIN_P5）
 *   port 4 → 恒为 P5.0（UART3 的备用脚，用于分辨"P0 没引出/被占"与"线上没信号"） */
static u8 at_rxd_pin_read(u8 port)
{
    if (port == 2U) return (u8)(P1 & 0x01U);
    if (port == 4U) return (u8)(P5 & 0x01U);
#if (AT_UART3_PIN_P5 != 0)
    return (u8)(P5 & 0x01U);
#else
    return (u8)(P0 & 0x01U);
#endif
}

/* UART2 波特率：Timer2 做波特率发生器，1T。j = 65536 - FOSC/(4*baud) */
static void at_uart2_baud(u32 baud)
{
    u16 j;

    j = (u16)(65536UL - (BSP_FOSC_HZ / 4UL / baud));

    T2R   = 0;                    /* 停 Timer2                            */
    T2_CT = 0;                    /* 定时器模式                           */
    T2x12 = 1;                    /* 1T                                   */
    T2H   = (u8)(j >> 8);
    T2L   = (u8)j;
    T2R   = 1;                    /* 启动                                 */
}

/* 发 1 字节（轮询发送完成标志，不等中断）
 *
 * ★ 发送期间必须顺手把收到的字节捞进环形缓冲：
 *   模组出厂 ATE1（回显开），我们还没发完它就开始回显；
 *   而 UART **只有一个接收寄存器**，晚取 87us 就被下一个字节覆盖 —— 丢字节。
 *   收进来的字节交给 at_wait_core() 的行解析，它不在乎先后。
 * ★ 退出条件带自旋上限：硬件/时钟真坏了也不能把整个 APP 卡死在 while 里。
 * ★ v0.5.0：寄存器一律用 ATR_* 别名（UART2/UART3 共用这段逻辑，逐字未改）。 */
static void at_uart2_putc(u8 c)
{
    static u16 xdata s_g;
    static u8  xdata s_done;

    ATR_TI = 0;
    ATR_BUF = c;
    s_g = 0x2000U;
    s_done = 0U;
    while (!ATR_TI)
    {
        if (ATR_RI)
        {
            ATR_RI = 0;
            ring_push(ATR_BUF);
        }
        if ((s_g & 0x00FFU) == 0U) at_wdt_tick();   /* 每 256 圈喂一次 */
        if (--s_g == 0U) break;                     /* 硬件异常不死等 */
    }
    if (ATR_TI) s_done = 1U;

    /* ★ 发送是否真的完成 = "UART 的时钟到底活没活"。
     *   TI 靠 Timer2 溢出把字节移出去；Timer2 不跑 ⇒ TI 永远不置位，
     *   于是本函数每次都靠 s_g 超限退出 —— **静默失败**，上层完全看不出来。
     *   这两个计数器就是把那次静默失败变成可读的证据。 */
    if (s_done != 0U)
    {
        if (s_tx_ok < 0xFFFFU) s_tx_ok++;
    }
    else
    {
        if (s_tx_to < 0xFFFFU) s_tx_to++;
    }

    ATR_TI = 0;
}

/* 收 1 字节（轮询接收标志），返回 1 收到 / 0 无数据 */
static u8 at_uart2_getc(u8 *p)
{
    if (!ATR_RI) return 0U;
    ATR_RI = 0;
    *p = ATR_BUF;
    return 1U;
}

/*==============================================================================
 * 环形缓冲（内嵌在 s_rxbuf 上，head/tail 用掩码取模）
 *============================================================================*/

static void ring_push(u8 dat)
{
    u16 next;
    next = (s_head + 1U) & AT_RING_MASK;
    if (next == s_tail)
    {
        s_ovf++;                   /* 满：丢新字节，不丢指针 */
        return;
    }
    s_rxbuf[s_head] = dat;
    s_head = next;
}

static u8 ring_pop(u8 *p)
{
    if (s_tail == s_head) return 0U;
    *p = s_rxbuf[s_tail];
    s_tail = (s_tail + 1U) & AT_RING_MASK;
    return 1U;
}

static u16 ring_avail(void)
{
    return (u16)((s_head - s_tail) & AT_RING_MASK);
}

static void ring_flush(void)
{
    s_tail = s_head;
    s_ovf  = 0U;
}

/*==============================================================================
 * 应答等待：核心是"轮询收字节 + 按行匹配"
 *============================================================================*/

/* 匹配行首前缀；返回 1 命中 */
static u8 line_has_prefix(const char *prefix)
{
    static u16 xdata i;
    if (prefix == 0) return 0U;
    for (i = 0; prefix[i] != 0; i++)
    {
        if (i >= s_line_len) return 0U;
        if (s_line[i] != (u8)prefix[i]) return 0U;
    }
    return 1U;
}

/* 行内容是否为 "OK" / "ERROR" */
static u8 line_is_ok(void)
{
    return (s_line_len == 2U && s_line[0] == 'O' && s_line[1] == 'K');
}

static u8 line_is_error(void)
{
    return (s_line_len == 5U && s_line[0] == 'E' && s_line[1] == 'R' &&
            s_line[2] == 'R' && s_line[3] == 'O' && s_line[4] == 'R');
}

/*==============================================================================
 * 初始化
 *============================================================================*/
ota_ret_t at_uart_init(u32 baud, u8 *rx_mem, u16 rx_mem_size)
{
    /* ★ 形参 9 B > 8 ⇒ 会生成参数块，第一条语句拷进 static 后不再读（铁律 9）。
     *   rx_mem/rx_mem_size 保留兼容：本实现用内嵌 s_rxbuf，不接管外部存储。 */
    static u32 xdata s_b;
    s_b = baud;

    /* baud == 0 表示"先不定档"：按合宙出厂值 115200 把 UART2 配起来，
     * 随后由 at_autobaud() 扫描确定真正的档位（现场要求）。 */
    if (s_b == 0UL) s_b = 115200UL;

    at_uart2_pin_init();
    at_uart2_baud(s_b);

    /* 通道模式：UART2 → 模式1 + 接收；UART3 → 八位可变 + Timer2 + 接收。
     * ★ 取值随口不同（见文件顶部 ATR_CON_INIT 说明），**不在此处写死数值**。 */
    ATR_CON = ATR_CON_INIT;

    /* ★ 铁律 13：BL 侧 static 隐式零初始化不生效（上电是 0xFF），
     *   凡是"靠 ==0 判首趟"的计数器必须在确定入口显式清。 */
    s_head     = 0U;
    s_tail     = 0U;
    s_ovf      = 0U;
    s_line_len = 0U;
    s_abort    = 0U;
    s_at_ms    = 0U;
    s_cur_baud = 0UL;          /* ★ 波特率尚未确认 ⇒ 数据一律不认 */
    /* ★ v0.7.0：URC 通道的四个状态位同样必须显式清（铁律 13） */
    s_ul_n     = 0U;
    s_urc_pend = 0U;
    s_at_busy  = 0U;
    g_urcn     = 0U;
    g_urc[0]   = 0;
    /* ★ v0.7.5：自证计数器同样不许依赖隐式零初始化 */
    g_pump_b     = 0U;
    g_pump_l     = 0U;
    g_pump_m     = 0U;
    g_urc_take_n = 0U;
    s_ready    = 1U;

    /* 清掉可能残留的接收标志/字节，避免第一帧应答错位 */
    ATR_RI = 0;
    ATR_TI = 0;
    (void)ATR_BUF;
    TF0  = 0;

    return OTA_OK;
}

/*==============================================================================
 * ★ v0.8.0 删除 at_send_raw()：全工程 0 调用点（命令一律走 g_at_tx + at_send_cur，
 *   HTTPREAD 数据体也不发裸字节）。保留它的唯一后果是占代码区。
 *============================================================================*/

/*==============================================================================
 * ★ 行结束符策略（v0.50 按现场实测修正：CR -> CRLF）
 *
 *   病史：v0.49 及之前 `at_send_cmd()` 只发 CR(0x0D)。现场用串口监听
 *         P1.1(TXD2) 抓到的是 `41 54 0D` —— **"AT" 后面没有换行**，
 *         串口助手里光标回到行首但不换行，很容易被当成"命令没发完"。
 *
 *   Y100P = 银尔达 Core-Y100P（Air780 平台 + 移远 AT 固件）。
 *   银尔达官方《移远AT固件HTTP测试实例》第 1 步原文是"发 AT\r"，
 *   但移远固件对 `AT\r\n` 和 `AT\r` **都接受** —— 发 CRLF 是超集。
 *   代价为零，收益是：① 串口监视器能正常换行，现场一眼确认命令已发出；
 *   ② 与 PC 端串口助手/厂商调试工具的默认行为一致，减少"我们和工具
 *   发的不是一回事"这类误判。
 *
 *   ★ 以前担心的"孤立 LF 被当空命令多回一个 OK"不成立：移远/合宙固件
 *     对空行一律忽略，且本工程的判定是"整行**恰好**等于 OK"，空行不匹配。
 *   ★ 想回退成只发 CR：把 AT_EOL_SEND_LF 改 0，其余代码不用动。
 *============================================================================*/
#define AT_EOL_SEND_LF  1

ota_ret_t at_send_cmd(const char *str)
{
    static const char * xdata s_tx;
    static u16 xdata i;
    static u8  xdata ch;
    static u16 xdata s_bad;

    s_tx = str;                       /* ★ 同 at_send_raw：先拷 static */

    if (!s_ready) return OTA_ERR_STATE;
    if (s_tx == 0) return OTA_ERR_PARAM;

    /* ★★★ v0.6.3 发送守门 —— 由现场日志逼出来的
     *
     *   上板证据（step58 / bridge_20261027_202122.log）：
     *     某个调用点连发三次 `<FF> x140 + 02 CR/LF`，模组一律回 ERROR。
     *   0xFF 是 "xdata 没写过" 的值（铁律 13）⇒ 这不是命令，是 **指针走错了内存空间**：
     *   字符串本该在代码段（段字节 0xFF），实际被当成 xdata（段字节 0x00），
     *   于是"发一条 12 字节的命令"变成了"顺着 xdata 一直读到 0 为止"。
     *   长度还会随指针变（另一份日志里是 230 字节），尾巴都是同一个 0x02 —— 
     *   而 0x02 正是 s_rxbuf[0]（autobaud 期丢进去的乱码首字节），
     *   说明它一直读到了 0x1000（?XD?UART_AT 的起点）才碰上 0。
     *
     *   两条对策：
     *     ① 见到不可打印字节立刻停手。**垃圾灌进模组只会让 AT 状态更难判** ——
     *        模组对 143 字节垃圾回 ERROR，看起来就像"这条命令不支持"。
     *     ② 把位置和字节打进日志，下次一眼看出是哪个缓冲区被当成了命令。
     *   ★ 正常命令全是可见 ASCII，这条守门不可能误伤。 */
    ch = (u8)s_tx[0];
    if ((ch < 0x20U) || (ch > 0x7EU))
    {
        s_bad++;
        LOG_E2("AT", "TX REFUSED b0=%x b1=%x",
               (u16)ch, (u16)(u8)s_tx[1]);
        return OTA_ERR_PARAM;
    }

    for (i = 0; s_tx[i] != 0; i++)
    {
        /* 留 3 字节余量：CR 必发，LF 可能还要发，外加 '\0' */
        if (i >= (AT_LINE_MAX - 3U)) return OTA_ERR_PARAM;
        ch = (u8)s_tx[i];
        if ((ch < 0x20U) || (ch > 0x7EU))
        {
            s_bad++;
            LOG_E2("AT", "TX STOP i=%u ch=%x", (u16)i, (u16)ch);
            return OTA_ERR_PARAM;
        }
        at_uart2_putc(ch);
    }

    /* ★ 先 CR 再 LF，顺序不能反：模组按 CR 判命令行结束，
     *   先发 LF 等于先把一个"空行"塞给模组。 */
    at_uart2_putc(0x0DU);
#if (AT_EOL_SEND_LF != 0)
    at_uart2_putc(0x0AU);
#endif
    return OTA_OK;
}

/*==============================================================================
 * ★★★ v0.6.11：零指针命令通道 —— 本工程唯一可靠的发命令方式
 *
 *   上板证据（step62 / bridge_20261028_082521.log，这一轮把病根定死了）：
 *
 *     MCU 自己的日志                    桥接日志（模组**真正收到**的）
 *     --------------------------------  ----------------------------------
 *     [AT] TX REFUSED b0=ff b1=ff       什么都没到
 *     [AT] TX REFUSED b0=87 b1=de       什么都没到
 *     [AT] TX REFUSED b0=a  b1=e2       什么都没到（连发 12 次/轮）
 *     （什么也没打）                     `ailed<CR><LF>` → 模组回 ERROR
 *
 *   三条铁证，缺一不可：
 *     ① 被拒的 b0/b1 是 **0x0A / 0xE2 / 0x87 / 0xDE** —— 这些是机器码字节，
 *        不是 ASCII ⇒ 指针**仍然指向代码段**，但低 16 位地址被改坏了，
 *        落到了别的字面量中间；
 *     ② `ailed` 正是某个字面量（"…failed"）的尾巴 ⇒ 同一种病：
 *        地址错、段字节还对 ⇒ 顺着代码段读到 '\0' 为止，于是
 *        "发一条 AT+HTTPACTION=0" 变成了"发 5 个字节 ailed"；
 *     ③ 同一个函数里相邻两条命令，一条干净一条乱码（`AT+HTTPTERM` 好、
 *        `AT+SAPBR=3,1,…` 坏），**按调用点发病**，同一调用点稳定复现
 *        ⇒ 与线路、波特率、模组**全部无关**，是编译产物。
 *
 *   根因：把**代码段字符串的指针当形参跨函数传递**。C251 的通用指针是 4 字节
 *   （段字节 + 24 位偏移），形参走寄存器时编译器会把指针值提前排进寄存器；
 *   中间任何一次调用（喂狗、延时、日志）都会覆盖它 —— 正是铁律 11：
 *   "跨调用还活着的值一律不许留寄存器，只读的临时值也不行"。
 *   这也解释了为什么"改命令串的写法"能救个别调用点，却永远救不全。
 *
 *   ⇒ 治法：**命令通路里一个指针都不传。**
 *     · 命令在模块级 xdata 缓冲 g_at_tx 里逐字节现拼（立即数 ⇒ 常量地址）；
 *     · 发送/等待入口形参 ≤3 B（只走寄存器，不生成 ?BYTE 参数块）；
 *     · 应答原文放 g_at_cap，调用方用 extern 全局数组直接读 ——
 *       全局变量不跨调用边界，两个编译单元在编译期就知道它的地址。
 *   这是本平台上唯一"结构上不可能再错"的写法，代价只是源码啰嗦。
 *============================================================================*/
char xdata g_at_tx[AT_TX_MAX];
u16  xdata g_at_txn;
char xdata g_at_cap[AT_CAP_MAX];
u16  xdata g_at_capn;

static char xdata s_ebuf[16];      /* expect 前缀现拼缓冲 */
static u8   xdata s_sr;            /* 发送结果暂存（避免局部变量） */

void at_tx_reset(void)
{
    g_at_txn = 0U;
    g_at_tx[0] = 0;
}

void at_tx_c(u8 c)
{
    if (g_at_txn >= (AT_TX_MAX - 3U)) return;   /* 留 CR / LF / '\0' 的位 */
    g_at_tx[g_at_txn] = (char)c;
    g_at_txn++;
    g_at_tx[g_at_txn] = 0;
}

void at_tx_d(u32 v)
{
    static u8  xdata d[10];
    static u8  xdata dl;
    static u32 xdata t;

    t  = v;
    dl = 0U;
    do { d[dl] = (u8)('0' + (u8)(t % 10UL)); dl++; t /= 10UL; } while (t != 0UL);
    while (dl > 0U) { dl--; at_tx_c(d[dl]); }
}

/* 发送 g_at_tx 里的内容（无形参 ⇒ 不存在指针被踩的可能） */
ota_ret_t at_send_cur(void)
{
    static u16 xdata i;
    static u8  xdata ch;

    if (!s_ready) return OTA_ERR_STATE;
    if (g_at_txn == 0U) return OTA_ERR_PARAM;

    for (i = 0U; i < g_at_txn; i++)
    {
        ch = (u8)g_at_tx[i];
        if ((ch < 0x20U) || (ch > 0x7EU))
        {
            LOG_E2("AT", "TX REFUSED i=%u ch=%x", (u16)i, (u16)ch);
            return OTA_ERR_PARAM;
        }
        at_uart2_putc(ch);
    }

    at_uart2_putc(0x0DU);
#if (AT_EOL_SEND_LF != 0)
    at_uart2_putc(0x0AU);
#endif
    return OTA_OK;
}

/* 发一条裸 "AT"（探活专用，同样不走指针） */
ota_ret_t at_send_at(void)
{
    at_tx_reset();
    at_tx_c((u8)'A');
    at_tx_c((u8)'T');
    return at_send_cur();
}

/*==============================================================================
 * ★ 自适应波特率（现场要求）
 *
 *   流程：设一个档位 -> 清空接收 -> 发 "AT\r\n" -> 300 ms 内只认"整行恰好是 OK/RDY"。
 *   关键纪律（现场原话："确认波特率之前的数据都丢弃不处理"）：
 *     · 进入本函数先把 s_cur_baud 清 0 —— 波特率没确认之前，"当前波特率"不存在；
 *     · 每换一个档位都 at_rx_flush() —— 上一档的残留（乱码）不带进下一档；
 *     · 解析时**只做一件事**：凑行、判断是否等于 "OK"/"RDY"。其余字节（回显、
 *       乱码、开机日志）全部当作不存在，不写 URC、不匹配 expect、不记错误。
 *       ★ Y100P 上电会**主动吐一大段开机日志**，这正是"必须过滤"的原因。
 *
 *   ★ 为什么命中 OK 要"连续两次"：波特率不对时收到的是乱码，凑巧拼出一行 "OK"
 *     并非不可能（尤其 9600 采样 115200 的信号，重复位多）。两次独立命中的
 *     联合概率足够低；而正确档位连命两次几乎无成本（各 300 ms 上限，实测几十毫秒）。
 *     ★ 但 **RDY 例外**：RDY 是"这一帧波特率对上了"的一次性通告，只可能出现一次，
 *       要求连命两次会永远等不到 ⇒ 见 RDY 立即采纳（见 at_autobaud）。
 *
 *   ★ 档位表用 switch 展开成立即数，**不用 const 数组** —— 铁律 7：代码基址
 *     0xFF0000 超 16 位，const 表取址只搬 WORD0（crc32_tab4 恒错的老坑）。
 *============================================================================*/

#define AT_BAUD_N       12U     /* 扫描档位数                                 */
/* 单档等待。
 * ★ 100 ms -> 300 ms 的原因：
 *   Y100P 上电/重启会**主动吐一大段开机日志**（银尔达官方原话：这些日志
 *   一般需要过滤），而且在低波特率（1200/2400）下这段日志要好几秒才吐完。
 *   115200 下 100 ms 只够 1150 B，日志一长 OK 就落到窗口外 ——
 *   表现为"明明通了却报 0 B"。300 ms 留 3 倍余量，期间一直喂狗（at_wdt_tick）。 */
#define AT_BAUD_TO_MS   300U

static u16 xdata s_pn[AT_BAUD_N];   /* 各档收到的字节数（诊断：全不通时看谁有动静） */
static u8  xdata s_pr[AT_BAUD_N];   /* 各档是否见到过 "RDY"（1 = 波特率对上的铁证） */
static u16 xdata s_pgot;            /* 本档收到的字节数                            */
static u8  xdata s_best_i;          /* 本次扫描里收到字节最多的档（0xFF=全 0）      */
static u16 xdata s_best_n;          /* 那一档收到的字节数                          */

/* 档位映射。
 * ★ 顺序依据：银尔达官方"默认 115200"、"自适应范围 1200~115200"。
 *   115200  —— 出厂默认，排第一最省时间
 *   9600 起 —— 现场最可能被人改成的档
 *   4800/2400/1200 —— 官方明确在自适应范围内，**以前漏了这三档**，必须补
 *   230400/460800 —— 只在被写死成固定高波特率时用得上（Y100P 上限 460800）
 *
 * ★ 档位表用 switch 展开成立即数，**不用 const 数组** —— 铁律 7：代码基址
 *   0xFF0000 超 16 位，const 表取址只搬 WORD0（crc32_tab4 恒错的老坑）。 */
static u32 at_baud_of(u8 idx)
{
    switch (idx)
    {
        case 0U: return 115200UL;
        case 1U: return 9600UL;
        case 2U: return 38400UL;
        case 3U: return 57600UL;
        case 4U: return 19200UL;
        case 5U: return 28800UL;
        case 6U: return 14400UL;
        case 7U: return 4800UL;
        case 8U: return 2400UL;
        case 9U: return 1200UL;
        case 10U: return 230400UL;
        case 11U: return 460800UL;
        default: return 0UL;
    }
}

/* 清空接收侧：软件环形缓冲 + 硬件寄存器里的残留字节 */
static void at_rx_flush(void)
{
    static u8 xdata d;

    s_head     = 0U;
    s_tail     = 0U;
    s_ovf      = 0U;
    s_line_len = 0U;
    /* 注：捕获计数 s_cap_n 定义在本文件后半段，且只服务于 at_cmd_wait，
     *     那边每次进入都会自己清零，这里不必也不该碰它。 */

    while (at_uart2_getc(&d)) { }     /* 硬件里最多再留 1 个字节，清干净 */
}

/* 探测结果（★ v0.49：从"通/不通"两态扩成三态）
 * ★ v0.52 起这三个宏搬到 uart_at.h —— app_main.c 里也要判 RDY/OK，
 *   两处各写一份迟早会漂。这里不再重复定义。 */

/* 在当前档位发一次 "AT\r"。
 *   返回 AT_PROBE_OK / AT_PROBE_RDY / AT_PROBE_NONE
 *
 * ★★ 为什么必须多认一个 "RDY"（Air724UG 换型的关键改动）───────────────
 *   《Air724UG 硬件设计手册》原文：
 *     "AT 指令版本默认情况下模块是自适应波特率(AT+IPR=0)，在自适应波特率模式下，
 *      开机后初始化信息（开头是 RDY）**不会回发给主控机**。
 *      主控机需首先发送 AT 字符给模块来训练主控机的波特率，此时模块会上报
 *      初始化信息，表明训练成功。"
 *   ⇒ 现场的时序是：我们发 AT → 模组拿这一帧测出波特率 → **先吐一大段开机
 *     横幅（首行 RDY）** → 之后才响应命令。
 *   ⇒ 只认 "OK" 的话，如果横幅长到把 300 ms 窗口撑满，就会"通了却报 0 B"。
 *     "RDY" 这个 3 字母整行是模组专属串，乱码拼出它的概率远低于拼出 "OK"，
 *     所以见到 RDY 就**不必**再等 OK —— 它已经证明"这一档波特率是对的"。
 *
 * ★ 无形参以外的形参只有 to_ms（2 B），其余全 static。 */
static u8 at_probe_once(u16 to_ms)
{
    static u32 xdata t0;
    static u32 xdata loops;
    static u32 xdata lim;
    static u16 xdata got;
    static u8  xdata c;
    static u8  xdata hit;
    static u8  xdata lp;      /* ★ v0.6.4 子串判据的滚动窗口（上一字节）   */
    static u8  xdata lp2;     /* 上上字节（认 RDY 用）                     */

    at_rx_flush();
    if (at_send_at() != OTA_OK) return AT_PROBE_NONE;
    at_wdt_tick();

    t0    = s_at_ms;
    loops = 0UL;
    lim   = (u32)to_ms * 500UL;
    got   = 0U;
    hit   = AT_PROBE_NONE;
    lp    = 0U;
    lp2   = 0U;

    for (;;)
    {
        /* ★ 必须先"捞进环形缓冲"再从环里解析，不能直接读硬件寄存器。
         *   原因：at_send_cmd() 里的 at_uart2_putc() 在发送期间就会顺手
         *   把收到的字节塞进环（模组 ATE1 回显来得极快）。如果这里只读
         *   S2BUF，那些已经进环的字节就**永远留在环里**：
         *     · OK 被漏掉 → 明明收到却判"没应答"；
         *     · s_pgot 只统计直读的字节 → "0 B" 这个诊断会撒谎。
         *   v0.47 上板看到的"10 档全 0 B"就有这个成分在。 */
        /* ★★★ v0.6.6：本函数原来还是"先把硬件搬空、再整段解析"的老结构 ——
         *   v0.6.5 只改了 at_wait_core，**漏了这里**，而 step61 恰恰死在
         *   这个函数上（4 轮 `try rc=4`，紧接着 at_line_diag 量到
         *   `edges=136 low=201` —— 线上有整整 4 条应答的电平活动）。
         *
         *   老结构的问题在数学上是确定的：解析循环里**一次都不读 S3BUF**，
         *   而 UART3 只有一个接收寄存器（115200 下一字节 87 us）。
         *   环里一旦积了 n 个字节，"搬空 -> 解析 n 个" 的周期 = 解析 n 个的时间，
         *   期间新到的字节全部被覆盖 ⇒ 收到的是原文的**子序列**（字符都对、
         *   数量少一半），这正是七轮来反复看到的形态。
         *   ⇒ 与 at_wait_core 统一成 **1:1 交错**：每圈先抢硬件，再处理 1 字节。 */
        if (at_uart2_getc(&c)) { ring_push(c); }

        if (ring_pop(&c))
        {
            got++;

            /* ★★★ v0.6.4：**改成子串级判定**，这是本版最关键的一处修正。
             *
             *   现场证据（step59 / step_dl_v0_6_3）：四次 `try` 全部 rc=4，
             *   而同一时刻桥接日志里 **每一次 `AT` 都收到了完整的
             *   `AT<CR><LF><CR><LF>OK<CR><LF>`**；随后 `at_line_diag()` 的
             *   4 条 AT 又量到 `edges=106 low=190`（≈2~3 条应答的电平活动）。
             *   ⇒ 应答到了线上，是**判据把它扔了**，不是收不到。
             *
             *   旧判据 `s_line_len == 2 && line == "OK"` 要同时满足三件事：
             *     ① CR/LF 一个字都不许丢（否则行不结束）；
             *     ② 回显行 `AT` 的 CR/LF 也得不丢（否则 `AT` 与 `OK` 粘成
             *        `ATOK`，行长 4，判据直接不成立）；
             *     ③ 'O','K' 两个字节都得活下来。
             *   在这条丢一半字节的线上，①③ 合计约 0.5^3 = 12.5%，
             *   再叠加 ②，单次成功率约 6%；而 at_autobaud 还要求**连命两次**
             *   ⇒ 一轮 12 档扫下来成功率 ~0.4%。四轮全挂完全符合。
             *
             *   子串判据要的只是"字节流里 'O' 紧跟 'K'"：
             *     丢字节只会把字符**拿掉**、绝不会凭空造出 'O''K' 相邻，
             *     而错档位下收到的是乱码（`cap_put` 会写成 '.'），
             *     随机凑出相邻 'O''K' 的概率约 1/65536 —— 比"两条应答都完整"
             *     低两个数量级。⇒ 只救真成功，不放过真失败。
             *   ★ 回显行 `AT` 里没有 'O'，所以子串判据不会误命中回显。
             */
            if (lp2 == (u8)'R' && lp == (u8)'D' && c == (u8)'Y')
            {
                hit = AT_PROBE_RDY;
                break;
            }
            if (lp == (u8)'O' && c == (u8)'K')
            {
                hit = AT_PROBE_OK;
                break;
            }
            lp2 = lp;
            lp  = c;

            /* 行缓冲仍然维护：at_autobaud 之后可能有别的调用点要看整行原文 */
            if (c == '\n')
            {
                if (s_line_len > 0U) s_line[s_line_len] = 0U;
                s_line_len = 0U;
            }
            else if (c != '\r')
            {
                if (s_line_len < (AT_LINE_MAX - 1U)) s_line[s_line_len++] = c;
                else s_line_len = 0U;       /* 超长行（乱码）整段丢 */
            }
        }
        if (hit != AT_PROBE_NONE) break;

        /* 时基每圈推（一条位测试），超时/兜底这类 32 位运算每 16 圈一次 */
        at_t0_poll();
        loops++;
        if ((loops & 0x0FU) == 0U)
        {
            if ((u32)(s_at_ms - t0) >= (u32)to_ms) break;
            if ((loops & 0x3FU) == 0U) at_wdt_tick();
            if (loops >= lim) break;        /* Timer0 没跑也不许死等 */
        }
    }

    s_pgot = got;
    return hit;
}

/*==============================================================================
 * ★ 不依赖中断的毫秒延时（v0.49 新增，专为"等 Air724UG 启动"而加）
 *
 *   为什么不能直接用 bsp_timer_delay_ms()：
 *     它靠 bsp_timer_ms() 的 s_ms 判到时，而 s_ms 由 Timer0 **中断**自增。
 *     P0 阶段 APP 全程 EA=0 ⇒ s_ms 永不增 ⇒ 那个函数会**永远等下去**
 *     （好在它内部喂狗，所以表现是"卡住但复位不了"，比看门狗复位更难查）。
 *   本函数用 TF0 标志轮询（与 at_t0_poll 同一套时基），EA=0 下照样准。
 *
 *   Air724UG 要求上电 2~3 s 后再发 AT，调用点见 app_main.c 的 APP_MODEM_BOOT_MS。
 *============================================================================*/
void at_delay_ms(u16 ms)
{
    static u32 xdata t0;
    static u32 xdata loops;
    static u32 xdata lim;

    t0    = s_at_ms;
    loops = 0UL;
    lim   = (u32)ms * 500UL;        /* 兜底：Timer0 没跑也不许死等 */

    for (;;)
    {
        at_t0_poll();
        if ((u32)(s_at_ms - t0) >= (u32)ms) break;
        loops++;
        if ((loops & 0x3FU) == 0U) at_wdt_tick();
        if (loops >= lim) break;
    }
    at_wdt_tick();
}

/*==============================================================================
 * ★★★ v0.6.12：接收保真度实测（定长应答数字节）
 *
 *   为什么要它 —— 八轮以来"丢字节是线路还是固件"一直靠形态推断：
 *     · "rx 里没有 '.' ⇒ 不是波特率"
 *     · "收到的字符是原文子序列 ⇒ 接收溢出"
 *   这些都对，但都不能给出一个**数**，于是每次上板都在同一个问题上绕圈。
 *
 *   这里换一条不需要推理的路：挑一条**应答长度完全固定**的命令，把收到的
 *   字节数直接数出来。
 *
 *       ATE0 下  AT+IPR?  ->  "\r\n+IPR: 115200\r\n\r\nOK\r\n"   = 22 B
 *       ATE1 下  （带回显）->                          再 + 8 B   = 30 B
 *
 *   判据没有解释空间：
 *       n == 22 / 30  ⇒ 这条链路上一个字节都没丢
 *       n <  22       ⇒ 丢了 22-n 个字节，**丢在线路或驱动这一侧**
 *                       （固件侧只有环形缓冲满才会丢，那由 s_ovf 记账）
 *
 *   ★ 为什么必须绕开 at_wait_core：宽松判据（AT_RESP_PART）见到相邻的
 *     'O''K' 就立即返回，根本等不到第 22 个字节 —— 用它数出来的数永远偏小，
 *     会把"线路干净"误判成"丢字节"。所以这里只数、不解。
 *
 *   ★ 收集窗口用"首字节到达后静默 60 ms"，而不是固定等满 1200 ms：
 *     22 字节在 115200 下只占 1.9 ms，60 ms 是 30 倍余量；
 *     8 次 × 60 ms 才半秒，不会把启动时间拖长。
 *     硬上限 1200 ms 兜"模组完全没回"的情况（那时 got 会是 0）。
 *============================================================================*/
u16 xdata g_rxp_ok;
u16 xdata g_rxp_min;
u16 xdata g_rxp_max;

void at_rx_probe(u8 tries)
{
    static u8  xdata i;
    static u8  xdata cc;
    static u8  bad;
    static u16 xdata got;
    static u16 xdata sp;
    static u16 xdata base;
    static u32 xdata t0;
    static u32 xdata tb;
    static u16 xdata g[20];      /* ★ 每次的长度；判据靠"彼此相等"，不靠具体数值 */

    g_rxp_ok  = 0U;
    g_rxp_min = 0xFFFFU;
    g_rxp_max = 0U;
    bad       = 0U;

    if (tries == 0U) tries = 1U;
    if (tries > 19U) tries = 19U;

    /* ★★★ v0.6.14：这条探针原来写的是 `AT+IPR?` + `got==22 || got==30`。
     *
     *   22/30 是 **SIMCom 专属**的应答长度。换 Air724UG 之后 `AT+IPR?` 的应答
     *   长度不同（甚至可能整条不支持、只回 7 字节 ERROR）⇒ 判据永远不成立，
     *   会打出 `exact 0/6`，把"链路完全正常"报成"每次都在丢字节"，
     *   然后整轮判读方向全错。判据不能钉死在某个模组的字节数上。
     *
     *   新判据（与模组型号无关，永远成立）：
     *     **同一条命令、同一模组状态，应答长度必然是定值。**
     *     所以只要 N 次测得的长度彼此相等（min==max），就是一个字节都没丢；
     *     长度参差不齐 ⇒ 每次丢的个数不同 ⇒ 真的在丢字节。
     *   代价是必须先把 20 个长度存下来（40 B xdata），比猜具体数值可靠得多。
     *
     *   命令同时换成 `AT+CGMR`（查固件版本）：
     *     · 它是 3GPP 标准命令，SIMCom / 合宙 / 移远全都有；
     *     · 应答 40 B 量级，长度足够暴露"长应答丢字节"（`AT` 只有 6 B，测不出）；
     *     · 顺带把模组型号和固件版本打出来 —— 换硬件后第一件要知道的事。
     *
     *   ★ 第一次不计入统计：刚发完 ATE0，模组可能还有一次回显残留，
     *     那一趟的长度天然与后续不同，算进去会白白贡献一个"不相等"。
     *============================================================================*/
    for (i = 0U; i <= tries; i++)
    {
        /* ① 清干净：上一个应答的尾巴绝不能算进这一次 */
        s_tail = s_head;
        while (at_uart2_getc(&cc)) { }

        /* ② 现拼命令（零指针：立即数 + at_send_cur，见文件里那段病根说明） */
        at_tx_reset();
        at_tx_c((u8)'A'); at_tx_c((u8)'T'); at_tx_c((u8)'+');
        at_tx_c((u8)'C'); at_tx_c((u8)'G'); at_tx_c((u8)'M'); at_tx_c((u8)'R');
        if (at_send_cur() != OTA_OK)
        {
            LOG_E0("AT", "cgmr probe: tx refused");
            return;
        }

        /* ③ 只数不解 */
        got = 0U;
        sp  = 0U;
        t0  = s_at_ms;
        tb  = t0;
        for (;;)
        {
            at_t0_poll();
            if (at_uart2_getc(&cc))
            {
                got++;
                tb = s_at_ms;             /* 末字节时刻 */
            }
            else
            {
                sp++;
                if ((sp & 0x3FU) == 0U) at_wdt_tick();
            }
            if (got != 0U && (u32)(s_at_ms - tb) >= 60UL) break;
            if ((u32)(s_at_ms - t0) >= 2000UL) break;   /* CGMR 应答长，放宽到 2 s */
        }

        if (i == 0U) continue;            /* 预热那一趟不参与统计 */

        g[i - 1U] = got;
        if (got < g_rxp_min) g_rxp_min = got;
        if (got > g_rxp_max) g_rxp_max = got;
    }

    /* ④ 判据：与第一次相等就算一致；基准太短（模组根本没应答）不算数 */
    base = g[0];
    if (base >= 8U)
    {
        for (i = 0U; i < tries; i++)
        {
            if (g[i] == base) g_rxp_ok++;
            else if (bad < 3U)
            {
                bad++;
                LOG_I2("AT", "  cgmr#%u n=%u", (u16)(i + 1U), g[i]);
            }
        }
    }

    LOG_I2("AT", "rxprobe exact %u/%u", g_rxp_ok, (u16)tries);
    LOG_I2("AT", "  n min=%u max=%u", g_rxp_min, g_rxp_max);
}

ota_ret_t at_autobaud(u32 *p_baud)
{
    static u8 xdata i;
    static u8 xdata idx;
    static u8 xdata a;

    if (!s_ready) return OTA_ERR_STATE;
    if (p_baud != 0) *p_baud = 0UL;

    /* ★ 确认之前，"当前波特率"不存在 ⇒ 任何数据都不处理 */
    s_cur_baud = 0UL;
    idx = 0xFFU;
    s_best_i = 0xFFU;
    s_best_n = 0U;

    /* ★★★ v0.6.6：**先在第 0 档（115200）上重点打三次**
     *
     *   step61 现场：4 轮 `try rc=4` 全挂，紧接着 at_line_diag 在同一档
     *   （115200）上量到 `edges=136 low=201` —— 说明模组**回了整整四条应答**，
     *   只是那四次探针一次都没凑巧拼出相邻的 'O''K'。
     *   这就是"单次命中即采纳"在 50% 丢字节下的真实成功率：远低于理论 25%，
     *   于是"4 轮 × 1 次"仍然会全军覆没。
     *   ⇒ 既然模组的波特率写死 115200（step60 桥接侧已证 AT+IPR 无效），
     *     就不该把 12 档平摊成 12 次机会，而该把力气全押在第 0 档。
     *   代价：多 2 × 0.5 s。收益：把"拼运气"变成"稳定可靠"。 */
    at_uart2_baud(at_baud_of(0U));
    s_pn[0] = 0U; s_pr[0] = 0U;
    for (i = 0U; i < 3U; i++)
    {
        a = at_probe_once(500U);
        if (s_pgot > s_pn[0]) { s_pn[0] = s_pgot; s_best_i = 0U; s_best_n = s_pgot; }
        if (a == AT_PROBE_OK || a == AT_PROBE_RDY)
        {
            idx = 0U;
            if (a == AT_PROBE_RDY) s_pr[0] = 1U;
            break;
        }
        at_delay_ms(120U);
    }

    for (i = 1U; (idx == 0xFFU) && (i < AT_BAUD_N); i++)
    {
        s_pr[i] = 0U;
        at_uart2_baud(at_baud_of(i));

        a = at_probe_once(AT_BAUD_TO_MS);
        s_pn[i] = s_pgot;
        if (s_pgot > s_best_n) { s_best_i = i; s_best_n = s_pgot; }

        /* ★ 见到 RDY 就立刻采纳，**不再要求第二次**：
         *   Air724UG 的 RDY 是"波特率训练成功"的一次性通告，只发一次。
         *   要求"连命两次"会永远等不到第二次 RDY，把已通的情况判成不通。 */
        if (a == AT_PROBE_RDY)
        {
            s_pr[i] = 1U;
            idx = i;
            break;
        }
        if (a != AT_PROBE_OK) continue;

        /* ★★ v0.6.4：删掉"同一档连命两次"。
         *
         *   原来那条规矩是防"乱码碰巧凑出 OK"。但 v0.6.4 起 at_probe_once()
         *   已改成**子串级**判据，错档位随机凑出相邻 'O''K' 的概率约 1/65536；
         *   而在这条丢一半字节的线上，"两条应答都完整"的概率是 0.25 × 0.25。
         *   ⇒ 旧规矩把成功率从 25% 压到 6%，是 step59 四轮全挂的直接原因。
         *   现在单次命中即采纳，代价可忽略（s_pgot 顺带记下收到多少字节）。 */
        idx = i;
        break;
    }

    if (idx == 0xFFU)
    {
        /* ★★ 必须把硬件拨回 115200，不能就这么走掉（v0.51 修）。
         *   循环结束时硬件停在**最后一档**（460800），若不复位：
         *     · 后面主循环的 AT keepalive 会用 460800 发 "AT"，
         *       工具侧按 115200 收 → 全是乱码，现场看到的就是
         *       "<DEL><EOT><NUL>... 未以换行结束"，害人以为线接错了。
         *     · 下次 autobaud 还会从第 0 档重新设，所以这里不影响扫描本身，
         *       只影响"扫描失败之后"的每一次发送。
         *   s_cur_baud 仍然保持 0 —— 那是"逻辑上未确认"，与硬件默认值无关。 */
        at_uart2_baud(115200UL);
        s_cur_baud = 0UL;                   /* 全档位不通：仍然不认 */
        return OTA_ERR_TIMEOUT;
    }

    s_cur_baud = at_baud_of(idx);
    at_uart2_baud(s_cur_baud);

    /* ★ 确认后先让模组把开机横幅吐完：Air724UG 训练成功会补发一大段初始化
     *   信息，此时立刻发命令容易和自己的横幅撞在一起。等 200 ms 再清干净，
     *   之后 at_get_baud() 才对外可见。 */
    at_delay_ms(200U);
    at_rx_flush();
    if (p_baud != 0) *p_baud = s_cur_baud;
    return OTA_OK;
}

u32 at_get_baud(void)
{
    return s_cur_baud;
}

u8 at_baud_count(void)
{
    return (u8)AT_BAUD_N;
}

u32 at_baud_of_idx(u8 idx)
{
    return at_baud_of(idx);
}

u16 at_autobaud_bytes(u8 idx)
{
    return (idx < AT_BAUD_N) ? s_pn[idx] : 0U;
}

/* 第 idx 档是否见到过 "RDY"（1 = 模组自报"波特率训练成功"）。
 * ★ 判读价值：bytes>0 但 rdy=0 ⇒ 有信号但拼不出完整行，多半是波特率差一档；
 *              rdy=1         ⇒ 这一档就是对的，不用再看 OK。 */
u8 at_autobaud_rdy(u8 idx)
{
    return (idx < AT_BAUD_N) ? s_pr[idx] : 0U;
}

/*==============================================================================
 * ★★★ v0.6.6：本次扫描的"最强档位" —— 一条能把"线上有没有字节"钉死的判据
 *
 *   为什么加：`try rc=4` 只说"没凑出 OK"，它把两件完全不同的事混成了一个数：
 *     ① 一个字节都没收到   ⇒ 收通道/接线/模组没说话；
 *     ② 收到 10 个字节但没拼出 'O''K' ⇒ 收到了，是判据或丢字节。
 *   而 at_line_diag 的 edges 只能证明"引脚在动"，证明不了"UART 收得到"。
 *   ⇒ 每轮把 12 档里收到字节最多的那一档连同字节数打出来：
 *     best b=115200 n=10  ⇒ 收通道好的，问题在判据（那就继续加宽判据）
 *     best b=0 n=0        ⇒ 12 档一个字节都没有 ⇒ 硬件（接线/对拉/供电）
 *   这一对数没有任何解释空间。
 *============================================================================*/
u32 at_sweep_best_baud(void)
{
    if (s_best_i >= AT_BAUD_N) return 0UL;
    return at_baud_of(s_best_i);
}

u16 at_sweep_best_n(void)
{
    return (s_best_i >= AT_BAUD_N) ? 0U : s_best_n;
}

/* 上一次 at_probe_once 收到的字节数（含乱码） */
u16 at_probe_bytes(void)
{
    return s_pgot;
}

/*==============================================================================
 * ★★ 线路诊断（v0.48）：不依赖波特率地问一句"线上到底有没有信号"
 *
 *   为什么必须加：v0.47 上板的结果是"10 档全 0 B"。"0 字节"这件事至少有三个
 *   完全不同的根因，只看串口分不开：
 *     ① 固件 —— UART2 时钟死（Timer2 没跑/重装值错）：连"发"都发不出去；
 *     ② 固件 —— 接收通道死（S2REN/引脚模式错）：发得出去但一个字收不回；
 *     ③ 硬件 —— 模组没上电/没启动/TXD 没接过来：线上根本没有信号。
 *   本函数只做一件事：**在完全绕开 UART 的情况下，数 P1.0 有没有跳过变**。
 *   只要模组在应答（哪怕波特率不对，收到的是乱码），引脚就一定会动。
 *     edges == 0 且 low_n == 0  ⇒ 线上全程恒高、一次跳变都没有 ⇒ ③（硬件）
 *     edges > 0 而 UART 仍 0 字节 ⇒ ②（收通道）
 *   与 at_diag_tx_to() 合起来三条判据互相独立，一次上板就能定位到具体环节。
 *
 *   ★ 采样窗口用 s_at_ms（TF0 时基）而不是循环计数：现场实测一条 AT 的
 *     往返约 100 ms，窗口必须盖得住，否则"没跳到"是采样太短造成的假象。
 *============================================================================*/
void at_diag_clear(void)
{
    s_tx_ok = 0U;
    s_tx_to = 0U;
    s_edges = 0U;
    s_low_n = 0U;
}

u16 at_diag_tx_ok(void) { return s_tx_ok; }
u16 at_diag_tx_to(void) { return s_tx_to; }
u16 at_diag_edges(void) { return s_edges; }
u16 at_diag_low(void)   { return s_low_n; }

#if (AT_DIAG_LEGACY != 0)   /* 见 uart_at.h：v0.7.0 起下线，源码保留 */
void at_line_diag(void)
{
    static u32 xdata t0;
    static u32 xdata guard;
    static u8  xdata v;
    static u8  xdata prev;
    static u8  xdata r;

    s_edges = 0U;
    s_low_n = 0U;
    at_rx_flush();                  /* 诊断不解析内容，先把残留倒掉 */

    prev = ATR_RXD();

    /* 连发 4 次，每次发完紧跟 150 ms 高频采样（盖得住 100 ms 的往返） */
    for (r = 0U; r < 4U; r++)
    {
        (void)at_send_at();

        t0    = s_at_ms;
        guard = 0UL;
        for (;;)
        {
            v = ATR_RXD();
            if (v != prev) { s_edges++; prev = v; }
            if (v == 0U)    { s_low_n++; }

            at_t0_poll();
            if ((u32)(s_at_ms - t0) >= 150UL) break;
            guard++;
            if (guard >= 200000UL) break;      /* Timer0 没跑也不许死等 */
            if ((guard & 0x3FFUL) == 0UL) at_wdt_tick();
        }
        at_wdt_tick();
    }

    /* ★ v0.53：诊断期间不读 S2BUF，环里/硬件里都挂着东西，退出前必须倒掉，
     *   否则第一条真正的命令会把这些旧回显当成自己的应答（见 at_wait_core 注释）。 */
    at_rx_flush();
}

/*==============================================================================
 * ★★★ v0.6.6：裸采样 —— 把「收到的字节原样打出来」，**不做任何判定**
 *
 *   为什么必须加这一条：`try rc=4` 是一个把三件事混成一个数的判据。
 *   它既可能是"一个字节都没收到"，也可能是"收到 10 个字节但没拼出 'O''K'"。
 *   七轮返工里反复出现"先判成硬件、后来发现是判据"的事故，根子都在这里。
 *   本函数只做一件事：在当前硬件波特率上发一条 "AT"，把收到的每个字节
 *   **按原样（不可打印的写成 '.'）** 塞进 s_line，返回字节数。
 *   ⇒ 现场只要看这一行原文，形态立刻分家：
 *        n=0                ⇒ 收通道/接线/供电（UART 一个字都没拿到）
 *        "AT..OK"           ⇒ 一字节不丢，链路完全干净
 *        "AT. . K"          ⇒ 字符都在、只是少了几个 = 整字节丢（信号被打坏）
 *        整片 "....."        ⇒ 波特率错（错帧）
 *   ★ 它不读 s_cur_baud，因此在 autobaud 失败（s_cur_baud==0）之后照样能用 ——
 *     这正是最需要证据的时刻，而 at_cmd_wait 那一套此时全都拒绝工作。
 *============================================================================*/
u16 at_raw_at(u16 to_ms)
{
    static u32 xdata t0;
    static u32 xdata lim;
    static u32 xdata loops;
    static u16 xdata n;
    static u8  xdata c;

    at_rx_flush();
    s_line_len = 0U;
    s_line[0]  = 0U;

    if (at_send_at() != OTA_OK) { s_pgot = 0U; return 0U; }
    at_wdt_tick();

    t0    = s_at_ms;
    loops = 0UL;
    lim   = (u32)to_ms * 500UL;
    n     = 0U;

    for (;;)
    {
        if (at_uart2_getc(&c))
        {
            if (n < (AT_LINE_MAX - 1U))
            {
                s_line[n] = (u8)(((c >= 0x20U) && (c < 0x7FU)) ? c : (u8)'.');
            }
            if (n < 0xFFFFU) n++;
        }

        at_t0_poll();
        loops++;
        if ((loops & 0x0FU) == 0U)
        {
            if ((u32)(s_at_ms - t0) >= (u32)to_ms) break;
            if ((loops & 0x3FU) == 0U) at_wdt_tick();
            if (loops >= lim) break;
        }
    }

    if (n > (AT_LINE_MAX - 1U)) n = (u16)(AT_LINE_MAX - 1U);
    s_line[n] = 0U;
    s_pgot   = n;
    return n;
}

/* 裸采样的原文（xdata 地址，调用方按 %s 打印）。
 * ★ 返回 u32 而不是指针：日志参数槽就是 u32，直接给地址最不容易出错。 */
u32 at_raw_line(void)
{
    return (u32)(s_line);
}

/*==============================================================================
 * ★★ 纯监听（v0.4.1）：**一个字节都不发**，只数 P1.0 在 ms 毫秒内跳过多少次
 *
 *   为什么要它 —— 它和 at_line_diag() 问的是两个不同的问题：
 *     at_line_diag()  = "我说了话，它有没有回？"   ⇒ 0 也可能只是它懒得回
 *     at_listen()     = "我不说话，它自己会不会开口？"
 *
 *   模组上电会**主动**吐一段开机日志（没人问也照说）。所以：
 *     · 抓到跳变 ⇒ 模组活着、TXD 这根线通、P1.0 输入正常
 *                   ⇒ 问题只剩"我们的话没送到它耳朵里"（TXD2 → 模组 RXD）
 *     · 全程 0  ⇒ 模组根本没在运行（没电 / 没按 PWRKEY / 线断），
 *                   与 AT 命令组、波特率、服务器**全都无关**
 *
 *   只采样：不发字节、不读 S2BUF、不碰波特率与闸门 ⇒ 结论不受任何固件状态影响。
 *============================================================================*/
u16 at_listen(u16 ms)
{
    static u32 xdata t0;
    static u32 xdata lim;
    static u32 xdata guard;
    static u8  xdata v;
    static u8  xdata prev;

    s_edges = 0U;
    s_low_n = 0U;

    prev  = ATR_RXD();
    t0    = s_at_ms;
    guard = 0UL;
    /* 兜底：Timer0 万一没跑，s_at_ms 不增 ⇒ 不能死等。
     * 一圈约 2~5 us，按 2000 圈/ms 取上限（宁可多等，不许提前收工）。 */
    lim = (u32)ms * 2000UL + 2000UL;

    for (;;)
    {
        v = ATR_RXD();
        if (v != prev) { s_edges++; prev = v; }
        if (v == 0U)    { s_low_n++; }

        at_t0_poll();
        if ((u32)(s_at_ms - t0) >= (u32)ms) break;
        if (++guard >= lim) break;
        if ((guard & 0x3FFUL) == 0UL) at_wdt_tick();
    }

    return s_edges;
}

/*==============================================================================
 * ★★ 交叉监听（v0.5.0）：一次上电同时看**两路** RX 引脚有没有信号
 *
 *   现场要回答的问题只有一个："UART2 到底是不是坏了？"
 *   而 at_listen() 只能看当前通道，答不了"另一路是不是有信号"。
 *
 *   本函数采的是**指定的**那一路 RX 引脚，与当前 AT 通道无关，于是：
 *     · 本口 edges=0，另一口 edges>0
 *         ⇒ 模组明明在说话，收不到的只是本口引脚
 *         ⇒ **这就是"UART2 引脚坏了"的直接证据**（换到 UART3 即可绕过）
 *     · 两路都 edges=0   ⇒ 模组根本没说话（供电 / PWRKEY / TXD 断）
 *     · 两路都 edges>0   ⇒ 引脚都好，问题在波特率或上层命令
 *
 *   ★ 只读电平：不动串口使能、不动波特率、不清环 ⇒ 不会干扰正在工作的通道。
 *     把"另一路"的 RX 脚临时设成准双向是安全的：那根脚要么本来就没用，
 *     要么本来就是 UART 的 RX（准双向正是它的正确形态）。
 *============================================================================*/
u16 at_listen_port(u8 port, u16 ms)
{
    static u32 xdata t0;
    static u32 xdata lim;
    static u32 xdata guard;
    static u8  xdata v;
    static u8  xdata prev;
    static u8  xdata want;

    want = port;                /* 铁律 11：第一条语句拷进 static，此后不再读形参 */

    /* 目标口 RX 脚 -> 准双向 + 锁存 1（否则读到的永远是 0） */
    if (want == 2U)
    {
        P1M1 &= (u8)~0x01;  P1M0 &= (u8)~0x01;
        P1   |= (u8)0x01;
    }
    else if (want == 4U)
    {
        P5M1 &= (u8)~0x01;  P5M0 &= (u8)~0x01;
        P5   |= (u8)0x01;
    }
    else
    {
#if (AT_UART3_PIN_P5 != 0)
        P5M1 &= (u8)~0x01;  P5M0 &= (u8)~0x01;
        P5   |= (u8)0x01;
#else
        P0M1 &= (u8)~0x01;  P0M0 &= (u8)~0x01;
        P0   |= (u8)0x01;
#endif
    }

    s_edges = 0U;
    s_low_n = 0U;

    prev  = at_rxd_pin_read(want);
    t0    = s_at_ms;
    guard = 0UL;
    lim   = (u32)ms * 2000UL + 2000UL;      /* 与 at_listen 同一兜底口径 */

    for (;;)
    {
        v = at_rxd_pin_read(want);
        if (v != prev) { s_edges++; prev = v; }
        if (v == 0U)    { s_low_n++; }

        at_t0_poll();
        if ((u32)(s_at_ms - t0) >= (u32)ms) break;
        if (++guard >= lim) break;
        if ((guard & 0x3FFUL) == 0UL) at_wdt_tick();
    }

    return s_edges;
}
#endif /* AT_DIAG_LEGACY */

/*==============================================================================
 * 主循环把缓冲里的字节解析成行 —— ★★ v0.8.0 整段删除
 *
 *   这一套（at_poll + URC 前缀注册表 + 回调分发）是 v0.4x 的"后台收字节"方案，
 *   v0.7.0 起已被 at_pump 完全取代：
 *     · 全工程没有任何调用点（grep 过：at_poll / at_urc_register 外部引用 0 处）
 *     · 它自己还占着 ~400 B 代码 + 342 B xdata（两张 8 项的指针表）
 *   APP 的代码区只剩几十字节余量，删掉它比"再省一项功能"划算得多。
 *   要恢复：从 backup/20261028_v0_6_13 里拷回来，逻辑一行没改。
 *============================================================================*/

/*==============================================================================
 * 应答等待
 *
 * ★★ 形参共 16 B（3+3+2+3+2+3）> 8 ⇒ 必生成 ?BYTE 参数块，正是铁律 14 说的
 *    "门禁函数"形态（v0.30 读 / v0.35 擦 / v0.36 写三次翻车都是它）。
 *    ⇒ 拆成"薄门面 + 厚实现"：门面只把形参逐字拷进模块级 static，
 *      核心 at_wait_core() **无形参**、只读 static。
 *
 * ★★ 等待期间**一行日志都不许打**。115200 下一行 30 字符要 2.6 ms，
 *    这期间 UART2 会丢约 30 个字节 ⇒ 应答被自己打的日志吃掉。
 *    所以：整段原始应答先捕获进调用方给的 resp 缓冲（非打印字符转 '.'），
 *    函数返回之后再一次性打出来 —— 既看得全，又不丢字节。
 *============================================================================*/
static const char * xdata s_w_cmd;
static const char * xdata s_w_expect;
static u16  xdata s_w_to;
static char * xdata s_w_resp;
static u16  xdata s_w_cap;
static u16 * xdata s_w_plen;
static u16  xdata s_cap_n;
static u8   xdata s_w_res;
/* ★ v0.6.11：1 = 命令来自 g_at_tx（零指针通道），0 = 来自调用方指针 */
static u8   xdata s_use_cur;

/*==============================================================================
 * ★★★ v0.6.17：HTTPREAD 数据体的"零间隙绑定"
 *
 *   step67 实测：`chunk 0 code=206 len=1024` 之后每次都 `short 1020/1024`
 *   （偶尔 1022）。**固定少 4 个字节，位置在数据体头部** ⇒ 整片前移 ⇒
 *   只有 CRC 能发现，而 CRC 一旦对不上就再也续不上。
 *
 *   病根不在线、也不在扫描循环，而在**交接**：
 *     旧路径 = 扫描命中 `+HTTPREAD: 1024` 那行 → 返回 →
 *              at_wait_post() 把捕获缓冲逐字节映射一遍（20 字节的循环）→
 *              at_tx_mac 收尾 → at_binary_enter 的范围检查 →
 *              at_binary_read 初始化 → 才开始抢字节。
 *   这一串在 11.0592 MHz 上是几百微秒，而硬件**只有一个**接收寄存器，
 *   115200 下一字节 87 us ⇒ 这期间到达的字节直接覆盖丢失，`ovf` 都不计。
 *   4 个字节 ≈ 350 us，与实测完全吻合。
 *
 *   修法：前缀行到手那一刻（还在同一个 for 循环里）就把"接下来写哪里"绑好，
 *   让扫描循环继续抢字节 —— 从判定到落盘只剩十几条指令，必然 < 87 us。
 *   ★ 这条路径一个函数都不调，也不写捕获、不做行解析、不判 OK/ERROR。
 *============================================================================*/
static u8 * xdata s_bin_dst;      /* 落盘目标（arm 时已校验 xdata 范围） */
static u16  xdata s_bin_max;      /* 本次要收多少（含要被丢弃的前导字节） */
static u16  xdata s_bin_n;        /* 已落盘多少（不含丢弃的） */
static u16  xdata s_bin_got;      /* 已收到多少（含丢弃的）—— 满它才收工 */
static u16  xdata s_bin_skip;     /* 本条应答头部要丢弃几个字节（实测值） */
static u8   xdata s_bin_go;       /* 1 = 正在收数据体 */
static u8   xdata s_w_is_read;    /* 1 = 本条命令期待 +HTTPREAD: 前缀 */

/* ★★★ v0.6.19：接收期间的"最长空转"探针
 *
 *   step69 现场（bridge 侧原始字节已核对）：模组发出的 1024 B 数据体是完整的
 *   `4f 54 41 49 00 40 ...`，而固件收进 s_io 的却是 `4f 49 00 40 ...`
 *   —— 第 0 个字节对，第 1、2 个字节没了。收满 1024 就必须有一处
 *   至少 174 us（2 个字节时间）没人去取硬件寄存器。
 *   静态看代码找不到这处停顿，于是就地量：每个字节落盘时看 spin
 *   （"自上一个字节以来空转了几圈"），记下最大值和它出现在第几个字节。
 *   ⇒ 下一版日志的 `gap %u at %u` 会直接指出停顿位置：
 *      at=0 说明停顿在数据体之前（交接/模组节奏），at=1 说明在第 0 个字节之后。
 *   ★ 代价：每个字节 3 次 xdata 比较 + 一次清零，约 1 us，仍在 87 us 预算内。 */
static u8   xdata s_gap;          /* 最长空转（圈数，饱和 255） */
static u16  xdata s_gap_at;       /* 出现最长空转时已落盘字节数 */
static u8   xdata s_gap0;         /* 前缀行 → 首字节 的空转（饱和 255 ≈ 1 ms） */

/*==============================================================================
 * ★★★ v0.6.13：`poll gap` 整段删除（接口留空壳）
 *
 *   它从一开始就量错了东西：gap 只在"收到字节"时清零，所以它量到的是
 *   "模组从收到命令到吐出第一个字节的沉默时间"（毫秒级，模组本来就慢），
 *   与固件快慢无关，必然大于 87 us。
 *   而且它在**每个字节**的热路径上多花 4~6 条 xdata 访问——而下面
 *   at_wait_scan 的实测证据说明：热路径缺的就是这几十个指令。
 *   ⇒ 删干净，判据只剩 at_rx_probe()（定长应答实测字节数）与
 *     at_take_overflow()（环满丢弃数，现在只能发生在 TX 期间）。
 *============================================================================*/
u16 at_poll_gap_us(void) { return 0U; }


/* 一行到齐：判终态。返回 1 = 终态已定（s_w_res 有效） */
static u8 at_try_line(void)
{
    if (s_line_len == 0U) return 0U;
    s_line[s_line_len] = 0;

    /* ★★ v0.6.1 修的真 bug（此前从未走到过这一步，所以一直没现形）：
     *
     *   `AT+HTTPACTION=0` 的应答是 **先一行 OK，随后才是 +HTTPACTION: 0,200,<len>**。
     *   原实现"先比前缀、再比 OK"，但 OK 那行同样会命中第二条 ⇒ 立刻返回
     *   AT_RESP_OK，而调用方要的是 AT_RESP_PREFIX ⇒ 直接判 `action rc=0` 失败，
     *   下载永远走不到 HTTPREAD。
     *   ⇒ 语义修正：**调用方指定了前缀，就只认前缀**，OK 只是"命令被接受"的回执，
     *     不是终态；但 ERROR / +CME 仍然立即终止（那是明确失败）。
     */
    if (s_w_expect != 0)
    {
        if (line_has_prefix((const char *)s_w_expect))
        {
            s_w_res = (u8)AT_RESP_PREFIX;

            /*------------------------------------------------------------------
             * ★★★ v0.6.17：HTTPREAD —— 前缀行到手就**立刻**接管数据体
             *
             *   绝不能在这里 return 1（老做法）。一 return，调用方要走完
             *   at_wait_post 的映射 + at_tx_mac 的收尾 + at_binary_enter，
             *   几百微秒里丢掉的正是数据体开头 4 个字节（见文件前面那段）。
             *
             *   返回 0 表示"还没结束"：at_wait_scan 的 for 循环继续跑，
             *   下一字节到达即直通 s_bin_dst，超时/abort 判据原样保留。
             *----------------------------------------------------------------*/
            if ((s_w_is_read != 0U) && (s_bin_dst != 0) && (s_bin_max != 0U))
            {
                s_bin_n   = 0U;
                s_bin_got = 0U;
                s_bin_go  = 1U;
                return 0U;
            }

            return 1U;
        }
        if (line_is_error()) { s_w_res = (u8)AT_RESP_ERROR; return 1U; }
        if (line_has_prefix("+CME ERROR") || line_has_prefix("+CMS ERROR"))
        {
            s_w_res = (u8)AT_RESP_CME;
            return 1U;
        }
        return 0U;          /* OK 行继续等前缀 */
    }

    /* 未指定前缀：标准终态 */
    if (line_is_ok())    { s_w_res = (u8)AT_RESP_OK;    return 1U; }
    if (line_is_error()) { s_w_res = (u8)AT_RESP_ERROR; return 1U; }
    if (line_has_prefix("+CME ERROR") || line_has_prefix("+CMS ERROR"))
    {
        s_w_res = (u8)AT_RESP_CME;
        return 1U;
    }
    return 0U;
}

/*==============================================================================
 * ★★★ v0.6.13 重写热路径 —— 本轮唯一真正的修复
 *
 *   现场证据（step64，同一根线、相隔几百毫秒，日志原文）：
 *
 *     [0][AT] rxprobe exact 6/6          <- at_rx_probe：只数不解
 *     [0][AT]   n min=22 max=22          <- 6 次全部 22 字节，一个都没丢
 *     [0][APP] [AT+CPIN?] rc=4 n=13      <- at_wait_core：带解析
 *                                         <- 同一条应答 22 字节只收到 13 个
 *
 *   两条路唯一的差别是"读的时候做不做事"：probe 的循环体只有
 *   `if (RI) {RI=0; got++;}`，而这里每个字节要过四个函数调用
 *   （at_uart2_getc → ring_push → ring_pop → cap_put）+ 十几个 xdata 访问。
 *   115200 下一字节只有 87 us，**扛不住**。这就是八轮来"丢字节"的全部真相：
 *   **线是干净的，是本函数读得比模组吐得慢。**
 *
 *   新结构（三条纪律，缺一不可）：
 *     ① **先抢硬件，再做事**：`if (ATR_RI) { ATR_RI=0; c=ATR_BUF; ... }`
 *        直接碰 SFR，不进任何函数 —— 硬件那 1 个字节必须在 87 us 内取走。
 *     ② **热路径上不调函数**：环形缓冲整段拿掉（TX 期间的字节照旧进环，
 *        但等待期间不再从环里取；见下面的收尾清理）；捕获改"原样存"。
 *     ③ **映射/拷贝挪到收尾**：'\r''\n'→空格、不可打印→'.' 只在结束时
 *        对整段做一遍（O(n) 只做一次，不在每个字节上做）。
 *
 *   计时也一并降频：`loops` 是 32 位 xdata，每圈 ++ 要 6 次 xdata 访问，
 *   比解析一个字节还贵。改成 u8 `spin` 自增，回绕（每 256 圈）才做一次
 *   32 位运算 + 喂狗。256 圈 ≈ 1~2 ms，对最短 500 ms 的超时完全够。
 *============================================================================*/
static void at_wait_scan(void)
{
    static u32 xdata t0;
    static u32 xdata loops;      /* 兜底计数：万一 Timer0 没在跑也不能死循环 */
    static u32 xdata lim;
    static u8  xdata c;
    static u8  xdata spin;       /* ★ u8：只在回绕时才动 32 位的 loops */
    static u16 xdata n16;        /* 捕获写入位置（局部 u16，别用 s_cap_n 反复读） */

    s_abort    = 0U;
    s_cap_n    = 0U;
    s_line_len = 0U;
    s_saw_ok   = 0U;      /* ★ v0.6.0 宽松判据的证据位，每次等待清零 */
    s_saw_err  = 0U;
    s_last_pr  = 0U;      /* ★ v0.6.2 尾巴判据 */
    s_prev     = 0U;
    s_prev2    = 0U;
    if (s_w_plen != 0) *s_w_plen = 0U;

    /* ★★ v0.53：进命令前先把"上一条命令的尾巴"倒掉（硬件里挂着的那 1 个字节
     *   也要读掉，否则它会当成本条命令的第一个字节）。 */
    s_tail = s_head;
    while (at_uart2_getc(&c)) { }

    if (s_w_cmd != 0)
    {
        /* ★ v0.6.11：命令来自 g_at_tx 时走**无形参**的 at_send_cur()，
         *   彻底不经过"指针当形参"这一步（见文件里那段病根说明）。 */
        s_sr = (s_use_cur != 0U) ? (u8)at_send_cur()
                                 : (u8)at_send_cmd((const char *)s_w_cmd);
        if (s_sr != (u8)OTA_OK)
        {
            s_w_res = (u8)AT_RESP_ERROR;
            return;
        }
    }

    /* ★ 兜底上限：一圈约 2~5 us，取 500 圈/ms ⇒ lim 圈 ≈ 0.5~1.2 倍名义超时。
     *   正常情况下 s_at_ms 会先到；只有 Timer0 真没跑时才会撞到这里。 */
    lim   = (u32)s_w_to * 500UL;
    loops = 0UL;
    spin  = 0U;
    t0    = s_at_ms;

    for (;;)
    {
        /*----------------------------------------------------------------------
         * ① 抢硬件：全部内联，一个函数都不调
         *--------------------------------------------------------------------*/
        if (ATR_RI)
        {
            ATR_RI  = 0;
            c       = ATR_BUF;          /* ★ 先取走，后面再慢也不会丢这一个 */

            /*------------------------------------------------------------------
             * ★★★ v0.6.17：数据体直通（放在最前，路径最短）
             *   不写捕获、不做行解析、不判 OK/ERROR —— 写一个字节、计数、
             *   比较，然后回循环顶部继续抢下一个。写满立刻返回，
             *   尾部剩下的 \r\nOK\r\n 由下一条命令开头的清场倒掉。
             *----------------------------------------------------------------*/
            if (s_bin_go != 0U)
            {
                /* ★ v0.6.18：应答头部有 s_bin_skip 个与数据无关的字节
                 *   （8910 在 `+HTTPREAD: <len>` 行后还多一个空行 = CRLF），
                 *   按"收满 s_bin_max 才收工、但只落盘非前导字节"处理。
                 *   这里仍是一个分支 + 两次自增，路径长度不变。 */
                if (s_bin_skip != 0U)
                {
                    s_bin_skip = (u16)(s_bin_skip - 1U);
                }
                else
                {
                    s_bin_dst[s_bin_n] = c;
                    s_bin_n = (u16)(s_bin_n + 1U);
                }
                /* ★ v0.6.19 空转探针：spin 记的是"上一个字节之后空转了几圈"，
                 *   拿它当微秒表的粗刻度（一圈约 3~5 us），回头在日志里报。
                 *   ★ 第 0 个字节那一次不计：那段空转是"模组准备数据体"的时间
                 *     （毫秒级，必然饱和 255），算进来会把真正的停顿盖掉。
                 *     要量的是**数据体内部**有没有停顿，所以从第 2 个字节起记。 */
                if (s_bin_n >= 2U)
                {
                    if (spin > s_gap)
                    {
                        s_gap    = spin;
                        s_gap_at = s_bin_n;
                    }
                }
                else
                {
                    s_gap0 = spin;      /* 前缀行 → 数据体首字节 的空转（饱和 255） */
                }
                spin = 0U;
                s_bin_got = (u16)(s_bin_got + 1U);
                if (s_bin_got >= s_bin_max)
                {
                    s_w_res = (u8)AT_RESP_PREFIX;
                    return;
                }
            }
            else
            {

            n16 = s_cap_n;              /* 捕获：原样存，映射留到收尾 */
            if (n16 < (AT_CAP_MAX - 2U))
            {
                g_at_cap[n16] = (char)c;
                s_cap_n = (u16)(n16 + 1U);
            }

            /* 子串判据（丢字节只会删字符，不会凭空造出相邻的 O K） */
            if (c == (u8)'K' && s_prev == (u8)'O')
            {
                s_saw_ok = 1U;
                if (s_lenient != 0U && s_w_expect == 0)
                {
                    s_w_res = (u8)AT_RESP_OK;
                    return;
                }
            }
            if (c == (u8)'R' && s_prev == (u8)'R' && s_prev2 == (u8)'E')
            {
                s_saw_err = 1U;
            }
            if (c >= 0x20U && c < 0x7FU) s_last_pr = c;   /* 尾巴判据 */
            s_prev2 = s_prev;
            s_prev  = c;

            if (c == (u8)'\n')
            {
                /*------------------------------------------------------------------
                 * ★★★ v0.6.19：HTTPREAD 前缀**就地比较**，不再进 at_try_line
                 *
                 *   这一行是整个下载唯一的时间敏感点：从这里的最后一个字节
                 *   （行尾 \n）到"第一个数据体字节被取走"，中间做的事情必须
                 *   远小于 87 us（115200 下一个字节的时间），否则那几微秒里
                 *   到达的字节当场被硬件覆盖，`ovf` 都不计。
                 *
                 *   老路径：LCALL at_try_line → line_has_prefix（**形参是
                 *   `const char *`，循环里 prefix[i] 走通用指针下标**）→
                 *   再 4 次 xdata 写。step69 的数据体第 0 字节之后少了 2 个字节，
                 *   就是这类"交接太慢"的典型形貌。
                 *
                 *   新路径：10 次**常数下标**的 xdata 比较 + 赋值，无调用、无指针。
                 *   命中就直接开工，同一条 for 循环继续抢字节。
                 *   非命中仍走老逻辑（ERROR/+CME 判据原样保留）。
                 *----------------------------------------------------------------*/
                if ((s_w_is_read != 0U) && (s_line_len >= 10U) &&
                    (s_line[0] == (char)'+') && (s_line[1] == (char)'H') &&
                    (s_line[2] == (char)'T') && (s_line[3] == (char)'T') &&
                    (s_line[4] == (char)'P') && (s_line[5] == (char)'R') &&
                    (s_line[6] == (char)'E') && (s_line[7] == (char)'A') &&
                    (s_line[8] == (char)'D') && (s_line[9] == (char)':'))
                {
                    s_w_res   = (u8)AT_RESP_PREFIX;
                    s_bin_n   = 0U;
                    s_bin_got = 0U;
                    s_bin_go  = 1U;
                    s_gap     = 0U;
                    s_gap_at  = 0U;
                    s_gap0    = 0U;
                    spin      = 0U;
                    s_line_len = 0U;
                    /* ★ 不 return：继续抢下一个字节 */
                }
                else
                {
                    if (at_try_line()) return;      /* 只在行尾调用，频率极低 */
                    s_line_len = 0U;
                }
            }
            else if (c != (u8)'\r')
            {
                if (s_line_len < (AT_LINE_MAX - 1U)) s_line[s_line_len++] = c;
                else s_line_len = 0U;           /* 超长行整段丢 */
            }

            }   /* end else：非数据体（命令应答扫描） */
        }

        /*----------------------------------------------------------------------
         * ② 时基：每圈推一次（只有一条位测试的开销），终止判定每 256 圈一次
         *--------------------------------------------------------------------*/
        at_t0_poll();
        spin++;
        if (spin == 0U)
        {
            loops += 256UL;
            if (s_abort)
            {
                s_w_res = (u8)AT_RESP_ABORT;
                return;
            }
            if ((u32)(s_at_ms - t0) >= (u32)s_w_to)
            {
                s_w_res = at_lenient_verdict();
                return;
            }
            at_wdt_tick();
            if (loops >= lim)
            {
                s_w_res = at_lenient_verdict();   /* Timer0 疑似没跑，硬超时 */
                return;
            }
        }
    }
}

/*==============================================================================
 * 收尾：把捕获缓冲"原样字节"映射成可读文本 + 给老接口回填调用方缓冲
 *
 *   ★ 为什么映射必须挪到这里：cap_put() 在热路径上每个字节都要判 4 个条件、
 *     写 1 个 xdata，是丢掉那 9 个字节的主要成本之一。挪到收尾，整段只做一次。
 *   ★ 为什么收尾还要 s_tail = s_head：TX 期间 at_uart2_putc() 会顺手把回显
 *     塞进环形缓冲（那是"防止发送时覆盖硬件"的必要动作），等待期间我们不再
 *     从环里取，所以这些字节必须在这里丢掉 —— 否则它们会漏进下一次
 *     at_binary_read()，变成镜像里的垃圾字节（v0.6.7 就是这么坏片的）。
 *============================================================================*/
static void at_wait_post(void)
{
    static u16 xdata i;
    static u16 xdata n;
    static u8  xdata ch;

    n = s_cap_n;
    for (i = 0U; i < n; i++)
    {
        ch = (u8)g_at_cap[i];
        g_at_cap[i] = (char)((ch == (u8)'\r' || ch == (u8)'\n') ? ' '
                            : ((ch >= 0x20U && ch < 0x7FU) ? ch : '.'));
    }

    /* 老接口（at_cmd_wait 带自己的缓冲）在收尾处拷一次；新接口直接用 g_at_cap */
    if (s_w_resp != 0 && s_w_resp != g_at_cap && s_w_cap > 1U)
    {
        i = 0U;
        while (i < n && i < (u16)(s_w_cap - 1U))
        {
            s_w_resp[i] = g_at_cap[i];
            i++;
        }
    }

    s_tail = s_head;          /* 丢掉 TX 期间塞进环的回显，见上面第 2 段 */
}

/* 核心：无形参，全读 static —— 铁律 9/14 的正解 */
static void at_wait_core(void)
{
    /* ★ v0.7.0：进出都置位/清零 busy 与拼行长度。
     *   命令期间字节归 at_wait_scan 管 ⇒ at_pump 必须让路（否则两边同时读
     *   同一个 ATR_BUF，谁都凑不齐一整行）；命令进出都丢掉半个旧行，免得
     *   命令之后的新字节被拼到命令之前的残行上，产出永远不匹配的垃圾行。 */
    s_at_busy = 1U;
    s_ul_n    = 0U;

    at_wait_scan();
    at_wait_post();

    s_ul_n    = 0U;
    s_at_busy = 0U;
}


/*==============================================================================
 * ★★★ v0.6.11：零指针发送 + 等待 + 捕获（三个形参一共 3 字节，只走寄存器）
 *
 *   exp_id 现拼到 s_ebuf（立即数），命令取 g_at_tx（全局常量地址），
 *   捕获写 g_at_cap（同样全局）⇒ 整条通路**没有任何指针跨调用**。
 *   调用方拿到结果后直接用 g_at_cap / g_at_capn，不需要再传缓冲。
 *============================================================================*/
/*==============================================================================
 * ★★★ v0.7.0：前缀现拼（从 at_tx_mac 里抽出来，给 at_urc_wait 共用）
 *
 *   只做一件事：按 exp_id 把前缀逐字节写进 s_ebuf（常数下标、无指针），
 *   然后把 s_w_expect 指到它。抽成函数不多花一个字节的栈 —— 无形参溢出槽
 *   （exp_id 只有 1 B，走寄存器）。
 *============================================================================*/
static void at_build_exp(u8 exp_id)
{
    static u8 xdata i;

    s_w_expect  = 0;
    s_w_is_read = 0U;              /* ★ v0.6.17：只有 HTTPREAD 会置 1 */

    if (exp_id == AT_EXP_NONE) return;

    i = 0U;
    if (exp_id == AT_EXP_HTTPACTION)
    {
        s_ebuf[i]=  '+'; i++; s_ebuf[i]='H'; i++; s_ebuf[i]='T'; i++;
        s_ebuf[i]='T'; i++; s_ebuf[i]='P'; i++; s_ebuf[i]='A'; i++;
        s_ebuf[i]='C'; i++; s_ebuf[i]='T'; i++; s_ebuf[i]='I'; i++;
        s_ebuf[i]='O'; i++; s_ebuf[i]='N'; i++; s_ebuf[i]=':'; i++;
    }
    else if (exp_id == AT_EXP_HTTPREAD)
    {
        s_ebuf[i]=  '+'; i++; s_ebuf[i]='H'; i++; s_ebuf[i]='T'; i++;
        s_ebuf[i]='T'; i++; s_ebuf[i]='P'; i++; s_ebuf[i]='R'; i++;
        s_ebuf[i]='E'; i++; s_ebuf[i]='A'; i++; s_ebuf[i]='D'; i++;
        s_ebuf[i]=':'; i++;
        s_w_is_read = 1U;      /* ★ v0.6.17：前缀命中即接管数据体 */
    }
    else if (exp_id == AT_EXP_SAPBR)
    {
        s_ebuf[i]=  '+'; i++; s_ebuf[i]='S'; i++; s_ebuf[i]='A'; i++;
        s_ebuf[i]='P'; i++; s_ebuf[i]='B'; i++; s_ebuf[i]='R'; i++;
    }
    else if (exp_id == AT_EXP_CONNOK)
    {
        s_ebuf[i]=  'C'; i++; s_ebuf[i]='O'; i++; s_ebuf[i]='N'; i++;
        s_ebuf[i]='N'; i++; s_ebuf[i]='E'; i++; s_ebuf[i]='C'; i++;
        s_ebuf[i]='T'; i++; s_ebuf[i]=' '; i++; s_ebuf[i]='O'; i++;
        s_ebuf[i]='K'; i++;
    }
    else if (exp_id == AT_EXP_CONNACK)
    {
        s_ebuf[i]=  'C'; i++; s_ebuf[i]='O'; i++; s_ebuf[i]='N'; i++;
        s_ebuf[i]='N'; i++; s_ebuf[i]='A'; i++; s_ebuf[i]='C'; i++;
        s_ebuf[i]='K'; i++; s_ebuf[i]=' '; i++; s_ebuf[i]='O'; i++;
        s_ebuf[i]='K'; i++;
    }
    else if (exp_id == AT_EXP_MSUB)
    {
        /* 只比 5 字节：手册里 URC 写作 "+MSUB："（全角冒号），实机是 ASCII，
         * 比到 "+MSUB" 为止两种写法都命中 —— 冒号本身一个字节都不解释。 */
        s_ebuf[i]=  '+'; i++; s_ebuf[i]='M'; i++; s_ebuf[i]='S'; i++;
        s_ebuf[i]='U'; i++; s_ebuf[i]='B'; i++;
    }
    else /* AT_EXP_SUBACK */
    {
        s_ebuf[i]=  'S'; i++; s_ebuf[i]='U'; i++; s_ebuf[i]='B'; i++;
        s_ebuf[i]='A'; i++; s_ebuf[i]='C'; i++; s_ebuf[i]='K'; i++;
    }

    s_ebuf[i] = 0;
    s_w_expect = (const char *)s_ebuf;
}

u8 at_tx_mac(u8 exp_id, u16 to_ms)
{
    at_build_exp(exp_id);

    s_w_cmd   = (const char *)g_at_tx;
    s_w_to    = to_ms;
    s_w_resp  = g_at_cap;
    s_w_cap   = AT_CAP_MAX;
    s_w_plen  = 0;
    s_use_cur = 1U;

    at_wait_core();

    s_use_cur = 0U;
    g_at_capn = s_cap_n;
    g_at_cap[(s_cap_n < (AT_CAP_MAX - 1U)) ? s_cap_n : (AT_CAP_MAX - 1U)] = 0;
    return s_w_res;
}

/*==============================================================================
 * ★★★ v0.7.0：业务层（MQTT）用的三件工具
 *============================================================================*/

/* 自由运行毫秒（顺手把 TF0 溢出记一次账，不丢时间） */
u32 at_ms(void)
{
    at_t0_poll();
    return s_at_ms;
}

/*==============================================================================
 * 非阻塞 URC 泵
 *
 *  ── 为什么不能用 at_wait_scan 收 URC ───────────────────────────────────
 *  它一进函数就 `s_line_len = 0; s_cap_n = 0`，出来又把环倒空（s_tail=s_head）。
 *  而一条 `+MSUB: fw/0001/ver,28 byte,0.7.1|https://...` 在 115200 下要
 *  5 ms 才收完。想要"非阻塞"，就得把窗口切到 1~2 ms 反复调用 —— 那正好把
 *  每条 URC 撕成永远凑不齐的两半。所以 URC 必须有自己的一条通道：
 *  只累加、不清零、不等待，凑到行尾才判一次前缀。
 *
 *  ── 它绝不抢命令的字节 ────────────────────────────────────────────────
 *  s_at_busy 由 at_wait_core 置位：命令在跑时直接 return，让 at_wait_scan
 *  独占 ATR_BUF。代价是"命令期间到达的 URC 会被当成普通行吃掉"——
 *  所以服务器的版本消息必须用 retain（新订阅即补发），见 docs/APP_MQTT.md。
 *============================================================================*/
u8 at_pump(void)
{
    /*--------------------------------------------------------------------------
     * ★★★ v0.7.8：这一段的每个变量都**不许留在 xdata**
     *
     *   115200 下一字节只有 87 us，而 C251 上一次 xdata 读改写要 20~30 us。
     *   原实现每个字节都要做两回：`g_pump_b++`（u16 xdata 自增）和
     *   `s_ul[s_ul_n++] = c`（读 s_ul_n、写数组、再写回 s_ul_n）——
     *   合计 60 us 起，加上比较分支必然越过 87 us ⇒ **每字节都可能来不及取**。
     *
     *   这样丢掉的字节不会少一个数（b 照涨、l 照涨），只会**内容被写坏**：
     *   `0.7.4` 里的 `4` 被后一个字节顶掉，`g_urc` 里就出现 `0.7..|https://…`。
     *   而长度字段 `32 byte` 是模组按真实长度给的 ⇒ 长度校验照过，
     *   解析把 `patch` 读成 0 ⇒ 现场表现就是"消息收到了、版本号永远 0.7.0"。
     *
     *   改法（与 v0.6.13 定案的"接收热路径不许慢"同一条纪律）：
     *     c / k / n / got 全部用**局部变量**（寄存器），循环体内只留
     *     `s_ul[n++] = c` 这一次 xdata 写；`g_pump_b` 改成循环外一次性累加。
     *------------------------------------------------------------------------*/
    u8  c;                 /* 当前字节（寄存器）             */
    u16 k;                 /* g_urc 写入下标                 */
    u8  n;                 /* s_ul_n 的镜像：循环内零回写    */
    u16 got;               /* 本次进函数一共取到几个字节     */

    /* 命令在跑：让路（at_wait_scan 独占 ATR_BUF）。
     * ★ 一定要回 0 而不是"假装收过"：主循环拿这个返回值当"线路还有没有
     *   在动"的判据，回 1 会让静默计数永远清零、内层循环永远出不来。 */
    if (s_at_busy != 0U) return 0U;

    /*--------------------------------------------------------------------------
     * ★★★ v0.8.0：无在途字节就直接返回
     *
     *   主循环一圈只给这一个函数 20~30 us，而下面末尾那两句（`s_ul_n = n`
     *   是 xdata 写、`g_pump_b` 比较）在"什么都没来"的时候占了将近一半。
     *   把这三次 xdata 访问省掉，主循环的周期就再短一截 —— 周期越短，
     *   硬件那 1 个接收寄存器被覆盖的机会越少（这才是丢字节的真正来源）。
     *------------------------------------------------------------------------*/
    if (!ATR_RI) return 0U;

    got = 0U;
    n   = s_ul_n;

    while (ATR_RI)
    {
        ATR_RI = 0;
        c = ATR_BUF;
        got++;

        /*--------------------------------------------------------------------------
         * ★★★ v0.7.5：删掉"未取就丢"的门禁
         *
         *   原实现是：
         *       if (s_urc_pend != 0U) continue;      // 上一条没人取：只倒字节
         *
         *   意图是"不许覆盖未读的 URC"。代价极大：只要业务层有一圈没来得及取
         *   （状态机正在重连、或 pend 被某条路径卡住），**之后所有 URC 全部
         *   被倒掉**，而且不留任何痕迹 —— 现场表现就是"服务器消息收到了、
         *   设备全程没反应、日志一个字都不多"，正是 v0.7.3 这次的症状。
         *
         *   URC 是"最新状态"语义：版本消息靠 retain 重发，payload 里带完整
         *   URL，覆盖旧的一条不会有任何损失；反过来"丢光"是致命的。
         *   所以这里改成：照常拼行，命中就把 g_urc 覆盖成最新一条并重新置位。
         *------------------------------------------------------------------------*/
        if (c == (u8)'\n')
        {
            g_pump_l++;

            /*------------------------------------------------------------------
             * ★★★ v0.8.0：**整行无条件留档**，只有前缀命中才置 pending
             *
             *   原来这段拷贝写在 `if (前缀命中)` 里面。于是现场看到的
             *   `urc b=270 l=11 m=0` 只能推导出"11 行都收到了、没有一行的
             *   头是 +MSUB"，**看不到那 11 行到底是什么**，只能继续猜。
             *   现在把拷贝提到外面：g_urc 永远是"最后一条够长的行"，
             *   心跳日志打一次 `urc [%s]` 就能一眼看到线上到底来了什么
             *   （头被吃掉 / 中间少字符 / 全是垃圾，三种形态一眼分家）。
             *
             *   ★ 代价：每行一次最多 127 字节的 xdata→xdata 拷贝（约 100 us）。
             *     一行 62 字节的 +MSUB 在 115200 下本来就占 5.4 ms，
             *     这点时间不影响任何东西；而它换掉的是"再猜一轮"。
             *
             *   ★ `s_urc_pend == 0U` 这个门是**保护**而不是丢弃：业务层每圈
             *     都会 at_urc_take()（实际窗口 < 1 ms），只有"已经有一条没取走"
             *     时才跳过覆盖 —— 否则 g_urc 会在 pend 还在的时候被下一行的
             *     内容替掉，mqtt_parse_ver() 就会解析到别人的行。
             *     这条门不影响取证：m 一直是 0 的情况下 pend 永远不置位。
             *----------------------------------------------------------------*/
            if ((n >= 5U) && (s_urc_pend == 0U))
            {
                k = 0U;
                while (k < n && k < (AT_URC_MAX - 1U))
                {
                    c = s_ul[k];
                    g_urc[k] = (char)(((c >= 0x20U) && (c < 0x7FU)) ? c : (u8)'.');
                    k++;
                }
                g_urc[k] = 0;
                g_urcn   = k;

                if ((g_urc[0] == (char)'+') && (g_urc[1] == (char)'M') &&
                    (g_urc[2] == (char)'S') && (g_urc[3] == (char)'U') &&
                    (g_urc[4] == (char)'B'))
                {
                    g_pump_m++;
                    s_urc_pend = 1U;
                }
                else
                {
                    /*----------------------------------------------------------
                     * ★★★ v0.8.2：行头被吃掉也要交上去
                     *
                     *   现场（v0.8.0，18:08:26）：桥接侧是完整的一条
                     *       +MSUB: "fw/0001/ver",32 byte,"0.7.9|https://…"
                     *   而 MCU 侧留档的是
                     *       +MSUB "fw/000/ver,32byte,"0..9|htps://kapa.lol/R6ky
                     *   —— 头**在**，行内每 4~10 字节掉一个（61 收 51）。
                     *   掉一个字节就够让 `+MSUB:` 变成 `+MSUB `，
                     *   于是"前缀不匹配 ⇒ 不置 pending ⇒ 永远不解析"。
                     *
                     *   判据换成"这一行里有没有 '|'"：业务版本消息的载荷
                     *   必然是 `<M>.<m>.<p>|<url>`，而且 mqtt_parse_ver()
                     *   本来就是从**最后一个逗号**往右取载荷、再用 `N byte`
                     *   校验长度 —— 行头丢没丢它根本不关心。
                     *   这样"丢头"从致命退化成"局部位错"，长度校验照样挡得住。
                     *--------------------------------------------------------*/
                    k = 0U;
                    while (k < g_urcn)
                    {
                        if (g_urc[k] == (char)'|') { s_urc_pend = 1U; break; }
                        k++;
                    }
                }
            }
            n = 0U;
        }
        else if (c != (u8)'\r')
        {
            if (n < (AT_URC_MAX - 1U)) s_ul[n++] = c;
            else                       n = 0U;   /* 超长行整段丢 */
        }
    }

    s_ul_n = n;
    if (got != 0U) g_pump_b = (u16)(g_pump_b + got);

    /* ★ v0.8.2：把"这一圈有没有真从 SFR 取到字节"交给主循环。
     *   主循环据此判"线路是不是还在动" —— 只要在动就不许离开接收循环。
     *   （以前信息只往 xdata 里堆，主循环看不见，只能按"干了哪些活"猜。） */
    return (u8)((got != 0U) ? 1U : 0U);
}

u8 at_urc_take(void)
{
    if (s_urc_pend == 0U) return 0U;
    s_urc_pend = 0U;
    g_urc_take_n++;
    return 1U;
}

/* 只听不说地等一个前缀 —— ★ v0.8.0 删除 at_urc_wait / at_wait_prefix /
 * at_arm_urc / at_wait_urc 四条：全工程 0 调用点。
 * 现在"等一条 URC"只有一条路：稳态 at_pump + at_urc_take（app_mqtt.c），
 * 建链期的两段式应答（CONNECT OK / CONNACK OK / SUBACK）走 at_tx_mac 的
 * exp 前缀 —— 它们本来就都在 at_wait_scan 的同一个扫描循环里命中，
 * 另开一条"只听不说"的等待器从来没被用上，留着只会让人以为有两条路。 */


at_resp_t at_cmd_wait(const char *cmd,
                      const char *expect,
                      u16 timeout_ms,
                      char *resp,
                      u16 resp_size,
                      u16 *p_resp_len)
{
    /* ★ 形参只在此处读一遍，之后全走 static */
    s_w_cmd    = cmd;
    s_w_expect = expect;
    s_w_to     = timeout_ms;
    s_w_resp   = resp;
    s_w_cap    = resp_size;
    s_w_plen   = p_resp_len;
    s_use_cur  = 0U;               /* 走调用方指针（旧通路） */

    at_wait_core();

    /* 收尾：补 '\0' + 回传捕获长度 */
    if (s_w_resp != 0 && s_w_cap > 1U)
    {
        s_w_resp[(s_cap_n < (u16)(s_w_cap - 1U)) ? s_cap_n : (u16)(s_w_cap - 1U)] = 0;
    }
    if (s_w_plen != 0) *s_w_plen = s_cap_n;

    return (at_resp_t)s_w_res;
}

/* ★ v0.8.0 删除 at_wait_prefix()（0 调用点，见上面 at_urc_wait 那段的说明） */

/*==============================================================================
 * URC 武装 / 收割 —— ★ v0.8.0 整段删除
 *
 *   注册表 + 回调这一套（at_urc_register / at_urc_unregister / at_arm_urc /
 *   at_wait_urc）自 v0.7.0 起就没有任何调用点：URC 现在只有 at_pump 一条路
 *   （它不按注册表分发，而是把整行交给业务层自己判）。留着它等于在 48 KB
 *   的代码区里养一套平行宇宙，还附带 342 B 的两张指针表。
 *============================================================================*/

void at_abort(void)
{
    s_abort = 1U;
}

/*==============================================================================
 * 二进制透传（+HTTPREAD 数据体）
 *============================================================================*/

/*==============================================================================
 * ★★★ v0.6.17：数据体接收的新入口（替代 at_binary_read 的"事后接管"）
 *
 *   调用次序**必须**是：
 *       at_binary_arm(dst, n);            // ① 先把落盘目标绑好
 *       at_tx_mac(AT_EXP_HTTPREAD, to);   // ② 再发命令并等待
 *       got = at_binary_taken();          // ③ 前缀命中后实际收到多少
 *       at_binary_disarm();
 *
 *   arm 必须在发命令**之前**，因为 at_try_line 一命中前缀就直接落盘，
 *   任何"事后"再准备缓冲的动作都晚了 —— 那正是丢 4 个字节的成因。
 *
 *   ★ dst 必须是本工程 xdata 缓冲。范围校验沿用 at_binary_read 的口径：
 *     越界宁可 arm 失败（该片重取），也不许往随机地址写一整片。
 *============================================================================*/
u8 at_binary_arm(u8 *dst, u16 max)
{
    static u32 xdata lo;

    s_bin_go  = 0U;
    s_bin_n   = 0U;
    s_bin_got = 0U;
    s_bin_skip = 0U;
    s_bin_dst = 0;
    s_bin_max = 0U;
    s_gap     = 0U;
    s_gap_at  = 0U;
    s_gap0    = 0U;

    if (dst == 0 || max == 0U) return 0U;

    lo = (u32)(u16)dst;
    if ((lo < 0x1000UL) || ((lo + (u32)max) > 0x2F00UL))
    {
        LOG_E2("AT", "BIN arm bad lo=%x n=%u", (u16)lo, max);
        return 0U;
    }

    s_bin_dst = dst;
    s_bin_max = max;
    return 1U;
}

u16 at_binary_taken(void)
{
    return s_bin_n;
}

/* ★ v0.6.19：接收期间最长空转（圈）与它出现在第几个字节之后 */
u8 at_binary_gap(void)
{
    return s_gap;
}

u8 at_binary_gap0(void)
{
    return s_gap0;
}

u16 at_binary_gap_at(void)
{
    return s_gap_at;
}

void at_binary_disarm(void)
{
    s_bin_go  = 0U;
    s_bin_dst = 0;
    s_bin_max = 0U;
    s_bin_skip = 0U;
}

/*==============================================================================
 * ★★★ v0.6.18：数据体头部无关字节数（每条 +HTTPREAD 应答都一样的定值）
 *
 *   为什么要它：8910 在 `+HTTPREAD: <len>` 这一行之后还多发一个空行（CRLF），
 *   旧路径靠"前缀命中后把环形缓冲整段倒掉"把它顺手吃掉了，反而看不出问题；
 *   v0.6.17 改成前缀一命中就落盘，于是这 2 个字节成了 s_io[0..1]，
 *   整片前移 ⇒ `header unrecognized` ⇒ total=0 ⇒ 进度恒 0%。
 *
 *   ★ 数值不是猜的：由调用方在第 0 片里用镜像魔数 "OTAI" 的位置测出来，
 *     0/2/4 都能正确处理。测出来之前一律按 0 处理（不丢任何字节）。
 *============================================================================*/
void at_binary_set_skip(u16 k)
{
    s_bin_skip = k;
}

/*==============================================================================
 * ★★★ v0.8.0 删除 at_binary_enter / at_binary_read / at_binary_exit / at_lock
 *     / at_unlock / at_is_locked
 *
 *   两条老通路，都是"曾经正确、现在没有任何调用点"：
 *
 *   ① at_binary_enter/read/exit —— v0.6.17 之前的"事后接管"数据体通道。
 *      v0.6.17 起 HTTPREAD 改走 at_binary_arm() + at_wait_scan 的前缀就地落盘
 *      （那才是零间隙写法：从判定到写 s_io 只有十几条指令，详见 at_try_line
 *      上面那段）。http_ota.c 里现在只剩 arm / taken / disarm 三个调用点。
 *      留着它的害处不只是占代码：at_poll 已删，s_binary 这个开关再没人看，
 *      一条"看起来能用、实际不接线"的老路只会误导下一次排查。
 *
 *   ② at_lock / unlock / is_locked —— 通道互斥。全工程 0 调用点：AT 通道本来
 *      就是独占的（HTTP 与 MQTT 在业务层用 MQ_ST_HOLD 交接，不是靠这把锁）。
 *
 *   合计约 550 B 代码 + 3 个 static 字节。APP 代码区只剩几十字节，
 *   这笔空间要拿去做"接收更快"，而不是养着两条死路。
 *============================================================================*/
u16 at_take_overflow(void)
{
    static u16 xdata v;
    v = s_ovf;
    s_ovf = 0U;
    return v;
}

#if (AT_DIAG_LEGACY != 0)
/*==============================================================================
 * ★★ v0.52：MCU 侧自证 —— P1.1/P1.0 短接检测
 *
 *   现场症状"12 档全 0 B + edges=0 + tx_ok=16"只有三种可能：
 *     A) 模组没开机 / 没供电                （模组自身）
 *     B) TX/RX 没交叉（模组 TXD 接在 P1.1、模组 RXD 接在 P1.0）
 *        ⇒ MCU 的 AT 全打在自己 TX 脚上，模组一个字节也收不到，
 *          模组当然不吭声 ⇒ P1.0 恒高、零跳变。**这是最常见的一种。**
 *     C) MCU 的 RXD2 通路坏了              （固件/引脚，理论上已排除）
 *
 *   要区分 C 与 A/B，只需把 P1.1 和 P1.0 用镊子/杜邦线短接一下（模组线拔掉）：
 *   本函数先驱动 P1.1 低、再驱动高，看 P1.0 是否跟随 —— 跟随即为短接。
 *   短接成立 ⇒ 把 at_ping() 跑一轮，若 `115200 -> 4 B`（自己发的 AT 被自己
 *   收到）则 MCU 侧收发**全部自证通过**，问题 100% 在模组侧或接线。
 *
 *   ★ 安全性：P1.1 会被短暂推挽驱动。模组 RXD 是高阻输入，无害；
 *     万一模组 TXD 正好接在 P1.1 上（正是 B 情形），也只是几微秒的轻微
 *     对拉，不会损坏。测完立刻 at_uart2_pin_init() 还原 UART 形态。
 *============================================================================*/
static void at_pin_settle(void)
{
    volatile u8 d;                  /* volatile：不许被优化掉 */
    for (d = 0U; d < 60U; d++) { }
}

u8 at_pin_loopback(void)
{
    static u8 xdata lo;
    static u8 xdata hi;

    /* TX 脚 -> 推挽输出；RX 脚 -> 准双向输入（锁存 1 = 弱上拉）
     * ★ v0.5.0：改用 ATR_PORT/ATR_PM0/ATR_PM1/ATR_RXD()，
     *   所以"短接哪两根"自动跟着 AT_UART_PORT 走：
     *     UART2 → 短接 P1.1 与 P1.0
     *     UART3 → 短接 P0.1 与 P0.0（或 P5.1 与 P5.0） */
    ATR_PM1 &= (u8)~0x02;  ATR_PM0 |= (u8)0x02;
    ATR_PM1 &= (u8)~0x01;  ATR_PM0 &= (u8)~0x01;
    ATR_PORT |= (u8)0x01;

    /* 1) 驱动低：RX 脚若跟着变低 ⇒ 两点短接 */
    ATR_PORT &= (u8)~0x02;
    at_pin_settle();
    lo = (ATR_RXD() == 0U) ? 1U : 0U;

    /* 2) 驱动高：必须跟着变高，排除"RX 脚被别的东西长期拉低"的假阳性 */
    ATR_PORT |= (u8)0x02;
    at_pin_settle();
    hi = (ATR_RXD() != 0U) ? 1U : 0U;

    /* 还原本通道引脚形态 + 接收状态（GPIO 测试动过 TX 脚的电平） */
    at_uart2_pin_init();
    ATR_CON = ATR_CON_INIT;
    ATR_RI  = 0;
    ATR_TI  = 0;
    (void)ATR_BUF;

    return (u8)(lo & hi);
}

/*==============================================================================
 * ★★ v0.52：轻量 ping —— 给主循环做"每 3 s 一次、带收字节数"的活体探测
 *
 *   和 at_cmd_wait("AT") 的区别：
 *     · 不碰 URC 表、不进环形缓冲解析、不改任何状态；
 *     · **回填本轮到底收到几个字节**（at_ping_bytes()），这是判"线到底有没有
 *       动静"的唯一硬指标 —— 只看"有没有 OK"会把"收到乱码"和"什么也没收到"
 *       混成同一个结论（v0.47 的"10 档全 0 B"就是这么被误读的）。
 *============================================================================*/
u8 at_ping(u16 to_ms)
{
    if (s_ready == 0U) return AT_PROBE_NONE;
    return at_probe_once(to_ms);
}

u16 at_ping_bytes(void)
{
    return s_pgot;
}

/*==============================================================================
 * ★★ v0.53：链路质量打分 —— 把"看着像乱码"变成一个有分子分母的数字
 *
 *   背景（v0.51 上板实证）：波特率确认成功、模组也回话，但应答**整字节丢失**，
 *   例如 "+CPIN: READY" 收成 "+PIN RAD"、"OK" 收成 "O"。
 *   这种"字符都对、只是少几个"的形态，与"波特率不对"（整片 '.'）是两种病：
 *     · 整片 '.'  ⇒ 波特率/时钟错，改固件有用；
 *     · 少数整字节丢 ⇒ 接收信号被打坏（线被两个输出对拉 / 损坏的 USB-TTL
 *       TX 脚挂在同一根线上 / 边沿太慢），或者接收端真的溢出。
 *   只靠肉眼看日志分不开这两种，所以这里连发 n 次 "AT"，数**干净答对的次数**：
 *     n/n   ⇒ 线干净
 *     ≥2/3  ⇒ 线勉强，偶发丢字节（生产环境会变成"偶发 OTA 失败"）
 *     <2/3  ⇒ 线坏，先修线，别改固件
 *   ★ 它只调 at_probe_once()，不碰 URC、不改波特率、不动 s_cur_baud。
 *============================================================================*/
u8 at_link_test(u8 n)
{
    static u8 xdata i;
    static u8 xdata ok;
    static u8 xdata r;

    if (s_ready == 0U) return 0U;
    if (n == 0U) return 0U;

    ok = 0U;
    for (i = 0U; i < n; i++)
    {
        r = at_probe_once(AT_BAUD_TO_MS);   /* 0=NONE 1=OK 2=RDY */
        if (r != AT_PROBE_NONE) ok++;
        at_wdt_tick();
    }
    return ok;
}

/*==============================================================================
 * ★★★ v0.6.3 降速重协商：115200 -> 9600
 *
 *   现场事实（step58）：115200 下 **模组→MCU 这一个方向只活约一半字节**，
 *   而且是"整字节没到"——收到的字符全对、帧没有错（rx 里一个 '.' 都没有，
 *   cap_put 会把非可打印字节写成 '.'）⇒ 不是波特率错、不是电平歪、不是串口坏，
 *   是那一帧根本没被识别出来。
 *
 *   一个字节占的时间：115200 → 87 us，9600 → 1042 us，**12 倍余量**。
 *   11059200/4/9600 = 288 整，分频无误差（115200 时是 24，同样整）。
 *   ⇒ 凡是"余量不足"这一类病（软件轮询跟不上、边沿太缓、驱动太弱、容性太重），
 *     降速都能一次性抹平；而如果降速后仍然丢，那就只剩硬件对拉/引脚损坏，
 *     改固件无用 —— 这个实验本身就是最好的判据。
 *
 *   ★ 顺序不能反：AT+IPR 的应答**仍走旧波特率**，所以必须"先让模组改、
 *     再让 MCU 改"。反了等于把自己关在门外。
 *   ★ 必须能退回来。9600 一个字节都听不到时，两边各灌一次 AT+IPR=115200：
 *     模组可能已经到了 9600，也可能压根没收到那条命令，两个方向都堵上，
 *     最坏情况只是"回到 115200 继续按老样子丢字节"，不会把板子锁死。
 *
 *   返回实际生效的波特率；at_baud_last_n() 给出降速后验到的字节数
 *   （0xFFFF = AT+IPR 这条本身没成功，压根没往下走）。
 *============================================================================*/
static u16 xdata s_bd_n;      /* 最近一次协商后验到多少字节（诊断用） */
static u16 xdata s_ng_n;

/* 盲发 AT+IPR=<b>（不看回执）
 *   ★ 依据（step58 实测，最强的一条方向性证据）：
 *       MCU→模组 方向 **0 丢字节** —— 模组把我们 autobaud 那 3 秒乱码原样
 *       回显，桥接侧一字不缺；丢字节只发生在 模组→MCU 这一个方向。
 *     ⇒ 凡是"只要它收到、不需要我们看到回执"的命令，都可以**盲发**。
 *       正是这条让"改波特率"从做不到变成必然可行：旧写法要先看到
 *       AT+IPR 的 OK 才算数，而 OK 恰恰是丢字节的受害者。
 *   ★ 命令在 xdata 里现拼，不用字面量取址（躲开"代码空间取址只搬 WORD0"，
 *     见 at_send_cmd 里那段 0xFF 垃圾的说明）。形参 4+2=6 B，无参数块。
 */
static void at_blind_ipr(u32 b, u8 times)
{
    static u8  xdata k;

    /* ★ v0.6.11：改走零指针通道（g_at_tx + at_send_cur），
     *   原来这里把 t 的指针传给 at_send_cmd，正是"指针跨调用被踩"的那类写法。 */
    at_tx_reset();
    at_tx_c((u8)'A'); at_tx_c((u8)'T'); at_tx_c((u8)'+'); at_tx_c((u8)'I');
    at_tx_c((u8)'P'); at_tx_c((u8)'R'); at_tx_c((u8)'=');
    at_tx_d(b);

    for (k = 0U; k < times; k++)
    {
        (void)at_send_cur();
        at_delay_ms(60U);
    }
}

/*============================================================================
 * ★★★ v0.6.4：盲切固定波特率（本版主改动）
 *
 *   为什么要"盲切"而不是"问着切"：
 *     step59 的四轮全挂已经证明 —— **发得出去、收不回来**。
 *     改波特率必须先让模组收到 AT+IPR，而"确认它收到了"依赖回执，
 *     回执正是丢字节的那个方向。旧写法因此永远卡在第一步。
 *     既然 MCU→模组 实测 0 丢字节，就**不必等回执**：发三遍、切过去、再验。
 *
 *   为什么非要降到 9600：
 *     115200 一个字节只占 87 us，9600 是 1042 us（**12 倍余量**），
 *     且 11059200/4/9600 = 288 整、无分频误差。
 *     凡是"来不及识别整帧"这一类病（CPU 来不及取、边沿太缓、驱动太弱、
 *     容性太重），降速一次性抹平；若降速后仍然丢，那就只剩硬件对拉/引脚
 *     损坏，改固件无用 —— **这个实验本身就是判据**。
 *
 *   失败必须能退回来：两边各灌一遍 AT+IPR=115200（模组可能已在 9600，
 *   也可能压根没收到），最坏只是"回到 115200 继续按老样子丢"，不会锁死。
 *
 *   ★ 全程 1 个形参（4 B）⇒ 不生成参数块（铁律 14）。
 *===========================================================================*/
u32 at_negotiate(u32 want)
{
    static u32 xdata cur;
    static u8  xdata k;
    static u8  xdata hit;

    s_ng_n = 0U;
    if (want == 0UL) want = 9600UL;

    cur = s_cur_baud;
    if (cur == 0UL) cur = 115200UL;     /* 未确认时硬件就在 115200（at_uart_init 口径） */

    if (cur != want)
    {
        at_blind_ipr(want, 3U);         /* ① 让模组先改（盲发） */
        at_delay_ms(250U);
        at_uart2_baud(want);            /* ② MCU 跟上 */
        at_delay_ms(150U);
    }

    /* ③ 验证：最多 10 次 AT。子串判据下每次约 25% ⇒ 10 次 ≈ 94% */
    hit = 0U;
    for (k = 0U; k < 10U; k++)
    {
        if (at_probe_once(AT_BAUD_TO_MS) == AT_PROBE_OK)
        {
            hit    = 1U;
            s_ng_n = s_pgot;
            break;
        }
        s_ng_n = s_pgot;
        at_delay_ms(120U);
    }

    if (hit != 0U)
    {
        s_cur_baud = want;
        at_delay_ms(150U);
        at_rx_flush();                  /* 倒掉验证期间的回显残渣 */
        s_bd_n = s_ng_n;
        return want;
    }

    /* ④ 失败：两边各灌一遍 115200，退回原档 */
    at_blind_ipr(115200UL, 2U);         /* 此时 MCU 在 want：模组若也在 want 就收到 */
    at_uart2_baud(115200UL);
    at_delay_ms(150U);
    at_blind_ipr(115200UL, 1U);         /* 模组本来就在 115200 时，这条也能收到 */
    at_delay_ms(100U);
    s_cur_baud = 0UL;
    s_bd_n    = s_ng_n;
    return 0UL;
}

/* 降速入口（保留旧名字，app_main 的调用点不用改）：失败返回原档 */
u32 at_baud_down(u32 cur)
{
    static u32 xdata r;
    r = at_negotiate(9600UL);
    return (r != 0UL) ? r : cur;
}

u16 at_baud_last_n(void)
{
    return s_bd_n;
}

u16 at_negotiate_n(void)
{
    return s_ng_n;
}
#endif /* AT_DIAG_LEGACY */
