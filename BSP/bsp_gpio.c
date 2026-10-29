/*==============================================================================
 *  bsp_gpio.c  --  GPIO 实现
 *  STC8/STC32 端口模式：PxM1:PxM0 = 00 准双向 / 01 推挽 / 10 高阻 / 11 开漏
 *============================================================================*/
#include "bsp_gpio.h"
#include "bsp_timer.h"

/*--------------------------- 端口名编译期解析 -------------------------------*/
#if   (LED_PORT_SEL == 0)
  #define P_LED   P0
  #define P_LEDM0 P0M0
  #define P_LEDM1 P0M1
#elif (LED_PORT_SEL == 1)
  #define P_LED   P1
  #define P_LEDM0 P1M0
  #define P_LEDM1 P1M1
#elif (LED_PORT_SEL == 2)
  #define P_LED   P2
  #define P_LEDM0 P2M0
  #define P_LEDM1 P2M1
#elif (LED_PORT_SEL == 3)
  #define P_LED   P3
  #define P_LEDM0 P3M0
  #define P_LEDM1 P3M1
#elif (LED_PORT_SEL == 4)
  #define P_LED   P4
  #define P_LEDM0 P4M0
  #define P_LEDM1 P4M1
#else
  #define P_LED   P5
  #define P_LEDM0 P5M0
  #define P_LEDM1 P5M1
#endif

#if   (AIR_PWRKEY_PORT_SEL == 0)
  #define P_KEY   P0
  #define P_KEYM0 P0M0
  #define P_KEYM1 P0M1
#elif (AIR_PWRKEY_PORT_SEL == 1)
  #define P_KEY   P1
  #define P_KEYM0 P1M0
  #define P_KEYM1 P1M1
#elif (AIR_PWRKEY_PORT_SEL == 2)
  #define P_KEY   P2
  #define P_KEYM0 P2M0
  #define P_KEYM1 P2M1
#elif (AIR_PWRKEY_PORT_SEL == 3)
  #define P_KEY   P3
  #define P_KEYM0 P3M0
  #define P_KEYM1 P3M1
#elif (AIR_PWRKEY_PORT_SEL == 4)
  #define P_KEY   P4
  #define P_KEYM0 P4M0
  #define P_KEYM1 P4M1
#else
  #define P_KEY   P5
  #define P_KEYM0 P5M0
  #define P_KEYM1 P5M1
#endif

#define LED_BIT  (u8)(1u << LED_PIN)
#define KEY_BIT  (u8)(1u << AIR_PWRKEY_PIN)

/*--------------------------- 实现 -------------------------------------------*/
void bsp_gpio_init(void)
{
    /* 状态灯：推挽输出，初始熄灭 */
#if (LED_ACTIVE_LOW)
    P_LED |= LED_BIT;
#else
    P_LED &= (u8)~LED_BIT;
#endif
    P_LEDM0 |= LED_BIT;
    P_LEDM1 &= (u8)~LED_BIT;

    /* PWRKEY：推挽输出，初始拉低 */
    P_KEY &= (u8)~KEY_BIT;
    P_KEYM0 |= KEY_BIT;
    P_KEYM1 &= (u8)~KEY_BIT;
}

void bsp_led_on(void)
{
#if (LED_ACTIVE_LOW)
    P_LED &= (u8)~LED_BIT;
#else
    P_LED |= LED_BIT;
#endif
}

void bsp_led_off(void)
{
#if (LED_ACTIVE_LOW)
    P_LED |= LED_BIT;
#else
    P_LED &= (u8)~LED_BIT;
#endif
}

void bsp_led_toggle(void)
{
    P_LED ^= LED_BIT;
}

void air_pwrkey_set(u8 high)
{
    if (high) P_KEY |= KEY_BIT;
    else      P_KEY &= (u8)~KEY_BIT;
}

void air_pwrkey_pulse_ms(u16 ms)
{
    air_pwrkey_set(1);
    bsp_timer_delay_ms(ms);
    air_pwrkey_set(0);
}
