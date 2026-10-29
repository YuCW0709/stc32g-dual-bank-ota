/*==============================================================================
 *  flash_iap.h  --  Flash IAP 擦 / 写 / 读 / 校验 与地址合法性检查
 *  Platform : STC32G12K128  (Keil C251, CodeRomSize = Huge)
 *  Depends  : stc32g.h, ota_types.h, ota_partition.h
 *  Version  : v3 —— 依据 STC-ISP 128K EEPROM 实测档位 + 三寄存器语义定稿
 *
 *==============================================================================
 *  【地址语义（v3，务必先读）】
 *  1) IAP 地址由 **三个** 寄存器拼成（官方例程统一写法，缺一不可）：
 *         IAP_ADDR  = (u8)(addr >> 16);   // bit16 及以上（最高地址字节）
 *         IAP_ADDRH = (u8)(addr >> 8);    // bit15 ~ bit8
 *         IAP_ADDRL = (u8)(addr);         // bit7  ~ bit0
 *
 *  2) IAP 地址不是"代码域绝对地址"，而是以 0 为基的 **EEPROM 视图地址**：
 *         IAP 0x00000 <-> 物理 FE:0000h
 *         IAP 0x10000 <-> 物理 FF:0000h  （与代码域重合，仅 EEPROM=128K 时可达）
 *     换算：phys = 0xFE0000 + iap_addr；code_addr = iap_addr + 0xFE0000。
 *     所以调用方**只能传 IAP 地址**，绝不能把 0xFF2000 这类代码地址传进来。
 *
 *  3) 当 STC-ISP「设置用户EEPROM大小」= 128 KB 时，整片 128 KB Flash 都在
 *     IAP 可寻址范围内 —— 这意味着 **Bootloader 自己就落在 IAP 0x10000 上，
 *     是 IAP 可见且可擦的**。本模块的写白名单必须显式拒绝该段，
 *     见 ota_part_writable(PART_USER_ISP) 与下方「地址合法性检查」。
 *
 *  4) 真正要防的失效模式（v1 说的"写 0x10000 回卷到 0x0000 擦掉 BL"是错的）：
 *       a. 地址越出已配置的 EEPROM 容量 -> 溢出写，落到非预期偏移；
 *       b. 搬运时长度算错 / 起点错，擦到 BL 自己的扇区 -> 变砖。
 *     两条都由 flash_iap_check_range() 在碰硬件之前拦下。
 *
 *==============================================================================
 *  【时序 / 安全约束】
 *  1) 擦除与编程期间 CPU 取指被硬件挂起，必须 EA=0 后再操作、完成后恢复，
 *     否则 UART 中断被延迟，AT 侧丢字节。
 *  2) 禁止擦写当前正在执行的扇区：BL 运行在 IAP 0x10000~0x11FFF，只能写
 *     0x12000 以上与 0x0FFFF 以下；APP 运行在 0x12000~0x1FFFF，只能写
 *     0x00000~0x11FFF 中的暂存/参数区。
 *  3) 写前必须整扇区擦除，由调用方保证"擦 -> 写"顺序。
 *============================================================================*/
#ifndef __FLASH_IAP_H
#define __FLASH_IAP_H

#include "ota_mcu.h"
#include "ota_types.h"
#include "ota_partition.h"

/*----------------------------- IAP 命令码 -----------------------------------
 * 已核对（STC32G 手册例程 / 国芯官方论坛 / 逐飞 STC32G12K128 库，三处一致）：
 *      IAP_CMD = 0  空闲        (standby)
 *      IAP_CMD = 1  **读出**    (read)
 *      IAP_CMD = 2  **编程/写** (program)
 *      IAP_CMD = 3  扇区擦除    (erase)
 *
 * ★★★ 2026-10-25 血案，绝对不要再凭记忆改这两个数 ★★★
 *   本文件曾经写成 PROGRAM=0x01 / READ=0x02（1 和 2 写反），编译、链接、
 *   烧录全部正常，**没有任何警告**。实际后果是：
 *      · 每一次"读 Flash"都在往 Flash 写一个字节（写的是 IAP_DATA，=0x00）；
 *      · 每一次"写 Flash"都是空读，什么都没写进去。
 *   一次上电就把 Bootloader 复位入口 0xFF0000 的前 4 字节改成了 00 00 00 00 ——
 *   下次复位直接起不来（ISP 冷启动下载仍然有效，因为引导程序在独立 ROM 里）。
 *   现场日志长得完全像"第三地址寄存器没生效"，差点据此改走方案 (b)。
 *   → 现在 flash_iap_raw() 里加了命令码白名单 + 写分区硬闸门，见该函数注释。
 *--------------------------------------------------------------------------*/
#define IAP_CMD_IDLE        0x00   /* 空闲                               */
#define IAP_CMD_READ        0x01   /* 字节读                             */
#define IAP_CMD_PROGRAM     0x02   /* 字节编程，单次 1 字节              */
#define IAP_CMD_ERASE       0x03   /* 扇区擦除，单位 OTA_IAP_SECTOR_SIZE  */

/*----------------------------- 自检开关 -------------------------------------
 * BL_STG_PROBE = 1 : 自检时对 STAGING 区(0x00000)做一次真实探针
 *                    （备份 512B -> 擦 -> 写 0xA5 -> 回读 -> 逐字节还原）。
 *                    这是**唯一**能实测证明"IAP 写得进去"的动作。
 *                    代价：每次上电都会擦掉 STAGING。
 * BL_STG_PROBE = 0 : 只读不写，自检退化成"读通路验证"。
 *
 * ★★★ 2026-10-26 改为 0：这个探针是 A1 一直失败的真正元凶 ★★★
 *
 *   它擦的是 STAGING 的**第一个扇区 = 镜像头所在**（magic 就在 0x00），
 *   然后靠一个 512 次单字节 PROGRAM 的循环"还原"，而那个循环：
 *       · 不校验还原结果（`(void)erase` + 逐个 program，全都不看返回码）
 *       · 不喂狗（`flash_iap_erase_range` 里有 bsp_wdt_feed，这里没有）
 *   于是"每次上电都擦一遍真镜像的头，再赌它还回去"。
 *
 *   实测：v0.11 第二次上电 `stg=W`（探针跑了）之后，
 *   `ota_verify` 从 0x000000 读到的 magic 是 FF FF FF FF
 *   → `rc=12 step=0` 稳定复现；而同一版固件在别的上电里
 *   探针又把头还回去了（probe 读到 4F 54 41 49 = "OTAI"）。
 *   同一个动作两种结果 —— 这就是"还原不可靠"。
 *
 *   结论：只要 STAGING 里可能有镜像，这个探针就**一次都不能跑**。
 *   想验证"写得进去"，用 APP 侧的 A1 造镜像（写 48K + 逐块回读校验），
 *   那才是真实的写通路验证，而且不会碰到头。
 *--------------------------------------------------------------------------*/
#ifndef BL_STG_PROBE
#define BL_STG_PROBE        0
#endif

/* IAP_TRIG 两拍握手魔数（STC 固定值：先 0x5A 再 0xA5 才触发） */
#define IAP_TRIG_KEY1       0x5A
#define IAP_TRIG_KEY2       0xA5

/*----------------------------- 第三地址寄存器名 -----------------------------
 * stc32g.h 中该寄存器可能定义为 IAP_ADDR（官方例程主流写法），也可能叫
 * IAP_ADDRE。注意 C251 的 sfr 是编译器关键字而非宏，**不能用 #ifdef 探测**，
 * 所以这里不做名字归一，由 flash_iap.c 直接按 stc32g.h 的实际拼写引用。
 * TODO(核对) 打开 stc32g.h 搜索 "IAP_ADDR" 确认拼写。
 *--------------------------------------------------------------------------*/

/* 擦写轮询上限，防止 IAP 硬件异常时死等 */
#define FLASH_IAP_POLL_MAX  0xFFFFU

/*==============================================================================
 * 初始化 / 容量配置
 *============================================================================*/

/**
 * @brief  初始化 IAP 模块：设置等待周期、复位寄存器、登记 EEPROM 容量。
 * @param  eeprom_kb  STC-ISP「设置用户EEPROM大小」的取值（KB）。
 *                    STC-ISP V6.90D 可选 1 ~ 128，本工程取 128
 *                    （= 整片 Flash 都在 IAP 域内）；传 0 表示按
 *                    OTA_EEPROM_SIZE 宏自动推导。
 * @return OTA_OK         成功；
 *         OTA_ERR_PARAM  取值不在 1~128 范围，或与 OTA_EEPROM_SIZE 不一致；
 *         OTA_ERR_ADDR   容量小于分区表所需（放不下）。
 * @note   IAP_TPS（等待周期/时序寄存器）必须按主频设置，否则擦写失败。
 *         TODO(核对手册) STC32G 是否存在 IAP_TPS 寄存器及其取值表。
 *         本函数是唯一允许登记容量的入口，运行期不可更改。
 */
ota_ret_t flash_iap_init(u16 eeprom_kb);

/**
 * @brief  查询登记的 EEPROM 容量（字节）。
 * @return 128 KB 时返回 0x20000。
 */
u32 flash_iap_eeprom_size(void);

/**
 * @brief  查询登记的 EEPROM 容量（KB）。
 * @return KB 数。
 */
u16 flash_iap_eeprom_kb(void);

/*==============================================================================
 * 地址寄存器装载（三个，顺序固定）
 *============================================================================*/

/**
 * @brief  把 IAP 地址拆装到 IAP_ADDR / IAP_ADDRH / IAP_ADDRL。
 * @param  iap_addr  IAP 地址（EEPROM=128K 时 0 ~ 0x1FFFF）。
 * @return 实际装载的地址（便于回读断言）。
 * @note   内部即官方三行写法；只装地址，不触发 IAP。
 */
u32 flash_iap_set_addr(u32 iap_addr);

/**
 * @brief  IAP 地址 -> 物理显示地址（仅用于日志/调试）。
 * @param  iap_addr  IAP 地址。
 * @return 形如 0xFE0100 / 0xFF2100 的 24 位物理地址。
 */
u32 flash_iap_to_phys(u32 iap_addr);

/*==============================================================================
 * 上电自检 / 探针（决定 OTA_IAP_CAN_REACH_CODE 的最终取值）
 *============================================================================*/

/**
 * @brief  安全探针：备份扇区 -> 擦除 -> 写特征值 -> 回读 -> 还原原内容。
 * @param  iap_addr  探测点，必须扇区对齐（512 B）。
 *                   ★ 必须传 OTA_APP_IAP_BASE（= 0x12000，APP 区首扇区）；
 *                     **绝对不要传 0x10000** —— 那是 Bootloader 自己的首扇区，
 *                     探针会当场把它擦掉（自毁探针）。
 * @param  backup    512 B 备份缓冲区（xdata），用于探测后还原；可为 NULL
 *                   表示不还原（仅限该扇区内容可牺牲的场景，例如空片产测）。
 * @return 1 可写且回读一致（IAP 可达 FF 块 -> 开关置 1）；
 *         0 不可写 / 回读不一致 / 越界（-> 开关置 0，走 A/B 方案）。
 * @note   三项判据（缺一不可），由 flash_iap_selftest() 统一编排：
 *           ① 擦除返回码：成功 / 越界拒绝
 *           ② 写入 0xA5 后回读该地址是否为 0xA5
 *           ③ 探测完成后 BL 首扇区（IAP 0x10000）内容未变
 *         只允许从 Bootloader 调用（BL 在 0x10000，探 0x12000 不冲突）。
 */
u8 flash_iap_probe(u32 iap_addr, u8 *backup);

/**
 * @brief  全量自检：容量一致性 + 三寄存器位宽行为 + 安全探针，输出结论日志。
 * @return OTA_OK        与登记容量一致，可达性符合 OTA_IAP_CAN_REACH_CODE；
 *         OTA_ERR_ADDR  登记容量与实测不符（回头改 STC-ISP 设置）；
 *         OTA_ERR_SUPPORT 实测结果与编译期开关不一致（必须改开关后重编）。
 * @note   日志形如：
 *         "[IAP] eeprom=128KB ok, 3addr-reg ok, probe@0x12000=W, BL intact"
 *         这是量产前第一项必须跑通的自检，也是定稿开关的唯一依据。
 */
ota_ret_t flash_iap_selftest(void);

/*----------------------------------------------------------------------------
 * 探针实测结果（供上层打印准确结论）
 *
 * ★ 为什么需要这个接口：
 *   探针有"因为目标区非空而跳过"的分支。跳过时必须返回 UNTESTED，
 *   绝不能用 writable=1 之类的默认值冒充"可写"——那会让上层打印出
 *   "OTA 架构成立"这种假结论，进而把整个方案选错。
 *   血的教训：2026-10-25 实测 APP 区非空 -> 跳过探针 -> 硬设 writable=1
 *   -> 日志显示 selftest OK -> 差点据此进第二步。
 *----------------------------------------------------------------------------*/
#define FLASH_PROBE_UNTESTED      0
#define FLASH_PROBE_WRITABLE      1
#define FLASH_PROBE_NOT_WRITABLE  2

/**
 * @brief  查询 flash_iap_selftest() 这一次实际做过的探针结果。
 * @param  which  0 = STAGING 区（IAP 0x00000，OTA 下载数据落点）
 *                1 = APP 区    （IAP 0x14000，OTA_APPLY_IS_MOVE 的搬运目标）
 * @return FLASH_PROBE_UNTESTED / WRITABLE / NOT_WRITABLE
 * @note   同一块芯片重复调用 selftest 会刷新这两个值。
 */
u8 flash_iap_probe_result(u8 which);

/*==============================================================================
 * 地址门禁 —— ★ v0.36 起**没有**统一的 flash_iap_check_range() 了
 *  ----------------------------------------------------------------------------
 *  原函数形参 4+4+1 = 9 B > 8 B，必然生成 ?BYTE 参数块；本工程凡是"经过它"
 *  的路径都偶发拿到别人的值，表现永远是 rc=2（OTA_ERR_ADDR）：
 *      v0.30 读通路内联掉 → 从此不再报（此前 chk a/l 给出 l=42ff57 这种不可能值）
 *      v0.35 擦除内联掉   → 96 个扇区一次擦净，`erase g=` 一行都没打
 *      v0.36 写入内联掉   → 见 write 里的注释
 *  所以函数已整段删除，判据由 read / erase_sector / write 各自内联。
 *  需要新增判据时，照抄其中任意一处，**不要再造一个多形参的门禁函数**。
 *
 *  内联时必须覆盖的四条（顺序不变，先便宜后昂贵）：
 *      1) len != 0
 *      2) addr <  size  且  len <= size - addr     （减法防溢出）
 *      3) ota_part_of(addr) 有效且 ota_part_writable() == 1
 *      4) 首字节与末字节同区（等价于"整段不跨分区"，且零函数调用）
 *  擦除额外要求扇区对齐（acc==2 才查）；PROGRAM 按字节编程，不查对齐。
 *============================================================================*/


/**
 * @brief  扇区对齐检查（纯算术，不查分区）。
 * @param  addr  IAP 地址。
 * @return 1 已按 OTA_IAP_SECTOR_SIZE 对齐；0 未对齐。
 */
u8 flash_iap_is_sector_aligned(u32 addr);

/*==============================================================================
 * 擦除
 *============================================================================*/

/**
 * @brief  擦除单个扇区（OTA_IAP_SECTOR_SIZE = 512 B）。
 * @param  addr  扇区起始 IAP 地址，必须 512 B 对齐且在可写分区内。
 * @return OTA_OK / OTA_ERR_ADDR（含 BL 保护区）/ OTA_ERR_IO（擦后回读非 0xFF）/
 *         OTA_ERR_BUSY。
 */
ota_ret_t flash_iap_erase_sector(u32 addr);

/**
 * @brief  擦除区间 [addr, addr+len)，逐扇区进行，处理非整扇区尾部。
 * @param  addr  起始 IAP 地址（内部向下对齐到扇区）。
 * @param  len   字节数，> 0。
 * @return OTA_OK / OTA_ERR_ADDR / OTA_ERR_IO。
 * @note   逐扇区开关中断，不长时间关中断；大区间请由上层切片以便喂狗上报。
 */
ota_ret_t flash_iap_erase_range(u32 addr, u32 len);

/*==============================================================================
 * 写入 / 读取
 *============================================================================*/

/**
 * @brief  从 addr 起写入 buf 的 len 字节（目标扇区必须已擦除）。
 * @param  addr  IAP 地址，落在可写分区内（BL 保护区会被拒）。
 * @param  buf   源缓冲区（xdata）。
 * @param  len   字节数，建议 <= 256（控制单次关中断时长）。
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_ADDR / OTA_ERR_IO（回读不一致）。
 * @note   不做擦除；IAP 只能 1->0，写非 0xFF 目标会失败。
 */
ota_ret_t flash_iap_write(u32 addr, const u8 *buf, u16 len);

/**
 * @brief  从 addr 起读取 len 字节到 buf（IAP 字节读，非 MOVC）。
 * @param  addr  IAP 地址。
 * @param  buf   目标缓冲区（xdata）。
 * @param  len   字节数，建议 <= 256。
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_ADDR。
 */
ota_ret_t flash_iap_read(u32 addr, u8 *buf, u16 len);

/**
 * @brief  读单字节。
 * @param  addr  IAP 地址。
 * @return 数据；地址非法返回 0xFF。
 */
u8 flash_iap_read_byte(u32 addr);

/**
 * @brief  判定区间是否全为 0xFF（擦除态）。
 * @param  addr  IAP 地址。
 * @param  len   字节数。
 * @return 1 全 0xFF；0 存在非 0xFF 数据或地址非法。
 */
u8 flash_iap_is_erased(u32 addr, u32 len);

/*==============================================================================
 * 校验
 *============================================================================*/

/**
 * @brief  比较 IAP 区间与 RAM 缓冲是否一致。
 * @param  addr  IAP 地址。
 * @param  buf   RAM 缓冲区（xdata）。
 * @param  len   比较字节数。
 * @return OTA_OK / OTA_ERR_CRC（有差异）/ OTA_ERR_PARAM / OTA_ERR_ADDR。
 */
ota_ret_t flash_iap_verify(u32 addr, const u8 *buf, u16 len);

/**
 * @brief  计算 IAP 区间 CRC32（分块 64 B 读取，不占大 RAM）。
 * @param  addr    IAP 地址。
 * @param  len     字节数。
 * @param  p_crc   输出 CRC32，不可为 NULL。
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_ADDR。
 * @note   与 crc32.h 同多项式（反射 0xEDB88320），保证与上位机一致。
 */
ota_ret_t flash_iap_crc32(u32 addr, u32 len, u32 *p_crc);

/**
 * @brief  同上，支持进度回调（喂狗 + 上报）。
 * @param  addr     IAP 地址。
 * @param  len      字节数。
 * @param  p_crc    输出 CRC32。
 * @param  on_tick  每处理 4 KB 调一次，参数为已完成字节数；可为 NULL。
 * @return 同 flash_iap_crc32()。
 */
ota_ret_t flash_iap_crc32_cb(u32 addr, u32 len, u32 *p_crc,
                            void (*on_tick)(u32 done));

/*==============================================================================
 * 底层原语（仅 flash_iap.c / Bootloader 内部使用）
 *============================================================================*/

/* 关闭 / 恢复总中断，返回进入前 EA 状态，成对使用 */
u8   flash_iap_critical_enter(void);
void flash_iap_critical_exit(u8 ea_state);

/* 直接下发一次 IAP 操作：不关中断、不做容量/分区校验，iap_addr 为 IAP 地址 */
ota_ret_t flash_iap_raw(u8 cmd, u32 iap_addr, u8 *p_dat);

/*==============================================================================
 * 硬件失败标志 CMD_FAIL（IAP_CONTR.bit4）
 *
 *   "擦不干净 / 写不进去"有两个完全不同的来源，必须分开：
 *       (a) 硬件拒绝了命令        → CMD_FAIL = 1
 *       (b) 命令跑了但物理没生效  → CMD_FAIL = 0，回读却不等于写入值
 *   (a) 往地址/选项字节/使能位上查；(b) 往供电与芯片本身查。
 *
 *   语义：**粘滞**。窗口内任何一次 raw 操作失败都会置 1，直到显式清零。
 *   典型用法：
 *       flash_iap_fail_clear();
 *       rc = flash_iap_erase_range(...);
 *       f  = flash_iap_last_fail();
 *============================================================================*/
u8 flash_iap_last_fail(void);
void flash_iap_fail_clear(void);

/*==============================================================================
 * v0.29：地址门禁探针 —— 上一次 flash_iap_check_range() 看到的三个量
 *
 *   [0] = addr   [1] = len   [2] = s_eeprom_size
 *
 *   闸门 1 的两条判据（地址超容量 / 长度越界）返回同一个 OTA_ERR_ADDR(2)，
 *   单看 rc=2 分不清是哪一条、也看不见容量。失败时把这三个值打出来即可定论。
 *   ★ 纯诊断量，A1 判读完成后删除。
 *============================================================================*/
extern u32 xdata g_cr[3];

/*==============================================================================
 * v0.22 一次性诊断（定位完连同 BL 的 bl_param_probe 一起删）
 *
 *   [0] = flash_iap_write 入口拿到的 addr 副本
 *   [1] = 同上的 len 副本
 *   [2] = 循环里最后一次 PROGRAM 用的地址
 *   [3] = 被拒掉（raw 返回非 OK）的 PROGRAM 次数
 *
 *   存在的理由：C251 的 ?func??BYTE 参数块按 7 字节步进而实际要 10 字节，
 *   "报错"和"数据没落地"这两种完全不同的故障在日志里长得一模一样。
 *   把写函数内部真正用到的三个数念出来，一次就能分开。
 *============================================================================*/
extern u32 xdata g_iap_dbg[4];

#endif /* __FLASH_IAP_H */
