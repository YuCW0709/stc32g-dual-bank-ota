/*==============================================================================
 *  app_mqtt.h  --  MQTT 控制面（业务层）：连接 / 订阅版本 / 周期上报 / 触发 OTA
 *  Project : Project/APP/STC32G_OTA.uvproj    CODE = 0xFF4000
 *  Platform: STC32G12K128 (Keil C251, Huge)
 *
 *  ── 它管什么、不管什么 ─────────────────────────────────────────────────
 *    管：把 AT 命令拼出来的 MQTT 会话（MCONFIG→SSL→MIPSTART→MCONNECT→MSUB）、
 *        把串口2 的 10 个模板打包上行、把服务器下发的版本消息解析成 OTA 动作。
 *    不管：TCP/TLS 细节（模组内置）、HTTP 下载（http_ota.c）、参数持久化。
 *
 *  ── 为什么不用 DRV/mqtt_ctrl.h 那套接口 ───────────────────────────────
 *    那个头文件是早期设计稿：`mqtt_ctrl_init(const mqtt_cfg_t *cfg, cb)` 要传
 *    结构体指针 + **函数指针回调**。本工程的定案（见 uart_at.h 顶部的病根
 *    说明）是：指针当形参跨函数在 C251 上会被提前排进寄存器、被下一次调用
 *    覆盖 ⇒ 发出去的命令变成 `ailed`。函数指针更是把这条风险乘以二。
 *    ⇒ 业务层改为"配置全用编译期宏、状态全用 static、回调改轮询"。
 *
 *  ── 时序（EMQX Serverless, TLS 8883）───────────────────────────────────
 *    每条 AT 命令本身是阻塞的（最长 8 s），但**只在状态迁移时**发生；
 *    稳态只剩每 5 s 一次 AT+MPUB。其余时间 mqtt_poll() 都是一条
 *    "没到点就 return"，不占 CPU（主循环要用 CPU 去喂串口2）。
 *============================================================================*/
#ifndef __APP_MQTT_H
#define __APP_MQTT_H

#include "ota_mcu.h"

/* 状态（与 app_mqtt.c 内部一一对应，外部只需要判断"通了没有"） */
#define MQ_ST_OFF       0U   /* 未启动：等模组 AT 通道就绪                    */
#define MQ_ST_PDP       9U   /* ★ v0.7.0：先把 PDP 承载拉起来（见下）          */
#define MQ_ST_CFG       1U   /* AT+MCONFIG                                    */
#define MQ_ST_SSL       2U   /* AT+SSLCFG="seclevel",88,0                     */
#define MQ_ST_TCP       3U   /* AT+SSLMIPSTART → 等 URC "CONNECT OK"          */
#define MQ_ST_CONN      4U   /* AT+MCONNECT    → 等 URC "CONNACK OK"          */
#define MQ_ST_SUB       5U   /* AT+MQTTMSGSET=0 + AT+MSUB → 等 "SUBACK"       */
#define MQ_ST_RUN       6U   /* 稳态：收 URC + 每 5 s 上报                    */
#define MQ_ST_DOWN      7U   /* 断链收尾：MDISCONNECT + MIPCLOSE              */
#define MQ_ST_BACK      8U   /* 退避，等下一轮重连                            */
#define MQ_ST_HOLD     10U   /* ★ v0.7.3：OTA 占用模组期间彻底让位（不发一个字节）*/

/*------------------------------------------------------------------------------
 * ★★★ 为什么要多一个 MQ_ST_PDP（v0.7.0 补的一个**必炸**的坑）
 *
 *   v0.7.0 第一版把"上电即下载"关掉（APP_HTTP_OTA_TEST=0）之后，PDP 承载
 *   就**没人拉起来了** —— 原来它是 http_ota 下载流程的第 ④ 步。
 *   而 `AT+SSLMIPSTART`（以及 MIPSTART）在**没有活动承载**时直接回 ERROR，
 *   于是 MQTT 会死在 TCP 那一步，日志只显示 "tcp rc=1"，
 *   看起来像 TLS/用户名的错，实际是根本没联网。
 *   ⇒ 在 MCONFIG 之前单独立一个状态，调用 http_ota_pdp_up()
 *     （就是 http_ota.c 里那套经过 v0.6.14~v0.6.16 现场打磨的实现，
 *      判据 = "看到带 IP 的那一行"，不复制第二份）。
 *---------------------------------------------------------------------------*/

/* 初始化（只记配置与清零，不发任何命令） */
void mqtt_init(void);

/* 主循环每圈调用：绝大多数调用立即返回，只有状态迁移时才阻塞 */
void mqtt_poll(void);

u8   mqtt_up(void);        /* 1 = 会话已建立，可以发布 */
u8   mqtt_state(void);     /* MQ_ST_* */
u16  mqtt_pub_ok(void);
u16  mqtt_pub_fail(void);

#endif /* __APP_MQTT_H */
