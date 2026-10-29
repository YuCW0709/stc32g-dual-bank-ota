/*==============================================================================
 *  http_ota.c  --  HTTP 下载引擎（Y100P AT 模式，APP 轮询版）
 *  Platform : STC32G12K128 (Keil C251, CodeRomSize = Huge, Memory Model = LARGE)
 *  Depends  : uart_at.h, flash_iap.h, crc32.h, log.h, ota_partition.h
 *
 *  ── 设计（本轮为"最快跑通命脉"，刻意从简）────────────────────────────────
 *   不做复杂 Range 断点续传，只做：
 *      AT+HTTPINIT
 *      AT+HTTPPARA="URL","<url>"        <- url 用 OTA_FW_URL 宏占位
 *      AT+HTTPPARA="CID",1
 *      AT+HTTPACTION=0                  -> 等 +HTTPACTION: 0,200,<len>
 *      AT+HTTPREAD=0,<n>                -> 循环读，每次 +HTTPREAD: <len> + 数据体
 *      把数据体逐片写入 STAGING（擦除后按扇区写）
 *      AT+HTTPTERM
 *   断点续传、Range、多失败重试等留到 MQTT 控制面之后叠加。
 *
 *  ── ★ 命令组已按官方手册确认（2026-10-27 查 pdf，不再是猜测）──────────
 *   依据：工程根目录《Air700ECT&Air780EHT&…AT指令手册 V1.0.10》（合宙原厂）
 *     §13.7  AT+HTTPREAD=<start_address>,<byte_size>   ← p.196，分片读**官方支持**
 *     §HTTP 流程示例                                   ← p.201，完整时序
 *
 *   ★ 之前"Y100P 是移远 AT 固件、要用 AT+QHTTP*"的判断**已被手册证伪**：
 *     全文 265 页搜 QHTTP / QICSGP / QIACT / QIOPEN **一次都没出现**，
 *     HTTP 只有 `AT+HTTPINIT/PARA/ACTION/READ/TERM` 这一套合宙写法。
 *     ⇒ 本文件的命令字符串**不用改**，原地就是对的。
 *
 *   ★★ 但真正让下载从没跑通的，是**缺了 GPRS 承载激活**（本次补上）：
 *     手册 p.201 规定 HTTP 之前必须先
 *       AT+SAPBR=3,1,"CONTYPE","GPRS"
 *       AT+SAPBR=3,1,"APN",""      ← 空串 = 模组按网络自动取 APN
 *       AT+SAPBR=1,1               ← 激活 PDP（耗时可到 10 s，超时用 AT_TO_NET）
 *       AT+SAPBR=2,1               ← 查状态，回 "+SAPBR:1,1,<IP>"
 *     而本工程此前**一条承载命令都没有**（全库 grep SAPBR / QICSGP 为空），
 *     没有 PDP 就没有 IP，HTTPINIT 之后 HTTPACTION 永远等不到 200。
 *     ⇒ 见文件末尾 hota_pdp_up()。
 *
 *   ── 时序要点 ─────────────────────────────────────────────────────────────
 *   AT+HTTPACTION=0 返回 OK 只是"已受理"，真正完成看 +HTTPACTION: <id>,<code>,<len>
 *   AT+HTTPREAD=<start>,<n> 返回 "+HTTPREAD: <len>\r\n<len字节>\r\nOK"
 *   code=200 表示全量；单次 HTTPREAD 的 length 本实现取 1024（安全值）。
 *
 *  ── 铁律遵从 ─────────────────────────────────────────────────────────────
 *   缓冲区 static；形参第一句拷进 static；日志固定槽宏；常量裸 const。
 *============================================================================*/
#include "http_ota.h"
#include "uart_at.h"
#include "flash_iap.h"
#include "crc32.h"
#include "log.h"
#include "bsp_timer.h"
#include "bsp_wdt.h"
#include "param.h"

/* 下载 URL（★ 测试前由用户填入真实直链） */
#ifndef OTA_FW_URL
#define OTA_FW_URL  "http://192.168.1.100/fw.bin"
#endif

/* 单次 HTTPREAD 长度（数据体直接轮询 UART2，不走环形缓冲）
 *
 * ★★★ v0.6.5：这个值现在**不是随便取的**，它被模组的 HTTP 缓冲上限锁死。
 *   手册 §13.7（p.196）原文：
 *       <start_address>  输出数据的起点  0~3356
 *       <byte_size>      输出数据的长度  1~3356
 *   也就是说模组 HTTP 栈的响应缓冲只有 **3356 字节**。
 *   step60 现场 `+HTTPACTION: 0,602,0` 三次全 602，手册 p.195 定义
 *       **602 = 内存不足(No memory)**
 *   ⇒ 一次 GET 一个 23104 字节的固件，模组根本装不下，与线路、与服务器、
 *     与 SSL 全都无关。这是"下载永远返回 602"的结构性原因。
 *   ⇒ 必须用 Range 分片：每片 <= 3356。取 1024（= 2 个 512 B 扇区，
 *     与 s_io[]、AT_BIN_CHUNK_MAX 一致，不用额外 xdata）。 */
#define HTTP_READ_CHUNK   1024U

/* 落盘缓冲区：1KB，一次读 512B（扇区对齐）写扇区 */
#define HOTA_IO_BUF       1024U

/*==============================================================================
 * 引擎上下文（static，物理隔离）
 *============================================================================*/
static http_ota_state_t xdata s_state;
static ota_ret_t         xdata s_err;

static u8   xdata s_url[PARAM_URL_MAX];      /* URL 副本（可被 fsm 改写） */
static u32  xdata s_total;                   /* 镜像总长（来自镜像头）   */
static u32  xdata s_offset;                  /* 已落盘字节数             */
static u32  xdata s_expect_len;              /* HTTPACTION 报告的长度    */
static u16  xdata s_status_code;             /* 200/206/...              */

static u8   xdata s_io[HOTA_IO_BUF];         /* 数据体接收缓冲           */

/* ---- v0.6.5 分片下载（Range）新增 ---- */
static u16  xdata s_chunk_got;               /* 本片 +HTTPACTION 报的字节数 */
static u16  xdata s_chunk_no;                /* 片号（日志/判读用）        */
static u8   xdata s_chunk_last;              /* 1 = 短片 ⇒ 已是文件尾      */
static u8   xdata s_try;                     /* 本片已重试次数             */

/* ---- v0.6.18 数据体头部无关字节（8910 在 +HTTPREAD 行后多发一个空行）----
 *   s_lead_ok = 0 表示还没测过：此时一律不丢字节（宁可乱、不可少）。
 *   测出来之后每片都按同一个值丢，因为它是**应答格式**的一部分，不会变。 */
static u8   xdata s_lead;                    /* 每片头部要丢几个字节       */
static u8   xdata s_lead_ok;                 /* 1 = 已实测                 */
/* ★ v0.6.11：原来的 s_pcmd[48]（AT+HTTPPARA 拼装）已删除 ——
 *   所有命令统一拼进 uart_at.c 的 g_at_tx（零指针通道），
 *   省下的 48 B xdata 正好补给 g_at_tx/g_at_cap。 */

/* ---- v0.6.7 断线自愈 ---- */
#define HOTA_RECOVER_MAX   6U    /* 一次下载里最多重连几次（每次约 5 s） */
#define HOTA_RESTART_MAX   2U    /* 重连都不成时，最多整包重下几次     */
static u8   xdata s_recover;                 /* 已重连次数                 */
static u8   xdata s_restart;                 /* 已整包重下次数             */

/* 初始化标志 */
static u8   xdata s_inited;

/*==============================================================================
 * 工具：十进制解析（AT 应答里的数字字段）
 *============================================================================*/

/* 解析 "+HTTPACTION: 0,200,1024" 里的 method / code / len
 *
 * ★ v0.6.5：改成**只认数字串**，不再依赖 ':' 和 ',' ——
 *   捕获缓冲里的逗号是这条链路上最容易丢的字符之一（只 1 个字节，
 *   丢了 `parse_httpaction` 就会把 "602,0" 读成 code=6020 len=0）。
 *   按"第 1 个数字串=method、第 2 个=code、第 3 个=len"取值，
 *   少几个逗号都不影响判读。 */
/* ★ v0.6.11：改成**零形参**，直接读 g_at_cap / g_at_capn。
 *   理由与命令同源：本工程不许把指针当形参跨函数传（step62 定案）。 */
static void parse_httpaction(void)
{
    static u16 xdata i;
    static u32 xdata v;
    static u8  xdata fld;

    i = 0U;
    fld = 0U;
    while (i < g_at_capn)
    {
        if (g_at_cap[i] >= '0' && g_at_cap[i] <= '9')
        {
            v = 0UL;
            while (i < g_at_capn && g_at_cap[i] >= '0' && g_at_cap[i] <= '9')
            {
                if (v < 100000UL) v = v * 10UL + (u32)(g_at_cap[i] - '0');
                i++;
            }
            fld++;
            if (fld == 2U)      s_status_code = (u16)v;
            else if (fld == 3U) { s_expect_len = v; return; }
        }
        else
        {
            if (g_at_cap[i] == 0) return;   /* 捕获串到此为止：后面是上一条命令的残留，
                                             *   继续扫会把旧数字当成这一条的 code/len */
            i++;
        }
    }
}

/*==============================================================================
 * URC 钩子（注册到 uart_at）
 *============================================================================*/
ota_ret_t http_ota_urc_read(const char *line, u16 len)
{
    (void)line; (void)len;      /* 本工程不用 URC 收数据，保留接口仅为兼容 */
    return OTA_OK;
}

ota_ret_t http_ota_urc_action(const char *line, u16 len)
{
    (void)line; (void)len;      /* 同上：真解析走 at_tx_mac 之后的零形参版本 */
    parse_httpaction();
    if (s_state == HOTA_REQ) s_state = HOTA_READ;
    return OTA_OK;
}

/*==============================================================================
 * 初始化
 *============================================================================*/
ota_ret_t http_ota_init(const http_ota_cfg_t *cfg)
{
    static u16 xdata i;
    if (cfg == 0) return OTA_ERR_PARAM;
    if (cfg->url == 0) return OTA_ERR_PARAM;
    /* io_buf 本轮不用（内部 s_io 承担接收缓冲），不做非空校验 */

    /* 拷贝 url */
    for (i = 0; i < (PARAM_URL_MAX - 1U); i++)
    {
        s_url[i] = cfg->url[i];
        if (cfg->url[i] == 0) break;
    }
    s_url[PARAM_URL_MAX - 1U] = 0;

    s_state = HOTA_IDLE;
    s_err   = OTA_OK;
    s_total = 0UL;
    s_offset = 0UL;
    s_expect_len = 0UL;
    s_status_code = 0U;
    /* ★ v0.6.18：铁律 13 —— static 初值不可信，前导字节数必须亲手置"未知" */
    s_lead = 0U;
    s_lead_ok = 0U;
    s_inited = 1U;

    /* 注册 +HTTPACTION URC（其实我们用 at_cmd_wait 的 expect 前缀匹配，不依赖 URC 回调） */
    return OTA_OK;
}

/*==============================================================================
 * GPRS 承载（PDP）激活 —— 手册 p.201，HTTP 之前**必须先做**
 *
 *   没有这几步就没有 IP，`AT+HTTPINIT` 之后 `AT+HTTPACTION=0` 永远等不到
 *   `+HTTPACTION:0,200,<len>`。本工程此前一条承载命令都没有，这是
 *   "HTTP 下载从未跑通"的**结构性**原因，与线路好坏无关。
 *
 *   ★ 无参数（不生成 ?BYTE 参数块，铁律 14）。
 *   ★ `AT+SAPBR=1,1` 失败**不判死**：承载可能已由上一次会话激活，
 *     第二次发会回 ERROR/+CME ERROR。生死由后面的 `2,1` 查询定。
 *   ★ 超时一律用 AT_TO_NET(15 s)：PDP 激活实测常在 2~10 s，
 *     用 AT_TO_CMD(2 s) 会把"慢"误判成"错"。
 *
 *   ── v0.6.0 改动（依据 v0.5.0 上板日志）──────────────────────────────
 *     ① **AT 家族已定案 = SIMCom/SAPBR 风格**：`SAPBR=3,1,"CONTYPE",...` 回 OK、
 *        `CSTT` 回 OK、`CIICR` 回 OK，而 `QICSGP` / `QIACT=1` 双双 **ERROR**
 *        ⇒ 移远那两条在本模组上根本不存在，任何"再试一次 Quectel"的想法都是死的。
 *     ② **`sapbr contype rc=4` 不是模组不认**：同一时刻 USB-TTL 侧看到的是干净的
 *        `OK`，是 MCU 的接收线成片吞字节把 'O'/'K' 吃掉了（详见 at_set_lenient）。
 *        ⇒ 本函数改为 hota_try() 三次重试 + 宽松判据，不再"一次丢字节判死"。
 *     ③ 失败时补发 `AT+CIFSR` 把 IP 原文打出来：有 IP 说明承载真的起来了，
 *        纯粹是应答被吞 —— 这条能把"没网"和"线坏"彻底分开。
 *============================================================================*/

/* 成功判定：严格 OK，或"漏了字节但确认收到 K"的残片（见 at_set_lenient 说明）。
 * ★ 整条 HTTP 初始化都走它 —— 这条线在丢字节，严格比 "OK" 两个字符等于自废。 */
#define HOTA_OK(r)   (((r) == AT_RESP_OK) || ((r) == AT_RESP_PART))

/*==============================================================================
 * ★★★ v0.6.11：命令一律"逐字节现拼"进 g_at_tx，**不传任何指针**
 *
 *   病灶（step62 现场定案，详见 uart_at.c「零指针命令通道」那段）：
 *     `[AT] TX REFUSED b0=ff/87/a` … 以及线上真正出现的 `ailed<CR><LF>`
 *     ⇒ 把代码段字符串的指针当形参跨函数传，被编译器排进寄存器后又跨调用存活，
 *       值被踩坏：段字节对、低 16 位错 ⇒ 顺着代码段读到别的字面量尾巴。
 *     按调用点稳定发病：`AT+HTTPTERM` 好、`AT+SAPBR=3,1,…` 坏，同一函数内相邻。
 *
 *   ⇒ 这里每条命令都用立即数一个字节一个字节地拼，编译器只能生成
 *     "常量地址 + 立即数"的存储，物理上不存在"指针"这个中间物。
 *   代价是源码长，但这是本平台唯一结构上不会再错的方式。
 *============================================================================*/
#define HC_CONTYPE   1U   /* AT+SAPBR=3,1,"CONTYPE","GPRS" */
#define HC_APN       2U   /* AT+SAPBR=3,1,"APN",""         */
#define HC_ACT       3U   /* AT+SAPBR=1,1                  */
#define HC_SAPQ      4U   /* AT+SAPBR=2,1                  */
#define HC_HTTPINIT  5U   /* AT+HTTPINIT                   */
#define HC_HTTPTERM  6U   /* AT+HTTPTERM                   */
#define HC_SSLON     7U   /* AT+HTTPSSL=1                  */
#define HC_SSLCFG    8U   /* AT+SSLCFG="seclevel",153,0    */
#define HC_SSLOFF    9U   /* AT+HTTPSSL=0                  */
#define HC_CID      10U   /* AT+HTTPPARA="CID",1           */
#define HC_ACTION   11U   /* AT+HTTPACTION=0               */
#define HC_ATE0     12U   /* ATE0                          */
#define HC_CPIN     13U   /* AT+CPIN?  查卡状态             */
#define HC_CSQ      14U   /* AT+CSQ    信号强度             */
#define HC_CGATT    15U   /* AT+CGATT? 是否附着分组域       */
#define HC_CREG     16U   /* AT+CREG?  是否注册到网络       */
#define HC_CGATT1   17U   /* AT+CGATT=1 主动请求附着        */

#define HC1(a)          at_tx_c((u8)(a))
#define HC4(a,b,c,d)    HC1(a); HC1(b); HC1(c); HC1(d)

static void hcmd(u8 id)
{
    at_tx_reset();
    switch (id)
    {
    case HC_CONTYPE:
        HC4('A','T','+','S'); HC4('A','P','B','R'); HC4('=','3',',','1');
        HC4(',','"','C','O'); HC4('N','T','Y','P'); HC4('E','"',',','"');
        HC4('G','P','R','S'); HC1('"');
        break;
    case HC_APN:
        HC4('A','T','+','S'); HC4('A','P','B','R'); HC4('=','3',',','1');
        HC4(',','"','A','P'); HC4('N','"',',','"'); HC1('"');
        break;
    case HC_ACT:
        HC4('A','T','+','S'); HC4('A','P','B','R'); HC4('=','1',',','1');
        break;
    case HC_SAPQ:
        HC4('A','T','+','S'); HC4('A','P','B','R'); HC4('=','2',',','1');
        break;
    case HC_HTTPINIT:
        HC4('A','T','+','H'); HC4('T','T','P','I'); HC1('N'); HC1('I'); HC1('T');
        break;
    case HC_HTTPTERM:
        HC4('A','T','+','H'); HC4('T','T','P','T'); HC1('E'); HC1('R'); HC1('M');
        break;
    case HC_SSLON:
        HC4('A','T','+','H'); HC4('T','T','P','S'); HC4('S','L','=','1');
        break;
    case HC_SSLOFF:
        HC4('A','T','+','H'); HC4('T','T','P','S'); HC4('S','L','=','0');
        break;
    case HC_SSLCFG:
        HC4('A','T','+','S'); HC4('S','L','C','F'); HC4('G','=','"','s');
        HC4('e','c','l','e'); HC4('v','e','l','"'); HC1(',');
        HC1('1'); HC1('5'); HC1('3'); HC1(','); HC1('0');
        break;
    case HC_CID:
        HC4('A','T','+','H'); HC4('T','T','P','P'); HC4('A','R','A','=');
        HC1('"'); HC1('C'); HC1('I'); HC1('D'); HC1('"'); HC1(','); HC1('1');
        break;
    case HC_ACTION:
        HC4('A','T','+','H'); HC4('T','T','P','A'); HC4('C','T','I','O');
        HC1('N'); HC1('='); HC1('0');
        break;
    case HC_ATE0:
        HC4('A','T','E','0');
        break;
    case HC_CPIN:
        HC4('A','T','+','C'); HC1('P'); HC1('I'); HC1('N'); HC1('?');
        break;
    case HC_CSQ:
        HC4('A','T','+','C'); HC1('S'); HC1('Q');
        break;
    case HC_CGATT:
        HC4('A','T','+','C'); HC4('G','A','T','T'); HC1('?');
        break;
    case HC_CREG:
        /* AT+CGREG? —— 分组域注册状态（手册 p.163）；注意不是 AT+CREG?，
         *   后者报的是 CS 域，Cat.1 上"能打电话"不代表"能开数据"。 */
        HC4('A','T','+','C'); HC4('G','R','E','G'); HC1('?');
        break;
    case HC_CGATT1:
        HC4('A','T','+','C'); HC4('G','A','T','T'); HC1('='); HC1('1');
        break;
    default:
        break;
    }
}

/* 单条命令带重试（只重试超时，ERROR/CME 不重试）。形参 1+2=3 B，无参数块。 */
static at_resp_t hota_send(u8 id, u16 to_ms)
{
    static at_resp_t xdata r;
    static u8 xdata k;

    r = (at_resp_t)AT_RESP_TIMEOUT;
    for (k = 0U; k < 3U; k++)
    {
        hcmd(id);
        r = (at_resp_t)at_tx_mac(AT_EXP_NONE, to_ms);
        if (r != AT_RESP_TIMEOUT) return r;
    }
    return r;
}

/* 发送**调用方刚拼好**的 g_at_tx（URL / BREAK / BREAKEND / HTTPREAD）。
 *   形参 1+2=3 B。注意：重试就是原样重发，命令内容在 g_at_tx 里没被改动。 */
static at_resp_t hota_send_cur(u8 exp_id, u16 to_ms)
{
    static at_resp_t xdata r;
    static u8 xdata k;

    r = (at_resp_t)AT_RESP_TIMEOUT;
    for (k = 0U; k < 3U; k++)
    {
        r = (at_resp_t)at_tx_mac(exp_id, to_ms);
        if (r != AT_RESP_TIMEOUT) return r;
    }
    return r;
}

/* 收尾类命令（不关心结果、也不重试） */
static void hota_cmd_once(u8 id, u16 to_ms)
{
    hcmd(id);
    (void)at_tx_mac(AT_EXP_NONE, to_ms);
}

/*==============================================================================
 * ★★★ v0.6.5 分片下载：拼并下发 `AT+HTTPPARA="BREAK",<off>` /
 *                              `AT+HTTPPARA="BREAKEND",<end>`
 *
 *   依据：手册 §13.3（p.192）HTTPParamTag 表 ——
 *     "BREAK"    HTTP 方法 GET 的参数，整数型，从断点取数据，最小 0
 *     "BREAKEND" 与 BREAK 一起用：
 *                BREAKEND > BREAK  ⇒ 取 [BREAK, BREAKEND] 这一段
 *                两者都为 0        ⇒ 不续传（默认）
 *   ★ 必须**两个都设**：只设 BREAK 表示"从 BREAK 到文件尾"，而文件尾有
 *     23104 字节 > 3356 ⇒ 照样 602。BREAKEND 就是把响应封顶的关键。
 *
 *   ★ 字面量取址是铁律 7 的老坑（代码基址 0xFF0000 超 16 位，只搬 WORD0），
 *     所以命令一律在 xdata 里现拼（s_pcmd），标签用 sel 选、数字除 10 得。
 *   ★ 形参 1 + 4 = 5 B，不生成参数块（铁律 14）。
 *============================================================================*/
static at_resp_t hota_para_idx(u8 sel, u32 val)
{
    static u8  xdata lsel;
    static u32 xdata lval;

    /* ★★★ v0.6.15：形参**必须在第一句就拷进 static**（铁律 9），这是现场坐实的。
     *
     *   step65 桥接侧原文：
     *     AT+HTTPPARA="BREAK",0        -> OK      （0 是对的，它经得起踩）
     *     AT+HTTPPARA="BREAKEND",43    -> OK      （应该是 1023！）
     *   1023 变成 43，只可能是 `val` 在读之前被改了。而本函数下面要连打
     *   `at_tx_c()` 十几次（HC4 宏展开成 4 次调用），C251 把 5 字节形参放在
     *   寄存器/局部参数区，任何一次调用都可能覆盖它。
     *   ⇒ 先拷 static，之后只用 lval / lsel，形参再也不碰。
     *   ★ 这个 bug 一直是"隐形的"：旧模组那几轮根本没走到这里（更早就失败了），
     *     所以直到链路彻底修好（v0.6.14 零丢字节）它才第一次现形。
     */
    lsel = sel;
    lval = val;

    at_tx_reset();
    HC4('A','T','+','H'); HC4('T','T','P','P'); HC4('A','R','A','=');
    HC1('"'); HC4('B','R','E','A'); HC1('K');
    if (lsel != 0U)
    {
        HC1('E'); HC1('N'); HC1('D');
    }
    HC1('"'); HC1(',');
    at_tx_d(lval);

    return hota_send_cur(AT_EXP_NONE, AT_TO_CMD);
}

/* 捕获里有没有**真正的 IP**？
 *
 * ★★★ v0.6.15：旧判据是"捕获里出现 '"' 就算有 IP"，**这个判据是错的**，
 *   而且已经现场坐实（step65，v0.6.14 上板）：
 *
 *     [AT+SAPBR=2,1] rx=  +SAPBR: 1,3,"0.0.0.0"   OK
 *     [HOTA] bearer UP                       <- 判成"承载已起"！
 *     之后 601 无限循环（601 = 网络错误）
 *
 *   没插 SIM 时模组回的正是 `+SAPBR: 1,3,"0.0.0.0"` —— 引号齐全、IP 全零。
 *   只要"有引号"就算数，就会把"完全没有承载"读成"承载很好"，
 *   然后在一个不存在的网络上反复发 HTTPACTION，把日志和时间全耗掉。
 *
 *   判据（v0.6.16 定稿，同时满足）：
 *     · `+SAPBR` 的 <Status> == 1（手册 p.233：3 = 已经关闭）
 *     · 捕获里存在"四段 1~3 位数字、点分隔、至少一个非零数字"的形状
 *       （**不看引号** —— 手册示例 `+SAPBR:1,1,010.169.179.213` 就是无引号的）
 *   丢字节只会破坏形状 ⇒ 判成"没 IP"，方向永远偏保守。
 */
static u8 hota_sapbr_stat(void);   /* 前向声明：下面两个判据共用同一个解析器 */

static u8 hota_ip_seen(void)
{
    static u16 xdata i;
    static u8  xdata g;     /* 已数到第几段（点分隔） */
    static u8  xdata d;     /* 当前段里的数字个数     */
    static u8  xdata nz;    /* 整段 IP 里的非零数字数 */
    static u8  xdata ch;

    /* ── 前提：+SAPBR 的 <Status> 必须等于 1 ──────────────────────────────
     *   手册 p.233 原文：0=正在连接 1=已经连接 2=正在关闭 3=已经关闭。
     *   step66 拿到的是 3（已关闭）+ IP 全零 —— 两个判据独立地都说"没起来"。
     *   只要有一个不成立就不放行，这是"只能更保守"的方向。 */
    if (hota_sapbr_stat() != 1U) return 0U;

    /* ── 再要求字形上是一个"真 IPv4" ────────────────────────────────────
     *   ★★★ v0.6.16 修：旧版要求 IP 被双引号包住才算数，**这是错的**。
     *   合宙 8910 手册 p.90 / p.202 / p.245 三处示例全是**不带引号**的：
     *       ← +SAPBR:1,1,010.169.179.213
     *   （还带前导 0）。现场 step66 只见过出错时的 `"0.0.0.0"`（带引号），
     *   于是"带引号"这条判据从没被真 IP 检验过 —— 一旦真起来，它会判 0，
     *   我们就会在"承载明明是好的"情况下永远下载不了。这一版改成不看引号，
     *   只看**数字与点的形状**：四段、每段 1~3 位、至少一个非零数字。
     *   0.0.0.0 / 缺段 / 空串 一律不成立；丢字节只会破坏形状 ⇒ 判 0，安全。 */
    g  = 0U;
    d  = 0U;
    nz = 0U;

    for (i = 0U; i < g_at_capn; i++)
    {
        ch = (u8)g_at_cap[i];

        if (ch >= (u8)'0' && ch <= (u8)'9')
        {
            if (d < 3U)
            {
                d++;
            }
            else
            {
                g = 0U; d = 1U; nz = 0U;   /* 出现第 4 位数字：不是 IPv4，重来 */
            }
            if (ch != (u8)'0') nz++;
            continue;
        }

        if (ch == (u8)'.' && d != 0U)
        {
            g++;
            d = 0U;
            if (g > 3U) { g = 0U; d = 0U; nz = 0U; }
            continue;
        }

        /* 非数字非点 ⇒ 一个候选结束，就地判定 */
        if (g == 3U && d != 0U && nz != 0U) return 1U;
        g = 0U; d = 0U; nz = 0U;
    }

    if (g == 3U && d != 0U && nz != 0U) return 1U;
    return 0U;
}


/* 捕获里是不是 "+CME ERROR: 10"（SIM not inserted）？
 *
 *   v0.6.15 现场：没插卡时全流程只打出一句 `[AT+CPIN?] rc=2`，从日志上
 *   完全看不出"卡没插"这件事 —— 后面 601 反复刷 12 轮，我还在猜 TLS 和 Range。
 *   这条判据的唯一目的就是**把根因写进日志**。
 *
 *   判法：找相邻的 '1''0'，往前跳过空格直到遇到 ':' 才算。
 *   （CR/LF 在收尾已被映射成空格，所以 "+CME ERROR: 10" 中间全是空格。）
 *   对 `+SAPBR: 1,3,"0.0.0.0"`/`+CGATT: 0`/`+CCLK: "00/01/01,..."` 均不成立。
 */
static u8 hota_sim_missing(void)
{
    static u16 xdata i;
    static u16 xdata j;

    for (i = 0U; (u16)(i + 1U) < g_at_capn; i++)
    {
        if (g_at_cap[i] != '1' || g_at_cap[i + 1U] != '0') continue;
        j = i;
        while (j > 0U && g_at_cap[j - 1U] == ' ') j--;
        if (j > 0U && g_at_cap[j - 1U] == ':') return 1U;
    }
    return 0U;
}


/*============================================================================
 * ★★★ v0.6.16：三个"把模组状态读成数"的解析器
 *
 *   为什么非要现在加 —— step66 现场把整条流程的病灶暴露得很干净：
 *
 *     [AT+CPIN?]  +CPIN: READY        ← 卡在
 *     [AT+CGATT?] +CGATT: 0           ← 但没附着（距上行只隔 30 ms！）
 *     [AT+SAPBR=1,1] +CME ERROR: 3    ← 没附着就激活，必错
 *     [AT+SAPBR=2,1] +SAPBR: 1,3,"0.0.0.0"
 *     [HOTA] bearer UP                ← ★ 固件还是说"起来了"，然后 601 刷 12 轮
 *
 *   三个数各回答一个问题，缺一个就只能靠猜：
 *     CSQ 的 rssi  → 有没有信号（99 = 无信号，天线/馈线/位置）
 *     CGATT 的 n   → 分组域附着没有（附着是注网的**结果**，需要时间）
 *     SAPBR 的 stat→ 承载状态（1=已激活 3=错误，比数 IP 里的点更本质）
 *
 *   全部零形参、全部只读 g_at_cap，不新增跨调用存活量（铁律 9/11）。
 *   取不到就返回哨兵值（0xFF / 0xFFFF），**绝不猜成 1** —— 判据只能更保守。
 *============================================================================*/
#define HOTA_NA_U8    0xFFU
#define HOTA_NA_U16   0xFFFFU

/* "+CGATT: <n>" → n（0/1）；取不到 0xFF */
static u8 hota_cgatt_val(void)
{
    static u16 xdata i;

    for (i = 0U; (u16)(i + 6U) < g_at_capn; i++)
    {
        if (g_at_cap[i] != '+') continue;
        if (g_at_cap[i + 1U] != 'C' || g_at_cap[i + 2U] != 'G') continue;
        if (g_at_cap[i + 3U] != 'A' || g_at_cap[i + 4U] != 'T' ||
            g_at_cap[i + 5U] != 'T') continue;

        i = (u16)(i + 6U);
        while (i < g_at_capn && g_at_cap[i] != ':') i++;
        i++;
        while (i < g_at_capn && (g_at_cap[i] == ' ' || g_at_cap[i] == '\r')) i++;
        if (i < g_at_capn && g_at_cap[i] >= '0' && g_at_cap[i] <= '9')
        {
            return (u8)(g_at_cap[i] - '0');
        }
        return HOTA_NA_U8;
    }
    return HOTA_NA_U8;
}

/* "+CSQ: <rssi>,<ber>" → rssi（0..99）；取不到 0xFFFF */
static u16 hota_csq_rssi(void)
{
    static u16 xdata i;
    static u16 xdata v;

    for (i = 0U; (u16)(i + 4U) < g_at_capn; i++)
    {
        if (g_at_cap[i] != '+') continue;
        if (g_at_cap[i + 1U] != 'C' || g_at_cap[i + 2U] != 'S' ||
            g_at_cap[i + 3U] != 'Q') continue;

        i = (u16)(i + 4U);
        while (i < g_at_capn && g_at_cap[i] != ':') i++;
        i++;
        while (i < g_at_capn && (g_at_cap[i] == ' ' || g_at_cap[i] == '\r')) i++;
        if (!(i < g_at_capn && g_at_cap[i] >= '0' && g_at_cap[i] <= '9'))
        {
            return HOTA_NA_U16;
        }
        v = 0U;
        while (i < g_at_capn && g_at_cap[i] >= '0' && g_at_cap[i] <= '9')
        {
            v = (u16)(v * 10U + (u16)(g_at_cap[i] - '0'));
            i++;
        }
        return v;
    }
    return HOTA_NA_U16;
}

/* "+SAPBR: <cid>,<stat>,<ip>" → stat（0..3）；取不到 0xFF
 *   ★ 这个数比"有没有引号"硬得多：stat 是模组自己给的结论。
 *     1 = activated，3 = error（step66 就是 3）。 */
static u8 hota_sapbr_stat(void)
{
    static u16 xdata i;

    for (i = 0U; (u16)(i + 6U) < g_at_capn; i++)
    {
        if (g_at_cap[i] != '+') continue;
        if (g_at_cap[i + 1U] != 'S' || g_at_cap[i + 2U] != 'A' ||
            g_at_cap[i + 3U] != 'P' || g_at_cap[i + 4U] != 'B' ||
            g_at_cap[i + 5U] != 'R') continue;

        i = (u16)(i + 6U);
        while (i < g_at_capn && g_at_cap[i] != ':') i++;
        i++;
        while (i < g_at_capn && g_at_cap[i] != ',') i++;    /* 跳过 cid */
        i++;
        while (i < g_at_capn && (g_at_cap[i] == ' ' || g_at_cap[i] == '\r')) i++;
        if (i < g_at_capn && g_at_cap[i] >= '0' && g_at_cap[i] <= '9')
        {
            return (u8)(g_at_cap[i] - '0');
        }
        return HOTA_NA_U8;
    }
    return HOTA_NA_U8;
}

/* "+CGREG: <n>,<stat>" → stat（1=本地网 5=漫游）；取不到 0xFF。
 *   只在附着失败时才问，用来分开"没信号"和"有信号但开不了数据"。
 *   ★ 用 CGREG 不用 CREG：Cat.1 上数据业务看分组域，CS 域注册说明不了 PDN。 */
static u8 hota_creg_stat(void)
{
    static u16 xdata i;
    static u16 xdata j;

    for (i = 0U; (u16)(i + 6U) < g_at_capn; i++)
    {
        if (g_at_cap[i] != '+') continue;
        if (g_at_cap[i + 1U] != 'C' || g_at_cap[i + 2U] != 'G' ||
            g_at_cap[i + 3U] != 'R' || g_at_cap[i + 4U] != 'E' ||
            g_at_cap[i + 5U] != 'G') continue;

        i = (u16)(i + 6U);
        while (i < g_at_capn && g_at_cap[i] != ':') i++;
        i++;
        while (i < g_at_capn && (g_at_cap[i] == ' ' || g_at_cap[i] == '\r')) i++;

        j = i;                                   /* 只有一个字段时的回退点 */
        while (i < g_at_capn && g_at_cap[i] != ',') i++;
        if (i < g_at_capn)                       /* 有逗号 ⇒ 取第二个字段 */
        {
            i++;
            while (i < g_at_capn && (g_at_cap[i] == ' ' || g_at_cap[i] == '\r')) i++;
        }
        else
        {
            i = j;
        }

        if (i < g_at_capn && g_at_cap[i] >= '0' && g_at_cap[i] <= '9')
        {
            return (u8)(g_at_cap[i] - '0');
        }
        return HOTA_NA_U8;
    }
    return HOTA_NA_U8;
}

/*============================================================================
 * PDP 激活 —— v0.6.16 重写成"状态机"，每一步都要一个数才往下走
 *
 *   step66 现场（v0.6.15 上板）把顺序错误钉死了：
 *     CPIN READY  →(30 ms)→  CGATT: 0  →  SAPBR=1,1 CME 3  →  601 × 12 轮
 *   卡是好的、线是干净的，唯一的问题是**我们从来没等过模组注网**。
 *   这一版把流程拆成四步，每步失败都打出"是哪一步、什么数"：
 *     ① CPIN  → 卡在不在（CME 10 = 没插卡）
 *     ② CSQ   → 有没有信号（rssi 99 = 无信号 / 天线 / 位置）
 *     ③ CGATT → 附着没有（**轮询 10 次 × 2 s**，不再问一次就下结论）
 *     ④ SAPBR → 承载（stat==1 且有真 IP 才算 UP）
 *
 * ★★ v0.6.2 重写（判据来自 step56 上板日志，此前两次都错在这里）
 *
 *   现场事实（同一时刻桥接日志 vs MCU 自己的 rx）：
 *     桥接侧  AT+SAPBR=3,1,"CONTYPE","GPRS" -> OK
 *     MCU 侧  rx="ATSPR31\"OTP\"\"PS  O "        ← K 被吃掉
 *     ⇒ v0.6.1 在这一条就判 sapbr contype rc=4 并直接返回，
 *       而**同一轮里 AT+SAPBR=2,1 已经拿到了 10.75.34.102**。
 *       也就是说：承载一直是好的，是"设置命令的 OK 没活下来"把流程掐死了。
 *
 *   ⇒ 新纪律（与"不许拿读代码代替硬件证据"配套）：
 *     ① 三条设置命令**全部幂等**，成败一律不作为门槛，只记录；
 *     ② 生死只由 "有没有拿到带 IP 的那一行" 决定，而且判 3 次取并集；
 *     ③ 判据不用 OK，用那一行里独有的字符 '"'（见 hota_ip_seen）。
 *   这样即使线在丢 50% 字节，"承载是否真的起来"依然可判。
 *===========================================================================*/
static ota_ret_t hota_pdp_up(void)
{
    static at_resp_t xdata r;
    static u8 xdata k;
    static u8 xdata up;
    static u8 xdata att;
    static u16 xdata rssi;

    /* 这条线在丢字节 ⇒ PDP 阶段开宽松判据（原理见 at_set_lenient 注释）。
     *   HTTPACTION / HTTPREAD 走前缀匹配，不受它影响。 */
    at_set_lenient(1U);

    /* ── ① 卡：在不在 ───────────────────────────────────────────────────
     *
     *   step65 现场：`+CME ERROR: 10`（SIM not inserted），之后
     *   `+SAPBR: 1,3,"0.0.0.0"` → 601 循环 12 轮、几十秒，日志里没有一个字
     *   提到"卡"。这一条把根因钉在日志第一行，省掉整轮盲目排查。
     *   判据严格（必须 literally `: 10`），拿不准就不判死，继续走老路。 */
    r = hota_send(HC_CPIN, AT_TO_CMD);
    if (r == AT_RESP_CME && hota_sim_missing() != 0U)
    {
        LOG_E0("HOTA", "SIM not inserted (CME 10) -> stop here");
        return OTA_ERR_NET;
    }

    /* ── ② 信号：rssi 是多少 ──────────────────────────────────────────────
     *
     *   ★ 这一条是本版**最重要的一行日志**。没有它，"CGATT=0" 到底是
     *     "还没注上"、"没天线"、"卡没数据业务"三者无法分开，只能猜。
     *   ★ 判据方向：99 = 无信号（GSM 规范固定值），不是 0 dBm。
     *     取不到（模组不应答）打 rc，绝不默认成"有信号"。 */
    r = hota_send(HC_CSQ, AT_TO_CMD);
    rssi = hota_csq_rssi();
    if (rssi == HOTA_NA_U16)
    {
        LOG_I1("HOTA", "csq rc=%u (no answer)", (u16)r);
    }
    else
    {
        /* 99 单独标出来 —— 现场一眼能认出"没信号"这四个字 */
        LOG_I1("HOTA", "csq rssi=%u (99=no signal)", rssi);
    }

    /* ── ③ 附着：CGATT 需要时间，问一次必为 0 ────────────────────────────
     *
     *   step66 实测：`+CPIN?` 与 `+CGATT?` 之间只隔 **30 ms**（52.655 → 52.685）。
     *   Cat4 注网要几秒到几十秒，这个时序下 CGATT 恒为 0，
     *   后面的 SAPBR=1,1 必然 CME ERROR: 3 —— 12 轮 601 全长在这个错上。
     *
     *   所以这里改成**轮询**：10 次 × 2 s = 20 s 上限。
     *   ★ 用 at_tx_mac 直接发、不走 hota_send 的 3 次重试 ——
     *     20 s 的预算要留给模组，不能耗在同一条命令的重发上。
     *   ★ 每轮都喂狗（at_delay_ms 内含），20 s 不会撞看门狗。 */
    att = HOTA_NA_U8;

    /* ★ 先**主动请求**一次附着：个别 Air724UG 出厂态停在 detached，
     *   不收到 CGATT=1 就一直不动。这条幂等（已附着时回 OK），不判死。 */
    r = hota_send(HC_CGATT1, AT_TO_CMD);
    if (!HOTA_OK(r)) LOG_W1("HOTA", "cgatt=1 rc=%u (poll anyway)", (u16)r);

    for (k = 0U; k < 10U; k++)
    {
        hcmd(HC_CGATT);
        (void)at_tx_mac(AT_EXP_NONE, AT_TO_CMD);
        att = hota_cgatt_val();

        LOG_I2("HOTA", "att %u/10 = %u", (u16)(k + 1U), (u16)att);

        if (att == 1U) break;
        at_delay_ms(2000U);
    }

    if (att != 1U)
    {
        /* 附着失败 ⇒ 再问一次注册状态，把"没信号"和"有信号但开不了数据"分开 */
        hcmd(HC_CREG);
        (void)at_tx_mac(AT_EXP_NONE, AT_TO_CMD);
        LOG_I1("HOTA", "cgreg stat=%u (1=home 2=search 3=deny)", (u16)hota_creg_stat());
        LOG_E0("HOTA", "no attach in 20 s -> RF/SIM data, NOT firmware");
        return OTA_ERR_NET;
    }

    /* ── ④ 承载：设置类幂等，生死看 SAPBR 的 stat + 真 IP ───────────────── */
    r = hota_send(HC_CONTYPE, AT_TO_CMD);
    if (!HOTA_OK(r)) LOG_W1("HOTA", "contype rc=%u (ignore)", (u16)r);

    /* 空串 APN：模组自己按注册到的网络取（手册 p.201 原文） */
    r = hota_send(HC_APN, AT_TO_CMD);
    if (!HOTA_OK(r)) LOG_W1("HOTA", "apn rc=%u (ignore)", (u16)r);

    /* ★ v0.6.14：Air724UG 是 Cat4，PDP 激活也慢，单独给 30 s */
    r = hota_send(HC_ACT, 30000U);
    if (!HOTA_OK(r)) LOG_W1("HOTA", "act rc=%u (maybe up)", (u16)r);

    /* ── ⑤ 生死判据 ──────────────────────────────────────────────────────
     *
     *   ★★★ v0.6.16 删掉了一行**必须删**的兜底：
     *
     *       if ((g_at_capn >= 6U) && HOTA_OK(r)) { up = 1U; break; }   ← 已删除
     *
     *   step66 现场：`+SAPBR: 1,3,"0.0.0.0"`（stat=3 = error，IP 全零），
     *   g_at_capn=28 ≥ 6 且 rc=0 ⇒ 这行判 up=1 ⇒ 打出 "bearer UP"，
     *   然后 601 刷了 12 轮、3 分半。v0.6.15 精心写的 hota_ip_seen()
     *   被这一行**完全绕过**。
     *   ⇒ 承载的唯一判据 = hota_ip_seen()（排除 0.0.0.0）。
     *     少判一次不要紧（后面还有 60 s 重试），误判一次要 3 分钟。 */
    up = 0U;
    for (k = 0U; k < 3U; k++)
    {
        hcmd(HC_SAPQ);
        r = (at_resp_t)at_tx_mac(AT_EXP_NONE, AT_TO_NET);

        LOG_I3("HOTA", "bearer q%u rc=%u stat=%u",
               (u16)(k + 1U), (u16)r, (u16)hota_sapbr_stat());
        LOG_I1("HOTA", "  ip=%s", g_at_cap);

        if (hota_ip_seen() != 0U) { up = 1U; break; }
        at_delay_ms(300U);
    }

    if (up == 0U)
    {
        LOG_E0("HOTA", "no bearer ip (stat!=1 or 0.0.0.0) -> cannot download");
        return OTA_ERR_NET;
    }
    LOG_I0("HOTA", "bearer UP");
    return OTA_OK;
}

/*==============================================================================
 * 对外：承载激活（v0.7.0）
 *
 *   只做转发，不复制任何判据 —— PDP 这套逻辑的每一行都是上板日志换来的
 *   （见 hota_pdp_up 上方的 v0.6.14 / v0.6.16 记录），绝不允许出现第二份。
 *
 *   为什么需要它：MQTT 这条路（APP_HTTP_OTA_TEST=0 时的唯一主路径）在
 *   MCONFIG 之前也必须联网，而"联网"= 有活动 PDP 承载。见 app_mqtt.h 的
 *   MQ_ST_PDP 说明。
 *============================================================================*/
ota_ret_t http_ota_pdp_up(void)
{
    return hota_pdp_up();
}

/*==============================================================================
 * HTTPS 开关（依据：手册 p.146 参数定义 + p.203 流程示例）
 *
 *   ★ 为什么必须加：现在能拿到手的"文件直链"几乎全是 https://
 *     （GitHub raw / catbox / 各家对象存储 / 网盘…），
 *     **公开的 http:// 静态二进制直链已经很难找了** —— 没有这条路，
 *     整个 OTA 验证会卡在"没有可用 URL"上，与固件无关。
 *
 *   ★ 手册 p.146 明确 <seclevel> 0 = "No authentication"
 *     ⇒ 不需要往模组文件系统里写 CA 证书（省掉 AT+FSCREATE / FSWRITE /
 *        SSLCFG="cacert" 那一大套），两条命令就够：
 *           AT+HTTPSSL=1
 *           AT+SSLCFG="seclevel",153,0
 *     153 = HTTP 功能固定的 SSL 上下文 id（手册 p.145）。
 *
 *   ★ 代价（必须记住）：不验证服务器证书 ⇒ 挡不住中间人。
 *     **测试期专用**；量产必须装 CA 并把 seclevel 改 1 或 2。
 *
 *   ★ 顺序按手册 p.203：HTTPINIT 之后、HTTPPARA 之前。
 *   ★ 非 https 也要显式发 AT+HTTPSSL=0：否则上一次会话开过的 SSL 会残留，
 *     下一条 http:// 链接会莫名握手失败。
 *   ★ 无参数：不生成 ?BYTE 参数块（铁律 14）。
 *============================================================================*/
static ota_ret_t hota_ssl_setup(void)
{
    /* ★ v0.6.3：全部改 static —— 铁律 5/9，本工程不许把跨调用还活着的量留在寄存
     *   器或 overlay 区。这里的 https 决定走哪条分支，绝不能是"碰巧对"。 */
    static at_resp_t xdata r;
    static u8   xdata https;

    https = 0U;
    if (s_url[0] == 'h' && s_url[1] == 't' && s_url[2] == 't' &&
        s_url[3] == 'p' && s_url[4] == 's')
    {
        https = 1U;
    }

    if (https != 0U)
    {
        /* ★★★ v0.6.11：改正走零指针通道（hcmd + at_tx_mac）。
         *   v0.6.3 曾把这条改成"逐字节填 xdata 再传指针"，救了这个调用点，
         *   但没救到别的（step62 里 contype/apn/act/seclevel/ping 全被拒），
         *   因为病根是**指针当形参跨调用**，不是字符串放在哪儿。 */
        r = hota_send(HC_SSLON, AT_TO_CMD);
        if (!HOTA_OK(r))
        {
            LOG_W1("HOTA", "httpssl on rc=%u (continue)", (u16)r);
        }

        r = hota_send(HC_SSLCFG, AT_TO_CMD);
        if (!HOTA_OK(r))
        {
            /* 拿不到也不能直接判死：有的固件默认就是 0 */
            LOG_W1("HOTA", "seclevel rc=%u", (u16)r);
        }
        LOG_I0("HOTA", "HTTPS mode, cert NOT verified");
    }
    else
    {
        r = hota_send(HC_SSLOFF, AT_TO_CMD);
        if (!HOTA_OK(r)) LOG_W1("HOTA", "httpssl off rc=%u", (u16)r);
    }

    return OTA_OK;
}

/*==============================================================================
 * ★ v0.6.7：把"HTTPINIT 之后的 PARA 设置"抽出来，start 与 recover 共用
 *
 *   为什么必须共用而不是各写一份：这段拼的是 URL（一笔一划 12 个字节的字面量
 *   前缀 + URL 原文），在本工程的历史上，凡是"命令字符串在两处各写一遍"的
 *   地方最后都漂了（AT+HTTPSSL 那次 0xFF 垃圾就是字面量取址的坑）。
 *   抽成一个函数之后，recover 走的就是**和第一次完全相同**的那条路径，
 *   不存在"重连成功但参数没设对"这种只在第 3 小时才暴露的错。
 *============================================================================*/
static ota_ret_t hota_setup_params(void)
{
    static u16 xdata i;
    at_resp_t r;

    /* 4) CID=1 —— ★ v0.6.16 按手册 p.246 的顺序，**CID 在 URL 之前**
     *    （手册原文：AT+HTTPPARA="CID",1 → AT+HTTPPARA="URL","https://..."）。
     *    两条都是纯参数设置，功能上先后无所谓，但既然手册给了确定顺序，
     *    就照抄 —— 免费去掉一个"万一 8910 是先建会话再补参数"的变量。 */
    r = hota_send(HC_CID, AT_TO_CMD);
    if (!HOTA_OK(r))
    {
        LOG_E1("HOTA", "http cid rc=%u", (u16)r);
        return OTA_ERR_NET;
    }

    /* 5) URL —— 前缀用立即数拼，URL 正文来自 s_url（**静态数组**，
     *    编译器给的是常量地址，不经过任何指针形参，所以不会出现 step62 那种
     *    "发出去只剩尾巴"的病）。 */
    at_tx_reset();
    HC4('A','T','+','H'); HC4('T','T','P','P'); HC4('A','R','A','=');
    HC1('"'); HC1('U'); HC1('R'); HC1('L'); HC1('"'); HC1(',');
    HC1('"');
    for (i = 0U; s_url[i] != 0 && g_at_txn < (AT_TX_MAX - 4U); i++)
    {
        at_tx_c((u8)s_url[i]);
    }
    HC1('"');

    r = hota_send_cur(AT_EXP_NONE, AT_TO_CMD);
    if (!HOTA_OK(r))
    {
        LOG_E1("HOTA", "http url rc=%u", (u16)r);
        return OTA_ERR_NET;
    }
    return OTA_OK;
}

/*==============================================================================
 * 开始下载
 *============================================================================*/
ota_ret_t http_ota_start(u32 start_offset, u32 expect_size)
{
    at_resp_t r;

    if (!s_inited) return OTA_ERR_STATE;
    if (s_state != HOTA_IDLE) return OTA_ERR_BUSY;

    /* 1) PDP 承载激活：没有 IP，下面三条命令全是空转（手册 p.201） */
    if (hota_pdp_up() != OTA_OK)
    {
        LOG_E0("HOTA", "pdp up FAIL (no IP) -> cannot download");
        s_err = OTA_ERR_NET; s_state = HOTA_FAIL;
        return OTA_ERR_NET;
    }

    /* 2) HTTPINIT —— ★★★ v0.6.12 改成**幂等**，不再当闸门
     *
     *   现场证据（step63 桥接侧，模组真正收到的/回的）：
     *     08:55:30.112  AT+HTTPINIT  ->  OK
     *     08:55:32.115  AT+HTTPINIT  ->  +CME ERROR: 3
     *     08:55:34.117  AT+HTTPINIT  ->  +CME ERROR: 3
     *   而 MCU 侧那一行是 `httpinit rc=4` —— 它**只看到半条 OK**，
     *   判超时 → 外层 60 s 后整包重来 → 再发一次 HTTPINIT → 模组回 CME ERROR: 3
     *   （会话已经开着，"operation not allowed"）→ 又判死。
     *   ⇒ 这是一个**重试自己把路堵死**的死循环：第一次"看起来失败"其实成功了，
     *     之后每一次真失败。
     *
     *   治法（与 PDP 那套"证据优先"同一纪律）：
     *     ① 发 HTTPINIT 之前先发一条 HTTPTERM 把状态收干净（没开会话就回 ERROR，
     *        忽略即可）。多花约 30~100 ms，换"每次进来状态都一样"。
     *     ② HTTPINIT 的返回值**只记录、不判死**。真没初始化成功的话，
     *        紧随其后的 URL / CID 必然失败，那里才是生死判据。
     */
    hota_cmd_once(HC_HTTPTERM, 800U);

    /* 3) ★★★ v0.6.16：HTTPS 开关**挪到 HTTPINIT 之前**
     *
     *   依据是合宙 8910 平台手册 p.245~246 的 HTTPS 完整流程（逐行抄下来的顺序）：
     *     AT+SAPBR=2,1  → +SAPBR:1,1,010.169.179.213
     *     AT+HTTPSSL=1                     ← 先开 SSL
     *     AT+SSLCFG="seclevel",153,2
     *     AT+HTTPINIT                      ← 再初始化协议栈
     *     AT+HTTPPARA="CID",1
     *     AT+HTTPPARA="URL","https://..."
     *   v0.6.15 及以前是 HTTPINIT → HTTPSSL → SSLCFG（SIMCom 的写法）。
     *   两个平台在这一步的顺序不同，而 8910 上"先 INIT 后开 SSL"可能让
     *   后面 HTTPACTION 拿不到 SSL 上下文 ⇒ 表现为 601，不是证书问题。
     *   ⇒ 按手册走，先 HTTPSSL/SSLCFG，再 HTTPINIT。
     *   ★ HTTPTERM 仍在最前面：SSL 开关是**配置**，不属于会话状态，
     *     放在 TERM 之后设置不会被清掉。 */
    if (hota_ssl_setup() != OTA_OK)
    {
        LOG_E0("HOTA", "ssl setup FAIL");
        s_err = OTA_ERR_NET; s_state = HOTA_FAIL;
        hota_cmd_once(HC_HTTPTERM, AT_TO_CMD);
        return OTA_ERR_NET;
    }

    r = hota_send(HC_HTTPINIT, AT_TO_CMD);
    if (!HOTA_OK(r))
    {
        LOG_W1("HOTA", "httpinit rc=%u (not fatal)", (u16)r);
    }

    /* 4)+5) URL / CID（与 recover 共用同一段，见 hota_setup_params） */
    if (hota_setup_params() != OTA_OK)
    {
        s_err = OTA_ERR_NET; s_state = HOTA_FAIL;
        hota_cmd_once(HC_HTTPTERM, AT_TO_CMD);
        return OTA_ERR_NET;
    }

    /* ★★ v0.6.7：**真正使用 start_offset**（此前这一行写的是 `s_offset = 0UL;`
     *   ⇒ 形参白传，续传永远从 0 开始，这是本轮修掉的一个真 bug）。
     *   对齐到片边界：s_offset 只会在整片写成功后前进，所以调用方给的
     *   断点必然已经是 1024 的整数倍；这里再对齐一次，防的是"param 里存了
     *   一个半片值"这种脏数据 —— 对齐只会让重下的字节变多，绝不会写错位置。
     *   ★ 顺带把 s_total 归零：真实总长要等第一片到手、解析镜像头才知道。 */
    s_offset = start_offset & ~((u32)HTTP_READ_CHUNK - 1UL);
    s_total  = expect_size;

    /* ★★★ v0.6.18：续传前必须已经知道"数据体头部有几个无关字节"
     *   那个值是靠第 0 片的 `OTAI` 魔数位置量出来的；从非 0 断点续传时
     *   手上没有魔数，量不了 ⇒ 若此时还不知道，就干脆从头下。
     *   代价最多 23 片（约十几秒），换来的是"绝不把整片前移 k 字节写进
     *   Flash"——CRC 只在最后才暴露，那时已经白跑一整轮。 */
    if ((s_offset != 0UL) && (s_lead_ok == 0U))
    {
        LOG_W1("HOTA", "resume %lu but lead unknown -> restart at 0", s_offset);
        s_offset = 0UL;
    }

    s_state = HOTA_REQ;
    /* ★ v0.6.5 分片状态显式清零（铁律 13：static 初值不可信，必须亲手写） */
    s_chunk_got  = 0U;
    s_chunk_no   = (u16)(s_offset / (u32)HTTP_READ_CHUNK);
    s_chunk_last = 0U;
    s_try        = 0U;
    /* ★ v0.6.7 自愈计数清零 */
    s_recover    = 0U;
    s_restart    = 0U;
    if (s_offset != 0UL)
    {
        LOG_I1("HOTA", "resume at offset %lu", s_offset);
    }
    return OTA_OK;
}

/*==============================================================================
 * ★★★ v0.6.7 断线自愈：重建 AT/PDP/HTTP 会话，从 s_offset 续传
 *
 *   返回 1 = 会话已重建（调用方回 HOTA_REQ 继续取片）；0 = 这次没救回来。
 *
 *   为什么要等模组而不是直接重发 AT：Y100P 重启要 ~3 s 才吐完开机日志、
 *   ~10 s 才注网。这期间发什么都超时，而且**越急越乱**——旧代码每条命令
 *   20 s 超时叠起来，一轮下来看门狗就复位了。所以这里先"敲门 + 等"，
 *   敲到再谈后面的事。
 *============================================================================*/
static u8 hota_reopen(void)
{
    static u8  xdata i;
    static u8  xdata ok;
    static at_resp_t xdata r;

    /* 1) 先把可能残留的会话收掉。模组真重启过的话这条必然失败，忽略即可 ——
     *    它的作用是"万一只是网络掉了、会话还在"，收干净才不会 CME ERROR: 3。 */
    hota_cmd_once(HC_HTTPTERM, 800U);

    /* 2) 等模组回来：最多 12 次 × 600 ms ≈ 7 s（覆盖重启 + 注网） */
    ok = 0U;
    for (i = 0U; i < 12U; i++)
    {
        /* v0.7.0：原来这里是 at_ping(600U) == AT_PROBE_OK。
         *   at_ping 属于"物理层取证"那一套，已随 AT_DIAG_LEGACY=0 下线
         *   （APP 48 KB 代码区放不下，见 uart_at.h）。
         *   这里要的判据只有一条 —— "发 AT 能拿回 OK" —— 用零指针通路
         *   自己发即可，语义与 at_ping 完全一致（rc=0），不经过任何形参块。
         *   ★ 必须走 at_tx_c() 逐个拼 + at_send_cur()，不能把字面量指针传下去。 */
        /* ★★★ v0.7.3：必须先 at_tx_reset()。
         *   上一句 hota_cmd_once(HC_HTTPTERM) 把 "AT+HTTPTERM" 留在 g_at_tx 里，
         *   而 at_tx_mac 发完**不清缓冲**（清不清是调用方的事）。少了这一句，
         *   线上发出去的就是 "AT+HTTPTERM"+"AT" = "AT+HTTPTERMAT"，再下一轮
         *   "AT+HTTPTERMATAT"…… 现场日志里那 100 多行 +CME ERROR: 58 就是它。 */
        at_tx_reset();
        at_tx_c('A');
        at_tx_c('T');
        if (at_tx_mac(AT_EXP_NONE, 600U) == AT_RESP_OK) { ok = 1U; break; }
    }
    if (ok == 0U)
    {
        LOG_E0("HOTA", "modem silent, reopen failed");
        return 0U;
    }
    LOG_I1("HOTA", "modem back after %u pings", (u16)(i + 1U));

    /* 3) 重启会把回显恢复成 ATE1（默认值），必须重新关掉，否则线上字节翻倍 */
    hota_cmd_once(HC_ATE0, AT_TO_CMD);

    /* 4) 重新起承载：重启后 PDP 一定是掉光的 */
    if (hota_pdp_up() != OTA_OK)
    {
        LOG_E0("HOTA", "reopen: pdp up failed");
        return 0U;
    }

    /* 5) HTTP 会话三步（顺序与 http_ota_start 完全一致）
     *    ★ v0.6.12：HTTPINIT 同样**不判死**（理由见 http_ota_start 第 2 步）。
     *    第 1 步已经发过 HTTPTERM，所以这里的 HTTPINIT 是"从干净状态开始"，
     *    失败只可能有两种：模组没回（下面 URL 也会失败）或命令被丢字节吃掉。
     *    ★ v0.6.16：SSL 挪到 HTTPINIT 之前（手册 p.245 顺序），两处必须一样。 */
    if (hota_ssl_setup() != OTA_OK) return 0U;
    r = hota_send(HC_HTTPINIT, AT_TO_CMD);
    if (!HOTA_OK(r))
    {
        LOG_W1("HOTA", "reopen: httpinit rc=%u (not fatal)", (u16)r);
    }
    if (hota_setup_params() != OTA_OK)
    {
        hota_cmd_once(HC_HTTPTERM, AT_TO_CMD);
        return 0U;
    }
    return 1U;
}

/*==============================================================================
 * ★★★ v0.6.7 失败统一出口：先想办法救，救不动才判死
 *
 *   reason 是给人看的数字（日志里 `reason=N`），取值见下表 —— 有了它，
 *   "这一版是被谁打死的"不用再靠上下文去推：
 *     1=BREAK 失败  2=BREAKEND 失败  3=HTTPACTION 无应答
 *     4=HTTP 状态码异常（含 602）     5=+HTTPREAD 无应答
 *     6=短读重取 3 次仍短             7=本片期望长度为 0
 *
 *   ★ Flash 写失败不走这里：那是本地硬件问题，重连一万次也没用。
 *============================================================================*/
static http_ota_state_t hota_recover_or_fail(ota_ret_t err, u8 reason)
{
    s_err = err;

    if (s_recover >= HOTA_RECOVER_MAX)
    {
        /* 重连额度用完 ⇒ 整包重下（"重新下载"这条退路） */
        if (s_restart < HOTA_RESTART_MAX)
        {
            s_restart++;
            s_recover = 0U;
            s_offset  = 0UL;
            s_chunk_no = 0U;
            s_total   = 0UL;
            LOG_W2("HOTA", "restart whole image #%u (reason=%u)",
                   (u16)s_restart, (u16)reason);
            if (flash_iap_erase_range(OTA_STAGE_BASE, OTA_STAGE_SIZE) != OTA_OK)
            {
                LOG_E0("HOTA", "restart: erase STAGING failed");
                s_state = HOTA_FAIL;
                return HOTA_FAIL;
            }
            s_state = HOTA_RECOVER;
            return HOTA_RECOVER;
        }
        LOG_E2("HOTA", "give up: %u reruns used (reason=%u)",
               (u16)s_restart, (u16)reason);
        s_state = HOTA_FAIL;
        return HOTA_FAIL;
    }

    s_recover++;
    LOG_W3("HOTA", "recover #%u from off=%lu reason=%u",
           (u16)s_recover, s_offset, (u16)reason);
    s_state = HOTA_RECOVER;
    return HOTA_RECOVER;
}

/*==============================================================================
 * 引擎推进（每 <= 20ms 调一次，内部可能阻塞）
 *============================================================================*/
http_ota_state_t http_ota_step(void)
{
    /* ★ v0.6.11：cmd[24] / line[64] / dbuf[6] / dl / tv / k 全部删除 ——
     *   命令改拼进 g_at_tx（零指针通道），应答原文在 g_at_cap，
     *   这一版把 http_ota 里的临时缓冲从 96 B 降到 0，全部补给 uart_at。 */
    static u16  xdata n;
    static u16  xdata got;
    static u16  xdata want;
    static u16  xdata i;
    static u8   xdata lead;      /* v0.6.18 首片实测的前导字节数 */
    static u8   xdata atry;      /* v0.6.2 HTTPACTION 重试计数 */
    at_resp_t r;

    if (!s_inited) return HOTA_FAIL;

    /*==========================================================================
     * ★★★ v0.6.7 恢复态：模组重启 / 掉线之后的唯一入口
     *
     *   这里**不取片**，只负责把会话重新立起来：
     *     等模组应答 → ATE0 → PDP → HTTPINIT/SSL/URL/CID
     *   成功就回 HOTA_REQ，从 s_offset 继续；失败就再走一次本状态
     *   （每次 hota_reopen 内部已经等 ~7 s，所以不会变成忙等）。
     *   ★ 计时/超时交给上层的重连计数管，这里不做次数判断 —— 那件事
     *     统一在 hota_recover_or_fail() 里做，免得两处各数一套。
     *========================================================================*/
    if (s_state == HOTA_RECOVER)
    {
        if (hota_reopen() != 0U)
        {
            s_try = 0U;
            s_state = HOTA_REQ;
            LOG_I1("HOTA", "link back, resume at %lu", s_offset);
            return HOTA_REQ;
        }
        /* 这一轮没救回来：记一次账（可能触发整包重下或最终失败） */
        return hota_recover_or_fail(OTA_ERR_NET, 8U);
    }

    if (s_state == HOTA_REQ)
    {
        /*======================================================================
         * ★★★ v0.6.5：每一片都重新设置 Range，再 GET
         *
         *   为什么要重设：HTTPACTION 每次都是"发起一次新的 GET"，
         *   BREAK/BREAKEND 只是 HTTPPARA 里的当前值 ⇒ 片与片之间必须改。
         *   顺序也不能反：BREAK 先、BREAKEND 后（手册把 BREAKEND 定义成
         *   "和 BREAK 一起用"，两个都设才封住响应长度）。
         *====================================================================*/
        /* ★ v0.6.14 备注：换 Air724UG 之后这一条是**最可能的卡点** ——
         *   合宙的 HTTPPARA 文档确实列了 BREAK/BREAKEND，但它们依赖服务器的
         *   Range 支持；若这里返回 ERROR/CME，就是模组不吃这个参数，
         *   日志里 `break rc=` 会直接指出来，届时改走"一次 GET 全量 + 分段 READ"
         *   （724 的 HTTPREAD 缓冲比 3356 大得多，整包 23104 B 能装下）。
         *   现在不改结构，是因为"模组不支持"还没有任何证据。 */
        r = hota_para_idx(0U, s_offset);                              /* BREAK    */
        if (!HOTA_OK(r))
        {
            LOG_E2("HOTA", "break rc=%u off=%lu (no Range?)", (u16)r, s_offset);
            return hota_recover_or_fail(OTA_ERR_NET, 1U);
        }
        r = hota_para_idx(1U, s_offset + (u32)HTTP_READ_CHUNK - 1UL); /* BREAKEND */
        if (!HOTA_OK(r))
        {
            LOG_E2("HOTA", "breakend rc=%u off=%lu", (u16)r, s_offset);
            return hota_recover_or_fail(OTA_ERR_NET, 2U);
        }

        /* 发 HTTPACTION，等 +HTTPACTION URC */
        s_expect_len = 0UL;
        s_status_code = 0U;

        for (atry = 0U; atry < 2U; atry++)
        {
            hcmd(HC_ACTION);
            r = (at_resp_t)at_tx_mac(AT_EXP_HTTPACTION, AT_TO_HTTP_ACTION);
            if (r == AT_RESP_PREFIX) break;
            /* ★ v0.6.13：gap 删了（它是模组沉默时间，不是固件慢的证据）。
             *   改打"实际收到多少字节"—— 这一版热路径重写是否见效，全看这个数：
             *   HTTPACTION 的应答约 26 字节，n≈26 = 收全了，n≈10 = 还在丢。 */
            LOG_W3("HOTA", "action try=%u rc=%u n=%u",
                   (u16)(atry + 1U), (u16)r, g_at_capn);
            if (r == AT_RESP_ERROR) break;
            at_delay_ms(500U);
        }
        if (r == AT_RESP_PREFIX)
        {
            parse_httpaction();
            LOG_I3("HOTA", "chunk %u code=%u len=%lu",
                   s_chunk_no, s_status_code, s_expect_len);

            /* 416 = 请求区间越界 ⇒ 上一片就是最后一片，正常收尾 */
            if (s_status_code == 416U)
            {
                hota_cmd_once(HC_HTTPTERM, AT_TO_CMD);
                s_state = HOTA_DONE;
                return HOTA_DONE;
            }
            if (s_status_code != 200U && s_status_code != 206U)
            {
                /* ★ 602 单独给一句：手册 p.195 602 = 内存不足。
                 *   收到它就说明"这一片还是太大"或"服务器根本不理 Range
                 *   （把整个 23104 B 塞回来）"，两种都不是改固件能糊过去的。 */
                if (s_status_code == 602U)
                {
                    LOG_E0("HOTA", "602=no memory: Range ignored or chunk too big");
                }
                /* ★ v0.6.15：601 = network error。它和 TLS 无关、和 URL 无关，
                 *   只说明"模组认为自己没有可用网络"（没卡/没注网/PDP 没起）。
                 *   没有这句解释时，601 很容易被误读成服务器问题。 */
                if (s_status_code == 601U)
                {
                    LOG_E0("HOTA", "601=network error (SIM/PDP/APN), not TLS");
                }
                LOG_E1("HOTA", "http status=%u", s_status_code);
                hota_cmd_once(HC_HTTPTERM, AT_TO_CMD);
                return hota_recover_or_fail(OTA_ERR_HTTP, 4U);
            }
            if (s_expect_len == 0UL)
            {
                /* ★ 200/206 但长度为 0 ⇒ 这一行的数字没解全（丢字节把 "1024"
                 *   吃成 "102" 甚至空），**不能当成文件尾**（那会静默截断镜像）。
                 *   重抓同一片，最多 3 次。 */
                LOG_W1("HOTA", "len=0 code=%u, refetch", s_status_code);
                goto read_retry;
            }
            /* 防御：报告长度超过请求量（服务器无视 BREAKEND）⇒ 只取前 N 字节，
             * 多出来的字节由下一条命令前的 flush 丢掉。 */
            if (s_expect_len > (u32)HTTP_READ_CHUNK) s_expect_len = (u32)HTTP_READ_CHUNK;

            s_chunk_got  = (u16)s_expect_len;
            /* ★ v0.6.7：文件尾优先用**镜像头算出的总长**判定，不再只靠"短片"。
             *   理由：服务的最后一片本来就可能恰好是整片（23104 = 22×1024+576，
             *   这里不是整除，但换一个镜像就是整除），那时"短片即尾"永远不成立，
             *   会一直取到 416 才收——多花一次往返，还多一条误判风险。
             *   s_total 在第一片落盘后由头解析出来（见下面的 header 解析）。 */
            if ((s_total != 0UL) && ((s_offset + (u32)s_chunk_got) >= s_total))
            {
                s_chunk_last = 1U;
            }
            else
            {
                s_chunk_last = (s_chunk_got < (u16)HTTP_READ_CHUNK) ? 1U : 0U;
            }
            s_state = HOTA_READ;
        }
        else
        {
            LOG_E1("HOTA", "action rc=%u", (u16)r);
            return hota_recover_or_fail(OTA_ERR_NET, 3U);
        }
    }

    if (s_state == HOTA_READ)
    {
        /* 本片期望字节数（由 +HTTPACTION 的 len 决定，不是固定 1024） */
        n = s_chunk_got;
        if (n == 0U)
        {
            return hota_recover_or_fail(OTA_ERR_HTTP, 7U);
        }

        /*----------------------------------------------------------------------
         * 发 HTTPREAD：AT+HTTPREAD=<start_address>,<byte_size>
         *
         * ★ start_address 是**本次响应数据体内部的偏移**，不是文件偏移
         *   （手册 §13.7 明写 <start_address> 输出数据的起点 0~3356）。
         *   文件偏移由 HTTPPARA 的 BREAK 负责，两者不要混。
         *--------------------------------------------------------------------*/
        at_tx_reset();
        HC4('A','T','+','H'); HC4('T','T','P','R'); HC4('E','A','D','=');
        HC1('0'); HC1(',');
        at_tx_d((u32)n);

        /* ★★★ v0.6.17：**先绑落盘目标，再发命令**
         *
         *   step67 现场：每片都 `short 1020/1024`（偶尔 1022），固定少 4 个字节、
         *   而且丢在数据体**头部** ⇒ 整片前移 ⇒ 只有 CRC 能发现。
         *   原因是老路径"前缀行命中 → 返回 → 收尾映射 → 范围检查 → 才开始收"，
         *   这几百微秒里模组已经吐了 4 个字节，而硬件只有一个接收寄存器。
         *   ⇒ 目标缓冲必须在发命令之前绑好，前缀一命中就直接落盘。
         *
         * ★★★ v0.6.18：前导字节是**额外多出来的**，不属于 <len> 那 n 个字节
         *   ⇒ 这一条命令总共要收 n + s_lead 个字节，其中后 n 个才是数据。
         *   若只收 n 个，每片会尾部少 s_lead 个真实字节（这个坑差点又埋进去）。
         *   没测出来（刚上电第 0 片）时 s_lead_ok==0，按 n 收、一个都不丢。 */
        want = n;
        if (s_lead_ok != 0U) want = (u16)(n + (u16)s_lead);

        if (at_binary_arm(s_io, want) == 0U)
        {
            LOG_E0("HOTA", "arm FAIL (buf out of range)");
            goto read_retry;
        }
        if (s_lead_ok != 0U) at_binary_set_skip(s_lead);

        r   = (at_resp_t)at_tx_mac(AT_EXP_HTTPREAD, AT_TO_HTTP_READ);
        got = at_binary_taken();
        at_binary_disarm();

        if (r != AT_RESP_PREFIX)
        {
            LOG_E3("HOTA", "httpread rc=%u want=%u n=%u",
                   (u16)r, n, g_at_capn);
            goto read_retry;
        }

        /*======================================================================
         * ★★★ v0.6.5：**短读就是丢字节的直接量化**，也是唯一能当场自证的判据
         *
         *   要 1024 B、只收到 998 B ⇒ 线上/接收端丢了 26 B。
         *   以前这一片会被当成"读完了"，然后写进 Flash 26 字节旧内容，
         *   最后只在 CRC 上暴露，把病根指错方向。
         *   ⇒ 短读一律重抓同一片（Range 幂等，重抓无副作用），最多 3 次；
         *     仍短读就判失败，日志里 `short got/want` 就是这一版的诊断证据。
         *   预期：线路真干净时 100% 全等；只要还出现 short，就说明丢字节没解决，
         *   而此时"短多少"直接给出丢包率。
         *====================================================================*/
        if (got != n)
        {
            LOG_W2("HOTA", "short %u/%u (bytes lost)", got, n);
            goto read_retry;
        }

        /*======================================================================
         * ★★★ v0.6.18：首片实测"数据体头部无关字节数"
         *
         *   step68（v0.6.17）现场：`short` 一行都没有、23 片一次跑完、总计
         *   正好 23104 B，可 `header unrecognized, total stays unknown` ——
         *   字节一个没少，但**位置不对**。本地镜像头是 `4f 54 41 49`（OTAI），
         *   所以只可能是数据体前面多了几个字节：8910 在 `+HTTPREAD: <len>`
         *   这一行后还多吐一个空行（CRLF）。
         *
         *   老路径为什么"看不见"：它在命中前缀后把环形缓冲整段倒掉
         *   （`s_tail = s_head`），这 2 个字节被顺手吃掉，同时也会多带 2 个
         *   真实数据字节 ⇒ step67 的 `short 1020`（4 = 2 前导 + 2 数据）与
         *   偶发的 1024 全对得上。
         *
         *   ⇒ 这里用**魔数位置**量出 k（0/2/4 都能处理），量出来之后：
         *     · 本片就地左移 k 字节（只动内存，不碰 Flash 地址）；
         *     · 后续片由 at_binary_set_skip(k) 在接收时直接丢弃。
         *   若首 5 个位置都找不到 OTAI，就不动它，照旧报
         *   `header unrecognized` —— 绝不拿"猜"当判据。
         *====================================================================*/
        if (s_offset == 0UL && s_lead_ok == 0U && got >= 8U)
        {
            LOG_I2("HOTA", "head %x %x", (u16)s_io[0], (u16)s_io[1]);
            LOG_I2("HOTA", "head+2 %x %x", (u16)s_io[2], (u16)s_io[3]);
            /* ★ v0.6.19 判据行：接收集群的"最长空转"及位置（见 uart_at.h）。
             *   正常流式应答应当 gap ≤ 1~2；出现几十圈就是那一处丢了字节。
             *   gap0 = 前缀行到首字节的空转（饱和 255 ≈ 1 ms，那段是模组准备
             *   数据体的时间，本来就长，不算异常）。 */
            LOG_I2("HOTA", "gap %u at %u", (u16)at_binary_gap(),
                   at_binary_gap_at());
            LOG_I1("HOTA", "gap0 %u (prefix->byte0)", (u16)at_binary_gap0());

            lead = HOTA_NA_U8;
            for (i = 0U; i <= 4U; i++)
            {
                if (s_io[i] == (u8)'O' && s_io[i + 1U] == (u8)'T' &&
                    s_io[i + 2U] == (u8)'A' && s_io[i + 3U] == (u8)'I')
                {
                    lead = (u8)i;
                    break;
                }
            }

            if (lead != HOTA_NA_U8 && lead != 0U)
            {
                s_lead    = lead;
                s_lead_ok = 1U;
                /* 左移 lead 字节后 got 才是真实数据长度 */
                for (i = 0U; i < (u16)(got - (u16)lead); i++)
                {
                    s_io[i] = s_io[i + (u16)lead];
                }
                got = (u16)(got - (u16)lead);
                LOG_I1("HOTA", "lead=%u B skipped (framing)", (u16)lead);
            }
            else if (lead == 0U)
            {
                s_lead    = 0U;
                s_lead_ok = 1U;
                LOG_I0("HOTA", "lead=0 (clean body)");
            }
            else
            {
                /*------------------------------------------------------------------
                 * ★★★ v0.6.19：认不出魔数 ⇒ **这一片必须重取**，不许"照旧写下去"
                 *
                 *   step69 的教训：头认不出时只打一行警告就继续写，结果 23 片
                 *   全部下完、进度 100%、日志干净，可 STAGING 里是坏的；用户按
                 *   提示掉电重上电，才在 BL 里看到 `STAGING fail rc=12`，白跑一轮。
                 *
                 *   首片只有 1 KB，重取成本 ≈ 0.1 s。Range 幂等，重取无副作用；
                 *   s_try 上限 3 次之后由 read_retry 升级成自愈链（不判死）。
                 *----------------------------------------------------------------*/
                LOG_W0("HOTA", "chunk0 head is not OTAI -> refetch same chunk");
                goto read_retry;
            }
        }

        /* ★★★ v0.6.7：**第一片里就有镜像头**，顺手把"真实总长"解出来
         *
         *   为什么要解：分片下载最怕"不知道终点"。有了 s_total，
         *     ① 进度百分比才是真的（不再是 offset/offset）；
         *     ② 文件尾判据从"短片"变成 `offset+len >= total`，与服务器
         *        实际返回多少无关；
         *     ③ 续传后能立刻知道自己已经走到总长的哪一段。
         *   头结构见 COMMON/ota_types.h（无填充、**大端**）：
         *     [0..3] "OTAI"  [4..5] hdr_len  [12..15] img_len
         *   ★★ 大端不是选择，是事实：**Keil C251 的多字节存储是高字节在前**
         *      （工具侧 make_img.py 用 struct.pack(">IHH…") 打的也是大端，
         *        两边天然一致）。这里若按小端拼，hdr_len 会读成 0x4000=16384、
         *        img_len 会读成 0x005A0000，于是校验必然不过 —— 幸好有下面的
         *        范围检查兜着，只退化成"进度不显示"，不会写坏 Flash。
         *   total = hdr_len + img_len。任一字段离谱就保持 0（退化为旧行为），
         *   绝不因为"头没看懂"就改动下载逻辑。
         *   ★ 只在 offset==0 的这一片上解，避免续传时用中途数据误读。 */
        if (s_offset == 0UL && got >= 16U)
        {
            static u32 xdata hv;
            static u32 xdata hv2;
            hv  = ((u32)s_io[4] << 8)  | (u32)s_io[5];                    /* hdr_len */
            hv2 = ((u32)s_io[12] << 24) | ((u32)s_io[13] << 16) |
                  ((u32)s_io[14] << 8)  | (u32)s_io[15];                   /* img_len */
            if ((s_io[0] == (u8)'O') && (s_io[1] == (u8)'T') &&
                (s_io[2] == (u8)'A') && (s_io[3] == (u8)'I') &&
                hv >= 16UL && hv <= 256UL && hv2 > 0UL &&
                ((hv + hv2) <= OTA_STAGE_SIZE))
            {
                s_total = hv + hv2;
                LOG_I2("HOTA", "header: hdr=%lu total=%lu", hv, s_total);
            }
            else
            {
                LOG_W0("HOTA", "header unrecognized, total stays unknown");
            }
        }

        /* 落盘：先擦对应扇区再写（512 B 扇区，s_offset 恒为 1024 对齐） */
        {
            static u32 xdata waddr;
            static u16 xdata seg;
            static u16 xdata wl;
            waddr = OTA_STAGE_BASE + s_offset;
            seg = 0U;
            while (seg < got)
            {
                if ((waddr & 0x1FFUL) == 0UL)
                {
                    flash_iap_erase_sector(waddr);
                }
                wl = (u16)(512UL - (waddr & 0x1FFUL));
                if (wl > (u16)(got - seg)) wl = (u16)(got - seg);
                flash_iap_write(waddr, &s_io[seg], wl);
                waddr += (u32)wl;
                seg += wl;
                bsp_wdt_feed();
            }
        }

        s_offset += (u32)got;
        s_chunk_no++;
        s_try = 0U;
        bsp_wdt_feed();

        /* ★ v0.6.6：原来打 `down %lu/%lu`，而 s_total 每片被重写成
         *   "本片起点+本片长度" ⇒ 两个数恒等，看起来像"每片都下完了"。
         *   改成打绝对偏移，读者不会误判。 */
        LOG_I2("HOTA", "chunk %u done, off=%lu", s_chunk_no, s_offset);

        if (s_chunk_last != 0U)
        {
            hota_cmd_once(HC_HTTPTERM, AT_TO_CMD);
            s_total = s_offset;          /* 以实际落盘字节为准 */
            s_state = HOTA_DONE;
            return HOTA_DONE;
        }
        s_state = HOTA_REQ;          /* 下一片 */
        return HOTA_READ;
    }

    return s_state;

read_retry:
    s_try++;
    if (s_try > 3U)
    {
        LOG_E2("HOTA", "chunk %u give up after %u retries",
               s_chunk_no, (u16)s_try);
        hota_cmd_once(HC_HTTPTERM, AT_TO_CMD);
        /* ★ v0.6.7：同一片重取 3 次都短/无应答 ⇒ 交给自愈链（等模组→重建会话），
         *   而不是判死。这才是"4G 抖一下不该让 23 KB 白下"的落点。 */
        return hota_recover_or_fail(OTA_ERR_NET, 6U);
    }
    s_state = HOTA_REQ;              /* 重抓同一片 */
    return HOTA_READ;
}

/*==============================================================================
 * 收尾 / 查询
 *============================================================================*/
ota_ret_t http_ota_abort(u8 keep_resume)
{
    hota_cmd_once(HC_HTTPTERM, AT_TO_CMD);
    s_state = HOTA_ABORTED;
    return OTA_OK;
}

ota_ret_t http_ota_close(void)
{
    s_state = HOTA_DONE;
    return OTA_OK;
}

u32 http_ota_get_offset(void)  { return s_offset; }
u32 http_ota_get_total(void)   { return s_total; }

u8 http_ota_get_progress(void)
{
    if (s_total == 0UL) return 0U;
    return (u8)((s_offset * 100UL) / s_total);
}

ota_ret_t http_ota_get_error(void) { return s_err; }

/* ★ v0.6.7：自愈相关读数（进度上报用，见 ota_fsm.c 的 [OTA] 行） */
u8  http_ota_get_recover(void) { return s_recover; }
u8  http_ota_get_restart(void) { return s_restart; }
u16 http_ota_get_chunk(void)   { return s_chunk_no; }
u8  http_ota_get_try(void)     { return s_try; }

const http_ota_ctx_t *http_ota_ctx(void) { return 0; }

/* 断点接口：本轮不做，返回空实现 */
ota_ret_t http_ota_set_break(u32 start, u32 end) { return OTA_ERR_SUPPORT; }
ota_ret_t http_ota_set_range_header(u32 start, u16 len) { return OTA_ERR_SUPPORT; }
u8 http_ota_range_supported(void) { return 0U; }
ota_ret_t http_ota_save_offset(void) { return OTA_OK; }
u32 http_ota_load_offset(void) { return 0UL; }
ota_ret_t http_ota_reset_offset(void) { return OTA_OK; }
