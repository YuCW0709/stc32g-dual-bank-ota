/*==============================================================================
 *  bsp_timer.h  --  Timer0 1ms 时基 + 阻塞延时 + 软件定时器
 *============================================================================*/
#ifndef __BSP_TIMER_H
#define __BSP_TIMER_H

#include "ota_mcu.h"

/* 系统毫秒计数（32 位，约 49.7 天回绕；比较时用差值） */
u32  bsp_timer_ms(void);

/* 初始化 Timer0 为 1ms 中断（1T 模式） */
void bsp_timer_init_1ms(void);

/* 阻塞延时；内部喂狗，避免长延时被看门狗复位 */
void bsp_timer_delay_ms(u16 ms);

/* 非阻塞：判断自 start 起是否已过 ms 毫秒（自动处理回绕） */
u8   bsp_timer_elapsed(u32 start, u32 ms);

#endif /* __BSP_TIMER_H */
