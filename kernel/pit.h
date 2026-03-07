/*
 * kernel/pit.h — 8253/8254 Programmable Interval Timer.
 *
 * Programs PIT channel 0 to fire IRQ 0 at 1000 Hz (1 ms per tick),
 * giving a monotonic millisecond counter via pit_get_ticks().
 */

#ifndef PIT_H
#define PIT_H

#include <stdint.h>

/* Configure PIT at 1000 Hz and register the IRQ 0 handler. */
void pit_init(void);

/* Return the number of timer ticks since pit_init() (≈ milliseconds). */
uint64_t pit_get_ticks(void);

/*
 * Busy-wait for at least ms milliseconds.
 * Uses pit_get_ticks(); requires interrupts to be enabled (STI).
 */
void pit_sleep_ms(uint64_t ms);

#endif /* PIT_H */
