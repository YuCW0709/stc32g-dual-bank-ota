/*==============================================================================
 *  ringbuf.c  --  环形缓冲实现（无锁 SPSC）
 *============================================================================*/
#include "ringbuf.h"

u8 ring_init(ring_t *r, u8 *mem, u16 size)
{
    if (r == 0 || mem == 0 || size < 2) return 0;
    if ((size & (size - 1)) != 0) return 0;          /* 非 2 的幂 */
    r->buf  = mem;
    r->size = size;
    r->head = 0;
    r->tail = 0;
    r->ovf  = 0;
    return 1;
}

u16 ring_avail(ring_t *r)
{
    return (u16)(r->head - r->tail);
}

u8 ring_push(ring_t *r, u8 dat)
{
    u16 head = r->head;
    if ((u16)(head - r->tail) >= r->size) {          /* 满 */
        r->ovf++;
        return 0;
    }
    r->buf[head & (r->size - 1)] = dat;
    r->head = (u16)(head + 1);
    return 1;
}

u8 ring_pop(ring_t *r, u8 *p)
{
    u16 tail = r->tail;
    if (tail == r->head) return 0;                   /* 空 */
    *p = r->buf[tail & (r->size - 1)];
    r->tail = (u16)(tail + 1);
    return 1;
}

u8 ring_peek(ring_t *r, u16 off)
{
    u16 tail = r->tail;
    if ((u16)(off) >= (u16)(r->head - tail)) return 0xFF;
    return r->buf[(u16)(tail + off) & (r->size - 1)];
}

u16 ring_pop_n(ring_t *r, u8 *dst, u16 max)
{
    u16 xdata n = 0;
    while (n < max)
    {
        if (!ring_pop(r, &dst[n])) break;
        n++;
    }
    return n;
}

u16 ring_flush(ring_t *r)
{
    u16 lost = ring_avail(r);
    r->tail = r->head;
    return lost;
}
