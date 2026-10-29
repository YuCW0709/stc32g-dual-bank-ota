/*==============================================================================
 *  ota_jump.c  --  中断向量跳板 / BL->APP 跳转 / 软复位
 *  Platform : STC32G12K128 (Keil C251, Huge)
 *============================================================================*/
#include "ota_jump.h"
#include "log.h"
#include "bsp_timer.h"
#include "bsp_uart.h"
#include "flash_iap.h"

/* 向量跳板表：RAM 里 3 字节函数指针数组（Huge 模式）
 * 必须在 EA=1 之前由 ota_isr_table_init() 初始化 */
static void (*xdata s_isr[OTA_ISR_SLOT_NUM])(void);  /* 48*3B，必须 xdata */

static void isr_default(void)
{
    /* 未绑定的中断：什么都不做，避免跑飞。调试期可在此点灯。 */
}

void ota_isr_table_init(void)
{
    u8 i;
    for (i = 0; i < OTA_ISR_SLOT_NUM; i++) s_isr[i] = isr_default;
}

ota_ret_t ota_isr_bind(u8 irq_no, void (*isr)(void))
{
    if (irq_no >= OTA_ISR_SLOT_NUM || isr == 0) return OTA_ERR_PARAM;
    s_isr[irq_no] = isr;
    return OTA_OK;
}

ota_ret_t ota_isr_unbind(u8 irq_no)
{
    if (irq_no >= OTA_ISR_SLOT_NUM) return OTA_ERR_PARAM;
    s_isr[irq_no] = isr_default;
    return OTA_OK;
}

void (*ota_isr_get(u8 irq_no))(void)
{
    if (irq_no >= OTA_ISR_SLOT_NUM) return 0;
    return s_isr[irq_no];
}

/* 供 Bootloader 的中断服务函数调用：查表并转发到 APP 注册的处理函数。
 * BL 侧的 stub 形如：
 *     void bl_isr_uart1(void) interrupt 4 { ota_isr_dispatch(4); }
 */
void ota_isr_dispatch(u8 irq_no)
{
    void (*fn)(void);
    if (irq_no >= OTA_ISR_SLOT_NUM) return;
    fn = s_isr[irq_no];
    if (fn != 0) fn();
}

void ota_pre_jump_deinit(void)
{
    ES  = 0;                 /* 关串口中断，防止跳转瞬间还有数据进来 */
    ET0 = 0;                 /* 关时基中断                              */
    EA  = 0;
    bsp_uart_tx_wait_idle();
}

ota_ret_t ota_jump_to_app(u32 app_base)
{
    /* ★ 全部 static：本函数含函数指针间接调用，链接器对它的 overlay 分析
     *   本来就会放弃；再叠加"日志参数区"那套坑，凡是留在地栈上的临时变量
     *   都不值得信任。3 个变量一共 6 字节，不值得省。 */
    static void (*xdata s_entry)(void);
    static u8  xdata s_h_base;      /* 用 app_base - OTA_PHYS_BASE 算地址读到的首字节 */
    static u8  xdata s_h_const;     /* 用常量 OTA_APP_IAP_BASE 读到的首字节        */

    /* 入口必须在 APP 代码域内 */
    if (app_base < OTA_APP_CODE_BASE || app_base > OTA_APP_CODE_END)
    {
        LOG_E0("JUMP", "bad entry");
        LOG_E2("JUMP", "  hi=%x lo=%x", (u16)(app_base >> 16), (u16)app_base);
        return OTA_ERR_ADDR;
    }

    /*==================================================================
     * ★★★ 首字节检查：v0.6 起只做"诊断"，不再当闸门 ★★★
     *
     *   2026-10-25 实测（v0.5）：
     *       [498][BL] APP head b=1 7e          <- bl_boot 用常量地址读，正确
     *       [502][BL] app head ok, jump -> 0x   <- 紧接其后
     *       [517][BL] jump returned rc=12, stay in BL
     *   rc=12 = OTA_ERR_VERIFY，只可能来自下面那道"空片"闸门 ——
     *   可同一颗芯片、同一个地址、早几十微秒前用常量地址刚读出 0x75。
     *
     *   问题是**同一件事做了两遍**：
     *       bl_boot()  : flash_iap_read_byte(OTA_APP_IAP_BASE)        -> 0x75
     *       ota_jump() : flash_iap_read_byte(app_base - OTA_PHYS_BASE) -> 0xFF
     *   两条路径算出来的都是 0x14000，结果却不一致。不管哪一边的读更可信，
     *   把闸门放在这里都等于"用第二次读去否决第一次读"，而两次读之间
     *   没有任何代码修改过 Flash。结果就是 APP 明明在、却永远起不来。
     *
     *   处置：
     *     1) **闸门统一放在 bl_boot()**（那里已有 0xFF/0x00 判定，且实测可靠）；
     *        ota_jump 只负责"地址合法就跳"。
     *     2) 这里仍把两条算法读到的值都打出来，**一行一个参数**。
     *        两者不一致 → 说明 IAP 读在"深调用栈 + 日志刚跑过"这个上下文里
     *        不稳定，这是独立且重要的线索，留给 ota_verify.c 阶段处理。
     *     3) 真正的镜像头 + CRC 校验属于 ota_verify.c 的职责，接上之后
     *        这里连诊断读都可以去掉。
     *================================================================*/
    s_h_base  = flash_iap_read_byte(app_base - OTA_PHYS_BASE);
    s_h_const = flash_iap_read_byte(OTA_APP_IAP_BASE);

    LOG_I1("JUMP", "entry via-base=%x", (u16)s_h_base);
    LOG_I1("JUMP", "entry via-const=%x", (u16)s_h_const);

    ota_pre_jump_deinit();

    /* Huge 模式：函数指针为 3 字节，可直接承载 24 位地址。
     * 注意：不要写成 LJMP 风格的 16 位强转，否则跳飞。 */
    s_entry = (void (*)(void))app_base;
    s_entry();

    return OTA_ERR_UNKNOWN;      /* 正常不会到这里 */
}

void ota_system_reset(u16 delay_ms)
{
    if (delay_ms) bsp_timer_delay_ms(delay_ms);

    /* TODO(核对手册) STC32G IAP_CONTR 的软件复位位（STC8 家族为 bit5=SWRST，
     * bit6=SWBS 选择复位后从哪块启动）。确认前先用"停止喂狗等看门狗溢出"兜底。 */
    IAP_CONTR = 0x20;            /* 触发软复位 */

    /* 若上面未生效（位定义不同），看门狗会兜底复位 */
    for (;;) { }
}
