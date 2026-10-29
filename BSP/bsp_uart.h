/*==============================================================================
 *  bsp_uart.h  --  UART1 底层收发（Air780EP / 日志共用同一路物理串口）
 *  Platform : STC32G12K128
 *
 *  ⚠ 端口冲突提醒：STC 的 ISP 下载固定走 UART1(P3.0/P3.1)。
 *    若 Air780EP 也接 UART1，下载时模组会干扰握手。量产板建议把模组改到
 *    UART2/UART3，或在 UART1 上加跳线帽，下载时断开模组。
 *    当前实现是 UART1；换端口只需改 bsp_uart_init() 里的 SFR 与中断号。
 *============================================================================*/
#ifndef __BSP_UART_H
#define __BSP_UART_H

#include "ota_mcu.h"

/* ★ v0.7.0：1024 → 512。
 *   这个环只给"日志口(UART1)偶尔收到的字节"用（正常业务里 UART1 只发不收，
 *   唯一消费者是短接回环自检），1 KB 纯属浪费。MQTT 的整条发布命令要塞进
 *   g_at_tx（152→256），多的 104 B xdata 正是从这里让出来的 ——
 *   xdata 上限 7936，v0.6.19 只剩 127 B，不腾地方写不了业务层。 */
#define BSP_UART_RX_SIZE    256U     /* 接收环形缓冲（2 的幂） */

/* 初始化：8N1，Timer2 做波特率发生器。返回 1 成功 */
u8   bsp_uart_init(u32 baud);

/* 发送 */
void bsp_uart_putc(u8 c);
void bsp_uart_write(const u8 *p, u16 n);
void bsp_uart_puts(const char *s);
void bsp_uart_tx_wait_idle(void);

/* 接收：主循环取字节 */
u8   bsp_uart_rx_pop(u8 *p);        /* 1 取到，0 空 */
u16  bsp_uart_rx_avail(void);
u16  bsp_uart_rx_flush(void);       /* 返回丢弃字节数 */
u16  bsp_uart_rx_overflow(void);    /* 累计溢出丢字节数 */

#endif /* __BSP_UART_H */
