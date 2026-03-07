/*
 * kernel/ring_buffer.h — Fixed-size 256-byte circular byte buffer.
 *
 * Uses uint8_t head and tail indices into a 256-byte array.  Natural
 * uint8_t overflow provides free modulo-256 wrapping with no branches.
 *
 * Full condition:  (uint8_t)(rb->tail + 1) == rb->head
 * Empty condition: rb->head == rb->tail
 *
 * Thread safety: safe for a single-producer / single-consumer pair on
 * one CPU (keyboard IRQ writes, main loop reads) without locks.
 */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stdbool.h>

#define RING_BUFFER_SIZE 256

typedef struct {
    uint8_t buf[RING_BUFFER_SIZE];
    uint8_t head;   /* read index  */
    uint8_t tail;   /* write index */
} ring_buffer_t;

/* Write one byte.  Returns false if the buffer is full (byte is dropped). */
bool ring_buffer_write(ring_buffer_t *rb, uint8_t data);

/* Read one byte into *data.  Returns false if the buffer is empty. */
bool ring_buffer_read(ring_buffer_t *rb, uint8_t *data);

/* True if no bytes are available to read. */
bool ring_buffer_is_empty(const ring_buffer_t *rb);

#endif /* RING_BUFFER_H */
