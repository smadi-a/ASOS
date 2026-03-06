/*
 * kernel/main.c — ASOS kernel entry point.
 *
 * Milestone 2 additions
 * ─────────────────────
 *   • gdt_init()  — replaces the UEFI GDT with our own; installs TSS
 *   • idt_init()  — wires all 256 vectors to our assembly stubs
 *   • Test fault  — divides by zero to verify the exception pipeline
 */

#include <stdint.h>

#include "../shared/boot_info.h"
#include "serial.h"
#include "framebuffer.h"
#include "gdt.h"
#include "idt.h"

#define ASOS_VERSION "ASOS v0.1.0"

void kernel_main(BootInfo *info)
{
    /* ── Serial ── */
    serial_init();
    serial_puts(ASOS_VERSION "\n");

    /* ── Framebuffer ── */
    fb_init(&info->framebuffer);
    fb_clear(COLOR_BLACK);
    fb_puts_at(ASOS_VERSION, 2, 0, COLOR_WHITE, COLOR_BLACK);

    /* ── GDT ── */
    gdt_init();
    serial_puts("[OK] GDT loaded\n");
    fb_puts_at("[OK] GDT loaded", 2, 2, COLOR_GREEN, COLOR_BLACK);

    /* ── IDT ── */
    idt_init();
    serial_puts("[OK] IDT loaded\n");
    fb_puts_at("[OK] IDT loaded", 2, 3, COLOR_GREEN, COLOR_BLACK);

    /* ── Test fault: division by zero ──
     *
     * volatile prevents the compiler from constant-folding the division.
     * This should fire vector 0 (#DE), invoke our ISR stub, and display
     * the exception screen.  Remove this block in Milestone 3.
     */
    serial_puts("Triggering #DE (div by zero) — expect exception screen\n");
    fb_puts_at("Triggering #DE...", 2, 5, COLOR_YELLOW, COLOR_BLACK);

    volatile uint64_t zero   = 0;
    volatile uint64_t result = 1 / zero;
    (void)result; /* suppress unused-variable warning */

    /* Should never reach here. */
    for (;;)
        __asm__ volatile ("hlt");
}
