/*==============================================================================
 *  param.h  --  参数区（EEPROM 数据 Flash）读写：双备份 + CRC + 单调序号
 *  Platform : STC32G12K128 (Keil C251, Huge)
 *  Depends  : flash_iap.h, crc32.h, ota_types.h, ota_partition.h
 *
 *  存储布局
 *      Slot A : 0x01E000 ~ 0x01E7FF  (2 KB)
 *      Slot B : 0x01E800 ~ 0x01EFFF  (2 KB)
 *  写入策略：永远写"非当前有效"那一份，写完回读 CRC 通过后再把有效标记
 *            切过去（seq 递增）。掉电落在任意时刻，上电总能选出 seq 更大
 *            且 CRC 正确的一份，最坏退化为上一版参数，不会全丢。
 *
 *  为什么不用 STC 的"扇区交换"：参数区只有 2 KB 但擦除单位 512 B，
 *  双槽轮写可以把擦写次数摊薄一半，并且天然获得掉电原子性。
 *============================================================================*/
#ifndef __PARAM_H
#define __PARAM_H

#include "ota_mcu.h"
#include "ota_types.h"
#include "ota_partition.h"

/*----------------------------- 存储格式 -------------------------------------*/
#define PARAM_MAGIC         0x50415241UL   /* "PARA"                          */
#define PARAM_FMT_VER       0x0001U        /* 参数区格式版本                  */
/* 单份参数可用字节数。param.c 里有编译期断言 sizeof(param_t) <= 本值；
 * 若断言报错，把它改成 1024（槽 2048 仍容得下，代价是多擦一个扇区）。 */
#define PARAM_MAX_SIZE      512U

/* 启动尝试次数上限：超过即判定 APP 启动失败，触发回滚 */
#define PARAM_BOOT_TRY_MAX  3U

/* MQTT 服务器地址长度上限（含 '\0'） */
#define PARAM_HOST_MAX      48U
#define PARAM_TOPIC_MAX     64U
#define PARAM_URL_MAX       128U
#define PARAM_CLIENTID_MAX  32U
#define PARAM_TOKEN_MAX     40U

/*==============================================================================
 * 参数结构体
 * 注意：C251 无 #pragma pack，字段顺序已手工按 4/2 字节边界排布；
 *       新增字段务必追加在 crc 之前，并递增 PARAM_FMT_VER。
 *============================================================================*/
typedef struct
{
    /* ---- 头部 ---- */
    u32         magic;              /* 必须 == PARAM_MAGIC，否则视为空白      */
    u16         fmt_ver;            /* == PARAM_FMT_VER                       */
    u16         len;                /* 本结构体有效字节数（含 crc 字段）      */
    u32         seq;                /* 单调递增序号，掉电后取最大者为有效    */

    /* ---- OTA 状态（掉电续传的核心） ---- */
    u8          ota_state;          /* 取值见 ota_fsm.h 的 ota_state_t（此处用
                                       u8 存放以避免 param.h <-> ota_fsm.h 的
                                       头文件循环包含；读时强转即可）        */
    u8          boot_try;           /* 本次新固件已尝试启动次数              */
    u8          active_slot;        /* 当前运行的 APP 槽位：0=A 1=B（保留）  */
    u8          rollback_flag;      /* 1 = 下次上电强制回滚到旧版本          */
    ota_ver_t   running_ver;        /* 当前运行固件版本                      */
    ota_ver_t   pending_ver;        /* 已下载待生效固件版本                  */
    u32         dl_offset;          /* 已成功写入暂存区的字节数（断点）      */
    u32         dl_total;           /* 镜像总长度（含镜像头）                */
    u32         dl_crc32;           /* 已下载部分的运行 CRC32（可选校验）    */
    u32         dl_img_crc32;       /* 镜像头中声明的正文 CRC32              */
    u32         dl_start_ms;        /* 本次下载开始时间戳（ms），用于统计    */

    /* ---- 网络配置 ---- */
    u8          apn[16];            /* APN，如 "CMNET"                        */
    u8          mqtt_host[PARAM_HOST_MAX];      /* MQTT 域名/IP               */
    u16         mqtt_port;                      /* MQTT 端口，通常 1883/8883  */
    u8          mqtt_clientid[PARAM_CLIENTID_MAX];
    u8          mqtt_user[24];
    u8          mqtt_pass[24];
    u8          sub_topic[PARAM_TOPIC_MAX];     /* 下行控制主题               */
    u8          pub_topic[PARAM_TOPIC_MAX];     /* 上行状态主题               */
    u16         keepalive_s;                    /* 保活秒数，建议 60          */
    u8          clean_session;                  /* 1 = clean session          */
    u8          ota_url[PARAM_URL_MAX];         /* 固件下载 URL               */

    /* ---- 统计与运维 ---- */
    u32         boot_cnt;           /* 累计启动次数                           */
    u32         ota_succ_cnt;       /* 累计 OTA 成功次数                      */
    u32         ota_fail_cnt;       /* 累计 OTA 失败次数                      */
    u8          log_level;          /* 持久化的日志级别                       */

    /* ---- 校验（必须是最后一个字段） ---- */
    u32         crc32;              /* 前面所有字节的 CRC32                   */
} param_t;

/*==============================================================================
 * 生命周期
 *============================================================================*/

/**
 * @brief  初始化参数模块：扫描 A/B 两槽，装载有效参数到 RAM 缓存。
 * @return OTA_OK            装载成功（取到 CRC 正确且 seq 最大的一份）；
 *         OTA_ERR_CRC       两槽均损坏，已自动写入出厂默认参数并返回该码；
 *         OTA_ERR_IO        Flash 读取失败（地址不可访问，见平台疑点 1）。
 * @note   本函数必须在 flash_iap_init(eeprom_kb) 之后、任何业务逻辑之前调用一次；
 *         参数区位于 IAP 域 0xE000~0xEFFF，地址为 EEPROM 相对地址。
 */
ota_ret_t param_init(void);

/**
 * @brief  取参数只读指针（指向 RAM 缓存，不做拷贝）。
 * @return 参数指针，永不为 NULL；param_init() 未调用时返回默认参数。
 * @note   调用方不得修改其内容；写操作一律走 param_set_* 或 param_save()。
 */
param_t *param_get(void);

/**
 * @brief  保存 RAM 缓存到 Flash（写非活动槽 -> 回读校验 -> 切换有效槽）。
 * @param  force  1 = 立即落盘；0 = 允许合并（距上次落盘 < 5 s 则只标脏）。
 * @return OTA_OK 成功；
 *         OTA_ERR_IO  擦写或回读失败（旧数据仍有效，安全）；
 *         OTA_ERR_BUSY 另一份正在写。
 * @note   一次写入约 10~20 ms（2 KB 扇区擦除 + 512 B 编程），期间关中断。
 */
ota_ret_t param_save(u8 force);

/**
 * @brief  是否有未落盘的修改。
 * @return 1 脏；0 干净。
 */
u8 param_is_dirty(void);

/**
 * @brief  恢复出厂默认参数（仅 RAM，需再调 param_save(1) 落盘）。
 * @return 无。
 */
void param_load_defaults(void);

/**
 * @brief  擦除整个参数区（A/B 两槽），危险操作，仅供工厂模式使用。
 * @return OTA_OK / OTA_ERR_IO。
 */
ota_ret_t param_factory_erase(void);

/*==============================================================================
 * 断点续传专用（高频、小颗粒写入）
 *============================================================================*/

/**
 * @brief  更新下载进度并落盘（每 N 个分片或每 M 秒调用一次）。
 * @param  offset      已写入暂存区的累计字节数。
 * @param  total       镜像总长度。
 * @param  crc_running 已下载部分的运行 CRC32。
 * @return OTA_OK / OTA_ERR_IO。
 * @note   只改动 3 个字段后整块重写，Flash 擦写寿命按 2304 B/次、
 *         每 4 KB 一次计：56 KB 镜像约 14 次写，寿命无压力。
 */
ota_ret_t param_update_download(u32 offset, u32 total, u32 crc_running);

/**
 * @brief  清零下载断点（下载完成、校验失败、主动取消时调用）。
 * @return OTA_OK / OTA_ERR_IO。
 */
ota_ret_t param_clear_download(void);

/**
 * @brief  读取断点信息。
 * @param  p_offset  输出已下载字节数，可为 NULL。
 * @param  p_total   输出镜像总长，可为 NULL。
 * @param  p_crc     输出运行 CRC32，可为 NULL。
 * @return 1 存在有效断点（offset > 0 且 offset <= total）；0 无断点。
 */
u8 param_get_download(u32 *p_offset, u32 *p_total, u32 *p_crc);

/*==============================================================================
 * 启动计数与回滚
 *============================================================================*/

/**
 * @brief  启动尝试计数 +1 并立即落盘（Bootloader 跳 APP 前调用）。
 * @return 递增后的尝试次数；> PARAM_BOOT_TRY_MAX 时应触发回滚。
 * @note   必须在跳转 APP 之前落盘，否则"启动即死"会无限循环。
 */
u8 param_boot_try_inc(void);

/**
 * @brief  清零启动尝试计数（APP 自检通过后调用，宣告本次启动成功）。
 * @return OTA_OK / OTA_ERR_IO。
 */
ota_ret_t param_boot_try_clear(void);

/*==============================================================================
 * 临时诊断（v0.14：定位"槽里 magic/len/fmt 全对、只有 CRC 不符"）
 *
 *   v0.13 实测这一行（校验和自洽，可信）：
 *       pA m=50415241 c=bb701478 k=e70c5373
 *   → 四个头部判据里三个通过，只有 CRC 不符。
 *   而 param_save 的返回码一直被 `(void)` 丢弃，所以
 *   "擦失败 / 擦不干净 / 写失败 / 写后校验有洞" 四种可能没被分开。
 *
 *   v0.14 起 param_save() 自己把四个动作的结果记下来（只读，不改状态）：
 *      g_pdg[0] = (擦除 rc << 8) | 擦完是否真的全 0xFF（1=干净）
 *      g_pdg[1] = 写入 rc（含 flash_iap_write 内部的整块回读校验）
 *      g_pdg[2] = 读回后与 RAM 镜像不同的字节数（0 = 写进去的和想写的完全一致）
 *      g_pdg[3] = 最小差异偏移
 *      g_pdg_rc = 读回本身的 rc
 *   判读：
 *      g_pdg[0] & 0xFF == 0  → **擦不干净**（残留位把正文 AND 掉 → CRC 必错）
 *      g_pdg[1] != 0         → 写后校验当场就发现了差异
 *      g_pdg[2]  == 0 但 CRC 仍不符 → 只可能是 CRC 计算本身有问题
 *   定位完成后连同 g_pdg[] 一起删除。
 *============================================================================*/
extern u32 xdata g_pdg[5];
extern u8  xdata g_pdg_rc;

/*------------------------------------------------------------------------------
 * v0.16 追加：g_pdg[2] / g_pdg[3] / g_pdg[4]
 *
 *   v0.15 上板把"硬件"这条线彻底否掉了（三行校验和全部自洽）：
 *       iap tps=11                       ← 擦写脉宽参数是对的
 *       ier1 rc=0 f=0 / ier seg=ff / ier2 f=0
 *                                        ← 0xC800 擦得干干净净，CMD_FAIL 从不置位
 *       prg  rc=0 f=0 r=5a a5            ← **4 字节写+回读校验通过**
 *   ⇒ Flash 擦、写、读三条通路都是活的，"芯片写不进去"这个方向作废。
 *
 *   同一个上电周期里 param_save 写 466 字节却 w=7（OTA_ERR_CRC），
 *   而且 `boot_cnt=4280937472`（= 0xFF29EC00，一个 BL 代码地址被搬进了 RAM）
 *   —— g_p 的 RAM 镜像自己在变。
 *
 *   于是这次不猜，直接量"源缓冲在写的过程中有没有变"：
 *      g_pdg[2] = cb = 写之前对 g_p 算的 CRC32
 *      g_pdg[3] = ca = 写之后对 g_p 算的 CRC32
 *      g_pdg[4] = (差异字节数 << 16) | 最小差异偏移
 *   判读：
 *      cb != ca            → **写窗口内 g_p 被改了** ⇒ 根因是 RAM 被踩，
 *                            w=7 只是它的表现（verify 拿变了的源去比）
 *      cb == ca 且 d=466   → 466 字节一个都没落地 ⇒ 编程真的没生效
 *      cb == ca 且 d==0    → 写进去了、也读得回，那 w 就不该是 7 ⇒ 查 verify
 *--------------------------------------------------------------------------*/

#endif /* __PARAM_H */
