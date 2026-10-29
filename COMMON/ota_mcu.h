/*==============================================================================
 *  ota_mcu.h  --  MCU 适配层：唯一允许 include "stc32g.h" 的地方
 *  Platform : STC32G12K128 (Keil C251, CodeRomSize = Huge, Memory Model = LARGE)
 *
 *  为什么需要这一层：
 *    Keil 自带 / STC-ISP 导入的 stc32g.h 版本不一，**并不保证定义 u8/u16/u32**
 *    （实测某些版本只有 unsigned char/int/long 与 BYTE/WORD）。工程里到处写
 *    u8 就会满屏 "missing declaration specifiers"。所以这里统一补齐。
 *
 *  若将来换成一个"自带 u8/u16/u32"的 stc32g.h，会出现 typedef 重定义错误，
 *  此时在工程 Options → C251 → Define 里加 OTA_HAS_STD_TYPES 即可跳过本段。
 *
 *  位宽依据（C251，实测 V5.60）：
 *      char = 8bit, int = 16bit, long = 32bit
 *      裸指针 `u8 *` 的宽度**取决于内存模型**，见下面的门禁
 *============================================================================*/
#ifndef __OTA_MCU_H
#define __OTA_MCU_H

#include "stc32g.h"

#ifndef OTA_HAS_STD_TYPES
typedef unsigned char   u8;
typedef signed   char   s8;
typedef unsigned int    u16;
typedef signed   int    s16;
typedef unsigned long   u32;
typedef signed   long   s32;
#endif

/* 位操作小工具（避免到处写 (1<<n) 的强制转换） */
#define BIT(n)          ((u8)(1u << (n)))
#define SET_BIT(r, n)   ((r) |=  BIT(n))
#define CLR_BIT(r, n)   ((r) &= (u8)~BIT(n))

/*==============================================================================
 *  ★★★ 指针模型门禁 —— 这两段绝对不要删，删掉就是"带病运行" ★★★
 *
 *  C251 里裸指针 `u8 *` 的宽度随内存模型变（本机 C251 V5.60 实测）：
 *
 *      模型            sizeof(u8 *)   能指向的空间
 *      XSMALL / TINY        2          EDATA 0x0000~0x0FFF（仅 4KB）
 *      SMALL  / LARGE       4          通用指针：data / edata / xdata / code 全部
 *
 *  本工程所有缓冲区参数都写成裸 `u8 *`（flash_iap_read 的 buf、ring_init 的
 *  buf、log_printf 的 tag ……），而缓冲区实际全在 **xdata**（物理基址 0x010000）。
 *  一旦用了 2 字节模型，编译器只给一句
 *          warning C150: 'xdata' pointer converted to 'edata' pointer
 *  的**警告**，然后把高 8 位直接丢掉 —— 函数会去 EDATA 里读写一个完全无关的
 *  地址，**数据静默损坏，程序看着还在跑**。数据量小于 256 字节时甚至"看不出
 *  问题"，等镜像一大才暴露，是最难定位的一类 bug。
 *
 *  所以把"必须是通用指针"做成编译期断言，两道：
 *    ① 宏门禁：漏配 Misc controls 时给人话提示（Define 里要有 OTA_PTR_GENERIC）
 *    ② 事实门禁：直接量 sizeof，模型真不对就编不过（编不过好过跑错）
 *
 *  正确配法见 docs/01_编译与烧录步骤.md §5.6：
 *    Options → C251 → Misc controls 填 `LARGE`（不要动 Memory Model 下拉框）
 *    两个工程（BL / APP）必须完全一致，否则跨工程传指针必崩。
 *============================================================================*/
#ifndef OTA_PTR_GENERIC
  #error "C251 内存模型必须是 LARGE：Options -> C251 -> Misc controls 填 LARGE，Define 加 OTA_PTR_GENERIC=1"
#endif

typedef char ota_ptr_must_be_generic_4byte[(sizeof(u8 *) >= 4) ? 1 : -1];

#endif /* __OTA_MCU_H */
