/*==============================================================================
 *  bsp_gpio.h  --  GPIO 初始化、状态灯、模组控制脚
 *  端口用"编号"选择（0=P0 1=P1 ...），编译期解析成具体 SFR。
 *  注意：SFR 是编译器关键字不是变量，不能写 `if (PORT == P1)` 这种运行时比较。
 *============================================================================*/
#ifndef __BSP_GPIO_H
#define __BSP_GPIO_H

#include "ota_mcu.h"

/* ---- 硬件映射：只改这四个宏 ------------------------------------------- */
#define LED_PORT_SEL        2       /* 状态灯端口：0=P0 1=P1 2=P2 3=P3 4=P4 5=P5 */
#define LED_PIN             0       /* 状态灯位号 0~7                          */
#define AIR_PWRKEY_PORT_SEL 1       /* Air780EP PWRKEY 端口                    */
#define AIR_PWRKEY_PIN      2       /* PWRKEY 位号                             */
#define LED_ACTIVE_LOW      1       /* 1=低电平点亮；0=高电平点亮               */

void bsp_gpio_init(void);

void bsp_led_on(void);
void bsp_led_off(void);
void bsp_led_toggle(void);

void air_pwrkey_set(u8 high);
void air_pwrkey_pulse_ms(u16 ms);

#endif /* __BSP_GPIO_H */
