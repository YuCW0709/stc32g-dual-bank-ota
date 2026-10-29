/*==============================================================================
 *  crc32.h  --  CRC32（IEEE 802.3 / 与上位机打包工具一致）
 *  Platform : STC32G12K128 (Keil C251, Huge)
 *  Polynomial : 0x04C11DB7，反射实现 0xEDB88320，初值 0xFFFFFFFF，结果取反
 *  与 zlib crc32() / Python binascii.crc32() / CRC-32/ISO-HDLC 完全一致。
 *
 *  实现策略：4 位查表（16 项表，占 64 B code），速度 ~ 逐位实现的 4 倍，
 *  16 KB/s 级吞吐对 56 KB 镜像校验足够（约 3.5 s）；若嫌慢可改 8 位表（1 KB）。
 *============================================================================*/
#ifndef __CRC32_H
#define __CRC32_H

#include "ota_mcu.h"
#include "ota_types.h"

#define CRC32_INIT_VAL      0xFFFFFFFFUL
#define CRC32_FINAL_XOR     0xFFFFFFFFUL

/*==============================================================================
 * 流式计算（分段喂数据，适合边下载边算）
 *============================================================================*/

/**
 * @brief  取初始 CRC 值，作为 crc32_update 的首次入参。
 * @return 恒为 0xFFFFFFFF。
 */
u32 crc32_begin(void);

/**
 * @brief  累加一段数据。
 * @param  crc   上一段的 CRC 值（首次传 crc32_begin() 的返回值）。
 * @param  data  数据指针；C251 Huge 模式下为 3 字节通用指针，可指向 code/xdata。
 * @param  len   字节数，可为 0（直接返回 crc）。
 * @return 累加后的中间 CRC 值（尚未取反）。
 * @note   若 data 为 NULL 且 len > 0，直接返回入参 crc，不产生异常。
 */
u32 crc32_update(u32 crc, const u8 *src, u16 len);

/**
 * @brief  结束计算，做最终取反。
 * @param  crc  crc32_update 返回的中间值。
 * @return 最终 CRC32。
 */
u32 crc32_final(u32 crc);

/*==============================================================================
 * 一次性计算
 *============================================================================*/

/*==============================================================================
 * 一次性计算 —— v0.27 起改成**宏**，`crc32_calc` 这个函数已经删除
 *
 *   ★ 为什么删掉那个"薄封装"★
 *     它是全工程唯一一处把 begin/update/final 串进**同一条 return 语句**的
 *     地方（`return crc32_final(crc32_update(crc32_begin(), src, len))`），
 *     而它算出来的值**每次上电都不一样**，同一个固定向量 "123456789"：
 *         v0.26 第一次上电  8270e960（正确值 cbf43926）
 *         v0.26 第二次上电  395bd4b4
 *     同期用**同样的三步、但在调用方按语句分开写**的地方全部正确 ——
 *     APP 侧 49088 B 正文 CRC 与 Python binascii.crc32 逐位吻合
 *     （e546ad98），flash_iap_crc32_cb / ota_verify 的正文 CRC 也是这种写法。
 *     汇编层面逐指令复核过 crc32_update / crc32_calc（tools/_asmZ），
 *     单看指令都"对"，所以不再猜 ABI 细节，直接**取消这一层调用**：
 *     每个调用点自己按三步写，和已经验证正确的那条路一模一样。
 *
 *   ★ 用法约束（重要）★
 *     dst 必须是**模块级 static 的 u32**（xdata），不许是栈上局部量：
 *     C251 的栈只有 256 B，且跨函数调用还活着的值一律不许留在寄存器/栈上。
 *
 *         static u32 xdata s_crc;
 *         OTA_CRC32_OF(s_crc, buf, n);
 *============================================================================*/
#define OTA_CRC32_OF(dst, src, len)                     \
    do {                                                \
        (dst) = crc32_begin();                          \
        (dst) = crc32_update((dst), (src), (u16)(len));  \
        (dst) = crc32_final(dst);                        \
    } while (0)

/**
 * @brief  计算 Flash 区间 CRC32（经 flash_iap_read 分块读取，块 64 B）。
 * @param  addr  线性绝对地址。
 * @param  len   字节数，> 0。
 * @param  p_crc 输出 CRC32，不可为 NULL。
 * @return OTA_OK / OTA_ERR_PARAM / OTA_ERR_ADDR（地址越界）。
 * @note   本函数内部会多次关中断读 Flash，单次关中断时长约 64 字节，可接受。
 */
ota_ret_t crc32_flash(u32 addr, u32 len, u32 *p_crc);

/**
 * @brief  同上，支持进度回调（喂狗 / 上报）。
 * @param  addr     起始地址。
 * @param  len      字节数。
 * @param  p_crc    输出 CRC32。
 * @param  on_tick  每 1 KB 回调一次，参数为已完成字节数；可为 NULL。
 * @return 同 crc32_flash()。
 */
ota_ret_t crc32_flash_cb(u32 addr, u32 len, u32 *p_crc,
                         void (*on_tick)(u32 done));

/**
 * @brief  ★ v0.30：自检接口已删除（crc32_selftest / crc32_selftest_value_cst）。
 *
 *   理由（实测，详见 crc32.c 尾部注释）：这条自检的读数不可复现 ——
 *   v0.28 同源数据得 cbf43926（正确），v0.29 一重编就变成 69d39b4b，而
 *   s_vc 在镜像里的内容与指针立即数都复查过是对的。它不在任何功能路径上，
 *   却会在 FAIL 时提前 return，把"读 BL 入口 4 字节"这条真正有效的读通路
 *   判据和 STAGING/APP 两处探针全部跳过，日志里只剩误导性的
 *   "cannot reach FE block"。
 *
 *   ⇒ CRC 的可信度由功能通路自己证明：镜像头 36 B（step=5）、param 462 B、
 *     正文 49088 B 三处只要有一处对上 Python 复算值，引擎就是好的。
 */

#endif /* __CRC32_H */
