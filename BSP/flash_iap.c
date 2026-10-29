/*==============================================================================
 *  flash_iap.c  --  IAP 擦/写/读/校验 + 地址门禁
 *  Platform : STC32G12K128 (Keil C251, Huge)
 *
 *  寄存器（stc32g.h）：
 *      IAP_DATA / IAP_ADDR / IAP_ADDRH / IAP_ADDRL / IAP_CMD / IAP_TRIG / IAP_CONTR
 *
 *  ★ 三地址寄存器装载顺序固定（bit16+ / bit15~8 / bit7~0）：
 *        <第三地址寄存器> = (u8)(addr >> 16);
 *        IAP_ADDRH        = (u8)(addr >> 8);
 *        IAP_ADDRL        = (u8)(addr);
 *    第三寄存器的名字随 stc32g.h 版本而异，甚至不存在 —— 见下方宏开关，
 *    **没有它就只能寻址 0~0xFFFF**，这是硬件事实，代码里用闸门如实反映。
 *============================================================================*/
#include "ota_mcu.h"
#include <intrins.h>
#include "flash_iap.h"
#include "crc32.h"
#include "log.h"
#include "bsp_clock.h"
#include "bsp_wdt.h"

/*------------------- 第三地址寄存器（bit16 及以上）-------------------------
 * 已核实：C:\KEIL_V5\C251\INC\STC\STC32G.H 里存在
 *      sfr IAP_TPS   = 0xF5;
 *      sfr IAP_ADDRE = 0xF6;      <-- 就是它，注意拼写是 ADDRE 不是 ADDR
 * 所以 128K 的 IAP 空间（0x00000~0x1FFFF）可以完整寻址，OTA 架构成立：
 * 暂存区(STAGING)才可能落在 IAP >= 0x10000，与程序区物理重合。
 *
 * 若换了一份**没有** IAP_ADDRE 的头文件，把下面这行注释掉即可：
 * 此时 FLASH_IAP_HIGH_OK = 0，flash_iap_check_range() 的"闸门 0"会拒绝
 * 0x10000 以上的任何请求并返回 OTA_ERR_ADDR —— 宁可明确报错，也绝不把数据
 * 静默写到错误偏移（地址算错等于随机破坏 Flash）。严禁猜寄存器地址。
 *--------------------------------------------------------------------------*/
#define OTA_IAP_ADDR_REG     IAP_ADDRE

#ifdef OTA_IAP_ADDR_REG
  #define FLASH_IAP_HIGH_OK     1
#else
  #define FLASH_IAP_HIGH_OK     0
#endif

/* IAP_CONTR：bit7 IAPEN 使能；bit3:0 等待周期（TODO 核对 STC32G 位定义）
 * 11.0592MHz 下等待周期取 0 即可 */
#define IAP_CONTR_ON     0x80

/* 模块状态 */
static u32 xdata s_eeprom_size = 0;
static u8  xdata s_inited      = 0;

/*==============================================================================
 * IAP_CONTR.bit4  CMD_FAIL  —— 硬件自己的判决
 *
 *   手册（STC32G 原理与应用 §EEPROM 控制寄存器）：
 *       CMD_FAIL = 0  操作正确
 *       CMD_FAIL = 1  操作失败        ← **需软件清零**
 *
 *   这是唯一能区分下面两种失败的凭据，而这两种在"回读 ≠ 写入值"这一个
 *   现象上长得完全一样：
 *       (a) 命令被硬件拒绝（地址/时序/使能位不满足）→ 根本没动 Flash
 *       (b) 命令执行了，但物理上没写进去（擦不净 / 编程不生效）
 *
 *   ★ 做成**粘滞**（|= 累积）而不是每次覆盖：
 *     flash_iap_erase_range 擦完还要做 4 次回读校验，若每次覆盖，
 *     擦除那次的失败立刻被后面的读操作冲掉 —— 等于什么都没测。
 *     调用方用 flash_iap_fail_clear() 显式开窗。
 *============================================================================*/
u8 xdata g_iap_fail = 0;

/*==============================================================================
 *  v0.29 地址门禁探针：上一次 flash_iap_check_range() 看到的三个量
 *
 *   [0] = s_ca（本次 addr）  [1] = s_cl（本次 len）  [2] = s_eeprom_size
 *
 *   为什么需要它：闸门 1 有两条判据
 *       `s_ca >= s_eeprom_size`            —— 地址超容量
 *       `s_cl >  s_eeprom_size - s_ca`     —— 长度越界
 *   而两条都返回同一个 OTA_ERR_ADDR(2)。v0.28 上板拿到
 *       [VER] STAGING fail rc=2 step=9
 *   之后，把 ver_body 的调用点、参数传递、s_eeprom_size 的落点逐条排查过一遍，
 *   静态层面全部正确（汇编/符号地址/段边界都核过），说明"读代码"已经到极限 ——
 *   那就把门禁自己看到的值打出来，用硬件读数定论。
 *   每次调用都覆盖，所以失败时读到的就是最后那次（= 出错的那次）。
 *============================================================================*/
u32 xdata g_cr[3];

u8 flash_iap_last_fail(void)
{
    return g_iap_fail;
}

void flash_iap_fail_clear(void)
{
    g_iap_fail = 0;
}

/*==============================================================================
 * 内部：地址装载
 *============================================================================*/
u32 flash_iap_set_addr(u32 iap_addr)
{
#ifdef OTA_IAP_ADDR_REG
    OTA_IAP_ADDR_REG = (u8)(iap_addr >> 16);      /* bit16+ */
#endif
    IAP_ADDRH = (u8)(iap_addr >> 8);              /* bit15~8 */
    IAP_ADDRL = (u8)(iap_addr);                   /* bit7~0  */
    return iap_addr;
}

u32 flash_iap_to_phys(u32 iap_addr)
{
    return (u32)(OTA_PHYS_BASE + iap_addr);
}

u8 flash_iap_critical_enter(void)
{
    u8 ea = EA;
    EA = 0;
    return ea;
}

void flash_iap_critical_exit(u8 ea_state)
{
    EA = ea_state;
}

/*==============================================================================
 * 初始化
 *============================================================================*/
ota_ret_t flash_iap_init(u16 eeprom_kb)
{
    u32 size;

    if (eeprom_kb == 0)
    {
        size = OTA_EEPROM_SIZE;
    }
    else
    {
        if (eeprom_kb > 128) return OTA_ERR_PARAM;
        size = (u32)eeprom_kb * 1024UL;
    }

    /* 容量必须覆盖分区表（暂存/参数/预留 已占满低 64K） */
    if (size < OTA_IAP_CODE_BLOCK_TOP) return OTA_ERR_ADDR;

#if (OTA_IAP_CAN_REACH_CODE != 0)
    if (size < OTA_EEPROM_SIZE) return OTA_ERR_ADDR;   /* 需要 128K 才能到 APP */
#endif

    s_eeprom_size = size;
    s_inited = 1;

    /* IAP 时序参数：STC32G 要求 IAP_TPS = 系统时钟的 MHz 数（11.0592MHz -> 11）。
     * 不设的话擦/写脉宽不对，典型症状是"偶尔写不进去""擦不干净还残留旧字节"，
     * 而且 EEPROM 参数的 IAP_TPS 与普通 SFR 不同源（0xF5），必须显式写。 */
    IAP_TPS = (u8)(BSP_FOSC_HZ / 1000000UL);

    return OTA_OK;
}

u32 flash_iap_eeprom_size(void)
{
    return s_eeprom_size;
}

u16 flash_iap_eeprom_kb(void)
{
    return (u16)(s_eeprom_size >> 10);
}

/*==============================================================================
 * 底层原语
 *============================================================================*/
ota_ret_t flash_iap_raw(u8 cmd, u32 iap_addr, u8 *p_dat)
{
    u8 ea;
    u8 bad = 0U;

    /*==========================================================================
     * 闸门 A：命令码白名单
     *
     *   ★★★ 2026-10-25 实测血案 ★★★
     *   IAP_CMD 的 1(读) / 2(写) 曾经写反。编译链接烧录全部正常、零警告，
     *   但每一次"读 Flash"实际都在往 Flash **写** IAP_DATA(0x00)。
     *   一次上电就把 BL 复位入口 0xFF0000 前 4 字节抹成 00 00 00 00，
     *   下次复位 Bootloader 直接起不来。
     *
     *   这里的白名单是第一道防线：命令码不在 0~3 之内，连 TRIG 都不给。
     *   哪怕哪天常量又写错成 4/5，也只是"操作失败"，不会误写 Flash。
     *========================================================================*/
    if (cmd > IAP_CMD_ERASE) return OTA_ERR_PARAM;
    if (!s_inited)           return OTA_ERR_STATE;

    /*==========================================================================
     * 闸门 B：写/擦只允许落在"可写分区"内
     *
     *   这道闸门与命令码无关，专门防"命令码又写错"：
     *   即使 READ/PROGRAM 再次搞反，写操作也进不了 BL 区 / 常量区。
     *   注意这里只判分区归属，**不判扇区对齐** —— 单字节编程的地址本来
     *   就不必对齐，用 flash_iap_check_range(acc=1) 会误拒合法写入。
     *========================================================================*/
    if (cmd == IAP_CMD_PROGRAM || cmd == IAP_CMD_ERASE)
    {
        ota_part_t p;

        if (iap_addr >= s_eeprom_size)                 return OTA_ERR_ADDR;
        p = ota_part_of(iap_addr);
        if (p == PART_NONE || !ota_part_writable(p))   return OTA_ERR_ADDR;
    }

    ea = flash_iap_critical_enter();

    IAP_CONTR = IAP_CONTR_ON;                 /* 使能 IAP */
    flash_iap_set_addr(iap_addr);             /* 三个寄存器 */
    IAP_CMD   = cmd;

    if (cmd == IAP_CMD_PROGRAM && p_dat != 0)
    {
        IAP_DATA = *p_dat;
    }

    IAP_TRIG = IAP_TRIG_KEY1;                 /* 0x5A */
    IAP_TRIG = IAP_TRIG_KEY2;                 /* 0xA5 */
    _nop_(); _nop_(); _nop_(); _nop_();

    /* ★ CMD_FAIL 必须在下面把 IAP_CONTR 写 0 之前取走 —— 那一下会连它一起清掉。
     *   按手册，操作完成前 CPU 不会往下走（擦扇区 4~6ms、编程 30~40us 由硬件
     *   控制），所以这里读到的是本次操作的真实结论，不是"还没做完"。
     *
     * ★★ 2026-10-26：以前这里只置粘滞 `g_iap_fail`、函数照样返回 OTA_OK，
     *    于是所有调用者都以为成功 —— `flash_iap_read` 里新加的 s_bad 永远为 0，
     *    "读失败"继续被伪装成"数据全 FF"。现在把本次判决如实返回。 */
    if (CMD_FAIL)
    {
        g_iap_fail = 1U;                      /* 粘滞：本次窗口内发生过失败 */
        CMD_FAIL   = 0;                       /* 手册要求软件清零 */
        bad        = 1U;
    }

    if (cmd == IAP_CMD_READ && p_dat != 0)
    {
        *p_dat = IAP_DATA;
    }

    IAP_CMD   = IAP_CMD_IDLE;
    IAP_CONTR = 0x00;                         /* 关 IAP，降低误触发风险 */

    flash_iap_critical_exit(ea);
    return (bad != 0U) ? OTA_ERR_IO : OTA_OK;
}

/*==============================================================================
 * 地址门禁
 *============================================================================*/
u8 flash_iap_is_sector_aligned(u32 addr)
{
    return (u8)((addr & (OTA_IAP_SECTOR_SIZE - 1UL)) == 0);
}

/*==============================================================================
 * ★★★ v0.36：flash_iap_check_range() 已**整段删除** ★★★
 *
 *   它不是"暂时不用"，而是**不许再用**（铁律 14 的第三次确认）：
 *       v0.30  读通路内联掉它 → 读再没出过 rc=2（此前 `chk a/l` 报 l=42ff57）
 *       v0.35  擦除内联掉它   → 96 个扇区一次擦净，`erase g=` 一行都没打
 *       v0.36  写入内联掉它   → （本轮验证）
 *   三次都是"经过它 ⇒ rc=2（OTA_ERR_ADDR），内联掉 ⇒ 立刻正常"，
 *   而它内部四道闸门单独看全是对的。病根是那 9 B 形参
 *   （4+4+1 > 8）必然生成的 ?BYTE 参数块 —— 本工程里凡是踩上参数块的通路
 *   都会偶发拿到别人的值，且**改一次布局就换一处受害**。
 *
 *   ⇒ 保留函数体只会让人下次又顺手调它（本项目已经这样栽过三轮），
 *     所以直接删掉，需要判据时各自内联（见 erase_sector / write / read）。
 *============================================================================*/

/*==============================================================================
 * 擦除
 *============================================================================*/
ota_ret_t flash_iap_erase_sector(u32 addr)
{
    /* ★ v0.22：形参/局部全部落地到 static。擦除地址在 4 次回读里被反复使用，
     *   而中间夹着 raw 调用 —— 地址被踩等于擦错扇区，这是最不能出的错。
     *   注意：这里**故意保持**"回读 rc 不计入返回值"的旧行为（v0.20 已确认
     *   它会把一次读失败误报成"擦不干净"），改语义要单独一轮上板验证。 */
    static u32 s_ea;
    static u8  dat;
    static u8  i;
    static u8  s_g;
    static ota_part_t s_p;

    s_ea = addr;
    s_g  = 0U;
    s_p  = PART_NONE;

    /*==========================================================================
     * ★★★ v0.35：擦除**不再**经过 flash_iap_check_range() ★★★
     *
     *   理由与 v0.30 把读通路的 check_range 内联掉**完全相同**（铁律 14）：
     *   它 4+4+1 = 9 B 形参 > 8 B，必然生成参数块；本工程凡是经过它的路径
     *   都反复出现 rc=2，而把它内联掉的读通路从此再没出过这个问题。
     *
     *   v0.34 是**第一次真正擦 APP 区**，当场吃到：
     *       [APL] erase app fail rc=2
     *   rc=2 就是 OTA_ERR_ADDR，而这次擦的是 0x14000：
     *       容量   0x14000 < 0x20000        ✓
     *       对齐   0x14000 & 0x1FF == 0     ✓
     *       分区   PART_APP_IAP, writable=1 ✓
     *   三条判据静态看全对 ⇒ 只能怀疑"参数传递"这一层，也就是当初读通路
     *   栽过的同一个坑。于是把三条判据逐条内联，并且**每条失败留一句自己的话**
     *   —— 一次上板就能看出是哪一道闸门拒的，不必再猜第二轮。
     *
     *   等价性：扇区 512 B、分区尺寸全是 512 的整数倍，所以"对齐 + 落在某个
     *   可写分区内"蕴含 check_range 闸门 4 的"整段不跨分区"。擦除只擦一个扇区，
     *   acc 恒为 2，闸门 2 只有对齐一条 —— 全部保留。
     *========================================================================*/
    if (!s_inited)                                        s_g = 1U;
    else if (s_ea >= s_eeprom_size)                       s_g = 2U;
    else if (!flash_iap_is_sector_aligned(s_ea))           s_g = 3U;
    else
    {
        s_p = ota_part_of(s_ea);
        if (s_p == PART_NONE || !ota_part_writable(s_p))   s_g = 4U;
    }
    if (s_g != 0U)
    {
        /* 三个值一次给全：gate（哪道闸门）+ part（分区表查出来的 id）+ addr。
         * gate=4 时 part 直接分案：0 ⇒ ota_part_of 查表就没查到，
         * 5 ⇒ 查到了 PART_APP_IAP，是 ota_part_writable 说的不可写。
         * 只在失败时打，正常路径零开销（16K 槽只剩两百多字节）。 */
        LOG_E3("IAP", "erase g=%u p=%u a=%lx", (u16)s_g, (u16)s_p, s_ea);
        return (s_g == 1U) ? OTA_ERR_STATE : OTA_ERR_ADDR;
    }

    flash_iap_raw(IAP_CMD_ERASE, s_ea, 0);

    /* 回读校验：擦除后应全 0xFF（抽 4 点即可，避免 512 次 IAP 读）
     *
     * ★★★ v0.24：把"读失败"和"不是 FF"分开 —— 这是本项目错误码说谎的第三处 ★★★
     *
     *   旧写法：`dat = 0x00; raw(READ,...); if (dat != 0xFF) return IO;`
     *   返回码被丢掉，而 dat 初值是 0x00，于是**只要那一下读失败**，
     *   就必然得到"dat=0x00≠0xFF"⇒ 返回 6。看上去像"擦不干净"，
     *   实际擦得干干净净。
     *
     *   v0.20/v0.23 上板实测：`ps1 e6 i1` —— 擦除 rc=6，但紧接着
     *   `is_erased()` 扫完 512 B 全是 FF，且同扇区 466 B 写+校验全过。
     *   更糟的是 param_save 里 `if (s_rc != OTA_OK) return s_rc;`
     *   会因为这个假 6 **直接跳过整个写动作** ⇒ 参数永远存不下来。
     *
     *   现在：读失败（raw 返回非 OK）⇒ 这一格不算数，继续下一格；
     *         读成功但值不是 FF ⇒ 才是真的没擦净，报 IO。
     *   dat 初值改成 0xFF：万一 raw 什么都没做，也不会伪造成"不干净"。 */
    for (i = 0; i < 4; i++)
    {
        dat = 0xFF;
        if (flash_iap_raw(IAP_CMD_READ, s_ea + (u32)i * 128UL, &dat) != OTA_OK) continue;
        if (dat != 0xFF) return OTA_ERR_IO;
    }
    return OTA_OK;
}

ota_ret_t flash_iap_erase_range(u32 addr, u32 len)
{
    /* ★ static：APP 工程实测 ?flash_iap_verify??BYTE 压住本函数局部段
     *   （010DC3-010DCC），a 与 rc 被写掉就会擦错扇区 —— 这是最不能出的错。 */
    static u32 s_a;
    static u32 s_end;          /* v0.22：上界先算好，循环条件里不再读形参 */
    static ota_ret_t s_rc;

    if (len == 0) return OTA_ERR_PARAM;

    s_a   = addr & ~(OTA_IAP_SECTOR_SIZE - 1UL);          /* 向下对齐 */
    s_end = addr + len;
    while (s_a < s_end)
    {
        s_rc = flash_iap_erase_sector(s_a);
        if (s_rc != OTA_OK) return s_rc;
        s_a += OTA_IAP_SECTOR_SIZE;
        bsp_wdt_feed();                                   /* 长擦除期间喂狗 */
    }
    return OTA_OK;
}

/*==============================================================================
 * 写 / 读
 *============================================================================*/
/* 多字节 IAP 序列期间喂狗间隔（字节）。
 *   每字节编程 30~40µs，32 B ≈ 1.1ms —— 远短于 WDT_PS_32（数百 ms 起）。
 *   喂狗只写 WDT_CONTR，不依赖中断，EA=0 下照样有效。 */
#define IAP_FEED_EVERY   32U

/*==============================================================================
 * ★★★ v0.38：单字节 IAP 命令的重试次数 ★★★
 *
 *   v0.37 上板（A2 第一次真搬）：
 *       [6839] copy off=6000          ← 已经搬了 24 KB，一切正常
 *       [7178] copy fail off=7080 rc=6
 *   rc=6 是 IO，只有两个来源：raw(PROGRAM) 被硬件拒（CMD_FAIL），
 *   或 raw(READ) 失败（回读校验 / 搬运时的源读）。
 *   **两者都不是地址问题**（同一段地址前 225 个 128 B 块全过），也不是门禁
 *   （门禁只会给 rc=2）。⇒ 是"偶发一次命令没成功"这一类。
 *
 *   这与 A1 一直存在的"首趟 CRC 偶发算错、第二趟就对"是**同一个家族**：
 *   `flash_iap_raw` 是一次性的，失败就失败，没有第二次机会。
 *   STC32G 的 IAP 命令本身没有"必须唯一"的语义：
 *       READ  幂等，重试绝对安全
 *       PROGRAM 只在 CMD_FAIL 时重试才安全（命令被拒 ⇒ 位没动过 ⇒ 可以再写一次）
 *   ⇒ 读、写各在**单字节层**重试 3 次；三次都失败才算真失败。
 *   门禁类错误（ADDR/PARAM/STATE）不重试也无所谓，多花 3 次判断而已。
 *============================================================================*/
#define IAP_RETRY        3U

/*==============================================================================
 * ★★★ 形参副本区 —— 2026-10-26 上板 + map 定论 ★★★
 *
 *   实测 map（v0.19）：
 *       ?flash_iap_write??BYTE  @0x10F0E   → buf@0x10F0E, addr@0x10F12, len@0x10F16
 *       ?flash_iap_read??BYTE   @0x10F15   → buf@0x10F15, addr@0x10F19, len@0x10F1D
 *       ?flash_iap_verify??BYTE @0x10F1C   → buf@0x10F1C, addr@0x10F20, len@0x10F24
 *
 *   相邻参数块只隔 **7 字节**，而每个块实际要用 **10 字节**（buf 4 + addr 4 + len 2），
 *   于是后一个块的 addr/len 正好压在前一个块的 buf 上。
 *   write→verify→read 是三层嵌套调用，调用者每写一次自己的块，
 *   就把**上一层还活着的 buf 指针**改掉：
 *
 *      verify 传 buf=0x10F1C  →  read 的 addr@0x10F19 与 len@0x10F1D 覆盖
 *      0x10F1C/0x10F1D/0x10F1E 三个字节  →  read 返回后 verify 拿一个
 *      **被改过的指针**去逐字节比对 ⇒ 必然不相等 ⇒ 恒返回 OTA_ERR_CRC(7)。
 *
 *   这解释了自 v0.18 起的全部现象（v0.16 之前没有 ea/s_bad，布局不同，所以能过）：
 *      `w4=7 但 r=5a a5`（数据真落地了，报错的是比对）
 *      `psv w=7`、`A1 param_save rc=7`、`[VER] rc=12`
 *      而 APP 工程布局不同 ⇒ A1 的 384×128 B 写+校验全过（不是它没问题，是没踩上）
 *
 *   修法：**形参在函数入口第一件事就拷进模块级 static**，此后只用副本。
 *   副本在 ?XD?FLASH_IAP 里（远离所有参数块），从此与布局无关 ——
 *   以后再加变量、再移位，都不会让这三条通路重新踩上。
 *============================================================================*/
static u16         s_wlen;      /* flash_iap_write  的 len 副本 */
static u32         s_waddr;     /* flash_iap_write  的 addr 副本 */
static const u8   *s_wbuf;      /* flash_iap_write  的 buf 副本（不许留在 DR12 里）*/
static u16         s_rlen;      /* flash_iap_read   的 len 副本 */
static u32         s_raddr;     /* flash_iap_read   的 addr 副本 */
static u8         *s_rbuf;      /* flash_iap_read   的 buf 副本 */

/*==============================================================================
 * 一次性诊断寄存器（v0.22，定位完删）
 *
 *   [0] = flash_iap_write 入口拿到的 addr
 *   [1] = 同上的 len
 *   [2] = 本次循环里最后一次 PROGRAM 用的地址
 *   [3] = 被拒掉的 PROGRAM 次数（raw 返回非 OK 的次数）
 *
 *   判读：
 *     [3]=466        → 每一次 PROGRAM 都被 raw 的门禁/硬件拒了 ⇒ 地址或 s_eeprom_size
 *                      在窗口内就变了，跟 Flash 物理无关
 *     [3]=0 且 a=49152 l=466 且写后仍读回 FF
 *                    → 命令被接受、数据没落地 ⇒ 只剩 IAP 时序/供电
 *     [0]!=49152     → 形参在拷贝前就被踩了（本次修复没生效）
 *============================================================================*/
u32 xdata g_iap_dbg[4];
static const u8   *s_vsrc;      /* flash_iap_verify 的 buf 副本（关键！） */
static u16         s_vlen;      /* flash_iap_verify 的 len 副本 */
static u32         s_vaddr;     /* flash_iap_verify 的 addr 副本 */

ota_ret_t flash_iap_write(u32 addr, const u8 *buf, u16 len)
{
    /* ★ static：本函数的局部段被 ?flash_iap_verify??BYTE 压住（实测），
     *   而 dat/i 正是写循环的控制量 —— 被写掉就会漏写或错位。
     *   本函数尾部调 verify，此时循环已结束，共用 static 无妨。 */
    static u8  s_dat;
    static u16 s_i;
    static ota_ret_t s_rc;
    static u8  s_try;                   /* v0.38：单字节重试计数 */
    static u8  s_g;                     /* v0.36：哪一道门禁拒的（0=全过） */
    static ota_part_t s_p;              /* 首字节所属分区 */
    static ota_part_t s_p2;             /* 末字节所属分区（判跨分区） */
    u8 ea;

    /*==========================================================================
     * ★★★ v0.22 铁律：形参必须在**第一条语句**就落地，此后一个字都不许再读 ★★★
     *
     *   实测 map（v0.21）：
     *       ?flash_iap_write??BYTE         = 0x1111C  → addr@+0, len@+8 → 占 ..0x11125
     *       ?XD?FLASH_IAP_WRITE??FLASH_IAP = 0x11120..0x11126  ← 本函数局部段
     *       ?flash_iap_read??BYTE          = 0x11123  ← 下一个块，正压在本块的 len 上
     *
     *   L251 给这些段排布时只按 **7 字节**步进，而 addr@+0 / len@+8 实际要
     *   **10 字节** ⇒ 本函数的 len 槽永远落在别人（或自己局部段）的头上。
     *   v0.21 上板：同一上电里 P1/P2（直接调）466 B 全过，param_save
     *   （隔着 is_erased / 更深的栈）却 w=7 且槽里一个字节都没落地 ——
     *   就是因为 `flash_iap_check_range(addr, len, 1)` 夹在"读到形参"和
     *   "拷进 static"之间，它一返回，形参可能已经不是调用方写的那个值了。
     *
     *   ⇒ 拷贝提到最前面，**且 buf 也必须拷**：它原先活在 DR12 里，
     *     跨 466 次 raw 调用，一旦被谁改掉，写进去的就是另一片内存。
     *========================================================================*/
    s_waddr = addr;
    s_wlen  = len;
    s_wbuf  = buf;

    g_iap_dbg[0] = s_waddr;
    g_iap_dbg[1] = (u32)s_wlen;
    g_iap_dbg[2] = 0UL;
    g_iap_dbg[3] = 0UL;

    if (s_wbuf == 0) return OTA_ERR_PARAM;

    /*==========================================================================
     * ★★★ v0.36：写入**不再**经过 flash_iap_check_range() （铁律 14 第三处）★★★
     *
     *   v0.35 上板（行尾和值逐行核过）：
     *       [9127] A2: apply staging -> app (erase+copy+verify)
     *       [9146] FORCE copy: idempotent check skipped
     *       [9349] copy fail off=0 rc=2        ← 注意：**没有** `[IAP] erase g=`
     *   没有 `erase g=` 那一行，说明 **96 个扇区擦得干干净净**（擦了 222 ms，
     *   96×2ms 对得上）。⇒ 擦除的病根就是 check_range 那条 9 B 形参的调用链，
     *   与 v0.30 读通路内联后"再没出过 rc=2"完全同型。
     *
     *   于是同一个坑换到写这一侧：`copy fail off=0 rc=2` 的 rc=2 只可能来自
     *   check_range（写链上再没有第二个 ADDR 来源：raw 的拒绝只计数不上抛，
     *   verify 上抛的是 CRC/IO）。而擦除已经把 check_range 的四道闸门里
     *   前三道（容量 / 对齐 / 分区可写）证明是好的 —— 擦 0x14000 全过。
     *   写比擦多用的**唯一一道**就是「闸门 4：整段不得跨分区」，
     *   而它调的是 `ota_part_contains(s_p, u32, u32)` —— 又是 1+4+4 = 9 B 形参，
     *   正是 map 注释里写着"压住 check_range 局部段"的那个函数。
     *
     *   ⇒ 四条判据全部内联，跨分区改用"首字节与末字节同区"来判
     *     （分区在 IAP 空间里是连续区间，首末同区 ⇔ 整段在内，等价且零调用）。
     *     失败打一行 `[IAP] wr g=<gate> p=<part> a=<addr>`。
     *========================================================================*/
    s_g  = 0U;
    s_p  = PART_NONE;
    s_p2 = PART_NONE;

    if (s_wlen == 0U)                                        s_g = 5U;
    else if (!s_inited)                                      s_g = 1U;
    else if (s_waddr >= s_eeprom_size)                       s_g = 2U;
    else if ((u32)s_wlen > (s_eeprom_size - s_waddr))         s_g = 2U;
    else
    {
        s_p  = ota_part_of(s_waddr);
        s_p2 = ota_part_of(s_waddr + (u32)s_wlen - 1UL);
        if (s_p == PART_NONE || !ota_part_writable(s_p))      s_g = 4U;
        else if (s_p2 != s_p)                                s_g = 6U;
    }
    if (s_g != 0U)
    {
        /* g=1 未初始化 → STATE(12)；g=5 长度 0 → PARAM(1)；其余一律 ADDR(2)。
         * p 直接分案：0 ⇒ ota_part_of 没查到；5 ⇒ PART_APP_IAP 但不可写；
         * 与末字节的 p 不等 ⇒ 跨分区。 */
        LOG_E3("IAP", "wr g=%u p=%u a=%lx", (u16)s_g, (u16)s_p, s_waddr);
        if (s_g == 1U)      return OTA_ERR_STATE;
        if (s_g == 5U)      return OTA_ERR_PARAM;
        return OTA_ERR_ADDR;
    }

    /*==========================================================================
     * ★★★ 整段关中断 —— 2026-10-26 上板实测定论 ★★★
     *
     *   flash_iap_raw() 自己只保护"一次命令"（EA=0 只在它内部），
     *   466 字节编程就有 **465 个开中断窗口**，每个都能被 1ms 的 Timer0 打进来。
     *========================================================================*/
    ea = flash_iap_critical_enter();
    for (s_i = 0; s_i < s_wlen; s_i++)
    {
        s_dat = s_wbuf[s_i];
        /* ★ v0.38：单字节编程失败先重试 3 次（理由见 IAP_RETRY 注释）。
         *   s_dat 不必每次重设：PROGRAM 只读它、不改它。 */
        for (s_try = 0; s_try < IAP_RETRY; s_try++)
        {
            if (flash_iap_raw(IAP_CMD_PROGRAM, s_waddr + (u32)s_i, &s_dat) == OTA_OK) break;
        }
        if (s_try >= IAP_RETRY)
        {
            g_iap_dbg[3]++;                  /* 三次都没成 ⇒ 记一次"真被拒" */
        }
        if ((s_i % IAP_FEED_EVERY) == (IAP_FEED_EVERY - 1U)) bsp_wdt_feed();
    }
    flash_iap_critical_exit(ea);

    s_rc = flash_iap_verify(s_waddr, s_wbuf, s_wlen);    /* 整块写后校验 */

    /* ★ v0.36：把"命令被硬件拒"和"写进去了但读回不一样"分开。
     *   两种情况在日志里原本都是 rc=7（CRC），但处置完全不同：
     *       rc=6（IO）⇒ raw(PROGRAM) 被拒（分区/容量/CMD_FAIL）——命令没生效
     *       rc=7（CRC）⇒ raw 说成功，Flash 却不认 —— 只剩 IAP 时序/供电
     *       rc=OK      ⇒ 真的写进去了（A2 从此往里走）
     *   只在上抛非 OK 时才多这一次判断，正常路径零开销。 */
    if (s_rc != OTA_OK && g_iap_dbg[3] != 0UL) return OTA_ERR_IO;
    return s_rc;
}

ota_ret_t flash_iap_read(u32 addr, u8 *buf, u16 len)
{
    /* ★ static：flash_iap_read 自己的 3 个形参会生成 ?flash_iap_read??BYTE，
     *   实测它压住了调用者 flash_iap_verify 的局部段 —— 调用者的循环计数器
     *   被被调者的参数写掉是"校验永远不过"的典型成因。
     *   把本函数的局部变量也搬出池子，双向都不留活口。 */
    static u16 s_i;
    static ota_ret_t s_rc;
    static u8 s_bad;          /* ★ 任一字节的 raw 读失败就置位，不再静默返回成功 */
    static u8 s_try;          /* v0.38：单字节重试计数 */
    u8 ea;

    /* ★ v0.22：形参第一条语句就落地（理由同 flash_iap_write）。read 是
     *   verify / is_erased / crc32 / param_init / BL 校验 STAGING 全部走的
     *   唯一读通路，它不稳则整条链路都不稳 —— 所以连 buf 一起拷。
     *   原来 check_range 夹在拷贝之前，而 read 自己的 len 槽恰好压在
     *   写通路的 addr 上，谁先跑谁踩谁。 */
    s_raddr = addr;
    s_rlen  = len;
    s_rbuf  = buf;

    if (s_rbuf == 0) return OTA_ERR_PARAM;

    /* ★★ v0.30：读通路**不再**调用 flash_iap_check_range() ★★
     *
     *   1) acc == 0 时 check_range 的全部动作就是"闸门 1（容量）→ return OK"，
     *      分区 / 扇区对齐 / 可写这三道闸门根本不会执行（它们是给擦写用的）。
     *   2) 但它的形参是 4+4+1 = 9 B > 8 B，**必然生成一个 ?BYTE 参数块** ——
     *      正是本工程反复出事的那个结构（铁律 9）。
     *   3) v0.28 / v0.29 两版都死在正文第 99 块：`rc=2`，而门禁自报
     *      `l=42ff57`。这个 len 由调用点的 `XRL WR0,WR0` 把高 16 位显式清零
     *      （镜像 0xFF0655 实测，全镜像只此一处），逻辑上不可能大于 0xFFFF。
     *      ⇒ 只能怀疑那次调用的参数传递本身。
     *
     *   读是 STAGING 校验（A1）、param 装载、写回读、擦除回读**共用的唯一通路**，
     *   它是整条链上最不该经过可省调用的地方。下面三条判据与 check_range 的
     *   acc==0 分支逐条等价：len==0 → PARAM；!s_inited → STATE；越容量 → ADDR。 */
    if (s_rlen == 0U)      return OTA_ERR_PARAM;
    if (!s_inited)         return OTA_ERR_STATE;
    if (s_raddr >= s_eeprom_size ||
        (u32)s_rlen > (s_eeprom_size - s_raddr))
    {
        /* 只在失败时留档，正常路径零开销。
         * ★ g_cr 的语义随之改变：现在是"**读通路**看到的 addr/len/size"，
         *   不再是"地址门禁看到的"。判读表见 docs/01 §8.32。 */
        g_cr[0] = s_raddr;
        g_cr[1] = (u32)s_rlen;
        g_cr[2] = s_eeprom_size;
        return OTA_ERR_ADDR;
    }

    s_bad = 0U;
    ea = flash_iap_critical_enter();
    for (s_i = 0; s_i < s_rlen; s_i++)
    {
        s_rbuf[s_i] = 0xFF;
        /* ★ v0.38：读是幂等的，单字节失败直接重试 3 次（理由见 IAP_RETRY）。
         *   这一条同时是 A1 "首趟 CRC 偶发算错、第二趟就对"的正面修法：
         *   以前靠上层再算一遍兜住，现在在出问题的最小单元上就重试。 */
        for (s_try = 0; s_try < IAP_RETRY; s_try++)
        {
            if (flash_iap_raw(IAP_CMD_READ, s_raddr + (u32)s_i, &s_rbuf[s_i]) == OTA_OK) break;
        }
        if (s_try >= IAP_RETRY) s_bad = 1U;
        if ((s_i % IAP_FEED_EVERY) == (IAP_FEED_EVERY - 1U)) bsp_wdt_feed();
    }
    flash_iap_critical_exit(ea);
    return (s_bad != 0U) ? OTA_ERR_IO : OTA_OK;
}

u8 flash_iap_read_byte(u32 addr)
{
    /* v0.22：addr 在 check_range 返回后还要用，先落到 static */
    static u32 s_baddr;
    static u8  s_bdat;

    /* ★ v0.25：**不再直接调 flash_iap_raw**，改走 flash_iap_read。
     *
     *   v0.24 上板两次读数（和值都自洽，所以是真值）：
     *       pra2 80 0 0      A 槽首字节 128、B 槽 0
     *       pra2 80 255 0    A 槽首字节 128、B 槽 255
     *   而同一上电里 param_init **没有**报 "param empty/corrupt"
     *   ⇒ 槽里确实躺着有效 param（首字节必为 'A' = 0x41），
     *   而 B 槽两个值（0 / 255）自相矛盾 ⇒ 坏的是这个函数，不是 Flash。
     *
     *   raw 是"一次命令"的裸通路，出参写法与 read 的批处理不同；read 是
     *   verify / is_erased / crc32 / param_init / BL 校验 STAGING 全部走过、
     *   且已被上板反复验证的路径。诊断读数必须走同一条路，否则
     *   "诊断说 A、功能说 B"会再耗掉几轮烧录。
     *
     *   代价：多一次 check_range，读一个字节时无所谓。 */
    s_baddr = addr;
    s_bdat  = 0xFF;                     /* static 不自动初始化，必须显式写 */
    if (flash_iap_read(s_baddr, &s_bdat, 1U) != OTA_OK) return 0xFF;
    return s_bdat;
}

u8 flash_iap_is_erased(u32 addr, u32 len)
{
    static u8 xdata s_e[32];   /* static：原地退出 overlay 池，见 crc32_selftest 的实测记录 */
    /* v0.22：控制量与形参全落地。循环里 `addr + done` 要跨 read 调用计算，
     *   任何一项被踩都会"读到别的扇区"并误判擦净 —— 而它是 param_save
     *   决定"能不能往下写"的唯一依据。 */
    static u32 s_iaddr;
    static u32 s_ilen;
    static u32 s_idone;
    static u16 s_in;
    static u16 s_ii;

    s_iaddr = addr;
    s_ilen  = len;
    s_idone = 0UL;

    while (s_idone < s_ilen)
    {
        s_in = (u16)((s_ilen - s_idone > 32UL) ? 32UL : (s_ilen - s_idone));
        if (flash_iap_read(s_iaddr + s_idone, s_e, s_in) != OTA_OK) return 0;
        for (s_ii = 0; s_ii < s_in; s_ii++)
        {
            if (s_e[s_ii] != 0xFF) return 0;
        }
        s_idone += s_in;
    }
    return 1;
}

/*==============================================================================
 * 校验
 *============================================================================*/
ota_ret_t flash_iap_verify(u32 addr, const u8 *buf, u16 len)
{
    static u8 xdata s_v[32];        /* 读回缓冲：static 才不在 overlay 池里 */
    /* ★ 控制量也必须 static：本函数的局部段被 ?flash_iap_read??BYTE 压住
     *   （实测 010E90-010E9B vs 参数块 @010E98），done/n 被写掉就是死循环
     *   或提前退出 —— 表现出来只是"校验不过"，极难反查。 */
    static u16 s_done;
    static u16 s_n;
    static u16 s_i;
    static ota_ret_t s_rc;

    /* v0.22：拷贝提到最前面（原来 `if (buf == 0)` 在拷贝之前 —— 读形参本身
     *   就有风险，虽然那时还没调用任何函数，但统一规矩比逐个论证可靠）。 */
    s_vsrc  = buf;
    s_vaddr = addr;
    s_vlen  = len;

    if (s_vsrc == 0) return OTA_ERR_PARAM;

    s_done = 0;                     /* static 不会自动清零，必须显式重置 */
    while (s_done < s_vlen)
    {
        s_n = (u16)((s_vlen - s_done > 32U) ? 32U : (s_vlen - s_done));
        /* ★ 原样上抛读通路的错误码。以前写成 `return OTA_ERR_ADDR`，
         *   于是"读失败(IO=6)"和"地址非法(ADDR=2)"在日志里长得一模一样，
         *   v0.18 那句 sw466=2 就是被这一行改写过身份的可疑值。 */
        s_rc = flash_iap_read(s_vaddr + s_done, s_v, s_n);
        if (s_rc != OTA_OK) return s_rc;
        for (s_i = 0; s_i < s_n; s_i++)
        {
            if (s_v[s_i] != s_vsrc[s_done + s_i]) return OTA_ERR_CRC;
        }
        s_done += s_n;
    }
    return OTA_OK;
}

ota_ret_t flash_iap_crc32(u32 addr, u32 len, u32 *p_crc)
{
    return flash_iap_crc32_cb(addr, len, p_crc, 0);
}

ota_ret_t flash_iap_crc32_cb(u32 addr, u32 len, u32 *p_crc,
                             void (*on_tick)(u32 done))
{
    static u8 xdata s_c[64];   /* static：原地退出 overlay 池，见 crc32_selftest 的实测记录 */
    /* v0.22：本条是 BL 校验 STAGING（A1 的 rc=12 就出在这里）的唯一 CRC 通路，
     *   形参和控制量必须全落地 —— 否则"CRC 不符"可能只是读错了地址。 */
    static u32 s_kaddr;
    static u32 s_klen;
    static u32 *s_kpcrc;
    static u32 s_kdone;
    static u32 s_kcrc;
    static u16 s_kn;
    static ota_ret_t s_krc;

    s_kaddr  = addr;
    s_klen   = len;
    s_kpcrc  = p_crc;
    s_kdone  = 0UL;

    if (s_kpcrc == 0 || s_klen == 0) return OTA_ERR_PARAM;

    s_kcrc = crc32_begin();
    while (s_kdone < s_klen)
    {
        s_kn = (u16)((s_klen - s_kdone > 64UL) ? 64UL : (s_klen - s_kdone));
        s_krc = flash_iap_read(s_kaddr + s_kdone, s_c, s_kn);
        if (s_krc != OTA_OK) return s_krc;
        s_kcrc = crc32_update(s_kcrc, s_c, s_kn);
        s_kdone += s_kn;
        if (on_tick != 0) on_tick(s_kdone);
    }
    *s_kpcrc = crc32_final(s_kcrc);
    return OTA_OK;
}

/*==============================================================================
 * 探针：备份 -> 擦 -> 写特征值 -> 回读 -> 还原
 *
 * ★★★ v0.33 整段停用 ★★★
 *   自检里对它的两处调用已删除（理由见 flash_iap_selftest() 里 "4) 两处探针"）。
 *   保留函数体是为了将来 P2 想手工验证"APP 区到底能不能写"时能一句话打开，
 *   默认不参与编译：它要 512 B xdata 备份缓冲 + 逐字节还原循环，
 *   在 16 K 槽只剩 200 多字节的现在没有任何理由常驻。
 *   要用时把 BL_STG_PROBE 改成 1，并在自检里恢复调用点。
 *============================================================================*/
#if (BL_STG_PROBE != 0)
static u8 xdata s_probe_bak[OTA_IAP_SECTOR_SIZE];  /* 512B 备份，必须 xdata */

u8 flash_iap_probe(u32 iap_addr, u8 *backup)
{
    const u8 PATTERN = 0xA5;
    u8 *bak;
    u8  val;
    u16 i;
    u8  ok = 1;

    /* 安全检查：不得探 BL 保护区（那会擦掉 Bootloader 自己） */
    bak = (backup != 0) ? backup : s_probe_bak;

    /* v0.36：check_range 已删除，探针自己内联（本块默认不参与编译） */
    if (iap_addr >= OTA_BL_IAP_BASE && iap_addr <= OTA_BL_IAP_END) return 0;
    if (iap_addr >= s_eeprom_size) return 0;
    if (!flash_iap_is_sector_aligned(iap_addr)) return 0;
    {
        ota_part_t pb = ota_part_of(iap_addr);
        if (pb == PART_NONE || !ota_part_writable(pb)) return 0;
    }

    /* 1) 备份整个扇区 */
    if (flash_iap_read(iap_addr, bak, OTA_IAP_SECTOR_SIZE) != OTA_OK) return 0;

    /* 2) 擦除（判据①：返回码） */
    if (flash_iap_erase_sector(iap_addr) != OTA_OK) { ok = 0; goto restore; }

    /* 3) 写特征值并回读（判据②） */
    val = PATTERN;
    if (flash_iap_raw(IAP_CMD_PROGRAM, iap_addr, &val) != OTA_OK) { ok = 0; goto restore; }
    val = 0x00;
    flash_iap_raw(IAP_CMD_READ, iap_addr, &val);
    if (val != PATTERN) ok = 0;

restore:
    /* 4) 还原原内容（擦 -> 逐字节写回） */
    (void)flash_iap_erase_sector(iap_addr);
    for (i = 0; i < OTA_IAP_SECTOR_SIZE; i++)
    {
        if (bak[i] == 0xFF) continue;
        val = bak[i];
        flash_iap_raw(IAP_CMD_PROGRAM, iap_addr + (u32)i, &val);
    }
    return ok;
}
#endif  /* BL_STG_PROBE != 0 */

/*==============================================================================
 * 自检
 *============================================================================*/

/* 两处探针的实测结果。0 = 本次没测过（目标区非空、主动跳过）。 */
static u8 s_r_stg = FLASH_PROBE_UNTESTED;   /* STAGING 区 0x00000 */
static u8 s_r_app = FLASH_PROBE_UNTESTED;   /* APP 区     0x14000 */

u8 flash_iap_probe_result(u8 which)
{
    return (which == 0) ? s_r_stg : s_r_app;
}

/* v0.33：`probe_str()` 随两处探针一起删除（它只服务于那两行日志） */

ota_ret_t flash_iap_selftest(void)
{
    /*==========================================================================
     * ★★★ v0.24：这 5 个字节必须 static ★★★
     *
     *   汇编实测（v0.23，flash_iap.lst §761）：
     *       MOV R15,R11   ; b0
     *       MOV R12,R11   ; b1
     *       MOV R13,R11   ; b2
     *                       b3 直接留在 R11(=A)
     *   编译器把 4 个结果全放进通用寄存器 —— 而它们要跨 4 次
     *   flash_iap_read_byte() 调用活着，那条链是
     *       read_byte → check_range → ota_part_of / ota_part_contains → raw
     *   被调者只保存 R13 与 WR14(R14:R15)，**R12 和 R11 不保存**。
     *
     *   实测后果（v0.22 / v0.23 两版完全一致）：
     *       bl head: ff ff ff ff   ⇒ b0 已经不是 0x02
     *       selftest FAIL rc=6     ⇒ 走进 b0!=0x02 分支提前 return
     *       ⇒ 后面 STAGING / APP 两处探针全被跳过，永远 untested
     *   而同一上电稍后 bl_iap_alias()（变量全 static）读 0x10000 得 2、
     *   读 0x14000 得 0x75 ⇒ **读通路完全正常，坏的是"存结果的地方"**。
     *
     *   ⇒ 与铁律 5/9 同源：跨函数调用还活着的值，一律不许留在寄存器/栈上。
     *     改 static 后它们进 ?XD?FLASH_IAP_SELFTEST（INSEG），任何参数块都压不到。
     *========================================================================*/
    static ota_ret_t rc;
    /* v0.30：s_stval 随 CRC 自检一起删除（自检不在功能路径上，理由见下） */
    /* v0.33：b0..b3 / bl_first 删除 —— 首字节判据改走批量读 s_pr，只看 s_pr[0] */
    static u8  s_pr[4];                /* v0.31：IAP 读的原始地面证据    */

    rc = OTA_OK;                       /* static 不自动清零，必须显式写 */

    /* ★★ v0.29：先把两个探针结果落到 UNTESTED，再往下判 ★★
     *
     *   v0.28 实测把一句**自相矛盾**的话打到了同一屏上：
     *       [BL] == summary: init=0 selftest=6 stg=untested app=untested ==
     *       [BL] -> STAGING NOT writable: IAP cannot reach FE block
     *   这两行在 bl_summary() 里是**互斥**的（`rs == UNTESTED` 会提前 return），
     *   同时出现只有一种可能：rs 既不是 0 也不是 1/2，而是 0xFF。
     *
     *   链条是这样的：
     *     · crc32 自检 FAIL 时本函数在下面提前 return；
     *     · 而 `s_r_stg = UNTESTED` 写在**那之后**，所以没被执行；
     *     · `static u8 s_r_stg = FLASH_PROBE_UNTESTED;`（=0）的 xdata 初值
     *       在 BL 里**没有生效** —— C251 的启动代码不搬运 ?XD? 初值数据，
     *       xdata 上电是 0xFF（本项目一切 "读数像垃圾" 的现象都先怀疑这条）；
     *     · bl_pstr() 对 0xFF 走兜底分支打 "untested"，
     *       而 bl_summary() 的判据是 `rs == 0`，于是继续往下走到
     *       `rs != WRITABLE` 那条错误分支，打出 "cannot reach FE block"。
     *
     *   ⇒ 赋值提到最前面，两个探针的"未测"状态就永远自洽。
     *   ★ 引申铁律：**凡 `static xdata x = 常量;` 都别指望初值**，
     *     要么在函数里显式赋值，要么干脆不给初值（免得读代码的人误信）。 */
    s_r_stg = FLASH_PROBE_UNTESTED;
    s_r_app = FLASH_PROBE_UNTESTED;

    LOG_I1("IAP", "eeprom=%uKB", (u16)(s_eeprom_size >> 10));

    /* ★★★ v0.33：v0.31 的 `d40` / `dbl0` 两条地面证据已使命完成 ★★★
     *
     *   它们上板给出的四个值（行尾和值全部核对通过，都是真值）：
     *       d40  = 75 84 0 7e   flash_iap_read(STAGING+0x40, buf, 4)  → 正文首块正确
     *       dbl0 = 2  r=0       flash_iap_read(0x10000,  buf, 4)      → 读通路正确
     *       bl head = ff ff ff ff（4 次 flash_iap_read_byte）        → 只有 len=1 的封装错
     *   判决：**批量读无罪、STAGING 内容无罪**，坏的是逐字节诊断封装。
     *   于是 v0.33 做了两件事：自检的读证改用批量读（见下面第 3 条），
     *   两条探针删除腾空间给 A2。s_pr[4] 保留给下面的首字节判据用。
     *------------------------------------------------------------------------*/
    /* 1) CRC 自检 —— ★ v0.30 整段删除 ★
     *
     *   这条自检从 v0.25 起连续 6 个版本都在打 FAIL（cbf41f48 / 9f4db109 /
     *   84d14afa / c6f39ea9 / 69d39b4b …），而同期**四条真正的功能通路**
     *   全部与 Python 的 binascii.crc32 逐位一致：
     *       APP 造镜像 49088 B、APP 回读 STAGING 49088 B、
     *       param 462 B、镜像头 36 B（step=5 连续三版都过）。
     *   ⇒ CRC 引擎、查表、通用指针、xdata+索引 全部无罪；
     *     唯一算错的就是这个 9 字节自检向量本身，而它**不在任何功能路径上**。
     *
     *   它还有两个实打实的害处：
     *     · 一旦 FAIL 就在下面提前 return，把**真正有价值的判据**（读 BL 头
     *       4 字节 = 读通路实测证明）以及 STAGING/APP 两处探针全跳过，
     *       日志里只剩一句误导人的 "cannot reach FE block"；
     *     · 占着 BL 那 16K 槽里本就不够用的字节数（A2 还要 450~550 B）。
     *
     *   ⇒ 读通路的可信度改由下面第 3) 条（读 BL 入口 0x02）来证明 ——
     *     那是**真固件真地址**，比自检常量向量强得多。 */

    /* 2) 容量一致性
     *
     *    ★★ 这一条只能查"我们自己传的值能不能覆盖分区表"，查不出"芯片实际设了多少" ★★
     *    s_eeprom_size 是 flash_iap_init(128) 的形参，不是从芯片选项字节读回来的。
     *    芯片若仍是默认的小 EEPROM，下面的 mismatch 永不触发，探针失败会被误判成
     *    "第三地址寄存器没生效"（那是方案 (b) 的结论，修法完全相反）。
     *    芯片真实值的唯一来源：STC-ISP「检测MCU选项」，或下面的写-回读探针。
     */
#if (OTA_IAP_CAN_REACH_CODE != 0)
    if (s_eeprom_size != OTA_EEPROM_SIZE)
    {
        LOG_E1("IAP", "capacity mismatch: cfg=%uKB want=128KB", (u16)(s_eeprom_size >> 10));
        return OTA_ERR_ADDR;
    }
#else
    if (s_eeprom_size != OTA_EEPROM_SIZE)
    {
        LOG_E1("IAP", "capacity mismatch: cfg=%uKB want=64KB", (u16)(s_eeprom_size >> 10));
        return OTA_ERR_ADDR;
    }
#endif

    /* 3) ★★ 先证明"读"是可信的，再允许任何写 ★★
     *
     *    BL 区（IAP 0x10000）里一定躺着真固件，而且复位入口是链接器生成的
     *    3 字节 LJMP，操作码固定 0x02（BL 自身实测 `bl head: 2 ...` 可印证）。
     *
     *    所以"能不能读到 0x02"就是 IAP 读通路的唯一实测判据：
     *      · 读到 0x02  -> 读通路可信，后面才允许擦/写；
     *      · 读到别的  -> 读通路不可信（命令码错 / 地址寄存器没生效 /
     *                      芯片 EEPROM 没设成 128K）。
     *
     *    ★ 为什么必须"先验证再写" ★
     *      2026-10-25 那次的教训：读通路坏的时候，探针的"备份 -> 擦 -> 写 -> 还原"
     *      会拿垃圾去覆盖真数据，而且探针还会"成功"返回。写操作是不可逆的，
     *      读操作永远可逆 —— 所以顺序只能是"先读证，后写测"，不能反过来。
     */
#if (OTA_IAP_CAN_REACH_CODE != 0)
    /* ★★★ v0.33：这一条改走**批量通路**（flash_iap_read 读 4 字节）★★★
     *
     *   v0.31 的 `d40` / `dbl0` 两行已经用上板实测把结论钉死了：
     *       d40  = 75 84 0 7e   ← flash_iap_read(STAGING+0x40, buf, 4)
     *       dbl0 = 2 r=0        ← flash_iap_read(0x10000, buf, 4)  ← 同一条路、同一地址
     *   而同一上电里 `bl head`（4 次 flash_iap_read_byte）读出 ff ff ff ff。
     *   同一个地址、同一个底层 read，只有"len=1 的逐字节封装"这条不行。
     *
     *   ⇒ 自检要证明的是"功能通路的读可信"，那就必须用**功能通路**去证。
     *     用一个不在功能路径上的诊断封装去证，结果就是自检恒 FAIL(rc=6)、
     *     后面所有探针全被跳过 —— 这正是 v0.22~v0.32 反复看到的现象。
     *     b0..b3 随之取消（只需要首字节），判据③（探针后复查 BL 首扇区）
     *     也一起删掉 —— 它依赖的探针本身已经删除（见下面 "4) 两处探针"）。 */
    rc = flash_iap_read(OTA_BL_IAP_BASE, s_pr, 4U);
    LOG_I4("IAP", "bl head: %x %x %x %x (want 2 ...)", (u16)s_pr[0], (u16)s_pr[1],
                                                      (u16)s_pr[2], (u16)s_pr[3]);
    if (rc != OTA_OK)
    {
        LOG_E1("IAP", "bl head read rc=%u, NO WRITE DONE", (u16)rc);
        return rc;
    }

    if (s_pr[0] != 0x02)
    {
        LOG_E1("IAP", "IAP READ path untrusted (head %x != 2), NO WRITE DONE", (u16)s_pr[0]);
        return OTA_ERR_IO;                 /* 绝不往下走任何擦写 */
    }
#else
    /* 方案 (b)：IAP 到不了 0x10000 以上，这个判据本来就不适用 */
    LOG_I0("IAP", "read-path check skipped (CAN_REACH=0)");
#endif

    /* ★★★ 4) 两处探针：v0.33 整段删除 ★★★
     *
     *   原逻辑是"备份整扇区 -> 擦 -> 写特征值 0xA5 -> 回读 -> 逐字节还原"，
     *   用来证明 IAP 真的能写。删掉的理由按重要性排序：
     *
     *   ① **风险大于收益。** APP 区探针只在"首字节读到 0xFF（空片）"时才做，
     *      而 `flash_iap_read_byte` 正是那条已知会误报 0xFF 的封装
     *      （v0.31 实测 `bl head: ff ff ff ff`，而别处读同一地址得 2）。
     *      一旦它在有效固件上误报 FF，探针就会擦掉 APP 首扇区
     *      （= 复位入口所在扇区）再"还原"，中途掉电即变砖。
     *
     *   ② **结论已被真业务覆盖。** A1 已让 APP 真的写 49088 B 到 STAGING
     *      并整块回读校验通过（crc=66c21819，APP / BL / Python 三方一致）；
     *      A2 还要真的擦写 48 K 到 APP 区 —— 真业务的写证比"写一个 0xA5"强。
     *
     *   ③ 16 K 槽只剩不到 300 B，A2 要 500 B 左右。摘掉它同时省 code 和
     *      512 B xdata（s_probe_bak）。
     *
     *   ⇒ 两个探针结果一律 UNTESTED（= 本次没测过），由 bl_summary 如实播报。
     *     不测就写"未测"，绝不假装成功 —— 2026-10-25 的假阳性就是这么来的。 */
    s_r_stg = FLASH_PROBE_UNTESTED;
    s_r_app = FLASH_PROBE_UNTESTED;

    /* 未测 ≠ 失败，但上层必须知道"可写性未验证"（OTA_ERR_SUPPORT = 11）。 */
    rc = OTA_ERR_SUPPORT;

    return rc;
}
