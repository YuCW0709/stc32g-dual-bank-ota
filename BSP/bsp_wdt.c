/*==============================================================================
 *  bsp_wdt.c  --  看门狗实现
 *  WDT_CONTR 位定义（STC8/STC32 通用，TODO 核对 STC32G 手册）：
 *      bit7 WDT_FLAG 溢出标志 | bit5 EN_WDT 使能 | bit4 CLR_WDT 喂狗
 *      bit3 IDL_WDT 空闲模式  | bit2:0 PS2:PS0 分频
 *  若 stc32g.h 未声明 WDT_CONTR，取消下面一行的注释即可。
 *============================================================================*/
#include "bsp_wdt.h"

/* sfr WDT_CONTR = 0xC1; */

#define WDT_EN      0x20
#define WDT_CLR     0x10
#define WDT_PS_MASK 0x07

void bsp_wdt_init(wdt_ps_t ps)
{
    WDT_CONTR = (u8)(WDT_EN | WDT_CLR | ((u8)ps & WDT_PS_MASK));
}

void bsp_wdt_feed(void)
{
    /* 只保留 EN 与 PS，其余位重新写：
     *   · 不把 WDT_FLAG(bit7) 写回 1（那是硬件置的溢出标志）；
     *   · EN 保留原值 —— 芯片可能已被 ISP 选项字节「上电复位自动启动看门狗」
     *     置成 1，喂狗不该"顺便"改变使能状态。
     * 老写法 `WDT_CONTR |= WDT_CLR` 是读-改-写，会把整字节（含标志位）写回去。 */
    WDT_CONTR = (u8)(WDT_CLR | (WDT_CONTR & (WDT_EN | WDT_PS_MASK)));
}

void bsp_wdt_stop(void)
{
    WDT_CONTR = (u8)(WDT_CONTR & (u8)~WDT_EN);
}
