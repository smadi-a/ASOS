/*
 * kernel/ring_buffer.c — Circular byte buffer implementation.
 */

#include "ring_buffer.h"

bool ring_buffer_write(ring_buffer_t *rb, uint8_t data)
{
    uint8_t next_tail = rb->tail + 1;   /* wraps at 256 automatically */
    if (next_tail == rb->head)
        return false;   /* full */
    rb->buf[rb->tail] = data;
    rb->tail = next_tail;
    return true;
}

bool ring_buffer_read(ring_buffer_t *rb, uint8_t *data)
{
    if (rb->head == rb->tail)
        return false;   /* empty */
    *data = rb->buf[rb->head];
    rb->head = rb->head + 1;   /* wraps at 256 automatically */
    return true;
}

bool ring_buffer_is_empty(const ring_buffer_t *rb)
{
    return rb->head == rb->tail;
}
