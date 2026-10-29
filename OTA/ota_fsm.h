/*==============================================================================
 *  ota_fsm.h  --  OTA 状态机（IDLE/DOWNLOADING/VERIFYING/PENDING_REBOOT/
 *                  COMMITTED/ROLLED_BACK）
 *  Platform : STC32G12K128 (Keil C251, Huge)
 *  Depends  : ota_types.h, http_ota.h, param.h, mqtt_ctrl.h, flash_iap.h
 *
 *  ── 状态迁移图（事件驱动，全部边由 ota_fsm_post() 喂入）──────────────────
 *
 *      ┌────────┐  EV_OTA_CMD        ┌─────────────┐
 *      │  IDLE  │───────────────────>│ DOWNLOADING │
 *      └────────┘                    └─────────────┘
 *           ^                          │        │
 *           │ EV_ABORT / 超时          │        │ EV_HTTP_DONE
 *           │                          v        v
 *           │                    (回 IDLE)  ┌───────────┐
 *           │                               │ VERIFYING │
 *           │                               └───────────┘
 *           │                                 │       │
 *           │                     EV_VERIFY_OK│       │EV_VERIFY_FAIL
 *           │                                 v       v
 *           │                        ┌────────────────┐  ROLLED_BACK
 *           │                        │ PENDING_REBOOT │  （校验失败直接回 IDLE）
 *           │                        └────────────────┘
 *           │                                 │ 上电后 Bootloader 校验通过
 *           │                                 v
 *           │                          ┌───────────┐
 *           └──────────────────────────│ COMMITTED │
 *                                      └───────────┘
 *   ROLLED_BACK 仅在以下两种情况进入，**PENDING_REBOOT 绝不允许停留**：
 *       · Bootloader 搬运/生效失败（含 ota_apply_staging 返回 OTA_ERR_SUPPORT）
 *         -> PENDING_REBOOT --EV_APPLY_FAIL--> ROLLED_BACK，跳回旧 APP；
 *       · 新固件连续启动失败超过 PARAM_BOOT_TRY_MAX
 *         -> PENDING_REBOOT --EV_BOOT_FAIL--> ROLLED_BACK。
 *   APP 上报一次 ROLLED_BACK 之后自行回 IDLE。
 *
 *  ── 持久化规则（掉电续传的关键）────────────────────────────────────────
 *   进入 DOWNLOADING / VERIFYING / PENDING_REBOOT 时立即 param_save(1)；
 *   Bootloader 启动时读 param.ota_state：
 *       PENDING_REBOOT -> 尝试跳新固件，boot_try 超限则回滚到备份
 *       其它状态       -> 正常跳 APP，并清断点
 *============================================================================*/
#ifndef __OTA_FSM_H
#define __OTA_FSM_H

#include "ota_mcu.h"
#include "ota_types.h"

/*==============================================================================
 * 状态
 *============================================================================*/
typedef enum
{
    OTA_ST_IDLE           = 0,   /* 空闲：无升级任务；掉电最安全的状态       */
    OTA_ST_DOWNLOADING    = 1,   /* 下载中：HTTP Range 分片落盘暂存区        */
    OTA_ST_VERIFYING      = 2,   /* 校验中：镜像头 + 长度 + CRC32 全量校验    */
    OTA_ST_PENDING_REBOOT = 3,   /* 待重启：校验通过，等复位后由 BL 搬运      */
    OTA_ST_COMMITTED      = 4,   /* 已生效：新固件运行且自检通过              */
    OTA_ST_ROLLED_BACK    = 5    /* 已回滚：Bootloader 判定启动失败，退回旧版 */
} ota_state_t;

/*==============================================================================
 * 事件
 *============================================================================*/
/* 事件队列深度：2 的幂，用环形队列下标掩码取模；4 足够（同一时刻只会有
 * "下载完成 + 中止"这类极少量的并发投递） */
#define OTA_EV_QUEUE_DEPTH      4U
typedef enum
{
    OTA_EV_NONE           = 0,   /* 空事件，用于 poll 占位                    */
    OTA_EV_OTA_CMD        = 1,   /* 收到升级指令（含 URL/版本/CRC）           */
    OTA_EV_HTTP_PROGRESS  = 2,   /* 下载进度推进（内部定时器产生）            */
    OTA_EV_HTTP_DONE      = 3,   /* 全部字节收齐                              */
    OTA_EV_HTTP_FAIL      = 4,   /* 下载失败（重试耗尽 / 网络错误）           */
    OTA_EV_VERIFY_OK      = 5,   /* 镜像校验通过                              */
    OTA_EV_VERIFY_FAIL    = 6,   /* 镜像校验失败                              */
    OTA_EV_REBOOT_REQ     = 7,   /* 请求复位（本机主动 or 云端下发）          */
    OTA_EV_BOOT_OK        = 8,   /* 新固件启动自检通过（APP 启动时投递）      */
    OTA_EV_BOOT_FAIL      = 9,   /* 新固件启动自检失败                        */
    OTA_EV_ABORT          = 10,  /* 中止升级                                  */
    OTA_EV_TIMEOUT        = 11,  /* 阶段超时（下载静默 / 校验超时）           */
    OTA_EV_APPLY_FAIL     = 12,  /* 搬运/生效失败（含 OTA_ERR_SUPPORT）       */
    OTA_EV_MAX            = 13
} ota_event_t;

/*==============================================================================
 * 升级任务描述（EV_OTA_CMD 携带的数据）
 *============================================================================*/
typedef struct
{
    u8 *url;      /* 固件下载 URL，长度 < PARAM_URL_MAX           */
    u32        expect_size;    /* 期望镜像总长（含头）；0 表示未知              */
    u32        img_crc32;      /* 期望正文 CRC32；0 表示以镜像头声明为准        */
    ota_ver_t  target_ver;     /* 目标版本，用于拒绝降级                        */
    u8         force;          /* 1 = 允许降级/同版本重刷                       */
} ota_cmd_t;

/*==============================================================================
 * 生命周期
 *============================================================================*/

/**
 * @brief  初始化 OTA 状态机：从 param 恢复状态，处理掉电残留。
 * @return 恢复后的状态（ota_state_t）。
 * @note   必须在 param_init() 之后调用。若恢复出 DOWNLOADING 但断点信息与
 *         暂存区不一致（offset 处不是有效数据），会降级为 IDLE 并清断点。
 */
ota_state_t ota_fsm_init(void);

/**
 * @brief  启动一次 HTTP 直拉下载（精简版入口，绕过事件队列）。
 * @param  url  固件下载直链（会深拷贝进 param.ota_url）。
 * @return OTA_OK 已进入 DOWNLOADING；OTA_ERR_BUSY 非 IDLE；OTA_ERR_PARAM url 空；
 *         OTA_ERR_IO 擦 STAGING 失败；OTA_ERR_NET 下载引擎启动失败。
 * @note   下载完成后由 ota_fsm_poll() 置 PENDING_REBOOT，调用方应软复位。
 */
ota_ret_t ota_fsm_start_download(const char *url);

/**
 * @brief  状态机主循环：消费事件队列、驱动下载引擎、执行迁移、持久化。
 * @return 当前状态。
 * @note   每 <= 20 ms 调用一次。内部对 http_ota_step() 的返回做统一处理，
 *         调用方不需要自己驱动 HTTP 引擎（但需要保证 UART/AT 已就绪）。
 */
ota_state_t ota_fsm_poll(void);

/**
 * @brief  投递事件（可在 URC 回调、MQTT 回调、定时器中调用）。
 * @param  ev    事件类型。
 * @param  p_arg 事件参数：
 *                 EV_OTA_CMD       -> ota_cmd_t*（内部深拷贝 url）
 *                 其它事件          -> NULL
 * @return OTA_OK 已入队；
 *         OTA_ERR_STATE 当前状态不接受该事件（丢弃，可记日志）；
 *         OTA_ERR_NOSPACE 事件队列满（深度 OTA_EV_QUEUE_DEPTH）。
 */
ota_ret_t ota_fsm_post(ota_event_t ev, const void *p_arg);

/**
 * @brief  查询当前状态。
 * @return ota_state_t。
 */
ota_state_t ota_fsm_state(void);

/**
 * @brief  查询状态名（用于日志，返回常量字符串，不占 RAM）。
 * @param  st  状态。
 * @return 形如 "DOWNLOADING" 的只读字符串；非法状态返回 "UNKNOWN"。
 */
const char *ota_fsm_state_name(ota_state_t st);

/**
 * @brief  查询事件名。
 * @param  ev  事件。
 * @return 形如 "EV_HTTP_DONE" 的只读字符串；非法事件返回 "EV_UNKNOWN"。
 */
const char *ota_fsm_event_name(ota_event_t ev);

/**
 * @brief  取最近一次错误码。
 * @return ota_ret_t；0 表示无错误。
 */
ota_ret_t ota_fsm_last_error(void);

/*==============================================================================
 * 迁移函数（内部使用，暴露以便 Bootloader 与单元测试直接调用）
 *============================================================================*/

/**
 * @brief  执行一次状态迁移。
 * @param  from  源状态（必须与当前内部状态一致，否则返回 OTA_ERR_STATE）。
 * @param  to    目标状态。
 * @param  ev    触发事件（用于日志与迁移表校验）。
 * @return OTA_OK 迁移成功（已执行 exit/enter 钩子并持久化）；
 *         OTA_ERR_STATE 迁移表中不存在该边（非法迁移，会被拒绝并记日志）。
 * @note   本函数不做业务动作，只做"状态记账 + 钩子 + 持久化"；业务动作由
 *         ota_fsm_poll() 在进入状态后执行。
 */
ota_ret_t ota_fsm_transit(ota_state_t from, ota_state_t to, ota_event_t ev);

/**
 * @brief  查询某事件在某状态下是否被接受（不产生副作用）。
 * @param  st  状态。
 * @param  ev  事件。
 * @return 1 接受；0 拒绝。
 */
u8 ota_fsm_is_accepted(ota_state_t st, ota_event_t ev);

/*==============================================================================
 * 各状态的业务钩子（由 ota_fsm.c 内部实现，此处仅约定行为）
 *============================================================================*/

/**
 * @brief  进入某状态时的动作（启动定时器、发 MQTT 通知、置 param 等）。
 * @param  st  目标状态。
 * @return OTA_OK；失败返回错误码（会使迁移回退）。
 */
ota_ret_t ota_fsm_on_enter(ota_state_t st);

/**
 * @brief  离开某状态时的清理（关引擎、停定时器、释放 AT 通道）。
 * @param  st  源状态。
 * @return OTA_OK。
 */
ota_ret_t ota_fsm_on_exit(ota_state_t st);

/*==============================================================================
 * 供 Bootloader 复用的校验入口（与 APP 侧同一份代码，保证判据一致）
 *============================================================================*/

/**
 * @brief  校验暂存区镜像：魔数 -> 头 CRC -> 长度范围 -> 正文 CRC32 -> 机型/版本。
 * @param  p_hdr  输出解析出的镜像头（可为 NULL）。
 * @return OTA_OK 通过；
 *         OTA_ERR_VERIFY 任一检查失败；
 *         OTA_ERR_CRC    正文 CRC32 不匹配；
 *         OTA_ERR_ADDR   暂存区 IAP 地址超出 EEPROM 容量（见 flash_iap.h v2）。
 * @note   本函数耗时与镜像大小成正比（56 KB 约 3~5 s），调用期间需喂狗。
 *         暂存区在 IAP 域（0 基），入口参数与返回值中的地址全部是 IAP 相对地址。
 */
ota_ret_t ota_verify_staging(ota_img_hdr_t *p_hdr);

/**
 * @brief  让暂存区镜像生效（仅 Bootloader 调用），行为由 ota_partition.h 的
 *         OTA_APPLY_IS_MOVE / OTA_IAP_CAN_REACH_CODE 两个开关决定：
 *           · OTA_APPLY_IS_MOVE = 1 且 OTA_IAP_CAN_REACH_CODE = 1
 *               -> 把 IAP 暂存区逐扇区搬运到 OTA_APP_IAP_BASE（FF 块的 APP 区）；
 *           · OTA_APPLY_IS_MOVE = 0（暂存区即运行区 / A-B 双区）
 *               -> 只校验 + 改参数里的"活动区指针"，不做任何搬运。
 * @param  hdr  已校验通过的镜像头。
 * @return OTA_OK 生效成功（搬运并回读校验通过 / 指针已切换）；
 *         OTA_ERR_IO        擦写失败；
 *         OTA_ERR_NOSPACE   长度超出 APP 区；
 *         OTA_ERR_SUPPORT   IAP 不可达 FF 块（OTA_IAP_CAN_REACH_CODE=0），
 *                           必须改走 A/B 方案。**这是设计级错误，但运行期仍要有
 *                           确定的降级动作**：调用方必须
 *                           ota_fsm_post(OTA_EV_APPLY_FAIL, NULL)，
 *                           使状态经 ROLLED_BACK 回到旧固件并上报，
 *                           严禁停留在 PENDING_REBOOT（会变成"每次都升级失败
 *                           但永远不重启"的死循环）。
 * @note   搬运按 512 B 扇区进行，支持"断电续搬"（复用 param 的 dl_offset
 *         作为搬运进度）。搬运完成后必须再做一次整体 CRC 校验才允许切指针。
 */
ota_ret_t ota_apply_staging(const ota_img_hdr_t *hdr);

#endif /* __OTA_FSM_H */
