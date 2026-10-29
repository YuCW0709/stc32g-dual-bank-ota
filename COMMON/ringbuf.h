/*==============================================================================
 *  ringbuf.h  --  通用单生产者/单消费者环形缓冲
 *  Platform : STC32G12K128 (Keil C251)
 *  Note     : 无锁实现，head 只由写方(ISR)改，tail 只由读方(主循环)改。
 *             长度必须为 2 的幂，用掩码取模。
 *============================================================================*/
#ifndef __RINGBUF_H
#define __RINGBUF_H

#include "ota_mcu.h"

typedef struct
{
    u8 *buf;       /* 存储区，长度必须为 2 的幂                        */
    u16  size;            /* == RING_SIZE，掩码 = size-1                      */
    volatile u16 head;    /* 写指针，仅生产者修改                             */
    volatile u16 tail;    /* 读指针，仅消费者修改                             */
    volatile u16 ovf;     /* 因满而丢弃的字节累计                             */
} ring_t;

/* 初始化；size 必须为 2 的幂，否则返回 OTA 风格错误码由调用方判断 */
u8   ring_init(ring_t *r, u8 *mem, u16 size);
/* 写入 1 字节（生产者）。返回 1 成功，0 满 */
u8   ring_push(ring_t *r, u8 dat);
/* 读出 1 字节（消费者）。返回 1 成功，0 空 */
u8   ring_pop(ring_t *r, u8 *p);
/* 预读第 off 个待读字节，不出队；越界返回 0xFF */
u8   ring_peek(ring_t *r, u16 off);
/* 批量读，返回实际字节数 */
u16  ring_pop_n(ring_t *r, u8 *dst, u16 max);
/* 当前可读字节数 */
u16  ring_avail(ring_t *r);
/* 清空并返回丢弃字节数 */
u16  ring_flush(ring_t *r);

#endif /* __RINGBUF_H */
