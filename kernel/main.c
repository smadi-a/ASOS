/*
 * kernel/main.c — ASOS kernel entry point (Milestone 4).
 *
 * Boot sequence
 * ─────────────
 *  1.  Clear BSS.
 *  2.  serial_init()
 *  3.  fb_init() + fb_clear()
 *  4.  gdt_init()
 *  5.  idt_init()
 *  6.  pmm_init()
 *  7.  Switch RSP to kernel BSS stack.
 *  8.  vmm_init()
 *  9.  heap_init()
 * 10.  pic_init()   — remap IRQs, mask all lines
 * 11.  pit_init()   — 1000 Hz timer, unmask IRQ 0
 * 12.  keyboard_init() — PS/2 keyboard, unmask IRQ 1
 * 13.  sti           — enable interrupts
 * 14.  Keyboard echo demo with uptime reporting
 */

#include <stdint.h>
#include <stddef.h>

#include "../shared/boot_info.h"
#include "serial.h"
#include "framebuffer.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "string.h"

#define ASOS_VERSION "ASOS v0.1.0"

extern char __bss_start[];
extern char __bss_end[];

#define KSTACK_SIZE  16384
static uint8_t g_kstack[KSTACK_SIZE] __attribute__((aligned(16)));

static void clear_bss(void)
{
    volatile char *p = __bss_start;
    while (p < __bss_end) *p++ = 0;
}

/* ── Simple decimal-to-string for uptime display ──────────────────────── */

static void serial_put_dec(uint64_t v)
{
    if (v == 0) { serial_putc('0'); return; }
    char tmp[20];
    int i = 0;
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i--) serial_putc(tmp[i]);
}

/* ── kernel_main ──────────────────────────────────────────────────────────*/

void kernel_main(BootInfo *info)
{
    clear_bss();

    serial_init();
    serial_puts(ASOS_VERSION "\n");

    fb_init(&info->framebuffer);
    fb_clear(COLOR_BLACK);
    fb_set_cursor(0, 0);

    gdt_init();
    serial_puts("[OK] GDT\n");

    idt_init();
    serial_puts("[OK] IDT\n");

    pmm_init(info);
    serial_puts("[OK] PMM\n");

    __asm__ volatile ("mov %0, %%rsp"
        :: "r"((uint64_t)(uintptr_t)(g_kstack + KSTACK_SIZE)) : "memory");

    vmm_init();
    serial_puts("[OK] VMM\n");

    heap_init();
    serial_puts("[OK] Heap\n");

    pic_init();
    pit_init();
    keyboard_init();

    /* Enable hardware interrupts — must come AFTER PIC init and all
     * handler registrations.  Before this point any IRQ would be
     * interpreted as a CPU exception. */
    __asm__ volatile ("sti" ::: "memory");
    serial_puts("[OK] Interrupts enabled.\n");

    /* ── Banner ───────────────────────────────────────────────────────── */
    fb_puts(ASOS_VERSION " — Keyboard active. Type something:\n",
            COLOR_WHITE, COLOR_BLACK);
    serial_puts(ASOS_VERSION " — Keyboard active. Type something:\n");

    /* ── Main event loop ─────────────────────────────────────────────── */
    uint64_t last_uptime_s = 0;

    for (;;) {
        /* Sleep until the next interrupt (timer or keyboard). */
        __asm__ volatile ("hlt" ::: "memory");

        /* ── Uptime reporting (serial only, once per second) ── */
        uint64_t ticks = pit_get_ticks();
        uint64_t now_s = ticks / 1000;
        if (now_s != last_uptime_s) {
            last_uptime_s = now_s;
            serial_puts("[TIMER] Uptime: ");
            serial_put_dec(now_s);
            serial_puts("s\n");
        }

        /* ── Drain keyboard ring buffer ── */
        char c;
        while (keyboard_read_char(&c)) {
            /* Echo to serial. */
            if (c == '\b') {
                serial_putc('\b');
                serial_putc(' ');
                serial_putc('\b');
            } else {
                serial_putc(c);
            }

            /* Echo to framebuffer via cursor-based terminal. */
            char str[2] = { c, '\0' };
            fb_puts(str, COLOR_WHITE, COLOR_BLACK);
        }
    }
}
