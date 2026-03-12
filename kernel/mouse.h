/*
 * kernel/mouse.h — PS/2 mouse driver interface.
 *
 * Handles standard 3-byte PS/2 mouse packets (no scroll wheel).
 * Mouse events are stored in a ring buffer for polling by the kernel.
 * The IRQ 12 handler is registered inside mouse_init().
 */

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t  dx;       /* X delta (positive = right) */
    int32_t  dy;       /* Y delta (positive = up, PS/2 convention) */
    uint8_t  buttons;  /* bit 0 = left, bit 1 = right, bit 2 = middle */
} mouse_event_t;

/* Initialise the PS/2 mouse and register the IRQ 12 handler. */
void mouse_init(void);

/*
 * Non-blocking read: retrieve one mouse event from the input buffer.
 * Returns false if no event is waiting.
 */
bool mouse_read_event(mouse_event_t *evt);

/* True if at least one event is available to read. */
bool mouse_has_event(void);

#endif /* MOUSE_H */
