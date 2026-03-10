/*
 * kernel/scheduler.c — Preemptive round-robin scheduler.
 *
 * Ready queue is a singly-linked list (head / tail pointers).
 *
 * scheduler_yield() — cooperative: moves current task to back of queue,
 * dequeues the next, and context-switches.
 *
 * scheduler_tick() — preemptive: called from the PIT IRQ handler every
 * 1 ms.  Decrements the current task's time slice; when it expires,
 * performs a forced context switch.
 *
 * An idle task exists as a fallback when the queue is empty and the
 * current task is dead.  It is never placed in the ready queue.
 */

#include "scheduler.h"
#include "process.h"
#include "heap.h"
#include "string.h"
#include "serial.h"
#include "tss.h"
#include "vmm.h"

extern void context_switch(uint64_t *old_rsp_ptr, uint64_t new_rsp);

/* ── State ───────────────────────────────────────────────────────────────*/

static task_t *g_current    = NULL;
static task_t *g_idle       = NULL;
static task_t *g_ready_head = NULL;
static task_t *g_ready_tail = NULL;

/* Global list of ALL tasks (regardless of state). */
static task_t *g_all_head   = NULL;
static task_t *g_all_tail   = NULL;

static void all_list_add(task_t *t)
{
    t->all_prev = g_all_tail;
    t->all_next = NULL;
    if (g_all_tail)
        g_all_tail->all_next = t;
    else
        g_all_head = t;
    g_all_tail = t;
}

static void all_list_remove(task_t *t)
{
    if (t->all_prev)
        t->all_prev->all_next = t->all_next;
    else
        g_all_head = t->all_next;

    if (t->all_next)
        t->all_next->all_prev = t->all_prev;
    else
        g_all_tail = t->all_prev;

    t->all_prev = NULL;
    t->all_next = NULL;
}

/* Switch CR3 to the address space of the current task. */
static void switch_cr3_for_current(void)
{
    uint64_t cr3 = g_current->pml4_phys ? g_current->pml4_phys
                                        : vmm_get_kernel_pml4();
    vmm_switch_address_space(cr3);
}

/* ── Ready-queue helpers ─────────────────────────────────────────────────*/

static void enqueue(task_t *t)
{
    t->next = NULL;
    if (g_ready_tail) {
        g_ready_tail->next = t;
    } else {
        g_ready_head = t;
    }
    g_ready_tail = t;
}

static task_t *dequeue(void)
{
    if (!g_ready_head) return NULL;
    task_t *t = g_ready_head;
    g_ready_head = t->next;
    if (!g_ready_head) g_ready_tail = NULL;
    t->next = NULL;
    return t;
}

/* ── Idle task ───────────────────────────────────────────────────────────*/

static void idle_entry(void)
{
    for (;;)
        __asm__ volatile ("hlt");
}

/* ── Public API ──────────────────────────────────────────────────────────*/

void scheduler_init(void)
{
    /* Create idle task (never goes into the ready queue). */
    g_idle = task_create_kernel("idle", idle_entry);
    g_idle->state = TASK_READY;
    all_list_add(g_idle);

    /*
     * Wrap the currently-running main thread in a task_t.
     * We don't allocate a stack for it (it already has one).
     * kernel_rsp will be filled in by context_switch on the first yield.
     */
    task_t *main_task = (task_t *)kmalloc(sizeof(task_t));
    memset(main_task, 0, sizeof(*main_task));
    main_task->id    = 0;
    main_task->state = TASK_RUNNING;
    main_task->time_slice_remaining = TIME_SLICE_TICKS;
    memcpy(main_task->name, "main", 5);

    g_current = main_task;
    all_list_add(main_task);

    serial_puts("[SCHED] Scheduler initialized.\n");
}

void scheduler_add_task(task_t *task)
{
    interrupts_disable();

    task->state = TASK_READY;
    task->time_slice_remaining = TIME_SLICE_TICKS;
    enqueue(task);
    all_list_add(task);

    interrupts_enable();

    serial_puts("[SCHED] Added: ");
    serial_puts(task->name);
    serial_puts(" (id=");
    {
        uint64_t v = task->id;
        if (v == 0) { serial_putc('0'); }
        else {
            char tmp[20]; int i = 0;
            while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
            while (i--) serial_putc(tmp[i]);
        }
    }
    serial_puts(")\n");
}

void scheduler_yield(void)
{
    interrupts_disable();

    task_t *prev = g_current;
    task_t *next = dequeue();

    /* Nothing to switch to? */
    if (!next) {
        if (prev->state == TASK_DEAD) {
            /* Current task is dead and queue is empty — run idle. */
            next = g_idle;
        } else {
            /* We're the only runnable task — keep running. */
            interrupts_enable();
            return;
        }
    }

    /* Put the previous task back in the queue (unless it's dead). */
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        enqueue(prev);
    }

    next->state = TASK_RUNNING;
    next->time_slice_remaining = TIME_SLICE_TICKS;
    g_current   = next;

    /* Update TSS RSP0 for ring 3→0 transitions. */
    if (next->kernel_stack_base)
        tss_set_rsp0(next->kernel_stack_base + next->kernel_stack_size);

    context_switch(&prev->kernel_rsp, next->kernel_rsp);

    /* We return here when we're scheduled back in.
     * Switch to our address space. */
    switch_cr3_for_current();
    interrupts_enable();
}

void scheduler_tick(InterruptFrame *frame)
{
    (void)frame;

    if (!g_current) return;

    /* If running the idle task, check if anything became ready. */
    if (g_current == g_idle) {
        task_t *next = dequeue();
        if (!next) return;

        next->state = TASK_RUNNING;
        next->time_slice_remaining = TIME_SLICE_TICKS;

        task_t *prev = g_idle;
        g_idle->state = TASK_READY;
        g_current = next;

        if (next->kernel_stack_base)
            tss_set_rsp0(next->kernel_stack_base + next->kernel_stack_size);

        context_switch(&prev->kernel_rsp, next->kernel_rsp);
        switch_cr3_for_current();
        return;
    }

    /* Mark user processes as having been preempted (for monitoring). */
    if (g_current->pml4_phys && !g_current->has_been_preempted)
        g_current->has_been_preempted = 1;

    /* Decrement time slice. */
    if (g_current->time_slice_remaining > 0)
        g_current->time_slice_remaining--;

    if (g_current->time_slice_remaining > 0)
        return;   /* Still has time left. */

    /* Time slice expired — preempt. */
    task_t *next = dequeue();
    if (!next) {
        /* No other task ready — reset slice and keep running. */
        g_current->time_slice_remaining = TIME_SLICE_TICKS;
        return;
    }

    task_t *prev = g_current;
    prev->state = TASK_READY;
    enqueue(prev);

    next->state = TASK_RUNNING;
    next->time_slice_remaining = TIME_SLICE_TICKS;
    g_current = next;

    if (next->kernel_stack_base)
        tss_set_rsp0(next->kernel_stack_base + next->kernel_stack_size);

    context_switch(&prev->kernel_rsp, next->kernel_rsp);
    switch_cr3_for_current();
}

task_t *scheduler_get_current(void)
{
    return g_current;
}

task_t *scheduler_find_dead_child(uint64_t parent_pid, int64_t target_pid)
{
    for (task_t *t = g_all_head; t; t = t->all_next) {
        if (t->state != TASK_DEAD) continue;
        if (t->parent_pid != parent_pid) continue;
        if (target_pid != -1 && t->id != (uint64_t)target_pid) continue;
        return t;
    }
    return NULL;
}

task_t *scheduler_find_task_by_pid(uint64_t pid)
{
    for (task_t *t = g_all_head; t; t = t->all_next) {
        if (t->id == pid) return t;
    }
    return NULL;
}

static char sched_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

task_t *scheduler_find_task_by_name(const char *name)
{
    for (task_t *t = g_all_head; t; t = t->all_next) {
        if (t->state == TASK_DEAD || t->state == TASK_CREATED)
            continue;
        /* Case-insensitive compare. */
        const char *a = name;
        const char *b = t->name;
        int match = 1;
        while (*a || *b) {
            if (sched_tolower(*a) != sched_tolower(*b)) { match = 0; break; }
            if (*a) a++;
            if (*b) b++;
        }
        if (match) return t;
    }
    return NULL;
}

void scheduler_remove_from_ready_queue(task_t *task)
{
    task_t *prev = NULL;
    task_t *cur  = g_ready_head;
    while (cur) {
        if (cur == task) {
            if (prev)
                prev->next = cur->next;
            else
                g_ready_head = cur->next;
            if (cur == g_ready_tail)
                g_ready_tail = prev;
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur  = cur->next;
    }
}

void scheduler_cleanup_task(task_t *task)
{
    interrupts_disable();
    all_list_remove(task);
    interrupts_enable();

    /* Free kernel stack (if allocated). */
    if (task->kernel_stack_base)
        kfree((void *)(uintptr_t)task->kernel_stack_base);

    /* TODO: free user page tables and mapped physical frames. */

    kfree(task);
}
