/*==============================================================================
 *  ota_partition.c  --  分区表查询（全部以 IAP 地址为输入）
 *  这里的数字全部来自 ota_partition.h 的宏，不允许在本文件出现第二个真源。
 *============================================================================*/
#include "ota_partition.h"

typedef struct
{
    u32 base;
    u32 size;
    u8  writable;      /* 1 = 允许 IAP 写入；0 = 只读（BL 保护区） */
    ota_part_t id;
} part_info_t;

/* ★ 不要加 `code` 修饰（2026-10-25 实测）
 *
 *   加了 code 之后编译器会生成 `?CO?OTA_PARTITION` 段，而 L251 把 CODE 类里的
 *   `?CO?`（常量）段排在 `?C_C51STARTUP` **前面**，于是：
 *      0xFF4000 = 分区表常量数据   ← BL 跳到 APP 入口会直接执行数据，必跑飞
 *      0xFF4058 = 真正的启动代码
 *   去掉 code 后常量进 HCONST(0xFF4200)，CODE 类只剩 start251 + startup，
 *   `?C_C51STARTUP` 正好落在 OTA_APP_CODE_BASE(0xFF4000)，
 *   于是"APP 入口 = 0xFF4000"这个约定才成立。
 *
 *   tools/check_layout.py 会对着 map 校验这一条，改坏了构建就报错。 */
static const part_info_t s_parts[] =
{
    { OTA_STAGE_BASE,      OTA_STAGE_SIZE,       1, PART_STAGING  },
    { OTA_PARAM_A_BASE,    OTA_PARAM_SLOT_SIZE,  1, PART_PARAM    },  /* A */
    { OTA_PARAM_B_BASE,    OTA_PARAM_SLOT_SIZE,  1, PART_PARAM    },  /* B */
    { OTA_RESERVED_BASE,   OTA_RESERVED_SIZE,    1, PART_RESERVED },
    { OTA_NCONST_BL_BASE,  OTA_NCONST_SLOT_SIZE, 0, PART_NCONST   },  /* ★ 只读 */
    { OTA_NCONST_APP_BASE, OTA_NCONST_SLOT_SIZE, 0, PART_NCONST   },  /* ★ 只读 */
#if (OTA_IAP_CAN_REACH_CODE != 0)
    { OTA_BL_IAP_BASE,     OTA_USER_ISP_SIZE,    0, PART_USER_ISP },  /* ★ 只读 */
    { OTA_APP_IAP_BASE,    OTA_APP_IAP_SIZE,     1, PART_APP_IAP  }
#endif
};

#define PART_CNT  (sizeof(s_parts) / sizeof(s_parts[0]))

/*==============================================================================
 * ★★★ v0.37：ota_part_of() 改成**纯常量比较**，从此不查表、不调用、不看形参 ★★★
 *
 *   v0.36 上板（日志行尾和值逐行核过）：
 *       [96] selftest: STAGING untested ...
 *       [265] [IAP] wr   g=4 p=0 a=c000      ← BL 写 param
 *       [0]   [IAP] wr   g=4 p=0 a=c000      ← APP 写 param
 *       [9470][IAP] erase g=4 p=0 a=14000    ← A2 擦 APP 首扇区
 *
 *   三处、两个工程、两个不同的地址（0x0C000 / 0x14000），**p 一律是 0
 *   （PART_NONE）**。而这两个地址静态看都该命中分区表（2 / 5），并且
 *       ① 同一份 hex 里 `ota_part_size(PART_STAGING/APP_IAP)` 打出 `stage=48K app=48K`
 *          —— 常量表本身、HCONST 寻址、下标计算全都对；
 *       ② v0.35 的擦除在同一个 0x14000 上是**过的**（`erase g=` 一行都没打）。
 *   ⇒ 表没问题、地址没问题，**出问题的是"把 u32 形参读第二遍"这件事**：
 *     原实现在循环里每轮读两次 `iap_addr`，而中间夹着从 HCONST 取
 *     `s_parts[i].base/.size` 的动作（`?C?LIB_CODE?` 里那几支 24 位取数helper）。
 *     参数一旦落在被 helper 不保存的寄存器里（铁律 11 的原型：R11/R12 不保存），
 *     第一轮之后 `iap_addr` 就是个天文数字 ⇒ 全部比较为假 ⇒ 恒返回 PART_NONE。
 *     v0.35→v0.36 只是布局挪了几十个字节，受害点就从别人换成了这里。
 *
 *   修法：分区表在 IAP 空间里是**首尾相接、无缝隙**的一维铺排（见 ota_partition.h
 *   的断言），所以"属于哪个分区"完全等价于"跟下一条分区的起点比大小"——
 *   一串 32 位立即数比较就够了，**零内存访问、零函数调用、零形参复读**。
 *   形参仍然按铁律 9 在第一条语句落地，此后只用 s_a。
 *   ★ 新增分区或改动边界时，s_parts[] 与本函数必须同改；两者不一致
 *     由「分区表断言 + 启动横幅 stage/app KB」间接守门。
 *============================================================================*/
ota_part_t ota_part_of(u32 iap_addr)
{
    static u32 s_a;                    /* 铁律 9/11：只读的临时值也必须落 static */

    s_a = iap_addr;

    if (s_a <  OTA_PARAM_BASE)     return PART_STAGING;   /* 0x00000-0x0BFFF 48K */
    if (s_a <  OTA_RESERVED_BASE)  return PART_PARAM;     /* 0x0C000-0x0CFFF  4K */
    if (s_a <  OTA_NCONST_BASE)    return PART_RESERVED;  /* 0x0D000-0x0DFFF  4K */
#if (OTA_IAP_CAN_REACH_CODE != 0)
    if (s_a <  OTA_BL_IAP_BASE)    return PART_NCONST;    /* 0x0E000-0x0FFFF  8K */
    if (s_a <  OTA_APP_IAP_BASE)   return PART_USER_ISP;  /* 0x10000-0x13FFF 16K */
    if (s_a <  OTA_EEPROM_SIZE)    return PART_APP_IAP;   /* 0x14000-0x1FFFF 48K */
#else
    if (s_a <  OTA_IAP_CODE_BLOCK_TOP) return PART_NCONST;/* 0x0E000-0x0FFFF  8K */
#endif
    return PART_NONE;
}

u8 ota_part_writable(ota_part_t part)
{
    static u8 s_p;                     /* 同上：形参落地后再也不读它 */

    s_p = (u8)part;

    /* 可写集合就是这四个；PART_NONE / PART_NCONST / PART_USER_ISP
     * 在这里一刀切死，任何调用方都绕不过去（与 s_parts[] 的 writable 字段同义）。 */
    if (s_p == PART_STAGING)  return 1;
    if (s_p == PART_PARAM)    return 1;
    if (s_p == PART_RESERVED) return 1;
    if (s_p == PART_APP_IAP)  return 1;
    return 0;
}

/* 同一 id 可能有多段（参数 A/B），这里返回第一个匹配段的起始 */
u32 ota_part_base(ota_part_t part, u8 *ok)
{
    u8 i;
    for (i = 0; i < PART_CNT; i++)
    {
        if (s_parts[i].id == part)
        {
            if (ok) *ok = 1;
            return s_parts[i].base;
        }
    }
    if (ok) *ok = 0;
    return 0;
}

u32 ota_part_size(ota_part_t part, u8 *ok)
{
    u8 i;
    for (i = 0; i < PART_CNT; i++)
    {
        if (s_parts[i].id == part)
        {
            if (ok) *ok = 1;
            return s_parts[i].size;
        }
    }
    if (ok) *ok = 0;
    return 0;
}

/*==============================================================================
 * ★★★ v0.37：ota_part_contains() **整段删除** ★★★
 *
 *   v0.36 的 map 实证它自己被自己的局部变量踩死（`?ota_part_contains??BYTE`
 *   = 0x1DCE 需 9 B，而局部 `u8 i` 正在 0x1DD4 —— L251 给形参块只按 6 B 排），
 *   于是 `i = 0` 把形参块尾清掉 ⇒ 返回 0 ⇒ 上游判"跨分区"⇒ OTA_ERR_ADDR(2)。
 *   这就是 v0.34/v0.35 两次 `copy fail off=0 rc=2` 和 `erase app fail rc=2` 的真身。
 *
 *   它唯一的调用方 flash_iap_check_range() 已在 v0.36 删除（铁律 14），
 *   三处判据各自内联；erase/write 的"整段不跨分区"改用
 *   「首字节与末字节 ota_part_of() 相等」判（分区是一维铺排，等价且零调用）。
 *   ⇒ 现在全工程零调用，留着只会让人下次又顺手调它，所以删掉。
 *   需要"某段是否完整落在某分区"时，把那两条判据就地写出来。
 *============================================================================*/
