/*==============================================================================
 *  http_ota.h  --  HTTP Range 分片下载引擎（Air780EP AT 模式）
 *  Platform : STC32G12K128 (Keil C251, Huge)
 *  Depends  : uart_at.h, flash_iap.h, param.h, crc32.h, ota_types.h
 *
 *  ── AT 指令时序（每一步都以 URC 为判据，不以 OK 为判据）───────────────
 *   AT+HTTPINIT                                  -> OK
 *   AT+HTTPPARA="URL","http://host/fw.bin"       -> OK
 *   AT+HTTPPARA="CID",1                          -> OK
 *   AT+HTTPPARA="BREAK",<start>,<end>            -> OK   (断点/Range，见下)
 *   AT+HTTPACTION=0                              -> OK   仅表示"已受理"
 *   ...                                          -> +HTTPACTION: 0,206,2048
 *                                                        ^^^^ 这才是完成判据
 *   AT+HTTPREAD=<0>,<n>                          -> +HTTPREAD: <len> + 数据体
 *   AT+HTTPTERM                                  -> OK   (会话收尾，必须调)
 *
 *  ── 单次 Range 片大小 ────────────────────────────────────────────────
 *  AT+HTTPREAD 的返回体 = "+HTTPREAD: <len>\r\n" + <len> 字节原始数据 + "\r\nOK"
 *  4 KB 环形缓冲下，单次 len 取 2048 最稳（头部余量 + 尾部 OK + 下一个 URC
 *  抢跑的空间），绝不要取 4096 —— 会直接把缓冲打满导致丢数据。
 *  TODO(核对手册) Air780EP AT 手册 HTTP 章节中 AT+HTTPREAD 的 length
 *     参数上限（不同固件版本存在 65535 / 819200 的差异），量产前实测确认。
 *============================================================================*/
#ifndef __HTTP_OTA_H
#define __HTTP_OTA_H

#include "ota_mcu.h"
#include "ota_types.h"
#include "ota_partition.h"

/*----------------------------- 分片参数 -------------------------------------*/
#define HTTP_OTA_CHUNK_DEFAULT  2048U   /* 单次 Range 长度（推荐值）           */
#define HTTP_OTA_CHUNK_MIN      512U    /* 下限：小于此值 Range 开销过大        */
#define HTTP_OTA_CHUNK_MAX      3072U   /* 上限：留足 4 KB 缓冲余量，不要到 4096 */
#define HTTP_OTA_RETRY_MAX      3U      /* 单片失败重试次数                    */
#define HTTP_OTA_MAX_FAIL       5U      /* 连续失败上限，超过即整体失败        */

/* HTTP 状态码（+HTTPACTION 第 2 参数） */
#define HTTP_STATUS_OK          200U    /* 全量返回                            */
#define HTTP_STATUS_PARTIAL     206U    /* 分片返回（服务端支持 Range）        */
#define HTTP_STATUS_REQ_RANGE   416U    /* Range 不满足，需重置 offset=0       */

/* 下载引擎内部状态 */
typedef enum
{
    HOTA_IDLE       = 0,   /* 未启动                                          */
    HOTA_INIT       = 1,   /* 刚 AT+HTTPINIT 完成，待设置 URL/PARA            */
    HOTA_REQ        = 2,   /* 已发 AT+HTTPACTION，等待 +HTTPACTION URC        */
    HOTA_READ       = 3,   /* 正在 AT+HTTPREAD 接收数据体并落盘               */
    HOTA_DONE       = 4,   /* 全部字节收齐，CRC 已对齐（未做镜像级校验）      */
    HOTA_FAIL       = 5,   /* 失败，可查询 err 字段                           */
    HOTA_ABORTED    = 6,   /* 被主动中止                                      */
    HOTA_RECOVER    = 7    /* ★ v0.6.7 断线自愈：重建 AT/PDP/HTTP 会话后续传  */
} http_ota_state_t;

/*----------------------------- 引擎上下文 -----------------------------------*/
typedef struct
{
    http_ota_state_t state;       /* 引擎状态                                    */
    ota_ret_t        err;         /* 最近一次错误码（state==HOTA_FAIL 时有效）    */

    u8 *url;               /* 下载 URL（指向 param 中的 ota_url，不拷贝）  */
    u32  flash_base;              /* 落盘目标：暂存区起始（IAP 相对地址,
                                    OTA_STAGE_BASE 当前为 0）                  */
    u32  total;                   /* 镜像总长度（含镜像头），来自 Content-Length  */
    u32  offset;                  /* 已成功写入并校验的字节数（断点基准）         */
    u32  seg_start;               /* 本轮 Range 起始（绝对偏移）                  */
    u16  chunk;                   /* 本轮 Range 长度，<= HTTP_OTA_CHUNK_MAX       */
    u16  got;                     /* 本轮已接收字节数                              */
    u16  expect;                  /* 本轮 +HTTPACTION 报告的长度                  */
    u16  status_code;             /* 本轮 HTTP 状态码（200/206/416/...）          */

    u8   retry;                   /* 当前分片已重试次数                            */
    u8   fail_cnt;                /* 连续失败计数                                  */
    u8   range_supported;         /* 1 = 服务端支持 Range（返回过 206）            */
    u8   use_break;               /* 1 = 使用 AT+HTTPPARA="BREAK" 设断点           */

    u32  crc_running;             /* 已下载数据运行 CRC32                          */
    u32  crc_expect;              /* 镜像头声明的正文 CRC32（校验阶段用）          */
    u32  img_len;                 /* 镜像头声明的正文长度                          */
    u32  last_save_ms;            /* 上次断点落盘时刻，用于节流                    */
    u8 *io_buf;           /* 数据体接收缓冲，长度 chunk，由 init 指定      */
} http_ota_ctx_t;

/*==============================================================================
 * 初始化与配置
 *============================================================================*/
typedef struct
{
    u8 *url;        /* 下载 URL，字符串，长度 < PARAM_URL_MAX             */
    u8 *io_buf;     /* 接收缓冲，必须 >= HTTP_OTA_CHUNK_DEFAULT（4 KB 内）*/
    u16 chunk;             /* 单次 Range 长度；0 则用 HTTP_OTA_CHUNK_DEFAULT     */
    u8  use_break;         /* 1 = 用 BREAK/BREAKEND 设断点；0 = 用 Range 头      */
    u8  auto_resume;       /* 1 = 启动时自动从 param 断点续传                    */
} http_ota_cfg_t;

/**
 * @brief  初始化下载引擎（不发起网络请求）。
 * @param  cfg  配置结构体，不可为 NULL；内容会被拷贝到内部上下文（url/io_buf 除外）。
 * @return OTA_OK / OTA_ERR_PARAM（url 或 io_buf 为 NULL、chunk 越界）。
 */
ota_ret_t http_ota_init(const http_ota_cfg_t *cfg);

/**
 * @brief  开始一次下载（内部会 AT+HTTPINIT + 设置 URL/PARA）。
 * @param  expect_size  期望镜像总长；0 表示以服务端 Content-Length 为准。
 * @param  start_offset 起始偏移；0 表示从头下载。若 auto_resume=1 且 param
 *                      中存在有效断点，则使用断点值覆盖本参数。
 * @return OTA_OK 已进入 HOTA_REQ；
 *         OTA_ERR_BUSY    引擎非 IDLE 或 AT 通道被占用；
 *         OTA_ERR_NET     AT+HTTPINIT 或 HTTPPARA 返回 ERROR；
 *         OTA_ERR_STATE   Bootloader 阶段调用（不允许）。
 */
ota_ret_t http_ota_start(u32 start_offset, u32 expect_size);

/**
 * @brief  引擎主循环：推进状态机（发下一片 / 收数据 / 落盘 / 判完成）。
 * @return 当前 http_ota_state_t；调用方据 state 与 err 做上层决策。
 * @note   必须每 <= 20 ms 调用一次；单次调用内部可能阻塞数十 ms（AT 等待），
 *         因此不要在中断或 URC 回调里调它。
 */
http_ota_state_t http_ota_step(void);

/**
 * @brief  中止下载并收尾（发 AT+HTTPTERM，清断点可选）。
 * @param  keep_resume  1 = 保留断点，下次可续传；0 = 清零断点与暂存区标记。
 * @return OTA_OK / OTA_ERR_NET（HTTPTERM 失败，可忽略）。
 */
ota_ret_t http_ota_abort(u8 keep_resume);

/**
 * @brief  收尾但保留结果（下载完成后由 OTA 状态机调用）。
 * @return OTA_OK。
 */
ota_ret_t http_ota_close(void);

/*==============================================================================
 * 断点 / Range 参数设置（对应 AT+HTTPPARA 的 BREAK 与 Range 两套做法）
 *============================================================================*/

/**
 * @brief  设置断点续传参数，下发 AT+HTTPPARA="BREAK",<start>,<end>。
 * @param  start  断点起始偏移（字节）。
 * @param  end    断点结束偏移（字节）；传 0 表示"到文件末尾"（若模组支持）。
 * @return OTA_OK / OTA_ERR_NET（模组回 ERROR，说明不支持，调用方应改用
 *         http_ota_set_range_header()）。
 * @note   TODO(核对手册) Air780EP 的 BREAK 参数是 "BREAK",<start>,<end>
 *         还是 "BREAK",<len>，"BREAKEND" 是独立命令还是第二参数，须按手册
 *         实际语法修正本函数内部拼接串；头文件接口保持不变。
 */
ota_ret_t http_ota_set_break(u32 start, u32 end);

/**
 * @brief  用自定义请求头方式设置 Range（服务端 206 兼容性更好）。
 * @param  start  起始偏移。
 * @param  len    分片长度。
 * @return OTA_OK / OTA_ERR_NET。
 * @note   内部下发 AT+HTTPPARA="USERDATA","Range: bytes=<start>-<end>\r\n"
 *         TODO(核对手册) 确认 Air780EP 是否支持 USERDATA 自定义头，
 *            以及是否需要自行补 CRLF。
 */
ota_ret_t http_ota_set_range_header(u32 start, u16 len);

/**
 * @brief  查询服务端是否支持 Range（由最近一次 206 响应推断）。
 * @return 1 支持；0 不支持或尚未确认。
 */
u8 http_ota_range_supported(void);

/*==============================================================================
 * 偏移量持久化（断点续传的落盘接口，底层走 param 参数区）
 *============================================================================*/

/**
 * @brief  把当前下载进度（offset/total/crc_running）写入参数区。
 * @return OTA_OK / OTA_ERR_IO。
 * @note   内部有 5 s 节流：距上次落盘不足 5 s 时只更新 RAM 不写 Flash，
 *         避免 512 B 扇区被高频擦写。掉电前由 ota_fsm 强制调 param_save(1)。
 */
ota_ret_t http_ota_save_offset(void);

/**
 * @brief  从参数区读回断点偏移。
 * @return 有效断点字节数；0 表示无断点（从头下载）。
 * @note   返回值会与 total 做一致性检查，offset > total 视为无效并返回 0。
 */
u32 http_ota_load_offset(void);

/**
 * @brief  清零断点（下载完成 / 校验失败 / 主动取消时调用）。
 * @return OTA_OK / OTA_ERR_IO。
 */
ota_ret_t http_ota_reset_offset(void);

/*==============================================================================
 * 进度查询
 *============================================================================*/

/**
 * @brief  当前已下载字节数。
 * @return 0 ~ total。
 */
u32 http_ota_get_offset(void);

/**
 * @brief  镜像总长度（Content-Length）。
 * @return 字节数；未知返回 0。
 */
u32 http_ota_get_total(void);

/**
 * @brief  进度百分比。
 * @return 0 ~ 100；total 未知返回 0。
 */
u8 http_ota_get_progress(void);

/**
 * @brief  最近一次错误码。
 * @return ota_ret_t。
 */
ota_ret_t http_ota_get_error(void);

/**
 * @brief  取引擎上下文只读指针（供 ota_fsm 读取 crc/img_len 等字段）。
 * @return 上下文指针，永不为 NULL。
 */
const http_ota_ctx_t *http_ota_ctx(void);

/*==============================================================================
 * ★ v0.6.7 断线自愈 —— 模组重启 / 掉线之后不放弃，从已落盘处继续
 *
 *   现场依据：Y100P 会因供电瞬间跌落、网络重注册而**自己重启**
 *   （COM7 掉线后重打 ^MODE / +E_UTRAN / +CGEV / +NITZ 就是它）。
 *   重启期间所有 AT 超时、HTTP 会话作废，但 **Flash 里已写入的字节是好的**
 *   ⇒ 没有任何理由从 0 重来。
 *
 *   自愈链（每一步都有界）：
 *     HOTA_RECOVER → 等模组应答(≤12×600 ms) → ATE0 → PDP → HTTPINIT
 *                  → HTTPSSL → URL → CID → 回 HOTA_REQ，从 s_offset 续传
 *   连续 HOTA_RECOVER_MAX 次仍失败 ⇒ 整包重下（擦 STAGING / offset 归零）；
 *   整包重下 HOTA_RESTART_MAX 次仍不成 ⇒ 才判 HOTA_FAIL。
 *
 *   ★ 为什么续传一定安全：s_offset 只在**整片 1024 B 全部收到并写成功**之后才
 *     前进 ⇒ 它永远落在一片边界上，按该偏移重取不重不漏。
 *   ★ 不再有"运行 CRC"：正文 CRC 由 BL 从 Flash 重算（A2 流程），
 *     续传不需要携带任何 CRC 状态，掉电也不会残留半截状态。
 *============================================================================*/
u8  http_ota_get_recover(void);   /* 本次下载已用掉的重连次数            */
u8  http_ota_get_restart(void);   /* 本次下载已整包重下过的次数          */
u16 http_ota_get_chunk(void);     /* 已完成的片数（进度上报用）          */
u8  http_ota_get_try(void);       /* 当前片的失败重取次数（进度上报用）  */


/*==============================================================================
 * URC 钩子（由 http_ota.c 内部在 init 时注册到 uart_at，此处仅声明）
 *============================================================================*/

/**
 * @brief  +HTTPACTION URC 处理：记录 status_code / expect，推进状态。
 * @param  line  URC 整行，形如 "+HTTPACTION: 0,206,2048"。
 * @param  len   行长。
 * @return OTA_OK。
 */
ota_ret_t http_ota_urc_action(const char *line, u16 len);

/**
 * @brief  +HTTPREAD URC 处理：进入二进制模式并把数据体写入暂存区。
 * @param  line  URC 整行，形如 "+HTTPREAD: 2048"。
 * @param  len   行长。
 * @return OTA_OK / OTA_ERR_IO（落盘失败时置 state=HOTA_FAIL）。
 */
ota_ret_t http_ota_urc_read(const char *line, u16 len);

/**
 * @brief  把 PDP 承载拉起来并提供 IP（模拟量：CPIN / CSQ / CGATT 轮询 / SAPBR）。
 *
 * ★ v0.7.0 新增对外入口：MQTT 连服务器前也必须有活动承载 ——
 *   `AT+SSLMIPSTART` / `AT+MIPSTART` 在没有承载时直接回 ERROR，
 *   表现为 "tcp rc=1"，很容易被误判成 TLS 或用户名的问题。
 *
 * 内部就是 http_ota 下载流程用的那套 hota_pdp_up()（v0.6.14~v0.6.16 现场打磨）：
 *   判据只有一条 —— **看到带真实 IP 的那一行**（排除 0.0.0.0），不看 OK。
 *   三条 SAPBR 设置命令幂等，失败只记录不判死。
 *
 * @return OTA_OK = 已拿到 IP；OTA_ERR_NET = 没拿到（调用方退避重试）。
 * @note   内含 20 s 级等待（等注网），**不要在快循环里反复调**。
 */
ota_ret_t http_ota_pdp_up(void);

#endif /* __HTTP_OTA_H */
