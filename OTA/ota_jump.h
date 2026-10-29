/*==============================================================================
 *  ota_jump.h  --  Bootloader -> APP 跳转、中断向量跳板、软件复位
 *  Platform : STC32G12K128 (Keil C251, CodeRomSize = Huge)
 *  Depends  : ota_partition.h, ota_types.h
 *
 *  ── 为什么需要"向量跳板"（对应平台疑点 2）────────────────────────────
 *  STC32G 的中断入口固定在 0x0000 起的低地址区，向量内容是一条跳转指令。
 *  Bootloader 与 APP 都想拥有中断，但 FF 块的向量入口只能被一份代码占用。
 *  因此：
 *
 *      FF:0000h 块（用户 ISP 区，Bootloader 独占，OTA 永不触碰）
 *        └─ 每条中断向量: EJMP isr_stub_<n>
 *                           └─ isr_stub_<n>:  调用 g_isr_tbl[n] 指向的函数
 *      运行期：APP 启动时调 ota_isr_bind(n, app_isr) 把函数指针写进 RAM 表
 *
 *  关键点：Huge 模式下函数指针为 3 字节，向量跳转必须由编译器生成 EJMP
 *  （24 位绝对跳转）；若误用 LJMP（16 位），APP 落在 FF:2000h 这种高地址时
 *  必然跳飞。
 *  所以本工程的 Bootloader 与 APP **必须使用同一套 C251 编译选项**
 *  （CodeRomSize = Huge），且跳板 stub 必须写在 Bootloader 工程里。
 *
 *  ── INTVECTOR ─────────────────────────────────────────────────────────
 *  若 STC32G 确实提供"中断向量基址重映射"寄存器（INTVECTOR 或同类），
 *  则可用更简单的方案：APP 自带完整向量表并链接到自己的 APP_BASE 起始处，
 *  Bootloader 在跳转前把向量基址指过去。
 *  TODO(核对手册) STC32G 是否存在 INTVECTOR 寄存器/STC-ISP 是否有
 *     "中断向量重映射"选项；若存在，需补 ota_intvector_set() 的实现，
 *     并在 ota_jump_to_app() 内替换掉跳板方案。当前实现不依赖该特性。
 *
 *  ── 跳转方式 ──────────────────────────────────────────────────────────
 *  用 ((void (far *)(void))OTA_APP_CODE_BASE)() 形式调用（Huge 模式的 far 调用），
 *  执行前必须：关总中断 -> 复位外设 -> 恢复 SP -> 打开总中断交给 APP 自己开。
 *============================================================================*/
#ifndef __OTA_JUMP_H
#define __OTA_JUMP_H

#include "ota_mcu.h"
#include "ota_types.h"
#include "ota_partition.h"

/* 中断向量槽数量：按 STC32G 最大中断号 + 1 留余量。
 * TODO(核对手册) STC32G12K128 的最大中断号（STC8H 为 40+），
 *    此处 48 为保守值，需按实际中断向量表收缩以省 RAM。
 */
#define OTA_ISR_SLOT_NUM    48U

/*==============================================================================
 * 中断向量跳板
 *============================================================================*/

/**
 * @brief  初始化向量跳板表（清空全部槽位指向"未绑定处理函数"）。
 * @return 无。
 * @note   由 Bootloader 在 main() 最开头、开总中断之前调用一次。
 */
void ota_isr_table_init(void);

/**
 * @brief  绑定某中断号的处理函数（APP 启动时调用）。
 * @param  irq_no  中断号，0 ~ OTA_ISR_SLOT_NUM-1。
 * @param  isr     APP 侧的中断服务函数（Huge 模式 3 字节函数指针）。
 * @return OTA_OK / OTA_ERR_PARAM（irq_no 越界或 isr 为 NULL）。
 * @note   必须在 EA=1 之前完成绑定，否则绑定前的第一次中断会落到默认处理。
 */
ota_ret_t ota_isr_bind(u8 irq_no, void (*isr)(void));

/**
 * @brief  解绑（APP 主动放弃某中断）。
 * @param  irq_no  中断号。
 * @return OTA_OK / OTA_ERR_PARAM。
 */
ota_ret_t ota_isr_unbind(u8 irq_no);

/**
 * @brief  分发中断：由 Bootloader 的中断 stub 调用，转发到 APP 注册的处理函数。
 * @param  irq_no  中断号。
 * @return 无。
 * @note   BL 侧 stub 写法（示例，中断号以 stc32g.h 为准）：
 *             void bl_isr_uart1(void) interrupt 4 { ota_isr_dispatch(4); }
 *         未绑定时转发到内部默认空函数，不会跑飞。
 */
void ota_isr_dispatch(u8 irq_no);

/**
 * @brief  查询当前绑定情况（调试用）。
 * @param  irq_no  中断号。
 * @return 已绑定的函数指针；未绑定或越界返回 NULL。
 */
void (*ota_isr_get(u8 irq_no))(void);

/*==============================================================================
 * 跳转
 *============================================================================*/

/**
 * @brief  跳转到 APP 区执行（不返回）。
 * @param  app_base  APP 入口的**代码域地址**（24 位，形如 0xFF4000），
 *                  通常传 OTA_APP_CODE_BASE；切勿传 IAP 域地址。
 * @return 理论上不返回；若地址越界则返回 OTA_ERR_ADDR（便于调试时停机）。
 * @note   v0.6 起本函数**不再**根据 Flash 首字节否决跳转（v0.5 实测：
 *         同一地址 bl_boot 读到 0x75、这里读到 0xFF，导致 rc=12 永不跳转）。
 *         "空片/半截镜像"闸门统一在 bl_boot()，镜像头+CRC 校验由
 *         ota_verify.c 负责。这里只打两条诊断日志（一行一个参数）。
 * @note   跳转前会：EA=0 -> 停止本工程所有外设（UART/定时器/看门狗按需保留）
 *         -> far call 入口。
 *         APP 入口必须是可重入的裸 C 函数，不得依赖 Bootloader 的任何全局变量。
 */
ota_ret_t ota_jump_to_app(u32 app_base);

/**
 * @brief  软件复位（STC 通过 IAP_CONTR 的 SWRST 位触发软复位）。
 * @param  delay_ms  复位前延时（留时间把日志发完），0 ~ 100 ms。
 * @return 不返回。
 * @note   TODO(核对手册) STC32G IAP_CONTR(0xC7) 的软件复位控制位定义
 *          （STC8 家族为 bit5=SWBS / bit4=SWRST，且需配合 ISP 引导码），
 *          若走看门狗复位则改用 WDT_CONTR 的 CLR_WDT 溢出让其自然复位。
 */
void ota_system_reset(u16 delay_ms);

/**
 * @brief  跳转到 APP 前的"最小必要外设复位"：
 *         UART 停止收发、定时器停止、AT 通道释放、Flash IAP 空闲。
 * @return 无。
 * @note   不关看门狗（否则 APP 若忘记喂狗会失去最后一道保护）。
 */
void ota_pre_jump_deinit(void);

#endif /* __OTA_JUMP_H */
