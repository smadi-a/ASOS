/*
 * kernel/scheduler.h — Preemptive round-robin scheduler.
 *
 * Milestone 6B: timer-driven preemption via scheduler_tick().
 * Cooperative yield() still works for voluntary context switches.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"
#include "isr.h"

#define TIME_SLICE_TICKS  20   /* 20 ms at 1000 Hz PIT */

/* Initialise the scheduler.  Creates the idle task and wraps the
 * currently running code (main thread) in a task_t.               */
void scheduler_init(void);

/* Add a task to the back of the ready queue. */
void scheduler_add_task(task_t *task);

/* Voluntarily yield the CPU to the next ready task.  If no other
 * task is ready, returns immediately.                              */
void scheduler_yield(void);

/* Called from the PIT IRQ handler (interrupt context, IF=0).
 * Decrements the current task's time slice and preempts if expired. */
void scheduler_tick(InterruptFrame *frame);

/* Return the currently executing task. */
task_t *scheduler_get_current(void);

/* Interrupt control helpers. */
static inline void interrupts_disable(void)
{
    __asm__ volatile ("cli" ::: "memory");
}

static inline void interrupts_enable(void)
{
    __asm__ volatile ("sti" ::: "memory");
}

#endif /* SCHEDULER_H */
