/*
 * kernel/process.c — Task creation and exit trampoline.
 *
 * task_create_kernel() allocates a task_t and a 16 KB kernel stack,
 * then builds an initial stack frame that context_switch can "return"
 * into.  When entry_point() returns, it falls through to the exit
 * trampoline which marks the task dead and yields.
 */

#include "process.h"
#include "scheduler.h"
#include "heap.h"
#include "string.h"
#include "serial.h"

/* context_switch.asm — push order: rbp rbx r12 r13 r14 r15, then ret. */
extern void context_switch(uint64_t *old_rsp_ptr, uint64_t new_rsp);

#define TASK_KSTACK_SIZE  16384   /* 16 KB = 4 pages */

static uint64_t g_next_id = 1;

/* ── Exit trampoline ─────────────────────────────────────────────────────
 *
 * Placed as the return address beneath entry_point on the initial stack.
 * When a thread's entry_point() returns normally, execution falls here.
 */
static void task_exit_trampoline(void)
{
    task_t *cur = scheduler_get_current();
    serial_puts("[PROC] Task ");
    /* Tiny decimal printer — avoids pulling in printf for one number. */
    {
        uint64_t v = cur->id;
        if (v == 0) { serial_putc('0'); }
        else {
            char tmp[20]; int i = 0;
            while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
            while (i--) serial_putc(tmp[i]);
        }
    }
    serial_puts(" exiting\n");

    cur->state = TASK_DEAD;
    scheduler_yield();

    /* Should never return. */
    for (;;) __asm__ volatile ("hlt");
}

/* ── Task creation ───────────────────────────────────────────────────────*/

task_t *task_create_kernel(const char *name, void (*entry_point)(void))
{
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    memset(t, 0, sizeof(*t));

    t->id    = g_next_id++;
    t->state = TASK_CREATED;

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
     *   [sp+0x38]  task_exit_trampoline   — entry_point's return addr
     *   [sp+0x30]  entry_point            — context_switch's ret target
     *   [sp+0x28]  0   (rbp)
     *   [sp+0x20]  0   (rbx)
     *   [sp+0x18]  0   (r12)
     *   [sp+0x10]  0   (r13)
     *   [sp+0x08]  0   (r14)
     *   [sp+0x00]  0   (r15)   <-- kernel_rsp
     *
     * ABI alignment: after 'ret' pops entry_point, RSP = sp+0x38.
     * stack_top is 16-byte aligned, sp = stack_top - 64.
     * sp + 0x38 = stack_top - 8  →  mod 16 == 8.  Correct.
     */
    uint64_t *sp = (uint64_t *)(uintptr_t)(t->kernel_stack_base
                                            + t->kernel_stack_size);

    *(--sp) = (uint64_t)(uintptr_t)task_exit_trampoline;  /* ret from entry */
    *(--sp) = (uint64_t)(uintptr_t)entry_point;           /* context_switch ret */
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
    {
        uint64_t v = t->id;
        if (v == 0) { serial_putc('0'); }
        else {
            char tmp[20]; int i = 0;
            while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
            while (i--) serial_putc(tmp[i]);
        }
    }
    serial_puts(")\n");

    return t;
}
