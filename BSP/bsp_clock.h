/*==============================================================================
 *  bsp_clock.h  --  系统时钟（STC32G 用 ISP 设定的 IRC，代码里只做确认与分频）
 *============================================================================*/
#ifndef __BSP_CLOCK_H
#define __BSP_CLOCK_H

#include "ota_mcu.h"

/* 编译期常量：必须与 STC-ISP 里「输入用户程序运行的IRC频率」一致 */
#define BSP_FOSC_HZ     11059200UL

/* 初始化：清分频、确认主频。返回实际主频(Hz) */
u32 bsp_clock_init(void);

/* 查询主频(Hz) */
u32 bsp_clock_fosc(void);

#endif /* __BSP_CLOCK_H */
