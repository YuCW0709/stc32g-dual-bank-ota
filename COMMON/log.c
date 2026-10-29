/*==============================================================================
 *  log.c  --  分级日志 + 精简格式化
 *  Platform: STC32G12K128 (Keil C251, Huge / LARGE)
 *
 *  ── 本版（v0.7）做了什么，为什么 ─────────────────────────────────────────
 *
 *  1) **去掉 varargs**。原因见 log.h 文件头：编译器生成的全局参数块
 *     `?log_printf??BYTE` 会被链接器当成普通 overlay 单元塞进 `_XDATA_GROUP_`
 *     池，位置完全不可控。BL 里它在池外（一直正常），APP 里它在池内、
 *     而且正好落在 `log_printf` 自己的溢出帧里面，函数一进门就把参数写坏。
 *     同一份源码、同样的编译选项，两个工程结果不同 —— 这种机制不能留。
 *
 *  2) **十进制转换不调用除法库**。原来的 `v % (u32)base` / `v /= (u32)base`
 *     会生成 `ECALL ?C?ULDIV?`，而除数 `base` 是个形参、要跨两次库调用活着；
 *     实测在 APP 里它的行为与 BL 不一致（小数值看不出来，多位数全错）。
 *     现在改成"逐字节长除 + 自己写的 16 位除 10"（只用移位/比较/减法），
 *     十六进制直接用移位取半字节。整条路径没有库调用、没有形参存活问题。
 *
 *  3) **本模块所有变量一律 static**，且全部集中在文件头声明一次。
 *     局部 xdata 变量会进 overlay 池，本文件不再依赖任何局部变量布局。
 *
 *  代码量代价约 +150 字节，换来的是"数字一定对"。值。
 *
 *  ── v0.8 追加：每行行尾带 8 位累加和 `*HH` ────────────────────────────────
 *
 *  v0.7 上板后数字全对了，但 1700 多个字符里出现了 2 处"某个字符变成空格"
 *  （`[7 9]`、`APP head a=75  4`）。查遍代码可以证明一件关键事实：
 *
 *      本文件所有数字通路（put_dec / put_hex32 / put_hex2）**只可能输出
 *      '0'~'9'、'a'~'f'、'-'**，绝不可能吐出 0x20。能吐空格的只有格式串里的
 *      字面空格和 put_str 的字符串。
 *
 *  也就是说，那两处空格要么是"缓冲区里就已经是空格"（格式化错，本文件不可能），
 *  要么是"缓冲区是对的、线上被打坏了"。**光看日志永远分不清这两种**，
 *  所以加一个行校验和：接收端一算就知道这一行有没有在传输中被动过。
 *
 *  校验规则：覆盖从行首到 `*` 之前的全部字节（含时间戳），8 位累加（mod 256），
 *  两个小写十六进制字符。不追求检错能力多强 —— 我们的失败模式是"单字节被替换"，
 *  累加和对这种错 100% 敏感，而它只花 15 字节代码、每行 3 个字符。
 *
 *  用法（PC 侧 tools/parse_boot_log.py 已实现）：
 *      `[689] APP head a=75 84*3c`  -> 重算 0x3c 一致 -> 本行可信
 *      `[689] APP head a=75 84*7a`  -> 不一致 -> 本行作废，是传输问题不是固件问题
 *============================================================================*/
#include "log.h"
#include "bsp_uart.h"
#include "bsp_timer.h"

/* 单条 %s 最多输出的字符数。
 * 上限不是为了省空间，是为了**保命**：万一参数错位，%s 会拿到野指针，
 * while(*s) 可能扫过整片内存，这一行剩下的字段全部消失。 */
#define PUT_STR_MAX   120U

/*--------------------------- 状态（全部 static）----------------------------*/
static char xdata s_line[LOG_LINE_MAX];
static u16  xdata s_pos;                 /* 当前写入位置           */
static u8   xdata s_ovf;                 /* 1 = 本行被截断         */
static log_level_t xdata s_lvl = LOG_LVL_INFO;
static u8   xdata s_ts = 1;              /* 1 = 打时间戳           */

/* 格式化上下文：不走参数块，全在本文件内传递 */
static const char *s_fmt;                /* 当前格式串指针         */
static u32  xdata s_av[LOG_ARG_MAX];     /* 4 个 32 位参数槽       */
static u8   xdata s_ai;                  /* 下一个要取的槽号       */
static u8   xdata s_argn;                /* 1 = 格式要的槽比给的多 */

/* 输出小工具与数字转换的工作区 */
static u16  xdata s_i;                   /* 通用循环变量（16 位）  */
static u8   xdata s_b0;                  /* 通用 8 位中间量        */
static u8   xdata s_b1;                  /* 通用 8 位中间量        */
static u32  xdata s_sh;                  /* 十六进制移位工作副本   */
static u8   xdata s_started;             /* 十六进制前导零抑制     */
static u8   xdata s_bin[4];              /* 十进制长除的 32 位副本 */
static u8   xdata s_dig[10];             /* 十进制数字（低位在前） */
static u8   xdata s_ndig;                /* 已产生的位数           */
static u8   xdata s_rem;                 /* 长除余数               */
static u16  xdata s_cur;                 /* 长除当前字节           */
static u16  xdata s_q;                   /* 除 10 的商             */
static u8   xdata s_ea;                  /* 读时间戳时保存的 EA    */
static u32  xdata s_ms;                  /* 时间戳                 */
static u8   xdata s_sum;                 /* 行校验和（v0.8）       */

/*--------------------------- 输出小工具 ------------------------------------*/
static void put_ch(char c)
{
    /* 边界用 `s_pos < LOG_LINE_MAX`（不是 -1）：
     * 写成 -1 会永远浪费最后一格，超长行补的 '\n' 写不进去，
     * 现场表现是"这一行和下一行粘在一起"。 */
    if (s_pos < LOG_LINE_MAX) s_line[s_pos++] = c;
    else                      s_ovf = 1;      /* 丢弃但记账，行尾打 "..." */
}

/* 双字符十六进制（hexdump 用） */
static void put_hex2(u8 v)
{
    put_ch("0123456789abcdef"[v >> 4]);
    put_ch("0123456789abcdef"[v & 0x0F]);
}

/*------------------------------------------------------------------------------
 * 行尾校验和：对**已经写进 s_line 的前 s_pos 个字节**做 8 位累加，再追加 `*HH`。
 *
 * 必须在所有正文写完之后调用；也就是说，如果正文中途出错，和值跟着一起错，
 * 接收端照样能发现（这正是我们要的：不是"校验通过就一定是好数据"，
 * 而是"校验不过就一定是坏数据"）。
 *
 * 调用后 s_pos 已经包含 `*HH`，可以直接补 \r\n 发出去。
 *----------------------------------------------------------------------------*/
static void put_line_sum(void)
{
    s_sum = 0;
    for (s_i = 0; s_i < s_pos; s_i++) s_sum = (u8)(s_sum + (u8)s_line[s_i]);
    put_ch('*');
    put_hex2(s_sum);
}

static void put_str(const char *s)
{
    s_i = 0;
    if (s == 0) { return; }
    while (*s && s_i < PUT_STR_MAX) { put_ch(*s++); s_i++; }
}

/*------------------------------------------------------------------------------
 * 16 位整数除 10：只用移位/比较/减法，不调用任何除法库。
 *
 * 教科书式的"移位相减"长除：被除数从高位往低位喂进余数寄存器 r，
 * r 每步不超过 19，够减就减 10 并把商的对应位置 1。
 * 16 次循环，全部 8 位运算，行为完全确定。
 *
 * 余数通过 s_rem 带回（不用指针形参：形参会占溢出槽，本模块坚持零局部）。
 *----------------------------------------------------------------------------*/
static u16 udiv10(u16 x)
{
    s_q   = 0;
    s_rem = 0;

    for (s_i = 0; s_i < 16; s_i++)
    {
        s_rem = (u8)(s_rem << 1);
        if (x & 0x8000U) s_rem = (u8)(s_rem | 1U);
        x = (u16)(x << 1);

        s_q = (u16)(s_q << 1);
        if (s_rem >= 10U)
        {
            s_rem = (u8)(s_rem - 10U);
            s_q   = (u16)(s_q | 1U);
        }
    }
    return s_q;
}

/*------------------------------------------------------------------------------
 * 十进制输出（32 位无符号）
 *
 * 逐字节长除：把 32 位值拆成 4 个字节，从高位字节开始，
 * 每步 cur = 上次余数*256 + 本字节（<= 9*256+255 = 2559），
 * 用 udiv10 求 cur/10，商写回该字节、余数留给下一字节。
 * 走完 4 个字节得到"v/10"和"v 的最低位数字"，循环到 v 变 0。
 *
 * 完全没有 32 位除法，也没有除法库调用。
 *----------------------------------------------------------------------------*/
static void put_dec(u32 v)
{
    s_bin[0] = (u8)(v >> 24);
    s_bin[1] = (u8)(v >> 16);
    s_bin[2] = (u8)(v >> 8);
    s_bin[3] = (u8)v;

    s_ndig = 0;

    do
    {
        s_rem = 0;
        for (s_b1 = 0; s_b1 < 4; s_b1++)
        {
            s_cur = (u16)(((u16)s_rem << 8) | (u16)s_bin[s_b1]);
            s_cur = udiv10(s_cur);            /* s_rem 带回余数 */
            s_bin[s_b1] = (u8)s_cur;
        }
        s_dig[s_ndig++] = (u8)('0' + s_rem);
    }
    while ((s_bin[0] | s_bin[1] | s_bin[2] | s_bin[3]) != 0U && s_ndig < 10U);

    while (s_ndig > 0U) put_ch((char)s_dig[--s_ndig]);
}

/*------------------------------------------------------------------------------
 * 十六进制输出（32 位，抑制前导零，至少 1 位）
 *
 * 用"左移 4 位取最高半字节"的方式，避免 32 位可变位数的右移
 * （可变移位在 C251 上会调库，且同样有跨调用存活问题）。
 *----------------------------------------------------------------------------*/
static void put_hex32(u32 v)
{
    s_sh      = v;
    s_started = 0;

    for (s_b1 = 0; s_b1 < 8; s_b1++)
    {
        s_b0 = (u8)(s_sh >> 28);             /* 常量移位，不调库 */
        s_sh = s_sh << 4;

        if (s_b0 != 0U || s_started != 0U || s_b1 == 7U)
        {
            s_started = 1;
            put_ch((char)((s_b0 < 10U) ? ((char)('0' + s_b0))
                                       : ((char)('a' + (s_b0 - 10U)))));
        }
    }
}

/* 取下一个参数槽。槽用完了就置标志，让整行末尾打 !!ARGN!! */
static u32 next_arg(void)
{
    if (s_ai >= LOG_ARG_MAX)
    {
        s_argn = 1;
        return 0UL;
    }
    return s_av[s_ai++];
}

/*--------------------------- 格式化 ----------------------------------------
 * 每个转换符正好吃一个槽（不再有 u16/u32 宽度之分）。
 * `%l` 前缀被忽略：%lu 与 %u 等价、%lx 与 %x 等价。
 * 这样调用点少一个"这里该不该加 l"的决策，也就少一类错。
 *----------------------------------------------------------------------------*/
static void vformat(void)
{
    s_ai   = 0;
    s_argn = 0;

    while (*s_fmt)
    {
        if (*s_fmt != '%') { put_ch(*s_fmt++); continue; }

        s_fmt++;
        if (*s_fmt == 'l') s_fmt++;

        switch (*s_fmt)
        {
        case 'd':
            s_sh = next_arg();
            if ((s32)s_sh < 0)
            {
                put_ch('-');
                put_dec((u32)(-(s32)s_sh));
            }
            else
            {
                put_dec(s_sh);
            }
            break;

        case 'u':
            put_dec(next_arg());
            break;

        case 'x':
            put_hex32(next_arg());
            break;

        case 'c':
            put_ch((char)(u8)next_arg());
            break;

        case 's':
            put_str((const char *)next_arg());
            break;

        case '%':
            put_ch('%');
            break;

        case '\0':
            return;                          /* 格式串以 % 结尾：直接收尾 */

        default:
            put_ch('?');
            break;
        }
        s_fmt++;
    }
}

/*--------------------------- 公共接口 --------------------------------------*/
ota_ret_t log_init(log_level_t lvl)
{
    if (lvl > LOG_LVL_TRACE) return OTA_ERR_PARAM;
    s_lvl = lvl;
    return OTA_OK;
}

void log_set_level(log_level_t lvl)
{
    if (lvl <= LOG_LVL_TRACE) s_lvl = lvl;
}

log_level_t log_get_level(void)
{
    return s_lvl;
}

void log_enable_timestamp(u8 en)
{
    s_ts = en ? 1 : 0;
}

/* 取毫秒时间戳
 *
 * bsp_timer_ms() 返回 u32、由 Timer0 中断自增。主循环读 4 字节变量时如果
 * 被中断打断，可能读到"低 2 字节旧、高 2 字节新"的撕裂值，打出一个跳变的
 * 鬼数字，很容易被误判成"程序复位过"。所以取时间戳时关一次中断。 */
static u32 log_now_ms(void)
{
    s_ea = EA;
    EA   = 0;
    s_ms = bsp_timer_ms();
    EA   = s_ea;
    return s_ms;
}

/* ★★ 参数槽是全局 static，不在任何函数的帧里 ★★
 *   链接器永远不复用 static 变量的地址，因此它物理上不可能被别的函数踩。
 *   这是 v0.9 之后本模块唯一依赖的"不被踩"保证 —— 详见 log.h 的说明。 */
u32 xdata g_log_av[LOG_ARG_MAX];

u16 log_x2(log_level_t lvl, const char *fmt)
{
    if (lvl > s_lvl) return 0;

    /* 立刻把全局槽拷进本模块私有的 s_av[]，此后格式化的整个过程中
     * 都不再读 g_log_av —— 即使调用方（或中断）改了它，本行也已定型。 */
    s_av[0] = g_log_av[0];
    s_av[1] = g_log_av[1];
    s_av[2] = g_log_av[2];
    s_av[3] = g_log_av[3];

    s_pos = 0;
    s_ovf = 0;

    if (s_ts)
    {
        put_ch('[');
        put_dec(log_now_ms());
        put_ch(']');
    }

    /* fmt 自带 "[tag] " 前缀（由 LOG__F 在编译期拼好），这里不再单独打 tag。
     * 少一次 put_str(tag) 调用，也少一个形参 —— 形参正是上一版的祸根。 */
    s_fmt = fmt;
    vformat();

    /* 格式要的槽比给的多：这不是"日志不好看"，是调用点写错了，必须看得见 */
    if (s_argn) put_str(" !!ARGN!!");

    /* 截断必须看得见：留 8 字节给 "..." + "*HH" + "\r\n"，
     * 否则补的换行会被边界挡掉，两行粘连。 */
    if (s_ovf)
    {
        s_pos = (u16)(LOG_LINE_MAX - 8U);
        s_line[s_pos++] = '.';
        s_line[s_pos++] = '.';
        s_line[s_pos++] = '.';
        s_ovf = 0;
    }

    put_line_sum();                          /* v0.8：行尾 *HH，可自证真伪 */
    put_ch('\r');
    put_ch('\n');

    /* 逐字节发送（不走 bsp_uart_write：少一层形参，链路上不留任何溢出槽） */
    for (s_i = 0; s_i < s_pos; s_i++) bsp_uart_putc((u8)s_line[s_i]);

    return s_pos;
}

/*------------------------------------------------------------------------------
 * 格式化自检
 *
 * 级别参数故意传 LOG_LVL_NONE(0)：级别过滤是 `lvl > s_lvl` 才丢弃，
 * 传 0 永远不会被丢，所以自检不受 LOG_LEVEL 编译开关和运行时级别影响。
 * 自检本来就该"任何时候都打得出来"，否则它自己也变成需要怀疑的对象。
 *----------------------------------------------------------------------------*/
static const char s_mid[] = "mid";

#if (LOG_SELFTEST != 0)
void log_selftest(void)
{
    /* tag 必须在编译期确定才能拼进字面量（LOG__F 是纯字符串拼接）。
     * 而 log.c 是两个工程共用的，所以按编译宏选 —— 效果和原来调用方
     * 传 "BL"/"APP" 完全相同，但不再需要形参。 */
#if defined(OTA_BUILD_APP) && (OTA_BUILD_APP != 0)
  #define LOG_ST_TAG    "APP"
#else
  #define LOG_ST_TAG    "BL"
#endif

    g_log_av[0] = (u32)0;
    g_log_av[1] = (u32)1;
    g_log_av[2] = (u32)9;
    g_log_av[3] = (u32)10;
    log_x2(LOG_LVL_NONE, LOG__F(LOG_ST_TAG, "chkA %%u: %u %u %u %u"));

    g_log_av[0] = (u32)15;
    g_log_av[1] = (u32)16;
    g_log_av[2] = (u32)255;
    g_log_av[3] = (u32)256;
    log_x2(LOG_LVL_NONE, LOG__F(LOG_ST_TAG, "chkB %%u: %u %u %u %u"));

    g_log_av[0] = (u32)65535;
    g_log_av[1] = (u32)4294967295UL;
    g_log_av[2] = 0UL;
    g_log_av[3] = 0UL;
    log_x2(LOG_LVL_NONE, LOG__F(LOG_ST_TAG, "chkC %%u: %u %u"));

    g_log_av[0] = (u32)0;
    g_log_av[1] = (u32)0xFF;
    g_log_av[2] = (u32)0x10000;
    g_log_av[3] = (u32)0xFF4000;
    log_x2(LOG_LVL_NONE, LOG__F(LOG_ST_TAG, "chkD %%x: %x %x %x %x"));

    g_log_av[0] = (u32)65537;
    g_log_av[1] = (u32)s_mid;
    g_log_av[2] = (u32)9;
    g_log_av[3] = (u32)0x14000;
    log_x2(LOG_LVL_NONE, LOG__F(LOG_ST_TAG, "chkE mix: %u|%s|%u|%x"));
}
#endif  /* LOG_SELFTEST */

/* ★ 整个函数（含定义）都用开关包住，不写成"空函数体"：
 *   C251 对 `(void)lvl;` 这种抑制警告的惯用法会报
 *   `C138 expression with possibly no effect`（实测 4 条），
 *   而写空函数体又会报未使用参数。直接不参与编译最干净。
 *   本项目当前 0 调用点；将来要用就把 log.h 的宏置 1，声明与定义一起回来。 */
#if (LOG_ENABLE_HEXDUMP != 0)
u16 log_hexdump(log_level_t lvl, const char *tag, const u8 *src, u16 len)
{
    if (lvl > s_lvl || src == 0) return 0;

    for (s_cur = 0; s_cur < len; s_cur = (u16)(s_cur + 16))
    {
        s_pos = 0;
        put_ch('[');
        put_str(tag);
        put_str("] ");
        put_hex32((u32)s_cur);
        put_str(": ");

        s_b0 = (u8)((len - s_cur >= 16) ? 16 : (len - s_cur));
        for (s_b1 = 0; s_b1 < 16; s_b1++)
        {
            if (s_b1 < s_b0)
            {
                put_hex2(src[s_cur + s_b1]);
                put_ch(' ');
            }
            else
            {
                put_str("   ");
            }
        }

        put_str(" |");
        for (s_b1 = 0; s_b1 < s_b0; s_b1++)
        {
            u8 c = src[s_cur + s_b1];
            put_ch((c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        put_ch('|');
        put_line_sum();                      /* v0.8：行尾 *HH */
        put_ch('\r');
        put_ch('\n');

        for (s_i = 0; s_i < s_pos; s_i++) bsp_uart_putc((u8)s_line[s_i]);
    }
    return len;
}
#endif  /* LOG_ENABLE_HEXDUMP */
