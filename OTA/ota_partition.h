/*==============================================================================
 *  ota_partition.h  --  分区地址表（唯一真源）· v5 定稿
 *  Platform : STC32G12K128  (Keil C251, CodeRomSize = Huge, Memory Model = LARGE)
 *
 *==============================================================================
 *  【地址模型（v3 起定稿，依据：STC-ISP V6.90D 实测 + STC32G 手册 IAP 章节）】
 *  128 KB Flash 是**一片**物理空间，EEPROM 与程序区是它的**两种逻辑视图**，
 *  不是相加关系。当 STC-ISP 中「设置用户EEPROM大小」= 128 KB 时：
 *
 *      IAP 地址 0x00000  <->  物理 FE:0000h
 *      IAP 地址 0x0FFFF  <->  物理 FE:FFFFh
 *      IAP 地址 0x10000  <->  物理 FF:0000h   <-- 与代码域重合，可达
 *      IAP 地址 0x1FFFF  <->  物理 FFFFFFh
 *      换算关系：phys      = 0xFE0000 + iap_addr
 *                code_addr = iap_addr + 0xFE0000
 *
 *  ⚠ 由此产生一条铁律：Bootloader 自己就落在 IAP 0x10000 ~ 0x13FFF 上。
 *    IAP 写保护必须**显式拒绝**这一段，否则一次越界搬运就把 Bootloader 擦了。
 *
 *  低 64K 的 code 视图（0x000000~0x00FFFF）是 IAP 0x0000~0xFFFF 的**别名窗口**，
 *  物理上是同一批字节（链接器会把 16 位寻址的 NCONST 类丢在那里）。
 *  所以低 64K 的每一段都必须当成"可被 IAP 直接擦写"来规划，见下。
 *
 *==============================================================================
 *  【v5 相对 v4 的三处变化（都是实测驱动，不是拍脑袋）】
 *
 *  ① NCONST 双槽（v4 在 0xE000 / 0xF000 各留 4 KB）**不再需要**。
 *     v4 这么做是因为 TINY 模型下字符串字面量走 NCONST（16 位地址）。
 *     改用 LARGE 模型后（详见 ota_mcu.h 的指针模型门禁），字面量全部落到
 *     HCONST（24 位），实测两个工程的 NCONST 用量都是 **0 字节**：
 *         ACTIVE MEMORY CLASSES:  NCONST  START 00F000H  USED (空)
 *     于是双槽合并成一块 8 KB 的只读兜底区（0xE000~0xFFFF），
 *     平时不占空间，真出现 near 常量也不会被擦暂存区时误伤。
 *
 *  ② Bootloader 从 8 KB 扩到 **16 KB**。
 *     实测：LARGE 模型下 BL 镜像 0xFF0000~0xFF1FC7，8 KB 槽只剩 57 字节。
 *     P1 还要加 ota_verify.c / ota_fsm.c（校验、搬运、回滚），8 KB 必爆。
 *
 *  ③ APP 入口随之下移到 **FF:4000h**，与 16 KB 的 BL 对齐。
 *
 *  ★ 附带修掉一个会"砖化"的隐患：`CODE(0xFF2000)` 写在 L251 的
 *    **Misc controls 里是无效的** —— CLASSES(...) 会整体替换类表，CODE 类不在
 *    表中就退回设备默认基址 0xFF0000。实测 APP 的启动代码因此一直落在
 *    0xFF0000~0xFF00E2，**正好压在 Bootloader 的向量表上**。
 *    正确写法是把 CODE 也写进 CLASSES：
 *        CLASSES(CODE(0xFF4000-0xFFFFFF), ECODE(0xFF4200-0xFFFFFF), ...)
 *    两个工程的 L251 配置见第 8 节。
 *
 *==============================================================================
 *  【单一分区表：IAP 平铺视图（0x00000 ~ 0x1FFFF）】
 *  ┌────────────────────┬───────────────┬────────────┬────────────────────┐
 *  │ 分区               │ IAP 地址      │ 物理       │ 大小 / 用途         │
 *  ├────────────────────┼───────────────┼────────────┼────────────────────┤
 *  │ STAGING 暂存区     │ 0x00000       │ FE:0000    │ 48 KB  下载落盘      │
 *  │ PARAM A            │ 0x0C000       │ FE:C000    │  2 KB  参数备份 A    │
 *  │ PARAM B            │ 0x0C800       │ FE:C800    │  2 KB  参数备份 B    │
 *  │ RESERVED           │ 0x0D000       │ FE:D000    │  4 KB  扩展位        │
 *  │ NCONST   ★只读     │ 0x0E000       │ FE:E000    │  8 KB  near 常量兜底 │
 *  │ BOOTLOADER ★只读   │ 0x10000       │ FF:0000    │ 16 KB  IAP 禁止写    │
 *  │ APP 运行区         │ 0x14000       │ FF:4000    │ 48 KB  固件          │
 *  └────────────────────┴───────────────┴────────────┴────────────────────┘
 *      0x0C000(48K 暂存) + 0x1000(4K 参数) + 0x1000(4K 预留)
 *    + 0x2000(8K 常量兜底) + 0x4000(16K BL) + 0xC000(48K APP)
 *    = 0x20000 ✔（正好 128 KB，无缝隙、无重叠）
 *
 *  ★ 暂存区必须 >= APP 区（见第 5 节断言），否则无法整包暂存 —— 两者都取 48 KB。
 *
 *  代码域地址 = IAP 地址 + 0xFE0000，两者用 OTA_IAP_TO_CODE / OTA_CODE_TO_IAP
 *  换算，**禁止手写魔数**：
 *      APP 入口 code = 0x14000 + 0xFE0000 = 0xFF4000 (FF:4000h)
 *
 *==============================================================================
 *  【STC-ISP 设置（两项必须与上表一致）】
 *      设置用户EEPROM大小 : 128 K      -> IAP 域 0x00000~0x1FFFF
 *      用户 ISP 区        : 不使用(0)   -> 本工程把 BL 当普通代码烧在 FF:0000，
 *                                        不用 STC 的「用户ISP程序」机制；
 *                                        OTA_USER_ISP_SIZE 仅表示"BL 占代码块前 16K"
 *  Keil 侧要设的是 L251 的 CLASSES（第 8 节），不是 STC-ISP 的"用户程序起始"。
 *============================================================================*/
#ifndef __OTA_PARTITION_H
#define __OTA_PARTITION_H

#include "ota_mcu.h"

/*==============================================================================
 * 0. 架构开关（探针结论落地处，只改这一段）
 *============================================================================*/
/* 1 = IAP 可触及 FF 代码块（EEPROM 配 128K）；0 = 只能到 FE 块（EEPROM 配 64K）
 * 默认 1：依据 STC-ISP V6.90D「设置用户EEPROM大小」下拉存在 128 K 档位，
 * 且 STC32G.H 里确实有 IAP_ADDRE(0xF6) 这个 bit16+ 地址寄存器（实测确认）。 */
#ifndef OTA_IAP_CAN_REACH_CODE
#define OTA_IAP_CAN_REACH_CODE      1
#endif

/* 1 = Bootloader 需把暂存区镜像搬运到 APP 区；0 = 暂存区即非活动运行区 */
#ifndef OTA_APPLY_IS_MOVE
#define OTA_APPLY_IS_MOVE           1
#endif

/*==============================================================================
 * 1. 地址换算（唯一允许的域转换入口，禁止手写 0xFE0000）
 *============================================================================*/
#define OTA_PHYS_BASE           0xFE0000UL      /* IAP 0 对应 FE:0000h        */
#define OTA_CODE_BANK_BASE      0xFF0000UL      /* 代码域 FF:0000h 块起始      */

/* IAP 地址 -> 代码域地址（用于跳转 / 入口 / 函数指针） */
#define OTA_IAP_TO_CODE(a)      ((a) + OTA_PHYS_BASE)
/* 代码域地址 -> IAP 地址（仅当目标落在 FF 块内且 EEPROM >= 128K 时有效） */
#define OTA_CODE_TO_IAP(a)      ((a) - OTA_PHYS_BASE)

/* FE 块与 FF 块的分界（IAP 视角） */
#define OTA_IAP_CODE_BLOCK_TOP  0x00010000UL

/*==============================================================================
 * 2. IAP 域低半区（FE 块）：暂存区 / 参数区 / 预留 / near 常量兜底
 *============================================================================*/
#define OTA_IAP_SECTOR_SIZE     0x00000200UL    /* 512 B，扇区擦除单位         */

/* 暂存区 48 KB */
#define OTA_STAGE_BASE          0x00000000UL
#define OTA_STAGE_SIZE          0x0000C000UL    /* 48 KB                       */
#define OTA_STAGE_END           (OTA_STAGE_BASE + OTA_STAGE_SIZE - 1UL)

/* 暂存区净可用容量：扣除镜像头预留（按 64 B 对齐） */
#define OTA_IMG_HDR_RESERVE     0x00000040UL
#define OTA_STAGE_PAYLOAD_MAX   (OTA_STAGE_SIZE - OTA_IMG_HDR_RESERVE)

/* 参数区（双备份，各 2 KB） */
#define OTA_PARAM_BASE          0x0000C000UL
#define OTA_PARAM_SLOT_SIZE     0x00000800UL    /* 每份 2 KB                   */
#define OTA_PARAM_A_BASE        0x0000C000UL
#define OTA_PARAM_B_BASE        0x0000C800UL
#define OTA_PARAM_SIZE          0x00001000UL    /* 共 4 KB                     */

/* 预留区（低半区尾部 4 KB，紧跟参数区；留给日志/计数的扩展位） */
#define OTA_RESERVED_BASE       0x0000D000UL
#define OTA_RESERVED_SIZE       0x00001000UL

/* near 常量兜底区（★ 只读，绝不可擦）：
 * 正常情况下用量为 0（LARGE 模型把字面量送进 HCONST/FF 块）。
 * 保留 8 KB 是为了防两件事：换回 TINY/SMALL 模型时字面量会掉回这里；
 * 将来某个文件写出 16 位寻址的常量。放在 0xE000 起，正好避开暂存区。 */
#define OTA_NCONST_BASE         0x0000E000UL
#define OTA_NCONST_BL_BASE      0x0000E000UL    /* BL 用 CLASSES 钉在 0xE000-0xEFFF */
#define OTA_NCONST_APP_BASE     0x0000F000UL    /* APP 用 CLASSES 钉在 0xF000-0xFFFF */
#define OTA_NCONST_SLOT_SIZE    0x00001000UL
#define OTA_NCONST_SIZE         0x00002000UL    /* 共 8 KB                     */

/*==============================================================================
 * 3. IAP 域高半区（FF 块 = 代码域，仅 OTA_IAP_CAN_REACH_CODE == 1 时可达）
 *============================================================================*/
#define OTA_USER_ISP_SIZE       0x00004000UL    /* 16 KB = BL 占用             */
#define OTA_APP_SIZE            0x0000C000UL    /* 48 KB = APP 占用            */

#if (OTA_IAP_CAN_REACH_CODE != 0)

/* EEPROM = 128 KB：IAP 域覆盖整片 Flash */
#define OTA_EEPROM_SIZE         0x00020000UL

/* Bootloader 在 IAP 域中的映射 —— ★ IAP 写保护必须拒绝这一段 */
#define OTA_BL_IAP_BASE         0x00010000UL
#define OTA_BL_IAP_END          (OTA_BL_IAP_BASE + OTA_USER_ISP_SIZE - 1UL)
#define OTA_BL_IS_READONLY      1

/* APP 区在 IAP 域中的映射 */
#define OTA_APP_IAP_BASE        (OTA_BL_IAP_BASE + OTA_USER_ISP_SIZE)   /* 0x14000 */
#define OTA_APP_IAP_SIZE        OTA_APP_SIZE
#define OTA_APP_IAP_END         (OTA_APP_IAP_BASE + OTA_APP_IAP_SIZE - 1UL)

#else   /* ------------------- 方案 (b)：EEPROM = 64 KB ------------------- */

/* EEPROM = 64 KB：IAP 域只到 FE 块，FF 块（BL/APP）不可经 IAP 写入 */
#define OTA_EEPROM_SIZE         0x00010000UL

#define OTA_BL_IAP_BASE         0xFFFFFFFFUL    /* 不可达 */
#define OTA_BL_IAP_END          0xFFFFFFFFUL
#define OTA_BL_IS_READONLY      1

#define OTA_APP_IAP_BASE        0xFFFFFFFFUL    /* 不可达 */
#define OTA_APP_IAP_SIZE        0UL
#define OTA_APP_IAP_END         0xFFFFFFFFUL

#endif

/*==============================================================================
 * 4. 代码域地址（只能用于跳转 / 入口 / 链接定位，禁止喂给 IAP）
 *============================================================================*/
/* 注意：这些宏会被 #if 预处理器求值，**不能包含 (u32) 之类强转**，
 * 否则报 "invalid integer const expression"。故一律写成纯常量表达式。 */
#define OTA_CODE_BANK_SIZE      0x00010000UL
#define OTA_BL_CODE_BASE        OTA_CODE_BANK_BASE                          /* FF:0000 */
#define OTA_APP_CODE_BASE       (OTA_CODE_BANK_BASE + OTA_USER_ISP_SIZE)    /* FF:4000 */
#define OTA_APP_CODE_END        (OTA_APP_CODE_BASE + OTA_APP_SIZE - 1UL)    /* FF:FFFF */

/*==============================================================================
 * 5. 编译期断言
 *============================================================================*/
#if (OTA_STAGE_BASE + OTA_STAGE_SIZE) != OTA_PARAM_BASE
#error "分区表错误: STAGING 与 PARAM 之间必须无空隙"
#endif
#if (OTA_PARAM_BASE + OTA_PARAM_SIZE) != OTA_RESERVED_BASE
#error "分区表错误: PARAM 与 RESERVED 之间必须无空隙"
#endif
#if (OTA_RESERVED_BASE + OTA_RESERVED_SIZE) != OTA_NCONST_BASE
#error "分区表错误: RESERVED 与 NCONST 兜底区之间必须无空隙"
#endif
#if (OTA_NCONST_BL_BASE + OTA_NCONST_SLOT_SIZE) != OTA_NCONST_APP_BASE
#error "分区表错误: BL 与 APP 的 NCONST 槽必须首尾相接"
#endif
#if (OTA_NCONST_BASE + OTA_NCONST_SIZE) != OTA_IAP_CODE_BLOCK_TOP
#error "分区表错误: 低半区必须正好填满 64KB(FE 块)，不得越到 0x10000"
#endif
#if (OTA_IAP_CAN_REACH_CODE != 0) && (OTA_EEPROM_SIZE != 0x00020000UL)
#error "架构不一致: 打开 OTA_IAP_CAN_REACH_CODE 时 EEPROM 必须配 128KB"
#endif
#if (OTA_IAP_CAN_REACH_CODE == 0) && (OTA_EEPROM_SIZE != 0x00010000UL)
#error "架构不一致: 关闭 OTA_IAP_CAN_REACH_CODE 时 EEPROM 必须配 64KB"
#endif
#if (OTA_IAP_CAN_REACH_CODE != 0)
  #if (OTA_BL_IAP_BASE + OTA_USER_ISP_SIZE) != OTA_APP_IAP_BASE
  #error "分区表错误: BL 与 APP 的 IAP 映射之间必须无空隙"
  #endif
  #if (OTA_APP_IAP_END + 1UL) != OTA_EEPROM_SIZE
  #error "分区表错误: APP 末尾必须正好顶到 128KB（0x20000）"
  #endif
#endif
#if (OTA_USER_ISP_SIZE + OTA_APP_SIZE) != OTA_CODE_BANK_SIZE
#error "分区表错误: 用户ISP区(BL) + APP 必须正好填满 64KB 代码块"
#endif
#if (OTA_APP_IAP_SIZE > 0UL) && (OTA_STAGE_SIZE < OTA_APP_IAP_SIZE)
#error "分区表错误: 暂存区必须 >= APP 区，否则无法整包暂存"
#endif
#if (OTA_APP_CODE_BASE != 0xFF4000UL)
#error "分区表错误: APP 代码域入口必须是 0xFF4000（FF:4000h）"
#endif
#if (OTA_IAP_TO_CODE(0x00014000UL) != 0xFF4000UL)
#error "分区表错误: OTA_IAP_TO_CODE 换算异常（应为 iap + 0xFE0000）"
#endif

/*==============================================================================
 * 6. 分区枚举与查询（全部针对 IAP 地址）
 *============================================================================*/
typedef enum
{
    PART_NONE       = 0,   /* 不属于任何分区                          */
    PART_STAGING    = 1,   /* 暂存区，可擦写                          */
    PART_PARAM      = 2,   /* 参数区，可擦写                          */
    PART_NCONST     = 3,   /* near 常量兜底区（8KB），★ IAP 只读       */
    PART_USER_ISP   = 4,   /* Bootloader 自身，★ IAP 只读，写入必拒    */
    PART_APP_IAP    = 5,   /* APP 区（IAP 视图），可擦写              */
    PART_RESERVED   = 6    /* 预留区（低半区 4KB），可擦写             */
} ota_part_t;

/**
 * @brief  返回 IAP 地址所属分区。
 * @param  iap_addr  IAP 地址（0x00000 ~ 0x1FFFF）。
 * @return ota_part_t；越界返回 PART_NONE；IAP 0x10000~0x13FFF 返回 PART_USER_ISP；
 *         IAP 0x0E000~0x0FFFF 返回 PART_NCONST（BL 与 APP 常量槽合并视作一区）。
 */
ota_part_t ota_part_of(u32 iap_addr);

/**
 * @brief  该分区是否允许 IAP 写入。
 * @param  part  分区。
 * @return 1 允许；0 拒绝（PART_USER_ISP / PART_NCONST / PART_NONE 恒为 0）。
 * @note   Bootloader 保护区与 near 常量区在这里一刀切死，任何调用方都绕不过去。
 */
u8 ota_part_writable(ota_part_t part);

/**
 * @brief  返回分区起始 IAP 地址。
 * @param  part  分区。
 * @param  ok    可选输出，1 表示分区有效；可为 NULL。
 * @return 起始 IAP 地址；无效返回 0。
 * @note   PART_PARAM 返回 A 槽起点；PART_NCONST 返回 0x0E000（BL 槽）。
 */
u32 ota_part_base(ota_part_t part, u8 *ok);

/**
 * @brief  返回分区长度（字节）。
 * @param  part  分区。
 * @param  ok    可选输出，1 表示分区有效；可为 NULL。
 * @return 长度；无效或不可达返回 0。
 * @note   PART_PARAM 返回单槽 2KB；PART_NCONST 返回 8KB（两块合计）。
 */
u32 ota_part_size(ota_part_t part, u8 *ok);

/**
 * @brief  区间是否完整落在指定分区内。
 * @param  part      分区。
 * @param  iap_addr  起始 IAP 地址。
 * @param  len       字节数。
 * @return 1 完整落在内部；0 否则。
 *
 * ★ v0.37：**已删除**（连同它唯一的调用方 flash_iap_check_range）。
 *   原因：1+4+4=9 B 形参必然生成 ?BYTE 参数块，而它的局部 `u8 i`
 *   正好排在自家块尾 ⇒ 偶发恒返回 0 ⇒ 上游一律 rc=2(ADDR)。
 *   需要这条判据时，就地写「首字节与末字节 ota_part_of() 相等」即可
 *   （分区在 IAP 空间里首尾相接，两者等价，且零调用）。
 */

/*==============================================================================
 * 7. 方案 (b) 降级说明（EEPROM 只能配 64K 时）
 *  ----------------------------------------------------------------------------
 *  · IAP 写不到 FF 块 -> OTA_APPLY_IS_MOVE 必须置 0；
 *  · APP 必须链接到 EEPROM 低半区运行，且划分为 A/B 双区，暂存区即非活动区：
 *        APP_A  : IAP 0x00000 ~ 0x05FFF (24 KB)
 *        APP_B  : IAP 0x06000 ~ 0x0BFFF (24 KB)
 *        PARAM  : IAP 0x0C000 ~ 0x0CFFF
 *        NCONST : IAP 0x0E000 ~ 0x0FFFF  (C251 16 位常量，仍然搬不走)
 *  · 代价：单包上限从 48 KB 降到 24 KB，BL 不再需要搬运逻辑（只切指针）。
 *  · 该布局与本文件第 2/3 节不兼容，需整段替换（届时再生成，不在当前
 *    分支上做无用功）。OTA_APPLY_IS_MOVE == 0 时 apply 只改启动指针。
 *============================================================================*/

/*==============================================================================
 * 8. 两个工程的 L251 配置（必须逐字一致，除了 CODE/ECODE/ZCONST 三处）
 *  ----------------------------------------------------------------------------
 *  BL 工程 L251 Misc controls:
 *      CLASSES(EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xE000-0xEFFF),
 *              HCONST(0xFF0100-0xFFFFFF), ECODE(0xFF0100-0xFFFFFF))
 *      （CODE 不写进 CLASSES -> 用设备默认 0xFF0000，正是 BL 要的）
 *
 *  APP 工程 L251 Misc controls:
 *      CLASSES(CODE(0xFF4000-0xFFFFFF),
 *              EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xF000-0xFFFF),
 *              HCONST(0xFF4200-0xFFFFFF), ECODE(0xFF4200-0xFFFFFF))
 *      ★ CODE(...) 必须写在 CLASSES **里面**，写在外面会被静默忽略，
 *        启动代码就跑到 0xFF0000 把 BL 盖掉了（v4 的实际事故）。
 *
 *  另外：L251 Locate 页的「Use Memory Layout from Target Dialog」必须**取消勾选**，
 *  否则它会自动再追加一条 CLASSES 覆盖手写配置；
 *  判断方法：Rebuild 后 .lnp 里 CLASSES 只能出现 1 次。
 *============================================================================*/

#endif /* __OTA_PARTITION_H */
