/*==============================================================================
 *  bsp_wdt.h  --  看门狗
 *  注意：ISP 下载/擦写期间若被复位会变砖，长操作前应评估喂狗点。
 *============================================================================*/
#ifndef __BSP_WDT_H
#define __BSP_WDT_H

#include "ota_mcu.h"

/* 预分频档位（WDT_CONTR 的 PS2:PS0），数值越小超时越短
 * TODO(核对手册) STC32G WDT_CONTR 的 PS 分频表与超时时间 */
typedef enum
{
    WDT_PS_2 = 0,
    WDT_PS_4,
    WDT_PS_8,
    WDT_PS_16,
    WDT_PS_32,
    WDT_PS_64,
    WDT_PS_128,
    WDT_PS_256
} wdt_ps_t;

/* 启动看门狗；ps 越大超时越长（产品建议 PS_32 以上，约几百 ms~数 s） */
void bsp_wdt_init(wdt_ps_t ps);

/* 喂狗（置 CLR_WDT） */
void bsp_wdt_feed(void);

/* 关闭看门狗（仅调试/擦写长流程时临时用，量产代码不应调用） */
void bsp_wdt_stop(void);

#endif /* __BSP_WDT_H */
