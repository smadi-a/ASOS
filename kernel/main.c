/*
 * kernel/main.c — ASOS kernel entry point (Milestone 1: boot and print).
 *
 * Execution context when kernel_main() is called
 * ───────────────────────────────────────────────
 *   • CPU is in 64-bit long mode (set up by UEFI before ExitBootServices).
 *   • The GDT and IDT are still the firmware's — we will replace them in a
 *     later milestone.
 *   • Interrupts are disabled (IF=0).
 *   • A valid stack is present (the firmware allocated one for us;
 *     we inherit it since we never called FreePages on it).
 *   • Boot services are gone — no UEFI calls are possible.
 *   • Physical memory is identity-mapped (UEFI page tables are still active).
 *
 * Milestone 1 goals
 * ─────────────────
 *   1. Initialise COM1 serial port and print a banner.
 *   2. Initialise the GOP framebuffer and clear the screen.
 *   3. Render "ASOS v0.1.0" to the framebuffer using the bitmap font.
 *   4. Spin in an hlt loop forever (no scheduler yet).
 *
 * Compiler flags (see Makefile):
 *   -ffreestanding  — no hosted-environment assumptions
 *   -nostdlib       — no standard library linked
 *   -mno-red-zone   — no 128-byte red zone below RSP (needed in kernel mode)
 *   -mcmodel=kernel — kernel code model (>2 GiB virtual addresses later)
 */

#include <stdint.h>

#include "../shared/boot_info.h"
#include "serial.h"
#include "framebuffer.h"

/* Version string — single source of truth. */
#define ASOS_VERSION "ASOS v0.1.0"

/*
 * kernel_main — the C entry point.
 *
 * The linker script sets this as the ELF entry point so the bootloader
 * jumps here directly after ExitBootServices().
 *
 * @param info  Pointer to the BootInfo struct filled by the bootloader.
 *              The struct lives in EfiLoaderData pages that remain mapped
 *              forever (we never freed them).
 */
void kernel_main(BootInfo *info)
{
    /* ── Step 1: Serial port ───────────────────────────────────────────
     *
     * Initialise COM1 first so we have a debug output channel even if
     * the framebuffer setup fails.
     */
    serial_init();
    serial_puts("\r\n" ASOS_VERSION "\r\n");
    serial_puts("Serial: OK\r\n");

    /* ── Step 2: Framebuffer ───────────────────────────────────────────
     *
     * Bind the framebuffer module to the GOP descriptor that the
     * bootloader placed in BootInfo.
     */
    fb_init(&info->framebuffer);
    serial_puts("Framebuffer: initialised\r\n");

    /* ── Step 3: Clear screen ──────────────────────────────────────────
     *
     * Write black (0x00000000) to every pixel.  In both RGB and BGR
     * mode, black is 0x000000, so no colour-swap is needed.
     */
    fb_clear(COLOR_BLACK);
    serial_puts("Framebuffer: cleared\r\n");

    /* ── Step 4: Render version banner ────────────────────────────────
     *
     * Draw "ASOS v0.1.0" in white on black, starting at glyph cell
     * (col=2, row=2) — 16 pixels from the left edge, 16 from the top.
     * Each glyph cell is 8×8 pixels (FONT_WIDTH × FONT_HEIGHT).
     */
    fb_puts_at(ASOS_VERSION, 2, 2, COLOR_WHITE, COLOR_BLACK);
    serial_puts("Framebuffer: banner rendered\r\n");

    serial_puts("Halting.\r\n");

    /* ── Step 5: Halt loop ─────────────────────────────────────────────
     *
     * We have no scheduler, no userspace, and no further work to do.
     * The `hlt` instruction suspends the CPU until the next interrupt;
     * because interrupts are disabled (IF=0) this loops forever at
     * near-zero power consumption.
     */
    for (;;)
        __asm__ volatile ("hlt");
}
