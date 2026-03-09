/*
 * kernel/scheduler.h — Cooperative round-robin scheduler.
 *
 * Milestone 6A: voluntary yield() only, no preemption.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"

/* Initialise the scheduler.  Creates the idle task and wraps the
 * currently running code (main thread) in a task_t.               */
void scheduler_init(void);

/* Add a task to the back of the ready queue. */
void scheduler_add_task(task_t *task);

/* Voluntarily yield the CPU to the next ready task.  If no other
 * task is ready, returns immediately.                              */
void scheduler_yield(void);

/* Return the currently executing task. */
task_t *scheduler_get_current(void);

#endif /* SCHEDULER_H */
