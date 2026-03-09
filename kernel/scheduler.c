/*
 * kernel/scheduler.c — Cooperative round-robin scheduler.
 *
 * Ready queue is a singly-linked list (head / tail pointers).
 * scheduler_yield() moves the current task to the back of the queue,
 * removes the next task from the front, and context-switches to it.
 *
 * An idle task exists as a fallback when the queue is empty and
 * the current task is dead.  It is never placed in the ready queue.
 */

#include "scheduler.h"
#include "process.h"
#include "heap.h"
#include "string.h"
#include "serial.h"

extern void context_switch(uint64_t *old_rsp_ptr, uint64_t new_rsp);

/* ── State ───────────────────────────────────────────────────────────────*/

static task_t *g_current   = NULL;
static task_t *g_idle      = NULL;
static task_t *g_ready_head = NULL;
static task_t *g_ready_tail = NULL;

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

    /*
     * Wrap the currently-running main thread in a task_t.
     * We don't allocate a stack for it (it already has one).
     * kernel_rsp will be filled in by context_switch on the first yield.
     */
    task_t *main_task = (task_t *)kmalloc(sizeof(task_t));
    memset(main_task, 0, sizeof(*main_task));
    main_task->id    = 0;
    main_task->state = TASK_RUNNING;
    memcpy(main_task->name, "main", 5);

    g_current = main_task;

    serial_puts("[SCHED] Scheduler initialized.\n");
}

void scheduler_add_task(task_t *task)
{
    task->state = TASK_READY;
    enqueue(task);

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
    task_t *prev = g_current;
    task_t *next = dequeue();

    /* Nothing to switch to? */
    if (!next) {
        if (prev->state == TASK_DEAD) {
            /* Current task is dead and queue is empty — run idle. */
            next = g_idle;
        } else {
            /* We're the only runnable task — keep running. */
            return;
        }
    }

    /* Put the previous task back in the queue (unless it's dead). */
    if (prev->state == TASK_RUNNING) {
        prev->state = TASK_READY;
        enqueue(prev);
    }

    next->state = TASK_RUNNING;
    g_current   = next;

    context_switch(&prev->kernel_rsp, next->kernel_rsp);
}

task_t *scheduler_get_current(void)
{
    return g_current;
}
