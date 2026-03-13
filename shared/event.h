/*
 * shared/event.h — Event structure shared between kernel and user space.
 *
 * Used by the per-process event queue and SYS_GET_EVENT syscall.
 */

#ifndef SHARED_EVENT_H
#define SHARED_EVENT_H

#include <stdint.h>

/* Event types. */
#define EVENT_NONE        0
#define EVENT_KEY_PRESS   1   /* code = ASCII character               */
#define EVENT_KEY_RELEASE 2   /* code = ASCII character               */
#define EVENT_MOUSE_MOVE  3   /* x, y = absolute cursor position      */
#define EVENT_MOUSE_DOWN  4   /* x, y = position; code = button mask  */
#define EVENT_MOUSE_UP    5   /* x, y = position; code = button mask  */

typedef struct {
    uint8_t  type;    /* EVENT_* constant                     */
    uint8_t  _pad;    /* Alignment padding                    */
    int16_t  x;       /* Mouse X position (or 0 for keyboard) */
    int16_t  y;       /* Mouse Y position (or 0 for keyboard) */
    uint16_t code;    /* Key character or mouse button mask    */
} event_t;

#endif /* SHARED_EVENT_H */
