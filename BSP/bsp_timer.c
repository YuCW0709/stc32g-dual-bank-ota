/*==============================================================================
 *  bsp_timer.c  --  Timer0 1ms 时基
 *============================================================================*/
#include "bsp_timer.h"
#include "bsp_clock.h"
#include "bsp_wdt.h"

/* 1ms 重装值： 65536 - FOSC/1000/1T
 * 11.0592MHz -> 65536 - 11059 = 54477 = 0xD4CD */
#define T0_RELOAD_1MS   (u16)(65536UL - (BSP_FOSC_HZ / 1000UL))

static volatile u32 xdata s_ms = 0;

void bsp_timer_init_1ms(void)
{
    AUXR |= 0x80;                    /* Timer0 工作在 1T 模式 */
    TMOD &= (u8)~0x0F;               /* Timer0: 16 位自动重装(模式 0) */
    TH0 = (u8)(T0_RELOAD_1MS >> 8);
    TL0 = (u8)(T0_RELOAD_1MS);
    ET0 = 1;                         /* 允许 Timer0 中断 */
    TR0 = 1;
}

/*------------------------------------------------------------------------------
 * 中断服务：两种形态由工程决定，函数体完全相同
 *
 *   OTA_BUILD_APP 未定义（Bootloader 工程）
 *       —— 本工程拥有中断向量，直接写 `interrupt 1`。
 *
 *   OTA_BUILD_APP 已定义（APP 工程）
 *       —— ★ APP 在 0xFF2000，硬件中断永远从 BL 的向量（0xFF000B）进，
 *          所以 APP **绝不能**再声明 `interrupt`：那会生成一份永远不会被
 *          调用的向量副本，白占 CODE 类空间，还会让 `?PR?IV?1` 与 BL 重复。
 *          APP 侧把本函数当普通函数，由 BL 的 stub 经 ota_isr_dispatch(1) 调入。
 *----------------------------------------------------------------------------*/
#if defined(OTA_BUILD_APP)
void bsp_timer_isr(void)
#else
void bsp_timer_isr(void) interrupt 1
#endif
{
    s_ms++;
}

u32 bsp_timer_ms(void)
{
    u32 v;
    u8  ea = EA;                     /* 32 位读非原子，短暂关总中断 */
    EA = 0;
    v = s_ms;
    EA = ea;
    return v;
}

void bsp_timer_delay_ms(u16 ms)
{
    u32 start = bsp_timer_ms();
    while (!bsp_timer_elapsed(start, (u32)ms))
    {
        bsp_wdt_feed();
    }
}

u8 bsp_timer_elapsed(u32 start, u32 ms)
{
    return (u8)((u32)(bsp_timer_ms() - start) >= ms);
}
