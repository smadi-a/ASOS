/*
 * kernel/syscall.c — Syscall MSR initialisation and handler dispatch.
 *
 * Sets up the syscall/sysret fast path using STAR, LSTAR, and FMASK MSRs.
 * Provides C handlers for SYS_READ, SYS_WRITE, SYS_EXIT, SYS_GETPID,
 * and SYS_YIELD.
 */

#include "syscall.h"
#include "serial.h"
#include "framebuffer.h"
#include "keyboard.h"
#include "scheduler.h"
#include "process.h"
#include "gdt.h"
#include "vmm.h"
#include "pmm.h"
#include <stdint.h>

/* ── MSR addresses ───────────────────────────────────────────────────────*/

#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_FMASK   0xC0000084

/* ── MSR helpers ─────────────────────────────────────────────────────────*/

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Assembly entry point ────────────────────────────────────────────────*/

extern void syscall_entry(void);

/* ── Tiny serial printer ─────────────────────────────────────────────────*/

static void sc_put_dec(uint64_t v)
{
    if (v == 0) { serial_putc('0'); return; }
    char tmp[20];
    int i = 0;
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i--) serial_putc(tmp[i]);
}

static void sc_put_hex(uint64_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(hex[(v >> i) & 0xF]);
}

/* ── syscall_init ────────────────────────────────────────────────────────*/

void syscall_init(void)
{
    /* 1. Enable the SCE (Syscall Enable) bit in EFER. */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1;   /* bit 0 = SCE */
    wrmsr(MSR_EFER, efer);

    /* 2. STAR: segment selectors for syscall/sysret.
     *
     *   Bits 47:32 = kernel CS (syscall loads CS from here, SS = CS+8)
     *                kernel CS = 0x08, kernel SS = 0x10.  Correct.
     *
     *   Bits 63:48 = sysret base (sysret loads SS = base+8, CS = base+16)
     *                We need user SS = 0x18 (user data), user CS = 0x20 (user code).
     *                So base = 0x20 - 16 = 0x10.  Check: 0x10+8=0x18, 0x10+16=0x20.
     *                sysret sets RPL to 3 automatically → 0x1B and 0x23.  Correct.
     */
    uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);

    /* 3. LSTAR: kernel entry point for 64-bit syscall. */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);

    /* 4. FMASK: clear IF and DF on syscall entry. */
    wrmsr(MSR_FMASK, 0x600);   /* bit 9 = IF, bit 10 = DF */

    serial_puts("[SYSCALL] Initialized, entry at ");
    sc_put_hex((uint64_t)(uintptr_t)syscall_entry);
    serial_puts("\n");
}

/* ── Syscall handlers ────────────────────────────────────────────────────*/

#define USER_ADDR_LIMIT  0x0000800000000000ULL

static int64_t sys_write(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (fd != 1) return -1;
    if (count == 0) return 0;
    if (count > 4096) count = 4096;

    /* Validate user-space buffer address. */
    if (buf_addr >= USER_ADDR_LIMIT) return -1;
    if (buf_addr + count > USER_ADDR_LIMIT) return -1;

    /*
     * During a syscall, CR3 still points to the user's page tables.
     * The user tables lack PML4[0] (identity map), so the framebuffer
     * at physical 0x80000000 is not accessible.  Write to serial only.
     * (Port I/O does not depend on page tables.)
     */
    const char *buf = (const char *)(uintptr_t)buf_addr;
    for (uint64_t i = 0; i < count; i++)
        serial_putc(buf[i]);
    return (int64_t)count;
}

static int64_t sys_read(uint64_t fd, uint64_t buf_addr, uint64_t count)
{
    if (fd != 0) return -1;
    if (count == 0) return 0;
    if (buf_addr >= USER_ADDR_LIMIT) return -1;
    if (buf_addr + count > USER_ADDR_LIMIT) return -1;

    char *buf = (char *)(uintptr_t)buf_addr;

    /* Block until at least one character is available. */
    uint64_t bytes_read = 0;
    while (bytes_read == 0) {
        char c;
        if (keyboard_read_char(&c)) {
            buf[bytes_read++] = c;
        } else {
            __asm__ volatile ("sti" ::: "memory");
            scheduler_yield();
            __asm__ volatile ("cli" ::: "memory");
        }
    }

    /* Read any additional available characters (non-blocking). */
    while (bytes_read < count) {
        char c;
        if (!keyboard_read_char(&c)) break;
        buf[bytes_read++] = c;
    }

    return (int64_t)bytes_read;
}

static void sys_exit(int status)
{
    task_t *cur = scheduler_get_current();
    serial_puts("[SYSCALL] Task ");
    sc_put_dec(cur->id);
    serial_puts(" (");
    serial_puts(cur->name);
    serial_puts(") exited with status ");
    sc_put_dec((uint64_t)(unsigned)status);
    serial_puts("\n");

    cur->state = TASK_DEAD;
    __asm__ volatile ("sti" ::: "memory");
    scheduler_yield();

    for (;;) __asm__ volatile ("hlt");
}

static int64_t sys_getpid(void)
{
    return (int64_t)scheduler_get_current()->id;
}

static void sys_yield(void)
{
    __asm__ volatile ("sti" ::: "memory");
    scheduler_yield();
    __asm__ volatile ("cli" ::: "memory");
}

static int64_t sys_sbrk(int64_t increment)
{
    task_t *cur = scheduler_get_current();

    /* sbrk(0) returns the current break. */
    if (increment == 0)
        return (int64_t)cur->heap_break;

    uint64_t old_break = cur->heap_break;
    uint64_t new_break = old_break + (uint64_t)increment;

    /* Bounds check. */
    if (new_break < cur->heap_start || new_break > cur->heap_max)
        return -1;

    /* Map any new pages needed between old and new break.
     * pmm_alloc_frame and vmm_map_user_page use the identity map,
     * which is only present in the kernel page tables.  Switch CR3
     * temporarily. */
    uint64_t old_page = (old_break + 0xFFF) & ~0xFFFULL;
    uint64_t new_page = (new_break + 0xFFF) & ~0xFFFULL;

    if (old_page < new_page) {
        uint64_t kernel_pml4 = vmm_get_kernel_pml4();
        vmm_switch_address_space(kernel_pml4);

        for (uint64_t page = old_page; page < new_page; page += 4096) {
            uint64_t frame = pmm_alloc_frame();
            vmm_map_user_page(cur->pml4_phys, page, frame,
                              PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NO_EXEC);
        }

        vmm_switch_address_space(cur->pml4_phys);
    }

    cur->heap_break = new_break;
    return (int64_t)old_break;
}

/* ── Dispatch ────────────────────────────────────────────────────────────*/

int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void)arg4;
    (void)arg5;

    switch (num) {
    case SYS_READ:    return sys_read(arg1, arg2, arg3);
    case SYS_WRITE:   return sys_write(arg1, arg2, arg3);
    case SYS_EXIT:    sys_exit((int)arg1); return 0;
    case SYS_GETPID:  return sys_getpid();
    case SYS_YIELD:   sys_yield(); return 0;
    case SYS_SBRK:    return sys_sbrk((int64_t)arg1);
    case SYS_WAITPID: return -1;   /* stub */
    case SYS_SPAWN:   return -1;   /* stub */
    default:
        serial_puts("[SYSCALL] Unknown syscall ");
        sc_put_dec(num);
        serial_puts("\n");
        return -1;
    }
}
