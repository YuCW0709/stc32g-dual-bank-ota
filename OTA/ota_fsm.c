/*==============================================================================
 *  ota_fsm.c  --  OTA 状态机（APP 侧，HTTP 直拉精简版）
 *  Platform : STC32G12K128 (Keil C251, CodeRomSize = Huge, Memory Model = LARGE)
 *  Depends  : ota_fsm.h, http_ota.h, param.h, flash_iap.h, crc32.h, log.h
 *
 *  ── 本轮范围（最快跑通命脉）──────────────────────────────────────────────
 *   APP 侧状态机只负责"下载 → 落盘 → 置 PENDING_REBOOT → 复位"。
 *   校验（镜像头 + 正文 CRC）、搬运到 APP 区、COMMITTED 收尾，全部交给
 *   **Bootloader** 已有的、已上板验证的 A2 流程去做（见 boot_main.c）。
 *
 *   因此本文件不实现完整的"事件队列 + 迁移表"（那是 MQTT 控制面阶段的活），
 *   只实现一条直线流程：
 *       IDLE --start(url)--> DOWNLOADING --http done--> 置 PENDING_REBOOT
 *   下载完成后调用方（app_main）软复位，BL 接手。
 *
 *  ── 铁律遵从 ─────────────────────────────────────────────────────────────
 *   缓冲区 static；形参第一句拷进 static；日志固定槽宏；常量裸 const。
 *============================================================================*/
#include "ota_fsm.h"
#include "http_ota.h"
#include "param.h"
#include "flash_iap.h"
#include "crc32.h"
#include "log.h"
#include "bsp_timer.h"
#include "bsp_wdt.h"

#define TAG_FSM  "FSM"
/* ★ v0.6.7：下载进度/健康度统一用这个 tag 打，UART1 上可直接 grep "[OTA]" */
#define TAG_OTA  "OTA"

/*==============================================================================
 * 模块级状态
 *============================================================================*/
static ota_state_t xdata s_state;
static ota_ret_t   xdata s_last_err;

static http_ota_cfg_t xdata s_cfg;      /* 下载引擎配置             */

static u8 xdata s_inited;

/* ★ v0.6.7 进度上报的去重记录（只在"数变了"时打一行，避免刷屏） */
static u32 xdata s_rep_off;             /* 上次已上报的偏移          */
static u8  xdata s_rep_rec;             /* 上次已上报的重连次数      */
static u8  xdata s_rep_rs;              /* 上次已上报的整包重下次数  */

/*==============================================================================
 * 状态名 / 事件名（只读字符串，常量进 HCONST）
 *============================================================================*/
static const char * const s_state_name[] =
{
    "IDLE", "DOWNLOADING", "VERIFYING", "PENDING_REBOOT", "COMMITTED", "ROLLED_BACK"
};

static const char * const s_evt_name[] =
{
    "NONE", "OTA_CMD", "HTTP_PROGRESS", "HTTP_DONE", "HTTP_FAIL",
    "VERIFY_OK", "VERIFY_FAIL", "REBOOT_REQ", "BOOT_OK", "BOOT_FAIL",
    "ABORT", "TIMEOUT", "APPLY_FAIL"
};

const char *ota_fsm_state_name(ota_state_t st)
{
    if (st > OTA_ST_ROLLED_BACK) return "UNKNOWN";
    return s_state_name[st];
}

const char *ota_fsm_event_name(ota_event_t ev)
{
    if (ev >= OTA_EV_MAX) return "EV_UNKNOWN";
    return s_evt_name[ev];
}

/*==============================================================================
 * 初始化
 *============================================================================*/
ota_state_t ota_fsm_init(void)
{
    s_state = OTA_ST_IDLE;
    s_last_err = OTA_OK;
    s_inited = 1U;
    s_rep_off = 0UL;
    s_rep_rec = 0U;
    s_rep_rs  = 0U;

    /* 从 param 恢复：若上次停在 DOWNLOADING，说明掉电中断了下载，回到 IDLE */
    {
        param_t *p = param_get();
        if (p->ota_state == (u8)OTA_ST_DOWNLOADING)
        {
            LOG_W0(TAG_FSM, "resume from interrupted download -> IDLE");
            p->ota_state = (u8)OTA_ST_IDLE;
            param_clear_download();
        }
    }
    return s_state;
}

/*==============================================================================
 * 启动下载
 *============================================================================*/
ota_ret_t ota_fsm_start_download(const char *url)
{
    param_t *p;

    if (!s_inited) return OTA_ERR_STATE;
    if (s_state != OTA_ST_IDLE) return OTA_ERR_BUSY;
    if (url == 0) return OTA_ERR_PARAM;

    /* 拷贝 url 到 param（param 区有持久化空间） */
    p = param_get();
    {
        static u16 xdata i;
        for (i = 0; i < (PARAM_URL_MAX - 1U); i++)
        {
            p->ota_url[i] = (u8)url[i];
            if (url[i] == 0) break;
        }
        p->ota_url[PARAM_URL_MAX - 1U] = 0;
    }

    /* 配置下载引擎 */
    s_cfg.url = p->ota_url;
    s_cfg.io_buf = 0;      /* 本轮 http_ota 用内部缓冲，不外部传入 */
    s_cfg.chunk = HTTP_OTA_CHUNK_DEFAULT;
    s_cfg.use_break = 0U;
    s_cfg.auto_resume = 0U;

    if (http_ota_init(&s_cfg) != OTA_OK)
    {
        LOG_E0(TAG_FSM, "http_ota_init fail");
        s_last_err = OTA_ERR_PARAM;
        return OTA_ERR_PARAM;
    }

    /* 擦 STAGING，准备落盘 */
    if (flash_iap_erase_range(OTA_STAGE_BASE, OTA_STAGE_SIZE) != OTA_OK)
    {
        LOG_E0(TAG_FSM, "erase STAGING fail");
        s_last_err = OTA_ERR_IO;
        return OTA_ERR_IO;
    }

    /* 置状态 DOWNLOADING 并落盘 */
    p->ota_state = (u8)OTA_ST_DOWNLOADING;
    p->dl_offset = 0UL;
    p->dl_total  = 0UL;
    param_save(1);

    if (http_ota_start(0UL, 0UL) != OTA_OK)
    {
        LOG_E0(TAG_FSM, "http_ota_start fail");
        s_last_err = OTA_ERR_NET;
        p->ota_state = (u8)OTA_ST_IDLE;
        return OTA_ERR_NET;
    }

    s_state = OTA_ST_DOWNLOADING;
    s_rep_off = 0UL;
    s_rep_rec = 0U;
    s_rep_rs  = 0U;
    LOG_I0(TAG_FSM, "download started");
    LOG_I0(TAG_OTA, "start (stage=DL)");
    return OTA_OK;
}

/*==============================================================================
 * ★★★ v0.6.7 进度上报（串口1 = 日志口，115200 8N1）
 *
 *   需求原文："串口1 报告下载状态、进度"。
 *   设计原则（现场看日志的人是按行读的，不是按屏幕刷的）：
 *     · 一行一件事，行首固定 `[OTA]` 前缀 ⇒ 一眼能从 AT 流水里摘出来；
 *     · 只在**数值变化**时打，不做定时轮询 ⇒ 正常 23 片正好 23 行；
 *     · 百分比用镜像头解出的真实总长算（见 http_ota 的头解析），
 *       所以 `100%` 就是真的 100%，不是"最后一片收到了"；
 *     · 重连/重下是"异常但可恢复"，用一条独立行明确说清"丢在哪、从哪续"。
 *   形态示例：
 *     [OTA] 0% 0/23104
 *     [OTA] 4% 1024/23104
 *     [OTA] link lost at 5120, reconnect #1
 *     [OTA] 22% 5120/23104
 *     [OTA] 100% 23104/23104
 *     [OTA] done: 23104 B, 23 chunks, 1 reconnect, 0 rerun
 *============================================================================*/
static void ota_rep_progress(u8 force)
{
    static u32 xdata off;
    static u32 xdata tot;
    static u8  xdata pct;

    off = http_ota_get_offset();
    if ((force == 0U) && (off == s_rep_off)) return;
    s_rep_off = off;

    tot = http_ota_get_total();
    if (tot == 0UL)
    {
        /* ★ v0.6.18：总长还没解出来时**不许**打 "0% x/0"。
         *   那句话会被读成"下载卡住了"，而实际上字节在稳定增长 ——
         *   step68 就是这个误会。这里如实说"总数未知"。 */
        LOG_I1(TAG_OTA, "off=%lu total=unknown", off);
        return;
    }

    pct = http_ota_get_progress();
    LOG_I3(TAG_OTA, "%u%% %lu/%lu", (u16)pct, off, tot);
}

/* 重连/重下：只在计数变了时播报，并说明"断点在哪" */
static void ota_rep_health(void)
{
    static u8 xdata rec;
    static u8 xdata rs;

    rs  = http_ota_get_restart();
    rec = http_ota_get_recover();

    if (rs != s_rep_rs)
    {
        s_rep_rs = rs;
        s_rep_rec = rec;
        LOG_W1(TAG_OTA, "rerun whole image #%u from 0", (u16)rs);
        return;
    }
    if (rec != s_rep_rec)
    {
        s_rep_rec = rec;
        LOG_W2(TAG_OTA, "link lost at %lu, reconnect #%u",
               http_ota_get_offset(), (u16)rec);
    }
}

/*==============================================================================
 * ★★★ v0.6.19：**下完那一刻的落地自检**（只读 Flash，判据与 BL 同源）
 *
 *   为什么要它 —— step69 的教训：23 片全下完、进度 100%、日志里没有一行
 *   异常，可 STAGING 里其实是坏的（首片数据体中段少了 2 个字节）。
 *   固件照样宣布 `PENDING_REBOOT`，用户按提示掉电重上电，才在 BL 里看到
 *   `STAGING fail rc=12` —— 白掉一次电、白等一轮。
 *   ⇒ 判据必须在**下完那一刻**就给，而不是等下一次上电。
 *
 *   判据与 ota_verify.c 前 9 步同源（magic / hdr_len / hdr_ver / 正文长度 /
 *   正文 CRC32），但**BL 仍是最终权威**：这里只是"别让用户白跑一趟"，
 *   不做搬运、不写任何 Flash。
 *
 *   失败时把 now/want 两个数都打出来：
 *     · want 离谱（0 或全 FF） ⇒ 头本身没写进去（首片就坏了）；
 *     · want 正常、now 不同    ⇒ 正文中途有字节被改写（丢字节/写法问题）。
 *   这两种现场长得一样，只有数值能分开 —— 所以必须打两个数。
 *============================================================================*/
static u8 ota_stage_landed(void)
{
    static u8  xdata h[OTA_IMG_HDR_RESERVE];   /* 64 B：既读头又当 CRC 读缓冲 */
    static u32 xdata body;                     /* 正文长度（镜像头里）         */
    static u32 xdata want;                     /* 头里记的正文 CRC32           */
    static u32 xdata now;                      /* 现算的正文 CRC32             */
    static u32 xdata off;                      /* CRC 读到第几字节             */
    static u16 xdata n;
    static u16 xdata hv;

    if (flash_iap_read(OTA_STAGE_BASE, h, (u16)OTA_IMG_HDR_RESERVE) != OTA_OK)
    {
        LOG_E0(TAG_OTA, "stg read FAIL (iap head)");
        return 0U;
    }

    if ((h[0] != (u8)'O') || (h[1] != (u8)'T') ||
        (h[2] != (u8)'A') || (h[3] != (u8)'I'))
    {
        LOG_E2(TAG_OTA, "stg head %x %x (want 4f 54)", (u16)h[0], (u16)h[1]);
        return 0U;
    }

    hv = ((u16)h[4] << 8) | (u16)h[5];
    if (hv != (u16)OTA_IMG_HDR_RESERVE)
    {
        LOG_E1(TAG_OTA, "stg hdr_len=%u (want 64)", hv);
        return 0U;
    }

    hv = ((u16)h[6] << 8) | (u16)h[7];
    if (hv != 1U)
    {
        LOG_E1(TAG_OTA, "stg hdr_ver=%u (want 1)", hv);
        return 0U;
    }

    /* 镜像头多字节字段是**大端**（C251 本身就高字节在前，打包工具也用 ">"） */
    body = ((u32)h[12] << 24) | ((u32)h[13] << 16) |
           ((u32)h[14] << 8)  | (u32)h[15];
    want = ((u32)h[16] << 24) | ((u32)h[17] << 16) |
           ((u32)h[18] << 8)  | (u32)h[19];

    if ((body == 0UL) || ((body + OTA_IMG_HDR_RESERVE) > OTA_STAGE_SIZE))
    {
        LOG_E1(TAG_OTA, "stg img_len=%lu out of range", body);
        return 0U;
    }

    now = crc32_begin();
    off = 0UL;
    while (off < body)
    {
        n = (u16)(((body - off) > (u32)OTA_IMG_HDR_RESERVE)
                  ? (u16)OTA_IMG_HDR_RESERVE : (u16)(body - off));
        if (flash_iap_read(OTA_STAGE_BASE + OTA_IMG_HDR_RESERVE + off, h, n) != OTA_OK)
        {
            LOG_E0(TAG_OTA, "stg read FAIL (iap body)");
            return 0U;
        }
        now  = crc32_update(now, h, n);
        off += (u32)n;
        bsp_wdt_feed();
    }
    now = crc32_final(now);

    if (now != want)
    {
        LOG_E1(TAG_OTA, "stg crc now =%lx", now);
        LOG_E1(TAG_OTA, "stg crc want=%lx", want);
        return 0U;
    }

    LOG_I1(TAG_OTA, "stg crc ok, body %lu B", body);
    return 1U;
}

/*==============================================================================
 * 主循环推进
 *============================================================================*/
ota_state_t ota_fsm_poll(void)
{
    http_ota_state_t hs;
    param_t *p;

    if (!s_inited) return s_state;

    if (s_state != OTA_ST_DOWNLOADING) return s_state;

    hs = http_ota_step();

    switch (hs)
    {
        case HOTA_READ:
        case HOTA_REQ:
        case HOTA_INIT:
            /* 下载中：更新断点（节流由 http_ota 内部处理，这里每次同步） */
            p = param_get();
            p->dl_offset = http_ota_get_offset();
            p->dl_total  = http_ota_get_total();
            /* ★ v0.6.7：进度与健康度各打一行（内部去重） */
            ota_rep_progress(0U);
            ota_rep_health();
            break;

        case HOTA_RECOVER:
            /* 会话正在重建：把断点照样同步进 param，这样"真掉电"也接得上 */
            p = param_get();
            p->dl_offset = http_ota_get_offset();
            ota_rep_health();
            break;

        case HOTA_DONE:
            /* 下载完成：先自检落地结果，通过了才置 PENDING_REBOOT */
            p = param_get();
            ota_rep_progress(1U);
            LOG_I4(TAG_OTA, "done: %lu B, %u chunks, %u reconnect, %u rerun",
                   http_ota_get_offset(), http_ota_get_chunk(),
                   (u16)http_ota_get_recover(), (u16)http_ota_get_restart());

            /* ★★★ v0.6.19：没有这一句，坏镜像会被当成成功（见上面函数注释） */
            if (ota_stage_landed() == 0U)
            {
                LOG_E0(TAG_OTA, "stg BAD -> NOT armed, do not power cycle");
                LOG_E0(TAG_OTA, "  fix the transfer, then download again");
                p->ota_state = (u8)OTA_ST_IDLE;
                param_clear_download();
                s_last_err = OTA_ERR_VERIFY;
                s_state = OTA_ST_IDLE;
                break;
            }

            p->ota_state = (u8)OTA_ST_PENDING_REBOOT;
            p->dl_offset = http_ota_get_offset();
            p->dl_total  = http_ota_get_total();
            /* pending_ver 由 BL 校验镜像头后回填；这里先不动 */
            param_save(1);
            s_state = OTA_ST_PENDING_REBOOT;
            LOG_I0(TAG_FSM, "-> PENDING_REBOOT, reboot now");
            break;

        case HOTA_FAIL:
        case HOTA_ABORTED:
            s_last_err = http_ota_get_error();
            LOG_E1(TAG_OTA, "download FAIL err=%u", (u16)s_last_err);
            LOG_E2(TAG_OTA, "  stopped at %lu, %u chunks done",
                   http_ota_get_offset(), http_ota_get_chunk());
            p = param_get();
            p->ota_state = (u8)OTA_ST_IDLE;
            param_clear_download();
            s_state = OTA_ST_IDLE;
            break;

        default:
            break;
    }

    return s_state;
}

/*==============================================================================
 * 查询接口
 *============================================================================*/
ota_state_t ota_fsm_state(void)  { return s_state; }
ota_ret_t ota_fsm_last_error(void) { return s_last_err; }

/*==============================================================================
 * 事件投递 / 迁移（本轮简化：仅占位，MQTT 阶段实现）
 *============================================================================*/
ota_ret_t ota_fsm_post(ota_event_t ev, const void *p_arg)
{
    return OTA_ERR_STATE;
}

ota_ret_t ota_fsm_transit(ota_state_t from, ota_state_t to, ota_event_t ev)
{
    return OTA_ERR_STATE;
}

u8 ota_fsm_is_accepted(ota_state_t st, ota_event_t ev)
{
    return 0U;
}

ota_ret_t ota_fsm_on_enter(ota_state_t st)
{
    return OTA_OK;
}

ota_ret_t ota_fsm_on_exit(ota_state_t st)
{
    return OTA_OK;
}
