/*==============================================================================
 *  mqtt_ctrl.h  --  MQTT 控制面（仅跑信令，不跑固件数据）
 *  Platform : STC32G12K128 (Keil C251, Huge)
 *  Depends  : uart_at.h, param.h, ota_types.h
 *
 *  AT 指令映射（Air780EP）
 *      AT+MCONFIG=<clientid>,<user>,<pass>         连接参数
 *      AT+MIPSTART="<host>",<port>                 建 TCP
 *      AT+MCONNECT=<clean>,<keepalive>             发 CONNECT
 *      AT+MSUB="<topic>",<qos>                     订阅
 *      AT+MPUB="<topic>",<qos>,<retain>,"<msg>"    发布（payload 为字符串）
 *      AT+MPUBEX="<topic>",<qos>,<retain>,<len>    发布（指定长度，可含二进制）
 *      AT+MDISCONNECT / AT+MIPCLOSE                断开
 *  下行 URC 形如：+MSUB: "<topic>",<len> byte,<payload>
 *  TODO(核对手册) 确认 URC 的精确格式（是否有 "byte" 字样、payload 是否
 *     带引号），以及 +MPUB 单条 payload 的最大长度限制。
 *
 *  设计约束（架构决策，不可推翻）
 *  1) 固件数据只走 HTTP，MQTT 只承载：升级指令下发、进度上报、心跳、告警。
 *  2) 因此本模块 payload 上限 MQTT_PAYLOAD_MAX = 512 B，且只处理可打印文本
 *     或极短的 JSON（手写拼装，不引 cJSON，省 8~12 KB code）。
 *  3) MQTT 与 HTTP 共用一路 AT 通道，publish 前必须 at_lock()，且允许失败
 *     （下载期间 MQTT 可短暂让路，心跳由 keepalive 兜底）。
 *============================================================================*/
#ifndef __MQTT_CTRL_H
#define __MQTT_CTRL_H

#include "ota_mcu.h"
#include "ota_types.h"

#define MQTT_PAYLOAD_MAX    512U    /* 单条 payload 上限（含 '\0'）            */
#define MQTT_TOPIC_MAX      96U     /* 主题字符串上限                          */
#define MQTT_QOS_MAX        2U      /* 支持 QoS 0/1/2                          */
#define MQTT_RECONN_MIN_MS  2000U   /* 重连最小间隔                            */
#define MQTT_RECONN_MAX_MS  60000U  /* 重连退避上限                            */

/* 连接状态 */
typedef enum
{
    MQTT_ST_DISCONNECTED = 0,   /* 未连接（初始 / 已断开 / 连接失败）        */
    MQTT_ST_TCP_OPEN     = 1,   /* TCP 已建立，未发 CONNECT                  */
    MQTT_ST_CONNECTING   = 2,   /* 已发 AT+MCONNECT，等 CONNACK              */
    MQTT_ST_CONNECTED    = 3,   /* CONNACK 成功，可收发                      */
    MQTT_ST_SUBSCRIBING  = 4,   /* 订阅中                                    */
    MQTT_ST_READY        = 5,   /* 已连接且订阅完成，业务可用                */
    MQTT_ST_ERROR        = 6    /* 不可恢复错误，需重新初始化                */
} mqtt_state_t;

/*==============================================================================
 * 配置与回调
 *============================================================================*/
typedef struct
{
    char *host;            /* 服务器域名或 IP，长度 < 48                     */
    u16   port;            /* 端口，1883 / 8883                             */
    char *client_id;       /* 客户端 ID，建议 "STC32G-<MAC 后 6 位>"        */
    char *username;        /* 用户名，可为 NULL                             */
    char *password;        /* 密码，可为 NULL                               */
    u16   keepalive_s;     /* 保活秒数，0 则用 60                           */
    u8    clean_session;   /* 1 = clean session                             */
} mqtt_cfg_t;

/**
 * @brief  下行消息回调原型（在 mqtt_ctrl_poll() 上下文执行，非中断）。
 * @param  topic    主题字符串（以 '\0' 结尾）。
 * @param  payload  消息体，不以 '\0' 结尾，需按 len 处理。
 * @param  len      消息体字节数。
 * @param  qos      本条消息的 QoS。
 * @return OTA_OK 表示已处理；非 OTA_OK 仅记日志。
 */
typedef ota_ret_t (*mqtt_msg_cb_t)(const char *topic,
                                   const u8 *payload,
                                   u16 len,
                                   u8 qos);

/*==============================================================================
 * 连接管理
 *============================================================================*/

/**
 * @brief  初始化 MQTT 控制面：注册 URC、清状态（不发网络请求）。
 * @param  cfg     配置，内容被拷贝到内部静态缓存。
 * @param  on_msg  下行消息回调，不可为 NULL。
 * @return OTA_OK / OTA_ERR_PARAM（host/client_id 为空或过长）。
 * @note   必须在 at_uart_init() 与 param_init() 之后调用。
 */
ota_ret_t mqtt_ctrl_init(const mqtt_cfg_t *cfg, mqtt_msg_cb_t on_msg);

/**
 * @brief  发起连接：AT+MCONFIG -> AT+MIPSTART -> AT+MCONNECT（阻塞至 CONNACK）。
 * @param  timeout_ms  总超时，建议 AT_TO_MQTT_CONN。
 * @return OTA_OK 已进入 MQTT_ST_CONNECTED；
 *         OTA_ERR_TIMEOUT  某一步无应答；
 *         OTA_ERR_NET      模组回 ERROR / CONNACK 拒绝；
 *         OTA_ERR_BUSY     AT 通道被 HTTP 占用（下载期间属正常，稍后重试）。
 */
ota_ret_t mqtt_ctrl_connect(u16 timeout_ms);

/**
 * @brief  断开连接（AT+MDISCONNECT + AT+MIPCLOSE）。
 * @param  timeout_ms  单步超时。
 * @return OTA_OK（无论模组是否回 OK，本地状态一定置 DISCONNECTED）。
 */
ota_ret_t mqtt_ctrl_disconnect(u16 timeout_ms);

/**
 * @brief  非阻塞自动重连：按指数退避判断是否该重连，并推进一次。
 * @return 当前 mqtt_state_t。
 * @note   主循环每 100 ms 调一次即可；内部自行处理退避与最大次数。
 */
mqtt_state_t mqtt_ctrl_reconnect_tick(void);

/**
 * @brief  查询当前状态。
 * @return mqtt_state_t。
 */
mqtt_state_t mqtt_ctrl_state(void);

/**
 * @brief  是否处于业务可用状态（READY）。
 * @return 1 可用；0 不可用。
 */
u8 mqtt_ctrl_is_ready(void);

/*==============================================================================
 * 订阅 / 发布
 *============================================================================*/

/**
 * @brief  订阅主题（AT+MSUB），成功后状态置 MQTT_ST_READY。
 * @param  topic  主题，长度 < MQTT_TOPIC_MAX。
 * @param  qos    0/1/2，>2 会被截断为 2。
 * @return OTA_OK / OTA_ERR_NET / OTA_ERR_TIMEOUT / OTA_ERR_BUSY / OTA_ERR_PARAM。
 */
ota_ret_t mqtt_ctrl_subscribe(const char *topic, u8 qos);

/**
 * @brief  取消订阅（AT+MUNSUB）。
 * @param  topic  主题。
 * @return OTA_OK / OTA_ERR_NET / OTA_ERR_TIMEOUT。
 */
ota_ret_t mqtt_ctrl_unsubscribe(const char *topic);

/**
 * @brief  发布可打印文本消息（AT+MPUB）。
 * @param  topic   主题。
 * @param  qos     0/1/2。
 * @param  retain  1 = 保留消息。
 * @param  msg     以 '\0' 结尾的文本，长度 < MQTT_PAYLOAD_MAX。
 * @return OTA_OK / OTA_ERR_PARAM（含双引号或超长）/ OTA_ERR_NET / OTA_ERR_TIMEOUT /
 *         OTA_ERR_BUSY（AT 通道被 HTTP 占用，调用方应稍后重试）。
 * @note   文本中不得出现 '"' 与 '\r' '\n'，否则会破坏 AT 帧；需要时改调
 *         mqtt_ctrl_publish_bin()。
 */
ota_ret_t mqtt_ctrl_publish(const char *topic, u8 qos, u8 retain, const char *msg);

/**
 * @brief  发布指定长度的二进制/任意字节消息（AT+MPUBEX）。
 * @param  topic  主题。
 * @param  qos    0/1/2。
 * @param  retain 1 = 保留消息。
 * @param  data   数据指针，不可为 NULL。
 * @param  len    字节数，1 ~ MQTT_PAYLOAD_MAX-1。
 * @return 同 mqtt_ctrl_publish()。
 * @note   TODO(核对手册) 确认 AT+MPUBEX 的参数顺序与是否需要进入
 *         透传模式（部分固件要求先 AT+MPUBEX 再发裸数据再 AT+MPUBEX=1）。
 */
ota_ret_t mqtt_ctrl_publish_bin(const char *topic, u8 qos, u8 retain,
                                const u8 *src, u16 len);

/**
 * @brief  上报 OTA 进度（固定 JSON 格式，供云端展示）。
 * @param  state    OTA 状态机状态（u8）。
 * @param  percent  0~100。
 * @param  ver      当前版本号。
 * @param  err      错误码，0 表示无错误。
 * @return OTA_OK / OTA_ERR_BUSY / OTA_ERR_NET。
 * @note   拼装格式：
 *         {"t":"ota","st":2,"pct":45,"v":"1.2.3","err":0,"ts":123456}
 *         下载期间每 5% 上报一次，避免 AT 通道被占满。
 */
ota_ret_t mqtt_ctrl_report_ota(u8 state, u8 percent, ota_ver_t ver, u8 err);

/**
 * @brief  上报心跳（含 RSSI、运行时长、启动次数）。
 * @return OTA_OK / OTA_ERR_BUSY / OTA_ERR_NET。
 */
ota_ret_t mqtt_ctrl_report_heartbeat(void);

/*==============================================================================
 * 主循环与 URC
 *============================================================================*/

/**
 * @brief  MQTT 主循环：处理 +MSUB 下行、CLOSED 断链、保活计时。
 * @return 本次处理的下行消息条数。
 * @note   每 <= 50 ms 调用一次；内部会调用 at_poll() 之外的 URC 队列消费，
 *         不会与 at_poll() 争抢缓冲。
 */
u8 mqtt_ctrl_poll(void);

/**
 * @brief  +MSUB 下行消息 URC 处理（由 at_urc_register 注册）。
 * @param  line  URC 整行。
 * @param  len   行长。
 * @return OTA_OK / OTA_ERR_PARAM（格式不符，丢弃）。
 */
ota_ret_t mqtt_ctrl_urc_msub(const char *line, u16 len);

/**
 * @brief  CLOSED / +MIPCLOSE 断链 URC 处理。
 * @param  line  URC 整行（可能为 "CLOSED"）。
 * @param  len   行长。
 * @return OTA_OK。
 */
ota_ret_t mqtt_ctrl_urc_closed(const char *line, u16 len);

#endif /* __MQTT_CTRL_H */
