/*
 * kernel/main.c — ASOS kernel entry point (Milestone 3).
 *
 * Boot sequence
 * ─────────────
 *  1.  Clear BSS (safety — bootloader already zeroed it, but be explicit).
 *  2.  serial_init()   — UART, no dependencies.
 *  3.  fb_init()       — store framebuffer descriptor from BootInfo.
 *  4.  gdt_init()      — install our GDT + TSS.
 *  5.  idt_init()      — install our IDT (256 vectors).
 *  6.  pmm_init()      — build physical frame bitmap from EFI memory map.
 *  7.  Switch RSP to a kernel BSS stack so the UEFI-allocated stack is
 *      no longer in use before vmm_init() invalidates the first 2 MB.
 *  8.  vmm_init()      — build 4-level page tables, switch CR3.
 *  9.  heap_init()     — set up kernel heap.
 * 10.  Tests: kmalloc/kfree sanity, null-pointer page fault.
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
#include "string.h"

#define ASOS_VERSION "ASOS v0.1.0"

/* ── Linker symbols ───────────────────────────────────────────────────────*/

extern char __bss_start[];
extern char __bss_end[];

/* ── Kernel stack ─────────────────────────────────────────────────────────
 *
 * We switch to this stack (in BSS, at a higher-half virtual address)
 * before vmm_init() so that vmm_init()'s CR3 switch doesn't invalidate
 * our active stack.  The UEFI-provided stack is in low physical memory
 * and the identity map skips the first 2 MB — if the UEFI stack happened
 * to be there it would fault.  Switching early avoids the risk entirely.
 */
#define KSTACK_SIZE  16384

static uint8_t g_kstack[KSTACK_SIZE] __attribute__((aligned(16)));

/* ── BSS clear ────────────────────────────────────────────────────────────*/

static void clear_bss(void)
{
    volatile char *p = __bss_start;
    while (p < __bss_end) *p++ = 0;
}

/* ── kernel_main ──────────────────────────────────────────────────────────*/

void kernel_main(BootInfo *info)
{
    /* ① BSS */
    clear_bss();

    /* ② Serial */
    serial_init();
    serial_puts(ASOS_VERSION "\n");

    /* ③ Framebuffer */
    fb_init(&info->framebuffer);
    fb_clear(COLOR_BLACK);
    fb_puts_at(ASOS_VERSION, 2, 0, COLOR_WHITE, COLOR_BLACK);

    /* ④ GDT */
    gdt_init();
    serial_puts("[OK] GDT\n");
    fb_puts_at("[OK] GDT", 2, 1, COLOR_GREEN, COLOR_BLACK);

    /* ⑤ IDT */
    idt_init();
    serial_puts("[OK] IDT\n");
    fb_puts_at("[OK] IDT", 2, 2, COLOR_GREEN, COLOR_BLACK);

    /* ⑥ PMM */
    pmm_init(info);
    serial_puts("[OK] PMM\n");
    fb_puts_at("[OK] PMM", 2, 3, COLOR_GREEN, COLOR_BLACK);

    /* ⑦ Switch to kernel BSS stack before vmm_init() changes CR3.
     *
     * After this inline asm, RSP points to the top of g_kstack (a
     * higher-half virtual address).  We do not return from kernel_main
     * so there is no caller frame to preserve.
     *
     * Clobber "memory" forces the compiler to flush all pending writes
     * before the stack switch.
     */
    __asm__ volatile (
        "mov %0, %%rsp"
        :: "r"((uint64_t)(uintptr_t)(g_kstack + KSTACK_SIZE))
        : "memory"
    );

    /* ⑧ VMM — builds new page tables and switches CR3. */
    vmm_init();
    serial_puts("[OK] VMM\n");
    fb_puts_at("[OK] VMM", 2, 4, COLOR_GREEN, COLOR_BLACK);

    /* ⑨ Heap */
    heap_init();
    serial_puts("[OK] Heap\n");
    fb_puts_at("[OK] Heap", 2, 5, COLOR_GREEN, COLOR_BLACK);

    /* ⑩ Memory management status */
    serial_puts("[OK] Memory management initialised.\n");
    fb_puts_at("[OK] Memory management", 2, 6, COLOR_GREEN, COLOR_BLACK);

    /* ── Heap test ────────────────────────────────────────────────────── */
    serial_puts("[TEST] kmalloc/kfree...\n");

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kmalloc(256);

    if (!a || !b || !c)
        serial_puts("[FAIL] kmalloc returned NULL\n");
    else
        serial_puts("[TEST] Three allocations OK.\n");

    /* Write to the blocks to confirm they're backed by real memory. */
    memset(a, 0xAA, 64);
    memset(b, 0xBB, 128);
    memset(c, 0xCC, 256);

    kfree(b);
    void *d = kmalloc(64);   /* should reuse b's space */
    if (!d) serial_puts("[FAIL] re-alloc after free\n");
    else    serial_puts("[TEST] Free+realloc OK.\n");

    kfree(a);
    kfree(d);
    kfree(c);

    serial_puts("[TEST] Heap test passed.\n");
    fb_puts_at("[OK] Heap test", 2, 7, COLOR_GREEN, COLOR_BLACK);

    /* ── Null-pointer dereference test ────────────────────────────────── */
    serial_puts("[TEST] Triggering null-pointer #PF...\n");
    fb_puts_at("Testing null #PF...", 2, 9, COLOR_YELLOW, COLOR_BLACK);

    volatile uint64_t *null_ptr = (volatile uint64_t *)0;
    (void)*null_ptr;   /* should trigger #PF — address 0 is not mapped */

    /* Should never reach here. */
    serial_puts("[FAIL] Null dereference did not fault!\n");
    for (;;) __asm__ volatile ("hlt");
}
