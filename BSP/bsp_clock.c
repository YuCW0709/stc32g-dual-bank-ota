/*==============================================================================
 *  bsp_clock.c  --  时钟初始化
 *  STC32G 的主频由 STC-ISP 烧进选项字节(IRC 频率)，程序里不能再"设"，
 *  只能读/确认 + 关掉分频。
 *  TODO(核对手册) STC32G 的分频寄存器名（CLKDIV / CLKDIV2 等）与位定义。
 *============================================================================*/
#include "bsp_clock.h"

u32 bsp_clock_init(void)
{
    /* STC32G 上电默认不分频；若手册确认 CLKDIV 存在且可写，在此清零：
     *     CLKDIV = 0x00;
     * 未确认前不要臆造寄存器，保持默认即 1T 全速。
     */
    return BSP_FOSC_HZ;
}

u32 bsp_clock_fosc(void)
{
    return BSP_FOSC_HZ;
}
