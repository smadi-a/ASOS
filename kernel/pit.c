/*
 * kernel/pit.c — PIT timer at 1000 Hz.
 *
 * PIT channel 0, mode 3 (square wave generator):
 *   Base clock:  1 193 182 Hz (≈ 1.19 MHz)
 *   Divisor:     1193  →  ~1000.15 Hz (close enough for ms-precision)
 *   Command:     0x36  (channel 0, lo/hi byte access, mode 3, binary)
 */

#include "pit.h"
#include "pic.h"
#include "isr.h"
#include "io.h"
#include "serial.h"
#include "scheduler.h"
#include <stdint.h>

#define PIT_CMD    0x43   /* command register (write-only)   */
#define PIT_CH0    0x40   /* channel 0 data port             */

#define PIT_FREQ   1193182UL
#define PIT_HZ     1000UL
#define PIT_DIV    (PIT_FREQ / PIT_HZ)   /* 1193 */

static volatile uint64_t g_ticks = 0;

static void pit_irq_handler(InterruptFrame *frame)
{
    g_ticks++;
    scheduler_tick(frame);
    /* EOI is sent by isr_handler() after this callback returns. */
}

void pit_init(void)
{
    /* Mode 3 (square wave), channel 0, lo+hi byte access. */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)(PIT_DIV & 0xFF));         /* low byte  */
    outb(PIT_CH0, (uint8_t)((PIT_DIV >> 8) & 0xFF));  /* high byte */

    isr_register_handler(32, pit_irq_handler);   /* IRQ 0 = vector 32 */
    pic_unmask_irq(0);

    serial_puts("[PIT] Timer running at 1000 Hz.\n");
}

uint64_t pit_get_ticks(void)
{
    return g_ticks;
}

void pit_sleep_ms(uint64_t ms)
{
    uint64_t target = g_ticks + ms;
    while (g_ticks < target)
        __asm__ volatile ("hlt");
}
