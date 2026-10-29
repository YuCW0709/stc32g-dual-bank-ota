/*==============================================================================
 *  uart_at.h  --  UART 环形缓冲 + AT 命令引擎（目标模组：Air724UG）
 *  Platform : STC32G12K128  (Keil C251, CodeRomSize = Huge)
 *  Depends  : stc32g.h, ota_types.h
 *
 *  设计要点
 *  1) 接收走 4 KB 环形缓冲 + UART 中断逐字节入队，主循环 at_poll() 解析。
 *  2) 行模式（文本 URC / 应答）与二进制模式（+HTTPREAD 数据体）严格分离：
 *     AT+HTTPREAD 返回的数据体可能包含 0x0D/0x0A/"OK"，绝不能用行解析。
 *  3) AT 通道是独占资源：HTTP 下载与 MQTT 保活共用一路 UART，必须在命令级
 *     加互斥（at_lock/at_unlock），否则 URC 会串到别人的应答里。
 *  4) URC 采用"前缀注册 + 回调"分发，最多 AT_URC_MAX 条，回调在 at_poll()
 *     上下文（主循环）执行，不在中断里执行。
 *  5) 所有 AT 命令长度受 AT_LINE_MAX 约束；MQTT 控制面 payload 建议 <= 512 B。
 *============================================================================*/
#ifndef __UART_AT_H
#define __UART_AT_H

#include "ota_mcu.h"
#include "ota_types.h"

/*==============================================================================
 * ★★ v0.5.0：AT 通道可在 UART2 / UART3 之间切换
 *
 *   起因：现场 `edges=0 low=0` + `tx_ok=16 tx_to=0` —— MCU 发送侧自证正常，
 *   线上零跳变。P1.0 曾在调试期被另一块 USB-TTL 的 TX 推挽对拉过，怀疑引脚
 *   输入通道被打坏。UART3 走另一组引脚，可一次分清"引脚坏"与"模组不说话"。
 *
 *   AT_UART_PORT = 2 → P1.0(RXD2) / P1.1(TXD2)，S2CON / S2BUF
 *   AT_UART_PORT = 3 → P0.0(RXD3) / P0.1(TXD3)，S3CON / S3BUF（S3_S=0）
 *                      或 P5.0 / P5.1（AT_UART3_PIN_P5=1，S3_S=1）
 *
 *   ★★★ 换口时必须先看这一条，否则"改了也白改"：
 *      S2CON 与 S3CON 的 **bit6 含义完全不同**：
 *        S2CON.6 = S2SM1  → =1 表示"模式1"
 *        S3CON.6 = S3ST3  → =1 表示"波特率改用 Timer3"
 *      所以 UART2 那句 `S2CON = 0x50` **绝不能**照抄成 `S3CON = 0x50`：
 *      抄过去等于选了个**根本没配置**的 Timer3 ⇒ 波特率全错 ⇒ 一个字节都
 *      收不到，而日志现象与"引脚坏"一模一样，会把人骗回去查硬件。
 *      UART3 正确值 = 0x10（S3SM0=0 八位可变波特率 + S3ST3=0 用 Timer2 +
 *      S3REN=1 接收使能）。Timer2 已由 at_uart2_baud() 配成 115200，直接复用。
 *
 *   ★ 附带事实（已查 STC32G 手册/官方例程）：UART3 要用 Timer2 或 Timer3 做
 *     波特率发生器，UART4 用 Timer2 或 Timer4，UART1 用 Timer1 或 Timer2。
 *     四路共用同一个定时器时分频相同 ⇒ 换到 UART3 不需要重配波特率。
 *============================================================================*/
#define AT_UART_PORT        3       /* 2 = P1.0/P1.1 ; 3 = P0.0/P0.1(或 P5.0/P5.1) */
#define AT_UART3_PIN_P5     0       /* 仅 AT_UART_PORT==3 有效：0 = P0.x，1 = P5.x */

/*============================================================================
 * ★★★ AT_DIAG_LEGACY（v0.7.0）—— 老探针总闸
 *
 *   v0.47 ~ v0.6.6 为了把"物理层丢字节"这件事定案，堆了整套取证工具：
 *     at_line_diag  只数 P1.0 跳变（绕开 UART）
 *     at_listen / at_listen_port  一个字不发，纯听
 *     at_raw_at     不判对错、只把字节原样打出来
 *     at_pin_loopback / at_ping / at_link_test  环回与链路打分
 *     at_blind_ipr / at_negotiate / at_baud_down  降速重协商（已证伪，见下）
 *
 *   现在结论已经定案（v0.6.13：线是干净的，是接收热路径读得太慢），
 *   而这套工具连同它在 app_main 里的调用点一共占 **约 3.9 KB** 代码 + 0.6 KB
 *   常量，而 APP 只有 48 KB 代码区（0x14200-0x1FFFF，ECODE 与 HCONST 共用）。
 *   加上 MQTT 之后已经顶到 16 位空间尽头（ADDRESS SPACE OVERFLOW），必须腾地方。
 *
 *   ⇒ 置 0：整段不编译，源码保留（可逆，改回 1 就全回来）。
 *     保留的判据只剩真正定过案的两条，它们才是验收口径：
 *       at_rx_probe()      —— 定长应答字节数，min=max=22 即零丢字节
 *       at_take_overflow() —— 环形缓冲溢出丢弃数
 *     再加 at_sweep_best_baud()/at_sweep_best_n()（每档收到几个字节）。
 *     **不要**为了"多打几行日志"把它打开 —— 48 KB 放不下，链接会直接失败。
 *
 *   注意：app_main.c 用同一个宏控制调用点，两边必须一致（都在本头文件里定）。
 *============================================================================*/
#define AT_DIAG_LEGACY      0


/* 日志用字符串（app_main.c 也直接引用，保证日志与真实配置同源） */
#if (AT_UART_PORT == 3)
  #define AT_PORT_NAME_STR    "UART3"
  #if (AT_UART3_PIN_P5 != 0)
    #define AT_PIN_NAME_STR   "P5.0(RXD3)/P5.1(TXD3)"
  #else
    #define AT_PIN_NAME_STR   "P0.0(RXD3)/P0.1(TXD3)"
  #endif
#else
  #define AT_PORT_NAME_STR    "UART2"
  #define AT_PIN_NAME_STR     "P1.0(RXD2)/P1.1(TXD2)"
#endif

/*----------------------------- 容量配置 -------------------------------------*/
/* ★ 2KB 而非 4KB：STC32G12K128 的 xdata 只有 8KB（物理 01:0000-01:1FFF），
 *   工程把 XDATA 钉在 0x1000-0x1FFF（4KB 可用）。AT 文本应答最长不过几行，
 *   2KB 环形缓冲绰绰有余；HTTP 数据体不走这里（由 http_ota 直接轮询 UART2）。 */
#define AT_RX_BUF_SIZE      2048U   /* 接收环形缓冲，必须为 2 的幂（2 KB）        */
#define AT_TX_BUF_SIZE      512U    /* 发送暂存，必须为 2 的幂                    */
#define AT_RING_MASK        (AT_RX_BUF_SIZE - 1U)
#define AT_LINE_MAX         256U    /* 单行最大长度（含结束符），超长行丢弃       */
#define AT_URC_SLOT_MAX     8U      /* URC 前缀注册表容量（与下面的行缓冲无关）   */
#define AT_BIN_CHUNK_MAX    1024U   /* 二进制单次读取上限（配合 1KB io buf）      */

/*----------------------------- 超时缺省值（ms） ------------------------------*/
#define AT_TO_SHORT         500U    /* "AT" 探活 / 状态查询                      */
#define AT_TO_CMD           2000U   /* 普通配置命令                              */
#define AT_TO_NET           15000U  /* 注网 / PDP 激活类命令                     */
#define AT_TO_MQTT_CONN     30000U  /* AT+MCONNECT                               */
#define AT_TO_HTTP_ACTION   25000U  /* AT+HTTPACTION -> +HTTPACTION URC
                                     * ★ v0.6.5：60 s -> 25 s。step60 实测模组的
                                     *   结论 3.3 s 就回来了（602），60 s 只是把
                                     *   "丢了应答"的白等放大 18 倍：3 次重试
                                     *   就是 180 s 无进展，整轮日志只剩三行。 */
#define AT_TO_HTTP_READ     10000U  /* AT+HTTPREAD 数据体接收                     */

/*==============================================================================
 * 环形缓冲区
 *============================================================================*/
/**
 * @brief 单生产者(ISR)/单消费者(主循环) 环形缓冲。
 *        无锁实现：head 仅由 ISR 递增，tail 仅由主循环递增，索引用掩码取模。
 */
typedef struct
{
    u8 *buf;      /* 缓冲区基址，由 at_uart_init() 指定，长度 AT_RX_BUF_SIZE */
    u16  size;           /* 缓冲区长度，必须为 2 的幂，等于 AT_RX_BUF_SIZE          */
    volatile u16 head;   /* 写指针：仅 UART ISR 修改，指向下一个可写位置            */
    volatile u16 tail;   /* 读指针：仅主循环修改，指向下一个可读位置                */
    volatile u16 used;   /* 当前已用字节数（= head-tail，供快速判空/判满）           */
    volatile u16 ovf_cnt;/* 溢出丢弃字节累计计数，>0 表示历史上有数据丢失            */
    volatile u8  busy;   /* 1 = 正在被二进制读取独占，行解析需等待                  */
} uart_ring_t;

/**
 * @brief  环形缓冲初始化（清零指针与计数）。
 * @param  ring  缓冲对象，不可为 NULL。
 * @param  mem   实际存储区，长度必须为 2 的幂且 >= 2。
 * @param  size  存储区长度（字节）。
 * @return OTA_OK / OTA_ERR_PARAM（size 非 2 的幂或 mem 为 NULL）。
 */
ota_ret_t uart_ring_init(uart_ring_t *ring, u8 *mem, u16 size);

/**
 * @brief  写入 1 字节（仅 UART ISR 调用）。
 * @param  ring  缓冲对象。
 * @param  dat   待写入字节。
 * @return 1 写入成功；0 缓冲满（已丢弃并累加 ovf_cnt）。
 */
u8 uart_ring_push_isr(uart_ring_t *ring, u8 dat);

/**
 * @brief  弹出 1 字节（仅主循环调用）。
 * @param  ring   缓冲对象。
 * @param  p_dat  输出字节；不可为 NULL。
 * @return 1 成功；0 缓冲空。
 */
u8 uart_ring_pop(uart_ring_t *ring, u8 *p_dat);

/**
 * @brief  查看第 offset 个待读字节但不弹出（用于前缀匹配预判）。
 * @param  ring    缓冲对象。
 * @param  offset  相对 tail 的偏移，0 表示最早字节。
 * @return 字节值；超出可读范围返回 0xFF。
 */
u8 uart_ring_peek(uart_ring_t *ring, u16 offset);

/**
 * @brief  批量弹出。
 * @param  ring   缓冲对象。
 * @param  dst    目标缓冲区（xdata）。
 * @param  max    最多读取字节数。
 * @return 实际读取字节数（0 ~ max）。
 */
u16 uart_ring_pop_n(uart_ring_t *ring, u8 *dst, u16 max);

/**
 * @brief  丢弃全部未读数据并清溢出计数。
 * @param  ring  缓冲对象。
 * @return 被丢弃的字节数。
 */
u16 uart_ring_flush(uart_ring_t *ring);

/**
 * @brief  当前可读字节数。
 * @param  ring  缓冲对象。
 * @return 字节数。
 */
u16 uart_ring_avail(uart_ring_t *ring);

/*==============================================================================
 * 物理层
 *============================================================================*/

/**
 * @brief  初始化 UART（引脚、波特率、中断优先级）与环形缓冲。
 * @param  baud        波特率，Air780EP 建议 115200（量产可 921600，需实测丢包）。
 * @param  rx_mem      接收环形缓冲存储区，长度必须 AT_RX_BUF_SIZE。
 * @param  rx_mem_size 存储区长度。
 * @return OTA_OK / OTA_ERR_PARAM。
 * @note   中断服务函数原型由 uart_at.c 提供，中断号需与 STC32G 手册一致
 *         （UART1 通常为 interrupt 4，TODO(核对) 见 stc32g.h 中断号表）。
 */
ota_ret_t at_uart_init(u32 baud, u8 *rx_mem, u16 rx_mem_size);

/**
 * @brief  ★ 自适应波特率：逐档发 "AT\r\n"，认"整行恰好 OK"或"整行恰好 RDY"。
 * @param  p_baud  命中后回填实际档位（可为 0，不关心）。
 * @return OTA_OK / OTA_ERR_TIMEOUT（全档位不通）/ OTA_ERR_STATE（未初始化）。
 * @note   扫描顺序（12 档）：115200(默认) -> 9600 -> 38400 -> 57600 -> 19200
 *         -> 28800 -> 14400 -> 4800 -> 2400 -> 1200 -> 230400 -> 460800，
 *         单档上限 300 ms。Y100P 的自适应范围是 **1200~115200**，
 *         所以低三档（4800/2400/1200）不能省。
 *
 *         ★ 现场纪律："确认波特率之前的数据都丢弃不处理" —— 本函数内
 *             1) 入口先清 s_cur_baud（0 = 未确认）；
 *             2) 每换档位先清空软/硬接收；
 *             3) 解析时只判断"整行 == OK / RDY"，回显、开机日志、乱码一律丢弃；
 *             4) 命中 OK 要**连续两次**才采纳（防乱码碰巧凑出 OK）；
 *                命中 **RDY 则立即采纳**（RDY 只出现一次，要求两次会永远等不到）。
 *         未命中前 at_get_baud() 恒为 0，调用方据此拒绝对任何应答做解释。
 */
ota_ret_t at_autobaud(u32 *p_baud);

/** @brief 当前已确认的波特率；0 = 尚未确认（此时不许解释任何收到的数据）。 */
u32 at_get_baud(void);

/** @brief 扫描档位总数（诊断用）。 */
u8 at_baud_count(void);

/** @brief 第 idx 档的波特率值；越界返回 0（诊断用）。 */
u32 at_baud_of_idx(u8 idx);

/** @brief 第 idx 档在本轮扫描中收到的字节数（诊断：全不通时看谁有动静）。 */
u16 at_autobaud_bytes(u8 idx);

/**
 * @brief  第 idx 档是否见到过整行 "RDY"。
 * @return 1 = 见到（Air724UG 自报"波特率训练成功"）；0 = 没见到。
 * @note   判读：bytes>0 但 rdy=0 ⇒ 有信号却拼不出完整行，多半波特率差一档；
 *         rdy=1 ⇒ 该档就是正确档位，不必再等 OK。
 */
u8 at_autobaud_rdy(u8 idx);

/**
 * @brief  ★ v0.6.6 诊断：本轮扫描中「收到字节最多」的那一档波特率。
 * @return 波特率值；0 = 12 档一个字节都没收到。
 * @note   判读（这条没有解释空间）：
 *           best_baud=115200 且 best_n=10  ⇒ 收通道好的，问题在判据/丢字节；
 *           best_baud=0      且 best_n=0   ⇒ 12 档全 0 字节 ⇒ 硬件（接线/对拉/供电）。
 *         单独看 `try rc=4` 分不开这两件事，必须配这一对值。
 */
u32 at_sweep_best_baud(void);

/** @brief 上面那一档收到的字节数。 */
u16 at_sweep_best_n(void);

/** @brief 最近一次 at_probe_once() 收到的字节数（含乱码）。 */
u16 at_probe_bytes(void);

/**
 * @brief  ★ v0.6.6 裸采样：在当前硬件波特率上发一条 "AT"，把收到的字节原样取回。
 * @param  to_ms 采样窗口（建议 600）。
 * @return 收到的字节数（不可打印字节在原文里写成 '.'）。
 * @note   **不做任何判定**，也不看 s_cur_baud ⇒ autobaud 失败之后照样可用，
 *         而那正是最需要"原文证据"的时刻。原文用 at_raw_line() 取。
 */
u16 at_raw_at(u16 to_ms);

/** @brief 裸采样原文的 xdata 地址（直接喂给 %s）。 */
u32 at_raw_line(void);

/**
 * @brief  ★ 不依赖中断的毫秒延时（TF0 标志轮询，内部喂狗）。
 * @param  ms  毫秒数。
 * @note   专供"等模组启动"这类纯等待使用。**不要**在 P0 阶段改用
 *         bsp_timer_delay_ms()：那个函数依赖 Timer0 中断自增，EA=0 时会永远
 *         等不到（且因为内部喂狗，表现为卡死而非复位，更难查）。
 */
void at_delay_ms(u16 ms);

/**
 * @brief  读当前 AT 通道 RX 引脚的实时电平（UART2 → P1.0，UART3 → P0.0/P5.0）。
 * @return 0 或 1。
 * @note   UART 空闲态恒为高电平。若 at_uart_init() 之后立刻读到 0，
 *         基本可以断定：模组没上电 / 模组 TXD 没接过来 / 接到了 GND /
 *         TX-RX 没交叉。这一条比"发 AT 等超时"快得多，也更能指明问题。
 */
u8 at_uart2_rxd_level(void);

/*==============================================================================
 * ★★ 线路诊断（v0.48）：把"0 字节"拆成三个互不重叠的判据
 *
 *   诊断顺序与判读：
 *     1) at_diag_tx_to() > 0        ⇒ UART2 **时钟死**（Timer2 没跑/重装值错）
 *                                      → 固件问题，跟模组无关
 *     2) at_line_diag() 后
 *        edges == 0 && low_n == 0   ⇒ 线上全程恒高、一次跳变都没有
 *                                      → 模组没上电/没启动/TXD 没接过来（硬件）
 *        edges > 0 但仍收不到        ⇒ 收通道坏（S2REN/引脚模式）
 *============================================================================*/
void at_diag_clear(void);
u16  at_diag_tx_ok(void);
u16  at_diag_tx_to(void);
u16  at_diag_edges(void);
u16  at_diag_low(void);

/**
 * @brief  线路诊断：连发 4 次 "AT\r"，每次紧跟 150 ms 高频采样 P1.0，
 *         统计跳变次数与低电平样本数。**完全绕开 UART/波特率**。
 * @note   会覆盖 at_diag_edges()/at_diag_low() 的结果，不动 tx_ok/tx_to。
 */
void at_line_diag(void);

/**
 * @brief  ★ 纯监听：**一个字节都不发**，只数 P1.0 在 ms 毫秒内的跳变次数。
 * @param  ms  采样时长（毫秒），建议 400~600。
 * @return 跳变次数；同时刷新 at_diag_edges() / at_diag_low()。
 *
 * @note   和 at_line_diag() 问的是两件事：
 *           at_line_diag  = "我说了话它有没有回"
 *           at_listen     = "我不说话它自己会不会开口"
 *         模组上电会主动吐开机日志 ⇒ 抓到跳变就证明模组活着、TXD 线通、
 *         P1.0 输入正常；全程 0 则模组根本没在运行（没电/没按 PWRKEY/线断），
 *         与 AT 命令组、波特率、服务器全无关。
 *         只采样：不发字节、不读 S2BUF、不碰波特率闸门。
 */
u16  at_listen(u16 ms);

/**
 * @brief  ★★ 交叉监听（v0.5.0）：采样**指定**通道的 RX 引脚，与当前 AT 通道无关。
 * @param  port  2 = P1.0(RXD2)；3 = P0.0 或 P5.0(RXD3，按 AT_UART3_PIN_P5)；
 *               4 = 恒为 P5.0（UART3 备用脚，用于分辨"P0 没引出"与"线上没信号"）
 * @param  ms    采样时长（ms），建议 400~600。
 * @return 跳变次数；同时刷新 at_diag_edges() / at_diag_low()。
 *
 * @note   存在的意义：一次上电就能同时看到"两路 RX 引脚各自有没有信号"，
 *         从而把三种情况彻底分开 ——
 *           · 本口 edges=0，另一口 edges>0 ⇒ 模组在说话，**收不到的是本口引脚**
 *                                          （这就是"UART2 坏了"的直接证据）
 *           · 两路都 edges=0              ⇒ 模组根本没说话（供电/PWRKEY/TXD 断）
 *           · 两路都 edges>0              ⇒ 线没问题，问题在波特率或更上层
 *         只读电平：不配串口、不发字节、不碰 S2BUF/S3BUF。
 */
u16  at_listen_port(u8 port, u16 ms);

/**
 * @brief  发送裸字节流（不追加 \r\n，不等待应答）。
 * @param  data  待发送数据。
 * @param  len   字节数，<= AT_TX_BUF_SIZE。
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_BUSY（发送缓冲未空）。
 */

/**
 * @brief  发送 ASCII 字符串并自动追加行结束符 **CR LF（0x0D 0x0A）**。
 * @param  str  以 '\0' 结尾的命令文本（不含行结束符），长度 < AT_LINE_MAX-3。
 * @return OTA_OK / OTA_ERR_PARAM（超长）。
 * @note   v0.50 由"只发 CR"改为"CR+LF"，原因见 uart_at.c 里 at_send_cmd 上方：
 *         现场监听 P1.1 抓到的是 `41 54 0D`，AT 后面没有换行，串口助手不换行，
 *         极易被误判成"命令没发完"。Y100P 用移远 AT 固件，对 CR / CRLF 都接受，
 *         发 CRLF 是超集、零代价，还能让现场一眼看出命令确实发出去了。
 *         想回退：uart_at.c 里把 AT_EOL_SEND_LF 改成 0。
 */
ota_ret_t at_send_cmd(const char *str);

/*==============================================================================
 * ★★★ v0.6.11 零指针命令通道 —— 本工程**唯一可靠**的发命令方式
 *
 *   为什么要有它（step62 现场定案，详见 uart_at.c 里那段说明）：
 *     把"代码段字符串的指针"当形参跨函数传，在 C251 上会被编译器提前排进
 *     寄存器，中间任何一次调用都会覆盖它 ⇒ 发出去的命令变成 `ailed`、
 *     或者首字节变成 0xFF/0x87/0xA2（代码段机器码）。按调用点稳定发病。
 *   ⇒ 命令一律在 g_at_tx 里逐字节现拼、等待结果看 g_at_cap，
 *     **全程不传任何指针**。用法：
 *       at_tx_reset();
 *       at_tx_c('A'); at_tx_c('T'); ... 或者 at_tx_d(1234);
 *       r = at_tx_mac(AT_EXP_NONE, 3000);      // 返回 AT_RESP_* / 0=OK
 *       if (r == AT_RESP_PREFIX) { 读 g_at_cap[0..g_at_capn) }
 *
 *   ★ 调用方读用的两个缓冲区是**全局变量**：编译期就有地址，
 *     不经过形参，所以不存在"指针被踩"这一整类问题。
 *============================================================================*/
/* ★ v0.7.0：152 → 256。
 *   MQTT 的 `AT+MPUB="topic",0,0,"<payload>"` 把**整条消息塞在命令里**，
 *   10 个模板的 payload 最长 140 B，加上主题与语法外壳 ≈168 B ⇒ 152 装不下。
 *   加 104 B xdata 的代价由 bsp_uart 的收缓冲让出来（1024→512，见 bsp_uart.h）。
 *   ★ 别再用 MPUBEX（数据模式）：它要先等一个 '>' 提示符，而本引擎只在
 *     行尾（\n）判前缀，'>' 后面没有 \n ⇒ 必然白等一个超时。 */
#define AT_TX_MAX     256U   /* 命令缓冲（MQTT 发布 + HTTP URL + 尾巴）      */
#define AT_CAP_MAX    88U    /* 应答原文捕获缓冲（v0.7.0：72→88，见下）     */
/* ★ v0.7.1：88 → 128。
 *   `+MSUB` 的整行长度 = 6 + 1 + 主题(13) + 1 + "N byte"(≤9) + 1 + 载荷。
 *   载荷最长 MQTT_OTA_URL_MAX+8 = 88（版本号+竖线+79 字符 URL），
 *   再加上引号，整行能到 **115 字符**。
 *   v0.7.0 的 88 只够装下你现在这条短链（61 字符），
 *   一旦服务器给的是 79 字符的长链，at_pump 会因为"超长行整段丢"而
 *   **永远收不到版本消息** —— 而且日志连报错都不会有。
 *   128 留足余量；这份 xdata 由下面的注册表缩容让出来（见 AT_URC_SLOT_MAX）。
 *
 *   ★ 注意这两个宏以前**同名**（都叫 AT_URC_MAX，值分别 8U / 88U），
 *     后定义覆盖前者 ⇒ 注册表数组被撑成 88 项，
 *     LARGE 模式下两个指针数组白占约 640 B xdata。现已拆成两个名字。 */
#define AT_URC_MAX    128U   /* ★ URC 行缓冲（+MSUB: 主题 + "N byte" + 载荷）   */

extern char xdata g_at_tx[AT_TX_MAX];
extern u16  xdata g_at_txn;
extern char xdata g_at_cap[AT_CAP_MAX];
extern u16  xdata g_at_capn;

void       at_tx_reset(void);
void       at_tx_c(u8 c);
void       at_tx_d(u32 v);
ota_ret_t  at_send_cur(void);      /* 发送 g_at_tx（无形参）          */
ota_ret_t  at_send_at(void);       /* 发送裸 "AT"（无形参）           */

/* at_tx_mac / at_urc_wait 的 expect 选择（前缀现拼，不传指针）
 *
 * ★ v0.7.0 新增 4、5、6：MQTT 的全部"终态"都是**模块主动上报的 URC**，
 *   不是命令应答 —— `AT+MIPSTART` 先回 OK，几十~几百毫秒后才吐 `CONNECT OK`；
 *   `AT+MCONNECT` 同理（OK → `CONNACK OK`）。所以必须能"只听不说"地等前缀。
 *   加一个 AT_EXP_* 就只多 10~13 次常数下标赋值，不影响任何既有通路。 */
#define AT_EXP_NONE        0U
#define AT_EXP_HTTPACTION  1U
#define AT_EXP_HTTPREAD    2U
#define AT_EXP_SAPBR       3U
#define AT_EXP_CONNOK      4U    /* "CONNECT OK"    TCP 已连（MIPSTART 后）  */
#define AT_EXP_CONNACK     5U    /* "CONNACK OK"    MQTT 会话已建立          */
#define AT_EXP_MSUB        6U    /* "+MSUB"         订阅消息上报             */
#define AT_EXP_SUBACK      7U    /* "SUBACK"        订阅成功                 */

/**
 * @brief 发送 g_at_tx 并等待终态（零指针通道）。
 * @param exp_id    AT_EXP_*，选择要额外等的前缀。
 * @param to_ms     超时（ms）。
 * @return AT_RESP_* 值（0=OK 1=ERROR 2=CME 4=超时 5=命中前缀 7=残片）。
 * @note   应答原文写在 g_at_cap[0..g_at_capn)，调用方直接读，不用传缓冲。
 */
u8  at_tx_mac(u8 exp_id, u16 to_ms);

/*==============================================================================
 * ★★★ v0.7.0：业务层（MQTT）需要的三件工具
 *
 *   共同前提：本工程 APP 侧 **EA 一直是 0**（中断向量在 BL，APP 的 ISR 永远
 *   不会被执行，见 app_main.c 顶部）。所以"后台接收"这件事不存在 ——
 *   要么阻塞等，要么主动去读。下面三个函数就是这两种姿势的最小集合。
 *============================================================================*/

/**
 * @brief  自由运行的毫秒计数（TF0 轮询时基，`at_delay_ms` 用的同一个）。
 * @return 上电以来的毫秒数（32 位，约 49 天回绕一次）。
 * @note   ★ 调用它就会顺手清一次 TF0 溢出，**不会丢时间**；
 *         但它只在被调用时才走，长时间不调用会少计（最大漏 t ms）。
 *         主循环每圈都会经过，所以实际误差 < 1 ms。
 *         Timer0 由 at_uart_init() 配好；没初始化过就恒返回 0。
 */
u32 at_ms(void);

/**
 * @brief  ★ 非阻塞 URC 泵：把**已经到达**的字节收进来，凑成整行后判前缀。
 * @return ★ v0.8.2：1 = 这一次真从 SFR 取到了字节（线路还在动）；0 = 一个字节都没有
 *         （已静默）或命令在跑（让路）。主循环就靠这个值判"什么时候才能离开接收循环"。
 *         函数内部**从不等待**（`while (ATR_RI)` 只处理在途字节）。
 *
 * @note   为什么必须有它：订阅消息 `+MSUB: <topic>,<len> byte,<msg>` 是模块
 *         主动吐的，而阻塞式 `at_wait_scan` 一进去就把行缓冲清零、出来再清一遍，
 *         短窗口反复调用会把一条**跨窗口**的 URC 撕成两半（永远凑不齐）。
 *         所以另开一条"只累加、不清零、不等待"的接收通道：
 *             · 主循环每圈调一次 ⇒ 115200 下一字节 87 us，轻松跟上；
 *             · 命中 "+MSUB" 的整行抄进 g_urc，置 pending 位；
 *             · 业务层用 at_urc_take() 取走（取走才允许下一条覆盖）。
 * @note   ★ 命令在跑（at_wait_core 里）时它**直接返回** —— 那时字节归
 *         at_wait_scan 管，两边同时读同一个 ATR_BUF 会互相偷字节。
 */
u8 at_pump(void);

/**
 * @brief  取走一条已收齐的 URC 行。
 * @return 1 = 取到了（内容在 g_urc[0..g_urcn)，已映射成可读文本）；
 *         0 = 没有。
 * @note   取走后 pending 位清零，下一行才允许覆盖。
 */
u8  at_urc_take(void);

/**
 * @brief  ★ 只听不说地等一个 URC 前缀（零指针：前缀按 exp_id 现拼）。
 * @param  exp_id  AT_EXP_CONNOK / AT_EXP_CONNACK / AT_EXP_MSUB / AT_EXP_SUBACK
 * @param  to_ms   超时（ms）
 * @return AT_RESP_*（5 = 命中前缀，内容在 g_urc；4 = 超时；1/2 = 模块拒绝）
 * @note   与 at_tx_mac 的唯一区别是**不发命令** ⇒ 不会打扰模块状态。
 *         用于"发完 MIPSTART 等 CONNECT OK"这类两段式应答。
 */

extern char xdata g_urc[AT_URC_MAX];
extern u16  xdata g_urcn;

/*==============================================================================
 * ★★★ v0.7.5：URC 通路自证计数器（只增不减，业务层只读）
 *
 *   为什么必须加（2026-10-28 现场）：
 *     服务器 retain 的 `0.7.4|https://kappa.lol/Ri6kyo` 在桥接日志里
 *     清清楚楚出现了 3 次，而 MCU 侧一条 URC 相关日志都没有 ——
 *     连 `urc len %u/%u drop`、`urc payload not a version` 都没打。
 *     这说明 **parse 根本没被调用**，而不是解析判错。
 *     光看"没有反应"分不清下面 5 种情况，这 4 个数一排就能定案：
 *
 *       g_pump_b=0                 → at_pump 一个字节都没读到（SFR/调用问题）
 *       g_pump_b>0, g_pump_l=0     → 收到字节但永远凑不齐行（行被撕/缓冲坏）
 *       g_pump_l>0, g_pump_m=0     → 行收到了但前缀不是 +MSUB（内容不对）
 *       g_pump_m>0, g_urc_take_n=0 → 拼出来了但业务层没取（状态机没到 RUN）
 *       g_urc_take_n>0 却无 srv 行 → 取到了，问题在 mqtt_parse_ver
 *============================================================================*/
extern u16 xdata g_pump_b;      /* at_pump 从 SFR 读到的字节数           */
extern u8  xdata g_pump_l;      /* at_pump 遇到的 '\n'（=完整行数）      */
extern u8  xdata g_pump_m;      /* 其中前缀命中 "+MSUB" 的行数           */
extern u8  xdata g_urc_take_n;  /* at_urc_take() 成功返回 1 的次数       */

/**
 * @brief 最近一次 at_wait_core 里"连续空转圈数折算成的微秒数"。
 * @note  ★ v0.6.12 更正口径：这个数**不是**"固件漏读"的证据。
 *        它把"模组还没开始应答"的那段沉默也算进去了（gap 只在收到字节时清零），
 *        而模组的应答延迟本来就是毫秒级，所以它必然远大于 87 us，
 *        与固件快慢无关。**不要再拿它判"固件太慢"。**
 *        判"固件是否漏读"只看两样：`at_take_overflow()`（环满丢字节）
 *        和 `at_rx_probe()`（定长应答的实际字节数）。
 */
u16 at_poll_gap_us(void);

/*==============================================================================
 * ★★★ v0.6.12：接收保真度实测 —— 本工程第一个"能把字节数数清"的判据
 *
 *   八轮以来"丢字节到底是线路还是固件"全靠形态猜。这里换一条不需要推理的路：
 *   挑一条**应答长度完全固定**的命令，把"收到多少字节"直接数出来。
 *
 *       ATE0 下  AT+IPR?  ->  "\r\n+IPR: 115200\r\n\r\nOK\r\n"   = 22 B
 *       ATE1 下  （带回显）->                          再 + 8 B   = 30 B
 *
 *   判据（没有解释空间）：
 *       n == 22 或 30        ⇒ 这条链路上一个字节都没丢
 *       n <  22              ⇒ 丢了 22-n 个字节，**线路/驱动丢的**，
 *                              （固件侧只有环满才会丢，见 at_take_overflow）
 *   连打 tries 次，"全对次数 / tries"就是一个比例，而不是印象。
 *
 *   ★ 为什么不复用 at_wait_core：宽松判据见到 'O''K' 就提前返回，
 *     根本等不到第 22 个字节 ⇒ 那套解析在这里必须绕开，只数不解。
 *
 * @param tries  尝试次数（1..20）
 * @note  结果放在 g_rxp_ok / g_rxp_min / g_rxp_max，不占形参（铁律 14）。
 *        次数的"全对数"另有一行日志，不必自己读变量。
 */
extern u16 xdata g_rxp_ok;      /* 字节数完全正确的次数              */
extern u16 xdata g_rxp_min;     /* 各次里的最小字节数（0xFFFF = 未测）*/
extern u16 xdata g_rxp_max;     /* 各次里的最大字节数                */
void at_rx_probe(u8 tries);

/*==============================================================================
 * 应答等待
 *============================================================================*/

/* 应答匹配结果 */
typedef enum
{
    AT_RESP_OK      = 0,   /* 收到 "OK"                                     */
    AT_RESP_ERROR   = 1,   /* 收到 "ERROR"                                  */
    AT_RESP_CME     = 2,   /* 收到 "+CME ERROR:"                            */
    AT_RESP_CMS     = 3,   /* 收到 "+CMS ERROR:"                            */
    AT_RESP_TIMEOUT = 4,   /* 超时未匹配                                    */
    AT_RESP_PREFIX  = 5,   /* 命中调用方指定的前缀（如 "+HTTPACTION:"）      */
    AT_RESP_ABORT   = 6,   /* 被 at_abort() 中止                            */
    AT_RESP_PART    = 7    /* ★ 宽松判据：超时但收到了 OK 的残片（见 at_set_lenient）*/
} at_resp_t;

/**
 * @brief  发送命令并等待终态应答。
 * @param  cmd        命令文本（不含 \r\n），NULL 表示只等待不应答发送。
 * @param  expect     额外期望的前缀（如 "+HTTPACTION:"）；NULL 表示只等 OK/ERROR。
 * @param  timeout_ms 总超时（ms），建议用 AT_TO_* 宏。
 * @param  resp       捕获缓冲区，用于回传命中行之后的全部文本（xdata）；可为 NULL。
 * @param  resp_size  resp 容量。
 * @param  p_resp_len 回传 resp 实际长度；可为 NULL。
 * @return AT_RESP_* 结果码。调用方必须区分 OK/ERROR/TIMEOUT。
 * @note   本函数内部持 AT 互斥锁；不可在 URC 回调中调用（会死锁，返回
 *         OTA_ERR_BUSY 由上层处理）。
 */
at_resp_t at_cmd_wait(const char *cmd,
                      const char *expect,
                      u16 timeout_ms,
                      char *resp,
                      u16 resp_size,
                      u16 *p_resp_len);

/* ★ v0.8.0 删除 at_wait_prefix / at_arm_urc / at_wait_urc：
 *   全工程 0 调用点。稳态收 URC 只有 at_pump + at_urc_take；
 *   建链期的两段式应答（CONNECT OK / CONNACK OK / SUBACK）走
 *   at_tx_mac() 的 exp 前缀 —— 它们本来就在同一个扇扫循环里命中。 */

/**
 * @brief  中止当前所有等待（用于超时兜底、异常复位），使阻塞中的等待立即返回。
 * @return 无。
 */
void at_abort(void);

/**
 * @brief  宽松判据开关（默认关）。
 *
 * @note   为什么需要（v0.6.0/0.6.1 上板证据）：现场那条 MCU 侧接收线会**成片吞掉
 *         整字节**（同一批数据在 USB-TTL 侧完整，在 MCU 的 rx 缓冲里缺一大截），
 *         于是 `OK` 经常收成 `O` 或 `K`，更致命的是**行尾 CR/LF 被吃掉**，
 *         `at_try_line()` 永远等不到 '\n' ⇒ 整行丢弃 ⇒ 明明收到了也判超时。
 *         开启后：不等行尾，直接在字节流里找相邻的 `O`+`K` 即判成功；
 *         超时若从未见到 "ERR" 且见到过 `OK`，返回 AT_RESP_PART。
 *         丢字节只会吃掉字符、不会凭空造出 `OK`，也不会把 ERROR 变出 OK
 *         ⇒ 这个判据只会把"真成功"救回来，不会把"真失败"放过。
 *         ★ 只对 at_cmd_wait() 的 OK/ERROR 判定生效；at_wait_prefix() 的
 *           +HTTPACTION: / +HTTPREAD: 前缀匹配完全不受影响（expect != 0 时
 *           子串命中不提前返回）。
 */
void at_set_lenient(u8 on);

/*==============================================================================
 * URC 注册与分发
 *============================================================================*/

/**
 * @brief  URC 回调原型。
 * @param  line  完整 URC 行（已去掉 \r\n，以 '\0' 结尾），内容形如
 *               "+HTTPACTION: 0,206,2048"；缓冲区在回调返回后即被复用，
 *               需要留存请自行拷贝。
 * @param  len   行长（不含 '\0'）。
 * @return OTA_OK 表示已消费；非 OTA_OK 仅用于日志，不影响分发。
 */
typedef ota_ret_t (*at_urc_cb_t)(const char *line, u16 len);

/**
 * @brief  注册 URC 前缀与回调。
 * @param  prefix  前缀字符串，如 "+HTTPACTION:"、"+MSUB:"、"CLOSED"。
 * @param  cb      回调函数指针（Huge 模式下为 3 字节通用指针）。
 * @return OTA_OK / OTA_ERR_NOSPACE / OTA_ERR_PARAM（prefix 为 NULL 或过长）。
 * @note   同一前缀重复注册将覆盖旧回调；回调只允许做"置标志/存数据"，
 *         禁止在回调内再发 AT 命令。
 */

/**
 * @brief  注销前缀。
 * @param  prefix  已注册的前缀。
 * @return OTA_OK / OTA_ERR_PARAM（未注册）。
 */

/**
 * @brief  AT 引擎主循环：把环形缓冲里的字节能解析多少解析多少。
 * @return 本次处理的完整行数（0 表示无数据或数据未成行）。
 * @note   必须在主循环中以 <= 10 ms 周期调用；内部按 AT_LINE_MAX 截断超长行。
 */

/*==============================================================================
 * 二进制透传（+HTTPREAD 数据体专用）
 *============================================================================*/

/**
 * @brief  进入二进制读取模式：此后 at_poll() 不再做行解析，字节原样留在缓冲。
 * @param  expect_len  即将到来的数据体字节数（来自 "+HTTPREAD: <len>"），
 *                     仅用于日志/超时估算，可为 0 表示未知。
 * @return OTA_OK / OTA_ERR_BUSY（已有其它二进制会话）。
 * @note   进入前会丢弃缓冲中残留的 "\r\n"，避免污染数据体首字节；
 *         读取完成必须调用 at_binary_exit()。
 */

/**
 * @brief  从缓冲读取二进制数据。
 * @param  dst          目标缓冲区（xdata）。
 * @param  max          本次最多读取字节数，建议 <= AT_BIN_CHUNK_MAX。
 * @param  timeout_ms   缓冲空时的等待超时。
 * @return 实际读取字节数；0 表示超时无数据。
 */

/**
 * @brief  退出二进制模式，恢复行解析。
 * @return 无。
 */

/*==============================================================================
 * ★★★ v0.6.17：+HTTPREAD 数据体的"零间隙"接收（推荐路径）
 *
 *   老路径（at_binary_enter + at_binary_read）在前缀行命中之后才起接收，
 *   中间隔了收尾映射/范围检查等几百微秒，而硬件只有 1 个接收寄存器
 *   （115200 一字节 87 us）⇒ 数据体头部必丢几个字节。step67 实测固定丢 4 个。
 *
 *   新路径把"落盘目标"在**发命令之前**绑好，前缀行一命中就直接落盘：
 *
 *       if (!at_binary_arm(s_io, n)) { 该片重取 }
 *       r   = at_tx_mac(AT_EXP_HTTPREAD, 3000);
 *       got = at_binary_taken();
 *       at_binary_disarm();
 *
 *   ★ 顺序不能颠倒：arm 必须在 at_tx_mac 之前。
 *============================================================================*/

/**
 * @brief  绑定数据体落盘目标（必须在发 HTTPREAD 之前调用）。
 * @param  dst  xdata 缓冲（0x1000-0x2EFF 内，越界直接失败）。
 * @param  max  期望字节数（= +HTTPACTION 报的 len）。
 * @return 1 = 绑定成功；0 = 参数非法/越界。
 */
u8   at_binary_arm(u8 *dst, u16 max);

/**
 * @brief  取本次实际落入目标缓冲的字节数（前缀命中前为 0）。
 * @return 字节数；小于期望值即为短读，该片应重取。
 */
u16  at_binary_taken(void);

/**
 * @brief  解除绑定（每片收完必须调用，异常路径也要调用）。
 * @return 无。
 */
void at_binary_disarm(void);

/**
 * @brief  接收期间的最长空转（圈数，饱和 255）与它出现在第几个字节之后。
 *
 *   v0.6.19 的现场探针：`spin` 每圈自增，字节到达时清零，所以它的峰值就是
 *   "这段时间里有多久没人取硬件寄存器"。正常流式应答应当 ≤ 1 圈；
 *   若看到 30~60 圈（≈ 150~300 us）且位置在某一片的开头，就说明那一处
 *   交接有超过一个字节时间的空窗 —— 这正是"字节无声消失"的唯一形貌。
 *
 * @return at_binary_gap() = 峰值圈数；at_binary_gap_at() = 落盘到第几字节时出现。
 */
u8   at_binary_gap(void);
u16  at_binary_gap_at(void);
u8   at_binary_gap0(void);

/**
 * @brief  设置"每条数据体应答头部要丢弃的字节数"（v0.6.18）。
 *
 *   8910 在 `+HTTPREAD: <len>` 行后多发一个空行（CRLF）：老路径把它连数据
 *   一起冲掉了，新直通路径会把它当成数据 ⇒ 首片花 2 字节、头部认不出、
 *   总长恒 0 ⇒ 进度恒 0%。
 *
 *   ★ 该值必须由调用方**实测**（第 0 片里找 "OTAI" 魔数的位置），不要写死；
 *     没有实测值就调 0，绝不丢字节。
 *   ★ 必须在 at_binary_arm() 之后、at_tx_mac() 之前调用；disarm 会清零。
 *
 * @param  k  要丢弃的头部字节数（0 = 不丢）。
 * @return 无。
 */
void at_binary_set_skip(u16 k);

/*==============================================================================
 * 通道互斥（HTTP 与 MQTT 共用一路 UART）
 *============================================================================*/

/**
 * @brief  申请 AT 通道所有权。
 * @param  timeout_ms  等待超时；0 表示非阻塞尝试。
 * @return OTA_OK 已获得；OTA_ERR_BUSY 超时未获得。
 */

/**
 * @brief  释放 AT 通道所有权。
 * @return 无。
 */

/**
 * @brief  查询通道是否被占用。
 * @return 1 占用；0 空闲。
 */

/**
 * @brief  读取并清零接收溢出计数（用于健康度上报）。
 * @return 自上次调用以来丢弃的字节数。
 */
u16 at_take_overflow(void);

#endif /* __UART_AT_H */

/*==============================================================================
 * ★★ v0.52：MCU 侧自证 + 活体 ping
 *
 *   现场出现"所有档位 0 B + edges=0"时，必须先把"MCU 侧是否有病"彻底排除，
 *   否则会一直在模组和固件之间来回猜。两条判据：
 *     1) at_pin_loopback() —— 把 P1.1 与 P1.0 短接（模组线拔掉）后应为 1；
 *     2) 短接状态下 at_ping(300) 应返回 AT_PROBE_OK（收到自己发的 AT 回环，
 *        内容是 "AT"，不是 OK —— 所以更准确的判据是 at_ping_bytes() == 4）。
 *   两条都过 ⇒ MCU 的 UART2 收发无误，问题一定在模组侧或接线。
 *============================================================================*/
/** @brief at_ping() 的三种结果（与 uart_at.c 内部一致，对外也要能判）。 */
#define AT_PROBE_NONE   0U      /* 没收到整行 OK/RDY                       */
#define AT_PROBE_OK     1U      /* 收到整行 "OK"                            */
#define AT_PROBE_RDY    2U      /* 收到整行 "RDY"（波特率训练成功的铁证）    */

u8  at_pin_loopback(void);
u8  at_ping(u16 to_ms);
u16 at_ping_bytes(void);

/**
 * @brief  ★ 链路质量打分：连发 n 次 "AT"，返回干净答对（OK/RDY）的次数。
 * @param  n  1~255（建议 8）
 * @return 0 = 全不回；n = 线干净；中间值 = 偶发丢字节
 * @note   用来区分"整片 '.'（波特率错）"与"少数整字节丢（接收信号被打坏）"。
 *         前者改固件有用，后者必须先修线（常见：损坏的 USB-TTL TX 脚
 *         与模组 TXD 挂在同一根线上，或 TX/RX 接成了直连）。
 */
u8  at_link_test(u8 n);

/**
 * @brief  ★ v0.6.3 降速重协商：把模组和 MCU 一起改到 9600 并验证。
 * @param  cur  当前已确认的波特率（at_get_baud()）
 * @return 实际生效的波特率（成功 = 9600，失败 = cur）
 * @note   动机：115200 下一个字节只占 87 us，现场那条线在这个速度下
 *         **模组→MCU 方向只活约一半字节**（收到的字符全对、帧没错，纯整字节没到）。
 *         9600 是 1042 us，12 倍余量，且 11059200/4/9600 = 288 整、无分频误差。
 *         ★ 顺序必须是"先让模组改（AT+IPR=9600）、再让 MCU 改" ——
 *           AT+IPR 的应答仍走旧波特率，反了就等于把自己关在门外。
 *         ★ 9600 一个字节都听不到会自动退回 115200，两边各灌一次 AT+IPR=115200，
 *           不会把板子锁死。
 *         ★ 这同时是一个判据：降速后仍丢 ⇒ 不再是余量问题，只能是硬件对拉/引脚损坏。
 */
/**
 * ★★★ v0.6.4 盲切固定波特率。
 * @param want 目标波特率（0 视为 9600）
 * @return 成功返回 want；失败返回 0 并把两边都拨回 115200（不会锁死）
 *
 * 为什么"盲"：MCU→模组 方向实测 0 丢字节，模组→MCU 方向才丢
 * ⇒ 只要模组收到就够的命令（AT+IPR）不必等回执，发三遍再验。
 * 1 个形参（4 B）⇒ 不生成参数块（铁律 14）。
 */
u32 at_negotiate(u32 want);

/** 上一次 at_negotiate() 验证时单条 AT 收到的字节数（诊断用，0 = 一字节没到）。 */
u16 at_negotiate_n(void);

u32 at_baud_down(u32 cur);

/** 上一次 at_baud_down() 在 9600 下验到的应答字节数；0xFFFF = AT+IPR 本身没成功。 */
u16 at_baud_last_n(void);
