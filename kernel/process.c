/*
 * kernel/process.c — Task creation, first-run wrappers, and exit trampoline.
 *
 * task_create_kernel() — kernel threads (ring 0).
 * task_create_user()   — user processes (ring 3).
 *
 * Both set up initial kernel stack frames that context_switch can "return"
 * into.  Kernel threads land in kernel_thread_start(); user processes
 * land in user_process_start() which does iretq to ring 3.
 */

#include "process.h"
#include "scheduler.h"
#include "heap.h"
#include "string.h"
#include "serial.h"
#include "pic.h"
#include "vmm.h"
#include "pmm.h"
#include "gdt.h"
#include "elf.h"

/* context_switch.asm — push order: rbp rbx r12 r13 r14 r15, then ret. */
extern void context_switch(uint64_t *old_rsp_ptr, uint64_t new_rsp);

#define TASK_KSTACK_SIZE  16384   /* 16 KB = 4 pages */

static uint64_t g_next_id = 1;

/* ── Tiny serial printers (local to this file) ───────────────────────────*/

static void proc_put_dec(uint64_t v)
{
    if (v == 0) { serial_putc('0'); return; }
    char tmp[20];
    int i = 0;
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i--) serial_putc(tmp[i]);
}

static void proc_put_hex(uint64_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc(hex[(v >> i) & 0xF]);
}

/* ── Exit trampoline ─────────────────────────────────────────────────────
 *
 * Called when a kernel thread's entry_point() returns.  Marks the task
 * dead and yields so the scheduler can pick the next runnable task.
 */
static void task_exit_trampoline(void)
{
    task_t *cur = scheduler_get_current();
    serial_puts("[PROC] Task ");
    proc_put_dec(cur->id);
    serial_puts(" exiting\n");

    cur->state = TASK_DEAD;
    scheduler_yield();

    /* Should never return. */
    for (;;) __asm__ volatile ("hlt");
}

/* ── Kernel thread first-run wrapper ─────────────────────────────────────
 *
 * When a newly created kernel thread is first switched to (via
 * context_switch's ret), it lands here.  We send EOI, enable
 * interrupts, switch back to kernel page tables, call the real
 * entry point, and fall through to the exit trampoline.
 */
static void kernel_thread_start(void)
{
    pic_send_eoi(0);
    vmm_switch_address_space(vmm_get_kernel_pml4());
    __asm__ volatile ("sti" ::: "memory");

    task_t *cur = scheduler_get_current();
    cur->entry();

    task_exit_trampoline();
}

/* ── User process first-run wrapper ──────────────────────────────────────
 *
 * context_switch rets into this function the first time a user process
 * is scheduled.  We send EOI (inherited un-acked timer IRQ), switch to
 * the process's address space, then iretq to ring 3.
 */
static void user_process_start(void)
{
    pic_send_eoi(0);

    task_t *cur = scheduler_get_current();

    /* Switch to this process's page tables. */
    vmm_switch_address_space(cur->pml4_phys);

    /* Build an iretq frame and jump to ring 3. */
    __asm__ volatile (
        "mov %[ss],  %%rax \n"
        "push %%rax        \n"     /* SS   */
        "push %[rsp]       \n"     /* RSP  */
        "pushq $0x202      \n"     /* RFLAGS: IF=1 */
        "push %[cs]        \n"     /* CS   */
        "push %[rip]       \n"     /* RIP  */
        "iretq             \n"
        :
        : [ss]  "r" ((uint64_t)(SEL_USER_DATA | 3)),
          [rsp] "r" (cur->user_stack_top_virt),
          [cs]  "r" ((uint64_t)(SEL_USER_CODE | 3)),
          [rip] "r" (cur->user_entry_virt)
        : "rax", "memory"
    );
    __builtin_unreachable();
}

/* ── Kernel thread creation ──────────────────────────────────────────────*/

task_t *task_create_kernel(const char *name, void (*entry_point)(void))
{
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    memset(t, 0, sizeof(*t));

    t->id    = g_next_id++;
    t->state = TASK_CREATED;
    t->entry = entry_point;

    /* Copy name (truncate to fit). */
    size_t len = 0;
    while (name[len] && len < sizeof(t->name) - 1) len++;
    memcpy(t->name, name, len);
    t->name[len] = '\0';

    /* Allocate 16 KB kernel stack (kmalloc guarantees 16-byte alignment). */
    uint8_t *stack = (uint8_t *)kmalloc(TASK_KSTACK_SIZE);
    memset(stack, 0, TASK_KSTACK_SIZE);

    t->kernel_stack_base = (uint64_t)(uintptr_t)stack;
    t->kernel_stack_size = TASK_KSTACK_SIZE;

    /* ── Build the initial stack frame ────────────────────────────────
     *
     * context_switch pushes: rbp rbx r12 r13 r14 r15 (6 regs)
     * then saves RSP.  On restore it pops r15 r14 r13 r12 rbx rbp
     * then does 'ret'.
     *
     * We lay out (growing downward from stack_top):
     *
     *   [sp+0x38]  alignment padding      — never touched
     *   [sp+0x30]  kernel_thread_start    — context_switch's ret target
     *   [sp+0x28]  0   (rbp)
     *   [sp+0x20]  0   (rbx)
     *   [sp+0x18]  0   (r12)
     *   [sp+0x10]  0   (r13)
     *   [sp+0x08]  0   (r14)
     *   [sp+0x00]  0   (r15)   <-- kernel_rsp
     *
     * ABI alignment: after context_switch pops 6 regs and rets,
     * RSP = stack_top - 64 + 48 + 8 = stack_top - 8.
     * stack_top is 16-aligned, so RSP mod 16 == 8.  Correct.
     */
    uint64_t *sp = (uint64_t *)(uintptr_t)(t->kernel_stack_base
                                            + t->kernel_stack_size);

    *(--sp) = 0;                                            /* alignment padding */
    *(--sp) = (uint64_t)(uintptr_t)kernel_thread_start;    /* context_switch ret */
    *(--sp) = 0;  /* rbp */
    *(--sp) = 0;  /* rbx */
    *(--sp) = 0;  /* r12 */
    *(--sp) = 0;  /* r13 */
    *(--sp) = 0;  /* r14 */
    *(--sp) = 0;  /* r15 */

    t->kernel_rsp = (uint64_t)(uintptr_t)sp;

    serial_puts("[PROC] Created kernel thread: ");
    serial_puts(t->name);
    serial_puts(" (id=");
    proc_put_dec(t->id);
    serial_puts(")\n");

    return t;
}

/* ── User process creation ───────────────────────────────────────────────*/

#define USER_CODE_VIRT   0x400000ULL
#define USER_STACK_VIRT  0x00007FFFFFF00000ULL
#define USER_STACK_PAGES 4   /* 16 KB */

task_t *task_create_user(const char *name, void (*entry_point)(void),
                         size_t code_size)
{
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    memset(t, 0, sizeof(*t));

    t->id    = g_next_id++;
    t->state = TASK_CREATED;

    /* Copy name. */
    size_t len = 0;
    while (name[len] && len < sizeof(t->name) - 1) len++;
    memcpy(t->name, name, len);
    t->name[len] = '\0';

    /* Kernel stack (for interrupt/syscall handling while in user mode). */
    uint8_t *kstack = (uint8_t *)kmalloc(TASK_KSTACK_SIZE);
    memset(kstack, 0, TASK_KSTACK_SIZE);
    t->kernel_stack_base = (uint64_t)(uintptr_t)kstack;
    t->kernel_stack_size = TASK_KSTACK_SIZE;

    /* Create per-process address space. */
    t->pml4_phys = vmm_create_user_address_space();

    /* Map user code pages. */
    size_t code_pages = (code_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    if (code_pages == 0) code_pages = 1;

    for (size_t i = 0; i < code_pages; i++) {
        uint64_t frame = pmm_alloc_frame();
        /* Copy code from kernel VA to the physical frame (identity-mapped). */
        size_t offset = i * PMM_PAGE_SIZE;
        size_t bytes = code_size - offset;
        if (bytes > PMM_PAGE_SIZE) bytes = PMM_PAGE_SIZE;
        memcpy((void *)(uintptr_t)frame,
               (const void *)((uintptr_t)entry_point + offset),
               bytes);
        /* Map read+execute, user-accessible, NOT writable. */
        vmm_map_user_page(t->pml4_phys,
                          USER_CODE_VIRT + offset,
                          frame,
                          PTE_USER);
    }
    t->user_entry_virt = USER_CODE_VIRT;

    /* Map user stack pages (read+write, no-execute). */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc_frame();
        vmm_map_user_page(t->pml4_phys,
                          USER_STACK_VIRT + (uint64_t)i * PMM_PAGE_SIZE,
                          frame,
                          PTE_USER | PTE_WRITABLE | PTE_NO_EXEC);
    }
    t->user_stack_top_virt = USER_STACK_VIRT
                           + (uint64_t)USER_STACK_PAGES * PMM_PAGE_SIZE;

    /* Build initial kernel stack frame — same layout as kernel threads
     * but ret target is user_process_start instead of kernel_thread_start. */
    uint64_t *sp = (uint64_t *)(uintptr_t)(t->kernel_stack_base
                                            + t->kernel_stack_size);
    *(--sp) = 0;                                            /* alignment padding */
    *(--sp) = (uint64_t)(uintptr_t)user_process_start;     /* context_switch ret */
    *(--sp) = 0;  /* rbp */
    *(--sp) = 0;  /* rbx */
    *(--sp) = 0;  /* r12 */
    *(--sp) = 0;  /* r13 */
    *(--sp) = 0;  /* r14 */
    *(--sp) = 0;  /* r15 */
    t->kernel_rsp = (uint64_t)(uintptr_t)sp;

    serial_puts("[PROC] Created user process: ");
    serial_puts(t->name);
    serial_puts(" (id=");
    proc_put_dec(t->id);
    serial_puts(", pml4=");
    proc_put_hex(t->pml4_phys);
    serial_puts(")\n");

    return t;
}

/* ── ELF-based user process creation ────────────────────────────────────*/

task_t *task_create_from_elf(const char *name,
                             const void *elf_data, size_t elf_size)
{
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    memset(t, 0, sizeof(*t));

    t->id    = g_next_id++;
    t->state = TASK_CREATED;

    /* Copy name. */
    size_t len = 0;
    while (name[len] && len < sizeof(t->name) - 1) len++;
    memcpy(t->name, name, len);
    t->name[len] = '\0';

    /* Kernel stack. */
    uint8_t *kstack = (uint8_t *)kmalloc(TASK_KSTACK_SIZE);
    memset(kstack, 0, TASK_KSTACK_SIZE);
    t->kernel_stack_base = (uint64_t)(uintptr_t)kstack;
    t->kernel_stack_size = TASK_KSTACK_SIZE;

    /* Create per-process address space. */
    t->pml4_phys = vmm_create_user_address_space();

    /* Load the ELF into the new address space. */
    uint64_t highest_addr = 0;
    uint64_t entry = elf_load(elf_data, elf_size, t->pml4_phys, &highest_addr);
    if (entry == 0) {
        serial_puts("[PROC] ELF load failed for ");
        serial_puts(name);
        serial_puts("\n");
        kfree(kstack);
        kfree(t);
        return NULL;
    }
    t->user_entry_virt = entry;

    /* Set up user heap just above the highest mapped ELF segment. */
    t->heap_start = highest_addr;
    t->heap_break = highest_addr;
    t->heap_max   = USER_STACK_VIRT;  /* Grow up to (but not into) the stack */

    /* Map user stack pages (read+write, no-execute). */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc_frame();
        vmm_map_user_page(t->pml4_phys,
                          USER_STACK_VIRT + (uint64_t)i * PMM_PAGE_SIZE,
                          frame,
                          PTE_USER | PTE_WRITABLE | PTE_NO_EXEC);
    }
    t->user_stack_top_virt = USER_STACK_VIRT
                           + (uint64_t)USER_STACK_PAGES * PMM_PAGE_SIZE;

    /* Build initial kernel stack frame — same as task_create_user. */
    uint64_t *sp = (uint64_t *)(uintptr_t)(t->kernel_stack_base
                                            + t->kernel_stack_size);
    *(--sp) = 0;                                            /* alignment padding */
    *(--sp) = (uint64_t)(uintptr_t)user_process_start;     /* context_switch ret */
    *(--sp) = 0;  /* rbp */
    *(--sp) = 0;  /* rbx */
    *(--sp) = 0;  /* r12 */
    *(--sp) = 0;  /* r13 */
    *(--sp) = 0;  /* r14 */
    *(--sp) = 0;  /* r15 */
    t->kernel_rsp = (uint64_t)(uintptr_t)sp;

    serial_puts("[PROC] Created process from ELF: ");
    serial_puts(t->name);
    serial_puts(" (id=");
    proc_put_dec(t->id);
    serial_puts(", entry=");
    proc_put_hex(t->user_entry_virt);
    serial_puts(")\n");

    return t;
}
