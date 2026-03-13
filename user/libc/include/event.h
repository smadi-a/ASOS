/*
 * event.h — User-space event types and get_event() wrapper.
 *
 * Mirrors shared/event.h so user programs can include it without needing
 * to reference kernel-internal paths.
 */

#ifndef _EVENT_H
#define _EVENT_H

#include <stdint.h>
#include <sys/syscall.h>

/* Event types — must match shared/event.h */
#define EVENT_NONE        0
#define EVENT_KEY_PRESS   1
#define EVENT_KEY_RELEASE 2
#define EVENT_MOUSE_MOVE  3
#define EVENT_MOUSE_DOWN  4
#define EVENT_MOUSE_UP    5

typedef struct {
    uint8_t  type;    /* EVENT_* constant                     */
    uint8_t  _pad;
    int16_t  x;       /* Mouse X position (or 0 for keyboard) */
    int16_t  y;       /* Mouse Y position (or 0 for keyboard) */
    uint16_t code;    /* Key character or mouse button mask    */
} event_t;

/*
 * get_event — Non-blocking: pop one event from the process event queue.
 * Returns 0 if an event was written to *out, -1 if no events pending.
 */
static inline int get_event(event_t *out)
{
    return (int)__syscall1(SYS_GET_EVENT, (uint64_t)(uintptr_t)out);
}

#endif /* _EVENT_H */
