/*==============================================================================
 *  app_mqtt.c  --  MQTT 控制面：连接 / 订阅固件版本 / 周期上报 / 触发 OTA
 *  Project : Project/APP/STC32G_OTA.uvproj    CODE = 0xFF4000
 *  Platform: STC32G12K128 (Keil C251, Huge)
 *
 *  ── 命令序列（照手册第 16 章，实测版本 V401880）───────────────────────
 *      AT+MCONFIG=<id>,<user>,<pass>            → OK
 *      AT+SSLCFG="seclevel",88,0                → OK（失败也继续）
 *      AT+SSLMIPSTART=<host>,<port>             → OK, 稍后 URC: CONNECT OK
 *      AT+MCONNECT=1,<keepalive>                → OK, 稍后 URC: CONNACK OK
 *      AT+MQTTMSGSET=0                          → 收到的消息直接 URC 上报
 *      AT+MSUB="fw/<dev>/ver",0                 → OK, URC: SUBACK
 *      AT+MPUB="fw/<dev>/up",0,0,"<payload>"    → OK（qos0 没有 PUBACK）
 *
 *  ── 三条不能想当然的地方（都是本工程踩过的同一类坑）───────────────────
 *   ① **TCP/MQTT 的"成功"是 URC，不是 OK。**  `AT+MIPSTART` 先回 OK，
 *      几十~几百毫秒后才吐 `CONNECT OK`。如果按 OK 判成功，下一步 MCONNECT
 *      会撞在"链路还没起来"上，表现为莫名 ERROR。
 *      ⇒ 直接用 exp 前缀等 URC：`at_tx_mac(AT_EXP_CONNOK, ...)`。
 *      这样 URC 先到、OK 后到也照样命中（引擎只认前缀，OK 那行忽略）。
 *   ② **订阅消息是"没有触发命令"的 URC。**  阻塞式等待会把跨窗口的 URC
 *      撕成两半（见 uart_at.c 的 at_pump），所以稳态用 at_pump + at_urc_take。
 *   ③ **载荷里绝不能出现逗号。**  URC 行是 `+MSUB: <topic>,<len> byte,<msg>`，
 *      解析靠"最后一个逗号之后就是载荷"。版本消息写成 `<ver>|<url>`，
 *      数据上报里的逗号只出现在**上行**载荷（下行不解析），互不干扰。
 *
 *  ── 升级触发 ──────────────────────────────────────────────────────────
 *      服务器 → fw/<dev>/ver : "0.7.1|https://kappa.lol/xxxx"   (retain=1)
 *      本文件解析出版本与直链 → 比当前版本 → 更高则断链并 ota_fsm_start_download()
 *      下载/校验/搬运/重启全交给既有的 HTTP OTA 与 BL，本文件不管。
 *      升级实例见 docs/APP_MQTT.md。
 *============================================================================*/
#include "ota_mcu.h"

#include "app_config.h"

#include "log.h"
#include "uart_at.h"
#include "uart2_data.h"     /* 10 个模板的去处 */
#include "ota_fsm.h"        /* ota_fsm_start_download / OTA_OK */
#include "http_ota.h"       /* http_ota_pdp_up：连服务器前先要有承载 */
#include "app_mqtt.h"

#define TAG     "MQ"

/*==============================================================================
 * 状态（全部 xdata + mqtt_init 里显式清零，铁律 13）
 *============================================================================*/
static u8  xdata s_st;              /* MQ_ST_*                          */
static u8  xdata s_st_prev;
static u8  xdata s_init;            /* 0x5A = mqtt_init 已执行          */
static u8  xdata s_rc;              /* 最近一条命令的 AT_RESP_*         */
static u8  xdata s_fail;            /* 连续失败次数                     */
static u32 xdata s_t_next;          /* 下次允许动作的时刻（ms）          */
static u32 xdata s_t_pub;           /* 上次发布时刻（ms）                */
static u16 xdata s_pub_ok;
static u16 xdata s_pub_fail;

/* 服务器版本消息解析结果 */
static u8  xdata s_vmaj;
static u8  xdata s_vmin;
static u8  xdata s_vpat;
static u8  xdata s_ota_pend;        /* 1 = 版本更高，待触发下载          */
static u8  xdata s_ver_ok;          /* ★ v0.8.2：本次 parse 走到"认下版本"才置 1 */
static u8  xdata s_resub;           /* ★ v0.8.2：版本行被写坏后的重订阅剩次     */
static u32 xdata s_t_resub;         /*   下次重订阅的时刻（ms）                 */
static char xdata s_url[MQTT_OTA_URL_MAX];

/* 追加常量串用的全局游标。
 * ★ 为什么不用 `tx_lit(const char *s)` 里那个形参直接遍历：形参指针会在
 *   循环里的 at_tx_c() 调用中被覆盖（uart_at.h 顶部那段病根）。
 *   首句先 Copy 到全局，此后只读全局 —— 铁律 9。 */
static const char *xdata s_lit;

/*==============================================================================
 * 小工具
 *============================================================================*/
static void tx_lit(const char *s)
{
    s_lit = s;                                   /* ★ 形参只读这一句 */
    while (*s_lit != '\0') at_tx_c((u8)*s_lit++);
}

/* 时刻到没到（at_ms 回绕安全：差值落在前半区即"已到"） */
static u8 due(u32 t)
{
    return (u8)(((u32)(at_ms() - t)) < 0x80000000UL);
}

static void mqtt_to_back(void)
{
    s_st     = MQ_ST_DOWN;
    s_t_next = (u32)(at_ms() + 500UL);
    s_fail   = 0U;
}

/*==============================================================================
 * 各步命令：返回 1 = 这一步成了；0 = 失败（rc 在 s_rc）
 *============================================================================*/

/* AT+MCONFIG=<clientid>,<user>,<pass>
 *
 * ★ 先 MDISCONNECT + MIPCLOSE 清残留会话（2026-10-28 定案）。
 *   模组里若留着上一次半开的 MQTT 会话/链路，MCONNECT 会**在本地**直接回
 *   ERROR（不走网络、约 60 ms），表现和"服务器拒绝"一模一样，极易误判。
 *   没连接时这两条都回 OK，幂等；MCU 复位但模组没断电时尤其需要。 */
static u8 step_cfg(void)
{
    at_tx_reset();
    tx_lit("AT+MDISCONNECT");
    (void)at_tx_mac(AT_EXP_NONE, 2000U);

    at_tx_reset();
    tx_lit("AT+MIPCLOSE");
    (void)at_tx_mac(AT_EXP_NONE, 2000U);

    at_tx_reset();
    tx_lit("AT+MCONFIG=");
    tx_lit(MQTT_CLIENT_ID);  tx_lit(",");
    tx_lit(MQTT_USER);       tx_lit(",");
    tx_lit(MQTT_PASS);

    s_rc = at_tx_mac(AT_EXP_NONE, 3000U);
    return (u8)((s_rc == (u8)AT_RESP_OK) ? 1U : 0U);
}

/* AT+SSLCFG 三条：seclevel → sslversion → hostname
 *
 * ★★ 2026-10-28 上板实测定案（Air724UG V401880 + EMQX Serverless 8883）：
 *
 *   只设 seclevel,88,0 时：SSLMIPSTART 能拿到 CONNECT OK（TLS 握手确实成了），
 *   但紧接着 AT+MCONNECT=1,60 在 **62 ms** 后回 ERROR + CLOSED，CONNACK 永远不来。
 *
 *   根因 = TLS ClientHello 里**没有 SNI**（服务器域名没上报）。
 *   EMQX Serverless 是共享负载均衡，靠 SNI 把连接路由到你的实例；
 *   而 seclevel=0 又不校验证书，于是"证书不匹配"这个本该报警的信号被掩盖，
 *   握手照样成功，问题被推迟到 MQTT 层，表现为"凭据没错、就是连不上"。
 *   同一个用户名密码在 PC 的 MQTT 客户端上完全正常 —— 差别就在 SNI。
 *
 *   补上 "hostname",88,"<域名>" 之后立刻 CONNACK OK。
 *   sslversion=3 是 TLS1.2（手册 12.7：0=SSL3.0 1=TLS1.0 2=TLS1.1 3=TLS1.2 4=ALL），
 *   一并钉死，免得依赖模组出厂默认值。
 *
 * ★ 三条都幂等、都"失败也继续"：有的固件不支持某条，不该因此断掉整条链；
 *   真正决定成败的是下一步 SSLMIPSTART 能不能拿到 CONNECT OK。 */
static u8 step_ssl(void)
{
    at_tx_reset();
    tx_lit("AT+SSLCFG=\"seclevel\",88,0");
    s_rc = at_tx_mac(AT_EXP_NONE, 2000U);

    at_tx_reset();
    tx_lit("AT+SSLCFG=\"sslversion\",88,3");
    s_rc = at_tx_mac(AT_EXP_NONE, 2000U);

    at_tx_reset();
    tx_lit("AT+SSLCFG=\"hostname\",88,\"");
    tx_lit(MQTT_HOST);
    tx_lit("\"");
    s_rc = at_tx_mac(AT_EXP_NONE, 2000U);

    return 1U;
}

/* AT+SSLMIPSTART / AT+MIPSTART —— 成败看 URC `CONNECT OK` */
static u8 step_tcp(void)
{
    static u8 xdata r;

    at_tx_reset();
#if (MQTT_TLS != 0)
    tx_lit("AT+SSLMIPSTART=");
#else
    tx_lit("AT+MIPSTART=");
#endif
    tx_lit(MQTT_HOST);
    tx_lit(",");
    at_tx_d((u32)MQTT_PORT);

    /* ★ 只有 exp 前缀命中才算通。等不到就别让下一步去撞 MCONNECT。 */
    r = at_tx_mac(AT_EXP_CONNOK, 10000U);
    s_rc = r;
    return (u8)((r == (u8)AT_RESP_PREFIX) ? 1U : 0U);
}

/* AT+MCONNECT=<clean>,<keepalive> —— 成败看 URC `CONNACK OK` */
static u8 step_conn(void)
{
    static u8 xdata r;

    at_tx_reset();
    tx_lit("AT+MCONNECT=1,");
    at_tx_d((u32)MQTT_KEEPALIVE);

    r = at_tx_mac(AT_EXP_CONNACK, 10000U);
    s_rc = r;
    return (u8)((r == (u8)AT_RESP_PREFIX) ? 1U : 0U);
}

/* AT+MQTTMSGSET=0 然后 AT+MSUB="<ver topic>",0 —— 成败看 URC `SUBACK` */
static u8 step_sub(void)
{
    static u8 xdata r;

    at_tx_reset();
    tx_lit("AT+MQTTMSGSET=0");
    r = at_tx_mac(AT_EXP_NONE, 2000U);
    /* 失败也继续：默认就是 0，这条只是保证语义 */

    at_tx_reset();
    tx_lit("AT+MSUB=\"");
    tx_lit(MQTT_TOPIC_VER);
    tx_lit("\",0");

    r = at_tx_mac(AT_EXP_SUBACK, 6000U);
    s_rc = r;
    return (u8)((r == (u8)AT_RESP_PREFIX) ? 1U : 0U);
}

/*------------------------------------------------------------------------------
 * 数据上报：AT+MPUB="fw/<dev>/up",0,0,"D1=...,D2=...,D3=##1229##,..."
 *
 *   载荷直接拼在 g_at_tx 里（不发两次），因为模组的 MPUB 要求消息在命令行内。
 *   没有数据的槽位打 DATA_DEF_TXT（用户约定 `##1229##`）。
 *   长度预算：10 个槽最坏 140 B，加主题与外壳 ≈168 B < AT_TX_MAX(256)。
 *------------------------------------------------------------------------------*/
static u8 step_pub(void)
{
    static u8 xdata r;
    static u8 xdata i;

    at_tx_reset();
    tx_lit("AT+MPUB=\"");
    tx_lit(MQTT_TOPIC_UP);
    tx_lit("\",0,0,\"");

    for (i = 1U; i <= DATA_SLOT_N; i++)
    {
        tx_lit("D");
        at_tx_d((u32)i);
        tx_lit("=");
        if (data_have(i) != 0U) at_tx_d(data_value(i));
        else                    tx_lit(DATA_DEF_TXT);
        if (i < DATA_SLOT_N) tx_lit(",");
    }
    tx_lit("\"");

    r = at_tx_mac(AT_EXP_NONE, 5000U);
    s_rc = r;
    return (u8)((r == (u8)AT_RESP_OK) ? 1U : 0U);
}

/*==============================================================================
 * 版本消息解析
 *
 *   输入（g_urc，来自 at_pump）：
 *       +MSUB: fw/0001/ver,28 byte,0.7.1|https://kappa.lol/xoxgMU
 *   做法：取**最后一个逗号之后**当载荷（载荷里没有逗号，见文件头 ③）。
 * =============================================================================*/
static void mqtt_parse_ver(void)
{
    static u16 xdata i;
    static u16 xdata j;
    static u16 xdata q;
    static u16 xdata end;
    static u16 xdata want;
    static u8  xdata dig;

    /* ① 定位载荷起点 */
    i = 0U;
    for (j = 0U; j < g_urcn; j++)
    {
        if (g_urc[j] == ',') i = (u16)(j + 1U);
    }
    if (i == 0U) { LOG_W0(TAG, "urc has no comma, ignored"); return; }

    /*--------------------------------------------------------------------------
     * ①.5 ★★★ 完整性校验：`<N> byte` 里的 N 是**含引号**的载荷字节数
     *
     *     +MSUB: "fw/0001/ver",32 byte,"0.7.1|https://kappa.lol/xoxgMU"
     *                           ↑ 裸载荷 30 + 2 个引号 = 32
     *     而 `32` 恰好等于"最后一个逗号之后到行尾"的字符数（引号也在内）。
     *     所以判据只有一条：N == g_urcn - i。
     *
     *   为什么非要有这一条（v0.7.2 上板现场）：
     *     接收路径偶发丢 1 个字节（v0.6.13 起的老问题，至今未根治）。
     *     一条 30 字节的版本消息丢 1 个字符，URL 就从 xoxgMU 变成 xogMU ——
     *     看起来完全正常、能过"http 开头"的检查，然后服务器回 404：
     *
     *         AT+HTTPPARA="URL","https://kappa.lol/xogMU"  → +HTTPACTION: 0,404,14
     *
     *     这是最难查的一类：错的不是代码，是运输途中掉了一个字节。
     *   模组白送的这个长度字段就是 CRC —— 不符就整条丢掉，等服务器重发。
     *   （同一条判据还抓到了 `0.7.1` 被读成 `0.71`：28+2=30 ≠ 32。）
     *------------------------------------------------------------------------*/
    end  = (u16)(g_urcn - i);          /* 实到字节数（含引号） */
    want = 0U;
    j    = 1U;                         /* 位权 */
    q    = i;
    if ((q >= 7U) && (g_urc[q - 2U] == (char)'e') && (g_urc[q - 3U] == (char)'t') &&
        (g_urc[q - 4U] == (char)'y') && (g_urc[q - 5U] == (char)'b') &&
        (g_urc[q - 6U] == (char)' '))
    {
        q = (u16)(q - 7U);             /* 指向个位数字，从右往左累加 */
        while ((q > 0U) &&
               (g_urc[q] >= (char)'0') && (g_urc[q] <= (char)'9'))
        {
            want = (u16)(want + (u16)((u16)(g_urc[q] - (char)'0') * j));
            j    = (u16)(j * 10U);
            q--;
        }
        if ((want != 0U) && (want != end))
        {
            LOG_W2(TAG, "urc len %u/%u drop", end, want);
            return;
        }
    }

    /* ★★★ 2026-10-28 实测定案：8910 在 MQTTMSGSET=0 下发的载荷**带双引号**，
     *   而且 "N byte" 的长度把引号也算了进去（实测 "0.7.2|https://..." 报
     *   32 byte，裸串只有 30）：
     *
     *       +MSUB: "fw/0001/ver",32 byte,"0.7.2|https://kappa.lol/xoxgMU"
     *
     *   而手册 16.14 的示例写的是**不带引号**的 `...,9 byte,SSSSddddd`。
     *   照手册写 ⇒ 第一个字符是 '"' 而不是数字 ⇒ 被当成"不是版本消息"丢掉，
     *   表现为"服务器消息明明收到了、设备一点反应都没有、日志也不报错"。
     *   这里统一跳过前导引号，URL 拷贝时遇引号截止，两种格式都兼容。 */
    if ((i < g_urcn) && (g_urc[i] == (char)'"')) i++;

    /* ② 必须形如 <数字>.<数字>.<数字>[|url]，否则当普通消息丢掉 */
    if ((i >= g_urcn) || (g_urc[i] < (char)'0') || (g_urc[i] > (char)'9'))
    {
        LOG_W0(TAG, "urc payload not a version, ignored");
        return;
    }

    /* ③ 三段十进制 */
    s_vmaj = 0U;
    while ((i < g_urcn) && (g_urc[i] >= (char)'0') && (g_urc[i] <= (char)'9'))
    { s_vmaj = (u8)((u16)s_vmaj * 10U + (u16)(g_urc[i] - (char)'0')); i++; }
    if ((i >= g_urcn) || (g_urc[i] != (char)'.'))
    { LOG_W0(TAG, "ver not M.m.p, ignored"); return; }
    i++;
    s_vmin = 0U;
    while ((i < g_urcn) && (g_urc[i] >= (char)'0') && (g_urc[i] <= (char)'9'))
    { s_vmin = (u8)((u16)s_vmin * 10U + (u16)(g_urc[i] - (char)'0')); i++; }

    /*--------------------------------------------------------------------------
     * ★★★ v0.7.8：第二段之后**必须是点**，第三段**至少要有一位数字**
     *
     *   原来是 `if ((i < g_urcn) && (g_urc[i] == '.')) i++;` —— 没有 else。
     *   于是 `0.7..|https://…`（第二段那个点之后又跟了一个被写坏的字符，
     *   在 g_urc 里表现为 '.'）会**跳过这个点继续读**，第三段读到 '.' 不是
     *   数字 ⇒ s_vpat 保持 0 ⇒ 打出 `srv ver 0.7 / patch 0`。
     *
     *   而服务器的 0.7.4 明明比板上的 0.7.6 低，于是**不升级**；
     *   现场看到的就是"消息一条接一条、版本号永远是 0.7.0、永不升级"。
     *   现在两处都收紧：宁可整条丢掉等下一次（用户会连发），
     *   也不许把一条坏消息解析成一个看似合法的低版本。
     *------------------------------------------------------------------------*/
    if ((i >= g_urcn) || (g_urc[i] != (char)'.'))
    { LOG_W0(TAG, "ver not M.m.p, ignored"); return; }
    i++;

    dig   = (u8)i;                    /* 第三段起点（行最长 127，u8 够） */
    s_vpat = 0U;
    while ((i < g_urcn) && (g_urc[i] >= (char)'0') && (g_urc[i] <= (char)'9'))
    { s_vpat = (u8)((u16)s_vpat * 10U + (u16)(g_urc[i] - (char)'0')); i++; }
    if ((u8)i == dig)
    { LOG_W0(TAG, "ver bad patch, ignored"); return; }

    /* ④ '|' 之后是升级直链 */
    s_url[0] = 0;
    if ((i < g_urcn) && (g_urc[i] == (char)'|'))
    {
        i++;
        dig = 0U;
        while ((i < g_urcn) && (dig < (MQTT_OTA_URL_MAX - 1U)))
        {
            if (g_urc[i] == (char)'"') break;      /* 尾部引号：到此为止 */
            s_url[dig] = g_urc[i];
            dig++;
            i++;
        }
        s_url[dig] = 0;
    }

    /* ★ 直链必须以 http 开头。万一以后服务器给的 URL 里带逗号（会被
     *   "最后一个逗号"切成两半），这里能立刻暴露，而不是拿半截 URL
     *   去下载、等到 HTTP 层才报一个看不懂的错。 */
    if ((s_url[0] != 0) &&
        ((s_url[0] != (char)'h') || (s_url[1] != (char)'t')))
    {
        LOG_W1(TAG, "url bad head, drop: %s", s_url);
        s_url[0] = 0;
    }

    /* ★ v0.7.8 自证：把真正参与解析的那一行**原样**打出来（带方括号看边界）。
     *   接收路径只要写坏一个字符，这里就一眼可见 —— 比再猜十次都值。 */
    LOG_I2(TAG, "urc [%s] n=%u", g_urc, (u16)g_urcn);

    /* ★ v0.8.2：走到这里才算"这一行是完整可用的版本消息"（长度校验、
     *   三段十进制、URL 头全部通过）。之后 mqtt_poll 据此决定要不要重订阅。 */
    s_ver_ok = 1U;
    s_resub  = 0U;

    LOG_I2(TAG, "srv ver %u.%u", (u16)s_vmaj, (u16)s_vmin);
    LOG_I1(TAG, "      patch %u", (u16)s_vpat);

    /* ⑤ 判定 */
#if (MQTT_OTA_ANY_VERSION != 0)
    if ((s_vmaj != (u8)APP_VER_MAJOR) || (s_vmin != (u8)APP_VER_MINOR) ||
        (s_vpat != (u8)APP_VER_PATCH))
#else
    /* 只有"严格更高"才升：否则服务器残留一个旧版本会让设备无限来回刷 */
    if ((s_vmaj != (u8)APP_VER_MAJOR) ? (s_vmaj > (u8)APP_VER_MAJOR) :
        ((s_vmin != (u8)APP_VER_MINOR) ? (s_vmin > (u8)APP_VER_MINOR) :
                                          (s_vpat > (u8)APP_VER_PATCH)))
#endif
    {
        if (s_url[0] == 0)
        {
            LOG_W0(TAG, "newer ver but no URL, ignored");
        }
        else
        {
            s_ota_pend = 1U;
            LOG_W0(TAG, "newer than me -> OTA armed");
            /* ★ 自证：把真正要下载的 URL 原样打出来。URL 只要少一个字符，
             *   服务器就回 404，而 404 长得和"文件不存在"一模一样。
             *   这一行是唯一能在烧录前就看出"线上传过来的到底是什么"的证据。 */
            LOG_I1(TAG, "  url %s", s_url);
        }
    }
}

/*==============================================================================
 * 触发 OTA：先礼貌断链，再把直链交给既有的 HTTP OTA
 *
 *   ★ 为什么必须先断开 MQTT：模组的内存是 HTTP/MQTT 共用的，602（no memory）
 *     就是这么来的（见 MEMORY.md 的 602 定案）。而且下载期间本文件没空维护
 *     会话，留着一个半死的连接只会让 keepalive 到期后被服务器踢。
 *============================================================================*/
static void mqtt_do_ota(void)
{
    s_ota_pend = 0U;

    at_tx_reset();
    tx_lit("AT+MDISCONNECT");
    (void)at_tx_mac(AT_EXP_NONE, 2000U);

    at_tx_reset();
    tx_lit("AT+MIPCLOSE");
    (void)at_tx_mac(AT_EXP_NONE, 2000U);

    s_st = MQ_ST_HOLD;

    if (ota_fsm_start_download(s_url) == OTA_OK)
    {
        LOG_I0(TAG, "OTA download started (trigger = MQTT ver)");
    }
    else
    {
        LOG_E0(TAG, "OTA refused (fsm busy), try next ver msg");
    }
}

/*==============================================================================
 * 初始化
 *============================================================================*/
void mqtt_init(void)
{
    s_st       = MQ_ST_OFF;
    s_st_prev  = MQ_ST_OFF;
    s_rc       = 0U;
    s_fail     = 0U;
    s_t_next   = 0UL;
    s_t_pub    = 0UL;
    s_pub_ok   = 0U;
    s_pub_fail = 0U;
    s_vmaj     = 0U;
    s_vmin     = 0U;
    s_vpat     = 0U;
    s_ota_pend = 0U;
    s_ver_ok   = 0U;
    s_resub    = 0U;
    s_t_resub  = 0UL;
    s_url[0]   = 0;
    s_lit      = 0;
    s_init     = 0x5AU;

#if (APP_MQTT_ENABLE != 0)
    LOG_I1(TAG, "broker %s", MQTT_HOST);
    LOG_I2(TAG, "port %u TLS=%u", (u16)MQTT_PORT, (u16)MQTT_TLS);
#endif
}

/*==============================================================================
 * 主循环推进
 *============================================================================*/
void mqtt_poll(void)
{
    if (s_init != 0x5AU) return;
#if (APP_MQTT_ENABLE == 0)
    return;
#else

    /*--------------------------------------------------------------------------
     * ★★★ v0.7.3：OTA 期间把模组**整个**让出去，一个字节都不发
     *
     *   现场（v0.7.2，16:44:23~16:44:27）：
     *     收到版本 → MDISCONNECT/MIPCLOSE → HTTP 建链、HTTPACTION 发出
     *     → 同一秒 MQTT 又从 PDP 重新起链（MCONFIG / SSLMIPSTART）
     *     → HTTP 会话被拆 → "modem silent, reopen failed" 连打 6 次
     *     → 整包重跑 2 次 → give up。
     *
     *   HTTP 与 MQTT 共用模组内存和 SSL 上下文（602 定案的同一个根子），
     *   物理上不能同时存在。谁在下载，另一个必须彻底闭嘴。
     *------------------------------------------------------------------------*/
    if (ota_fsm_state() != OTA_ST_IDLE)
    {
        if (s_st != MQ_ST_HOLD)
        {
            s_st = MQ_ST_HOLD;
            LOG_W0(TAG, "ota in progress -> modem handed over");
        }
        s_ota_pend = 0U;
        return;
    }
    if (s_st == MQ_ST_HOLD)
    {
        s_st       = MQ_ST_OFF;      /* OTA 结束：回干净状态重新起链 */
        s_ota_pend = 0U;
        s_t_next   = at_ms();
        LOG_I0(TAG, "ota finished -> mqtt restart");
    }

    /* 未启动：等 AT 通道确认波特率（模组可能比 APP 晚就绪好几秒） */
    if (s_st == MQ_ST_OFF)
    {
        if (s_ota_pend != 0U) { mqtt_do_ota(); return; }
        if (at_get_baud() == 0UL) return;
        s_st     = MQ_ST_PDP;
        s_t_next = at_ms();
        LOG_I0(TAG, "at link ready -> mqtt start");
    }

    /* 稳态下 URC 要"随时收"，不能等节拍 */
    if (s_st == MQ_ST_RUN)
    {
        if (at_urc_take() != 0U)
        {
            s_ver_ok = 0U;
            mqtt_parse_ver();

            /*--------------------------------------------------------------
             * ★★★ v0.8.2：版本行被写坏 → **重订阅一次，把 retained 消息要回来**
             *
             *   现在接收路径已经"线路一停就立刻收"（app_main 的静默窗口），
             *   正常不该再坏。但只要还坏（模组错帧、UART1 日志把 CPU 占满、
             *   以后新加的活），这条消息就永久丢了 —— 因为 retain 只在
             *   "订阅那一刻"补发，之后没人再发。
             *
             *   MQTT 的订阅是幂等的：再发一次 AT+MSUB 同一个 topic，
             *   broker 立刻再推一条 retained。这就是现成的"请重发"。
             *   最多连试 3 次、间隔 2 s：坏一次是运气，坏三次就是别的问题，
             *   此时放弃并把发布继续跑下去（不能让升级判据卡死上报）。
             *------------------------------------------------------------*/
            if (s_ver_ok == 0U)
            {
                if (s_resub == 0U)
                {
                    s_resub   = 3U;
                    s_t_resub = (u32)(at_ms() + 2000UL);
                }
            }
        }
    }

    if (s_ota_pend != 0U) { mqtt_do_ota(); return; }

    if (due(s_t_next) == 0U) return;

    if (s_st != s_st_prev)
    {
        LOG_I2(TAG, "state %u -> %u", (u16)s_st_prev, (u16)s_st);
        s_st_prev = s_st;
    }

    switch (s_st)
    {
    case MQ_ST_PDP:
        /* ★ v0.7.0：连服务器前必须先有活动承载（AT+SSLMIPSTART 没承载会 ERROR）。
         *   判据与重试全在 http_ota_pdp_up() 里，这里只负责"失败就退避、别死磕"。
         *   正常机 1~3 s 通过；没插卡/没信号时这里会打 SIM / rssi / attach 三行。 */
        if (http_ota_pdp_up() == OTA_OK)
        {
            /* 承载阶段用的是"宽松判据"（超时但见到 OK 残片也算过），
             * 那是给丢字节的线准备的。MQTT 这几条命令要**严格**判：
             * 宁可重试，也不要拿残片当成功 —— 后面 SSLMIPSTART 一旦用错状态
             * 就会连着失败到退避。OTA 流程自己会重新打开宽松判据。 */
            at_set_lenient(0U);
            s_fail = 0U;
            s_st   = MQ_ST_CFG;
            s_t_next = at_ms();
        }
        else
        {
            s_fail++;
            LOG_E1(TAG, "pdp failed (x%u), retry in 5 s", (u16)s_fail);
            s_t_next = (u32)(at_ms() + 5000UL);
        }
        break;

    case MQ_ST_CFG:
        if (step_cfg() != 0U)
        {
            LOG_I0(TAG, "MCONFIG ok");
            s_fail   = 0U;
            s_st     = MQ_ST_SSL;
            s_t_next = at_ms();
        }
        else
        {
            s_fail++;
            LOG_E1(TAG, "MCONFIG rc=%u", (u16)s_rc);
            if (s_fail >= 3U) mqtt_to_back();
            s_t_next = (u32)(at_ms() + 500UL);
        }
        break;

    case MQ_ST_SSL:
        (void)step_ssl();
        s_st     = MQ_ST_TCP;
        s_t_next = at_ms();
        break;

    case MQ_ST_TCP:
        if (step_tcp() != 0U)
        {
            LOG_I0(TAG, "TCP/TLS up (CONNECT OK)");
            s_fail   = 0U;
            s_st     = MQ_ST_CONN;
            s_t_next = at_ms();
        }
        else
        {
            s_fail++;
            LOG_E1(TAG, "MIPSTART rc=%u", (u16)s_rc);
            mqtt_to_back();
        }
        break;

    case MQ_ST_CONN:
        if (step_conn() != 0U)
        {
            LOG_I0(TAG, "MQTT session up (CONNACK OK)");
            s_fail   = 0U;
            s_st     = MQ_ST_SUB;
            s_t_next = at_ms();
        }
        else
        {
            s_fail++;
            LOG_E1(TAG, "MCONNECT rc=%u", (u16)s_rc);
            mqtt_to_back();
        }
        break;

    case MQ_ST_SUB:
        if (step_sub() != 0U)
        {
            LOG_I1(TAG, "subscribed: %s", MQTT_TOPIC_VER);
            s_st     = MQ_ST_RUN;
            s_fail   = 0U;

            /*--------------------------------------------------------------------------
             * ★★★ v0.7.6：订阅成功后**必须先静默一段，再开始周期上报**
             *
             *   原来这里是 `s_t_pub = at_ms()`（立刻发第一条）。后果是致命的：
             *   broker 的 retained 版本消息是在 SUBACK 之后**立刻**推送的，
             *   而设备恰好在这一瞬间发 `AT+MPUB=...` —— 那条 URC 落在
             *   at_wait_core 的命令窗口里，被 at_wait_scan 当成"命令响应行"
             *   读走并丢弃（at_wait_core 进出会把环倒空、拼行缓冲清零）。
             *
             *   ⇒ **每次订阅都必然丢掉服务器补发的那一条**，之后没人再发，
             *     设备就一直静默。现场表现正是 v0.7.5 那次：
             *       pub ok=1 frames=0
             *       urc b=3 l=0        ← 只有零星残字节，从没见过一整行
             *       urc m=0 t=0
             *
             *   现在改成订阅后静默 MQTT_SUB_QUIET_MS（1.5 s）：
             *     这段时间主循环只剩 data_poll + at_pump，URC 独占线路，
             *     retained 消息必到且必被拼成整行；收到就先判版本（要不要
             *     升级在这一刻就定了），到点再开始 5 s 周期的数据上报。
             *
             *   ★ 1.5 s 不是拍脑袋：4G 下 SUBACK→retained 推送的 RTT 实测
             *     一两百毫秒，留 10 倍余量；而周期上报晚 1.5 s 毫无影响。
             *------------------------------------------------------------------------*/
            s_t_pub  = (u32)(at_ms() + MQTT_SUB_QUIET_MS);
            s_t_next = at_ms();
        }
        else
        {
            s_fail++;
            LOG_E1(TAG, "MSUB rc=%u", (u16)s_rc);
            mqtt_to_back();
        }
        break;

    case MQ_ST_RUN:
        /* ★ v0.8.2：版本行被写坏 → 重订阅（见上面对 s_resub 的说明） */
        if ((s_resub != 0U) && (due(s_t_resub) != 0U))
        {
            s_resub--;
            LOG_W1(TAG, "ver line damaged, resub %u", (u16)s_resub);
            if (step_sub() != 0U)
            {
                /* 订阅成功：retained 会在 SUBACK 后马上到，先静默再上报 */
                s_t_pub = (u32)(at_ms() + MQTT_SUB_QUIET_MS);
            }
            else
            {
                mqtt_to_back();
            }
            break;
        }

        /* 到点上报；失败累计 3 次就重连（服务器踢人/TCP 半死都靠这条兜） */
        if (due(s_t_pub) != 0U)
        {
            if (step_pub() != 0U)
            {
                s_pub_ok++;
                s_fail = 0U;

                /* ★ 自证（v0.7.0 新增）：成功的发布必须留下痕迹。
                 *   原来这一支只加计数不打日志，现场只能看到"没有失败"，
                 *   分不清"一直在发"和"根本没进过这里" —— 而这两件事的
                 *   排查方向完全相反。第一条立刻打（证明通道通了），
                 *   之后每 6 条（30 s）打一次当心跳，不刷屏。
                 *   frames = 串口2 收到过几帧，与 pub 计数并排看就能判断
                 *   "数据没上来"到底是串口这边还是 MQTT 这边。 */
                if ((s_pub_ok == 1U) || ((s_pub_ok % 6U) == 0U))
                {
                    LOG_I2(TAG, "pub ok=%u frames=%u",
                           s_pub_ok, data_frames());

                    /* ★ v0.7.5 自证：URC 通路四个数（判读表见 uart_at.h）。
                     *   "服务器发了、设备没反应"必须先分清卡在哪一段，
                     *   否则任何修改都是猜。两组各 2 个值，符合一行≤2 的纪律。 */
                    LOG_I2(TAG, "urc b=%u l=%u",
                           g_pump_b, (u16)g_pump_l);
                    LOG_I2(TAG, "urc m=%u t=%u",
                           (u16)g_pump_m, (u16)g_urc_take_n);

                    /* ★ v0.8.0：把 at_pump 收到的**最后一条完整行**原样打出来。
                     *   v0.7.8 的 `b=270 l=11 m=0` 只能证明"11 行的头都不是
                     *   +MSUB"，看不到那 11 行是什么 —— 于是只能猜。
                     *   现在这一行就是线上原文，三种形态一眼分家：
                     *     `+MSUB: "fw/…",32 byte,"0.8.1|http…"`  ⇒ 正常
                     *     `SUB: …` / `MSUB: …`                     ⇒ 行头被吃掉
                     *     `0.8.1|https://…"`                      ⇒ 只收到尾巴
                     *     （空）                                    ⇒ 一行都没凑齐 */
                    LOG_I2(TAG, "urc [%s] n=%u", g_urc, (u16)g_urcn);

                    if ((data_bad_crc() | data_bad_seq()) != 0U)
                    {
                        LOG_I2(TAG, "  bad crc=%u seq=%u",
                               data_bad_crc(), data_bad_seq());
                    }
                }
            }
            else
            {
                s_pub_fail++;
                s_fail++;
                LOG_E2(TAG, "MPUB rc=%u fail=%u", (u16)s_rc, (u16)s_fail);
                if (s_fail >= 3U)
                {
                    LOG_W0(TAG, "3 pubs failed -> reconnect");
                    mqtt_to_back();
                }
            }
            s_t_pub = (u32)(at_ms() + MQTT_PUB_PERIOD_MS);
        }
        s_t_next = (u32)(at_ms() + 2UL);     /* 2 ms 一次，别每圈都进 switch */
        break;

    case MQ_ST_DOWN:
        at_tx_reset();
        tx_lit("AT+MDISCONNECT");
        (void)at_tx_mac(AT_EXP_NONE, 2000U);
        at_tx_reset();
        tx_lit("AT+MIPCLOSE");
        (void)at_tx_mac(AT_EXP_NONE, 2000U);
        s_st     = MQ_ST_BACK;
        s_t_next = (u32)(at_ms() + 3000UL);
        LOG_W0(TAG, "link closed, retry in 3 s");
        break;

    case MQ_ST_BACK:
        /* 退避结束回 PDP 而不是直接 CFG：模组可能刚重启过，承载也一起没了。
         * pdp_up 是幂等的，承载还在时 1~2 s 就走完，代价可接受。 */
        s_st     = MQ_ST_PDP;
        s_t_next = at_ms();
        break;

    default:
        s_st     = MQ_ST_PDP;
        s_t_next = at_ms();
        break;
    }
#endif  /* APP_MQTT_ENABLE */
}

/*==============================================================================
 * 查询
 *============================================================================*/
u8 mqtt_up(void)     { return (u8)((s_st == MQ_ST_RUN) ? 1U : 0U); }
u8 mqtt_state(void)  { return s_st;     }
u16 mqtt_pub_ok(void)   { return s_pub_ok;   }
u16 mqtt_pub_fail(void) { return s_pub_fail; }
