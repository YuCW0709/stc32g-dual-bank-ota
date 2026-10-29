/*==============================================================================
 *  log.h  --  分级日志（输出到调试串口）
 *  Platform : STC32G12K128 (Keil C251, Huge / LARGE)
 *
 *  ── ★★ 为什么这里**不用可变参数**（2026-10-25 血泪结论，不要改回去）──────
 *
 *  C251 的 varargs 在 xdata 里按"固定区"传递：编译器生成一个全局符号
 *  `?log_printf??BYTE` 作参数块，调用方把参数**写在它下面**（BYTE-2, BYTE-4...），
 *  `va_start` 拿到的是块顶，`va_arg` 向下走。
 *
 *  而链接器把这个参数块当成一个普通的 **overlay 单元**塞进 `_XDATA_GROUP_` 池，
 *  于是它能不能被别人踩，完全取决于链接顺序 —— 同一份源码、同样的选项，
 *  换个工程的段大小就会换一个结果：
 *
 *      BL  参数块 = 0x010DBF，池 = 0x010D56-0x010D9F   -> 侥幸在池外，正确
 *      APP 参数块 = 0x010CFA，池 = 0x010CCB-0x010D18   -> 在池内，被踩
 *
 *  APP 里更糟：`log_printf` 自己的溢出帧是 0x010CD2-0x010D02，参数块正好落在
 *  它内部，函数一进门就把自己收到的参数写坏了。实测现象就是
 *  "前 3 个字段对、后面全错"，且**代码功能完全正常，只有日志撒谎**。
 *
 *  已经连续四个版本（v0.3~v0.6）在这个机制上翻车，每次修好一处又从另一处冒出来。
 *  结论：**这个机制在本工具链上不可控，不能用**。
 *
 *  现在的接口是"固定 4 个 32 位槽"：
 *      log_x(lvl, tag, fmt, a, b, c, d)
 *  形参全是标量（走寄存器），进函数第一件事就是搬进 static 数组，
 *  此后不再引用任何形参。没有全局参数块、没有 va_list、没有"向下增长"，
 *  链接器怎么排段都影响不到它。
 *
 *  ── 格式串 ────────────────────────────────────────────────────────────────
 *  支持：%% %c %s %d %u %x %ld %lu %lx
 *  每个转换符**正好吃一个槽**（不再有 u16/u32 宽度之分）：
 *      %u %d %x %lu %ld %lx  -> 槽里的 32 位值
 *      %c                    -> 槽的低 8 位
 *      %s                    -> 槽里的 32 位指针（LARGE 模型下指针就是 4 字节）
 *  槽不够用时该行尾会打 `!!ARGN!!`，一眼就能看出"这行参数写少了"。
 *
 *  ── 行尾 `*HH`：这一行到底可不可信（v0.8）───────────────────────────────
 *
 *  每行末尾固定跟 `*` + 两个小写十六进制字符，是**行首到 `*` 之前全部字节的
 *  8 位累加和**（含时间戳，不含 \r\n）。例：
 *
 *      [689] APP head a=75 84*c8\r\n
 *
 *  为什么要有它：v0.7 上板后数字全对，但 1700 多字符里有 2 处字符变成了空格
 *  （`[7 9]`、`APP head a=75  4`）。而本模块的数字通路只可能输出
 *  `0-9 a-f -`，**物理上吐不出 0x20**；能吐空格的只有格式串字面量和 %s。
 *  所以只剩两种可能：缓冲区里就是空格（格式化错），或者线上被打坏（传输错）。
 *  **只看日志永远分不清**，而这两种的修法完全相反。
 *
 *  有了和值就能当场分开：
 *      和值对 -> 缓冲区与线上一致，问题在固件
 *      和值错 -> 线上被打坏，固件无辜
 *
 *  校验算法刻意选"累加和"而不是 CRC8：我们的失败模式是**单字节被替换**，
 *  累加和对它 100% 敏感，而代价只有 15 字节代码、每行 3 个字符。
 *  注意它的语义是**单向**的：
 *      "和值不对" ⇒ 这行一定坏（必须作废）
 *      "和值对"   ⇒ 只说明这一行没被改动，不能证明内容逻辑正确
 *
 *  PC 侧校验已内置在 `tools/parse_boot_log.py`，坏行会被点名并剔除。
 *  如果自己用串口助手肉眼看，务必**以 `*HH` 为依据**，不要凭印象。
 *
 *  ── 为什么参数个数要写在宏名里 ────────────────────────────────────────────
 *  C89 预处理器不能数 `__VA_ARGS__` 的个数，而这里又必须把参数摊成固定 4 个。
 *  所以宏名后缀直接标出**数值参数的个数**：
 *      LOG_I0(tag, "boot ok")                   0 个
 *      LOG_I1(tag, "rc=%u", rc)                 1 个
 *      LOG_I2(tag, "seq=%lu cnt=%lu", s, c)     2 个
 *      LOG_I3 / LOG_I4                          3 / 4 个
 *  这不是麻烦，是**自证**：看一眼名字就知道这行有几个值，超过 4 个必须拆行。
 *============================================================================*/
#ifndef __LOG_H
#define __LOG_H

#include "ota_mcu.h"
#include "ota_types.h"

/*----------------------------- 日志级别 -------------------------------------
 * ★ 必须用 #define 而不是 enum：下面的编译期裁剪宏是 `#if (LOG_LEVEL >= xxx)`，
 *   而 C 预处理器**不认识 enum 常量**（会把它们当 0 处理，导致
 *   "unknown identifier" 警告 + 所有级别判断失效）。
 *--------------------------------------------------------------------------*/
#define LOG_LVL_NONE    0    /* 全关                                   */
#define LOG_LVL_ERROR   1    /* 仅错误                                 */
#define LOG_LVL_WARN    2    /* 错误 + 告警                            */
#define LOG_LVL_INFO    3    /* 关键流程节点（推荐量产默认）            */
#define LOG_LVL_DBG     4    /* AT 命令原文 / 状态迁移细节              */
#define LOG_LVL_TRACE   5    /* 每包数据长度、缓冲水位                  */

typedef u8 log_level_t;      /* 运行时仍用类型，便于函数签名 */

/* 构建期裁剪：Release 改成 LOG_LVL_ERROR，Debug 用 LOG_LVL_DBG */
#ifndef LOG_LEVEL
#define LOG_LEVEL   LOG_LVL_INFO
#endif

/* 单条日志最长字符数（含时间戳与 '\r\n'）。
 * 顶到边界时会打 "..."，让截断看得见（以前是静默丢弃，看起来像格式化坏了）。 */
#define LOG_LINE_MAX    160U

/* 一条日志最多带几个数值槽。超过就拆行 —— 见文件头说明。 */
#define LOG_ARG_MAX     4

/* 十六进制转储开关。
 *   1 = 编进 log_hexdump()（逐 16 字节一行 + ASCII 侧栏）
 *   0 = 编成一个空函数
 * 本项目当前**没有任何调用点**，而它实测占 512 B code + 一份常量字符串。
 * BL 的 16 KB 槽只剩几百字节（见 docs §8.13），白占不起。
 * 将来做 Flash 镜像比对/抓包分析时再打开。 */
#ifndef LOG_ENABLE_HEXDUMP
#define LOG_ENABLE_HEXDUMP  0
#endif

/*------------------------------------------------------------------------------
 * LOG_SELFTEST -- log_selftest() 的 5 行自检（chkA~chkE）是否参与编译
 *
 *   0 : 不编译（省约 200 字节 code）。**量产与做空间吃紧的诊断时取 0。**
 *   1 : 编译并调用，用来证明"日志本身没在骗人"（换工具链/内存模型后必跑）。
 *
 *   ★ 关掉不会失去"格式串可靠性"的证据：任何一条带 %u/%x/%s 的日志
 *     都在持续验证同一条通路；chkA~chkE 只是把"已知值"固定下来而已。
 *--------------------------------------------------------------------------*/
#ifndef LOG_SELFTEST
#define LOG_SELFTEST        0
#endif

/*==============================================================================
 * 初始化与配置
 *============================================================================*/

/**
 * @brief  初始化日志模块。
 * @param  lvl  初始级别，见 log_level_t。
 * @return OTA_OK / OTA_ERR_PARAM（lvl 越界）。
 * @note   必须在 bsp_uart_init() 之后调用（复用同一路 UART 的发送通道）。
 */
ota_ret_t log_init(log_level_t lvl);

/**
 * @brief  运行期修改级别（可由 MQTT 下发命令动态打开调试）。
 */
void log_set_level(log_level_t lvl);

/**
 * @brief  查询当前级别。
 */
log_level_t log_get_level(void);

/**
 * @brief  开关时间戳前缀（默认开，格式 "[12345]"）。
 */
void log_enable_timestamp(u8 en);

/*==============================================================================
 * 输出核心
 *============================================================================*/

/**
 * @brief  参数槽：调用方先填，本模块只负责格式化。共 LOG_ARG_MAX 个。
 *
 * ★★ 为什么是全局而不是形参（v0.9 血的教训）★★
 *
 *   v0.7 把"编译器生成的可变参数块"换成了固定 4 槽形参，看着很干净，
 *   但形参本身超过寄存器容量时，C251 仍然会生成 `?log_x??BYTE` 参数块 ——
 *   而 L251 把这种段当**孤儿**，只按"大小"往 _XDATA_GROUP_ 池里塞，
 *   并不知道它属于哪个函数，于是可以和其他函数的局部变量重叠。
 *
 *   v0.9 实测 map：
 *       _XDATA_GROUP_       010E4A-010E95
 *       ?log_x??BYTE        010E4D          参数块 16 B -> 010E4D-010E5C
 *       log_x?/log          0E52-0E65       log_x 自己的局部段 -> 010E52-010E65
 *   两者重叠 11 字节。LOG_I2 写的第二个槽 lo(0x4000) 落在 010E51-010E54，
 *   而 log_x 的局部变量把 010E52 从 0x40 写成 0x50 —— 日志里 lo 就变成了
 *   0x5000。同一版里 crc32_selftest 的 v[9] 也被 ?crc32_update??BYTE 踩，
 *   导致 CRC 自检失败、进而 STAGING 被判为不可写、A1 复制 rc=2。
 *
 *   所以：**参数槽必须是 static 全局**。链接器永远不会复用 static 的地址，
 *   这是唯一能"物理保证"不被踩的形态。
 */
extern u32 xdata g_log_av[LOG_ARG_MAX];

/**
 * @brief  格式化核心：只收 2 个形参（级别 + 已含 [tag] 前缀的格式串）。
 * @param  lvl  本条级别，低于当前级别则直接丢弃。
 * @param  fmt  格式串，**必须以 "[tag] " 开头**，见下方 LOG_xN 宏。
 * @return 实际发送字节数；被过滤或失败返回 0。
 *
 * ★ 只有 2 个形参是刻意的：lvl(2 B) + fmt(4 B) = 6 B，C251 全走寄存器，
 *   **不生成任何参数块**。map 里不该再出现 `?log_x2??BYTE`；出现即为回归。
 * @note 本函数不可重入；不要直接调用，用下面 LOG_xN 宏。
 */
u16 log_x2(log_level_t lvl, const char *fmt);

/**
 * @brief  把 tag 拼进格式串 —— 纯编译期字符串字面量拼接，零运行时开销。
 * @note   tag 处必须写字面量（如 "BL"），不能写变量。
 *         需要运行时 tag 的场合（目前只有 log_selftest）走编译期 #if。
 */
#define LOG__F(tag, fmt)        "[" tag "] " fmt

/*==============================================================================
 * 便捷宏：LOG_<级别><数值个数>
 *
 * 每个宏做两件事：① 把值写进 g_log_av，② 调 log_x2。
 * 写法统一为 do{}while(0)，可作为 if/else 的单条语句使用。
 *
 * ★ 级别不够（编译期裁掉）时宏退化为空语句 —— 参数表达式**不会被求值**，
 *   所以即使参数里有函数调用也没有副作用残留（这点和旧版一样）。
 *============================================================================*/

#if (LOG_LEVEL >= LOG_LVL_ERROR)
#define LOG_E0(tag, fmt) do { \
    g_log_av[0] = 0UL; g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_ERROR, LOG__F(tag, fmt)); } while (0)
#define LOG_E1(tag, fmt, a) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_ERROR, LOG__F(tag, fmt)); } while (0)
#define LOG_E2(tag, fmt, a, b) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_ERROR, LOG__F(tag, fmt)); } while (0)
#define LOG_E3(tag, fmt, a, b, c) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_ERROR, LOG__F(tag, fmt)); } while (0)
#define LOG_E4(tag, fmt, a, b, c, d) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = (u32)(d); \
    log_x2(LOG_LVL_ERROR, LOG__F(tag, fmt)); } while (0)
#else
#define LOG_E0(tag, fmt)        do { } while (0)
#define LOG_E1(tag, fmt, a)     do { } while (0)
#define LOG_E2(tag, fmt, a, b)  do { } while (0)
#define LOG_E3(tag, fmt, a, b, c) do { } while (0)
#define LOG_E4(tag, fmt, a, b, c, d) do { } while (0)
#endif

#if (LOG_LEVEL >= LOG_LVL_WARN)
#define LOG_W0(tag, fmt) do { \
    g_log_av[0] = 0UL; g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_WARN, LOG__F(tag, fmt)); } while (0)
#define LOG_W1(tag, fmt, a) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_WARN, LOG__F(tag, fmt)); } while (0)
#define LOG_W2(tag, fmt, a, b) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_WARN, LOG__F(tag, fmt)); } while (0)
#define LOG_W3(tag, fmt, a, b, c) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_WARN, LOG__F(tag, fmt)); } while (0)
#define LOG_W4(tag, fmt, a, b, c, d) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = (u32)(d); \
    log_x2(LOG_LVL_WARN, LOG__F(tag, fmt)); } while (0)
#else
#define LOG_W0(tag, fmt)         do { } while (0)
#define LOG_W1(tag, fmt, a)      do { } while (0)
#define LOG_W2(tag, fmt, a, b)   do { } while (0)
#define LOG_W3(tag, fmt, a, b, c) do { } while (0)
#define LOG_W4(tag, fmt, a, b, c, d) do { } while (0)
#endif

#if (LOG_LEVEL >= LOG_LVL_INFO)
#define LOG_I0(tag, fmt) do { \
    g_log_av[0] = 0UL; g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_INFO, LOG__F(tag, fmt)); } while (0)
#define LOG_I1(tag, fmt, a) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_INFO, LOG__F(tag, fmt)); } while (0)
#define LOG_I2(tag, fmt, a, b) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_INFO, LOG__F(tag, fmt)); } while (0)
#define LOG_I3(tag, fmt, a, b, c) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_INFO, LOG__F(tag, fmt)); } while (0)
#define LOG_I4(tag, fmt, a, b, c, d) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = (u32)(d); \
    log_x2(LOG_LVL_INFO, LOG__F(tag, fmt)); } while (0)
#else
#define LOG_I0(tag, fmt)         do { } while (0)
#define LOG_I1(tag, fmt, a)      do { } while (0)
#define LOG_I2(tag, fmt, a, b)   do { } while (0)
#define LOG_I3(tag, fmt, a, b, c) do { } while (0)
#define LOG_I4(tag, fmt, a, b, c, d) do { } while (0)
#endif

#if (LOG_LEVEL >= LOG_LVL_DBG)
#define LOG_D0(tag, fmt) do { \
    g_log_av[0] = 0UL; g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_DBG, LOG__F(tag, fmt)); } while (0)
#define LOG_D1(tag, fmt, a) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_DBG, LOG__F(tag, fmt)); } while (0)
#define LOG_D2(tag, fmt, a, b) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_DBG, LOG__F(tag, fmt)); } while (0)
#define LOG_D3(tag, fmt, a, b, c) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_DBG, LOG__F(tag, fmt)); } while (0)
#define LOG_D4(tag, fmt, a, b, c, d) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = (u32)(d); \
    log_x2(LOG_LVL_DBG, LOG__F(tag, fmt)); } while (0)
#else
#define LOG_D0(tag, fmt)          do { } while (0)
#define LOG_D1(tag, fmt, a)       do { } while (0)
#define LOG_D2(tag, fmt, a, b)    do { } while (0)
#define LOG_D3(tag, fmt, a, b, c) do { } while (0)
#define LOG_D4(tag, fmt, a, b, c, d) do { } while (0)
#endif

#if (LOG_LEVEL >= LOG_LVL_TRACE)
#define LOG_T0(tag, fmt) do { \
    g_log_av[0] = 0UL; g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_TRACE, LOG__F(tag, fmt)); } while (0)
#define LOG_T1(tag, fmt, a) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = 0UL; g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_TRACE, LOG__F(tag, fmt)); } while (0)
#define LOG_T2(tag, fmt, a, b) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = 0UL; g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_TRACE, LOG__F(tag, fmt)); } while (0)
#define LOG_T3(tag, fmt, a, b, c) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_TRACE, LOG__F(tag, fmt)); } while (0)
#define LOG_T4(tag, fmt, a, b, c, d) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); g_log_av[2] = (u32)(c); g_log_av[3] = (u32)(d); \
    log_x2(LOG_LVL_TRACE, LOG__F(tag, fmt)); } while (0)
#else
#define LOG_T0(tag, fmt)        do { } while (0)
#define LOG_T1(tag, fmt, a)     do { } while (0)
#define LOG_T2(tag, fmt, a, b)  do { } while (0)
#define LOG_T3(tag, fmt, a, b, c) do { } while (0)
#define LOG_T4(tag, fmt, a, b, c, d) do { } while (0)
#endif

/* AT 通道原文抓包（仅 DBG 级别真正展开，用于对照模组手册排查时序） */
#define LOG_AT_TX(cmd)         LOG_D1("AT", "TX> %s", cmd)
#define LOG_AT_RX(line)        LOG_D1("AT", "RX< %s", line)

/*==============================================================================
 * 自检与调试辅助
 *============================================================================*/

/**
 * @brief  格式化自检：把一组已知值原样打出来，用来判断"日志到底可不可信"。
 *
 * 换编译选项、升级工具链、改内存模型、改分区表之后，先跑一遍这个自检，
 * 用已知值确认"日志没有骗人"，再去相信任何一条诊断结论。
 *
 * @param  tag  模块标签。
 * @note   输出 5 行，覆盖 1~4 个槽、十进制/十六进制/字符串混排：
 *           chkA  0 1 9 10
 *           chkB  15 16 255 256
 *           chkC  65535 4294967295
 *           chkD  %x  0 f 10000 ff4000
 *           chkE  65537|mid|9|14000
 *         任何一行与源码里的值不符，就说明日志在撒谎。
 */
#if (LOG_SELFTEST != 0)
void log_selftest(void);
#endif

/**
 * @brief  十六进制转储（每行 16 字节，offset + hex + ascii）。
 * @param  lvl    级别。
 * @param  tag    模块标签。
 * @param  src    数据指针。
 * @param  len    字节数，建议 <= 256（一次调用输出约 len/16 行）。
 * @return 实际发送字节数。
 */
#if (LOG_ENABLE_HEXDUMP != 0)
u16 log_hexdump(log_level_t lvl, const char *tag, const u8 *src, u16 len);
#endif

#endif /* __LOG_H */
