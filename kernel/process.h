/*
 * kernel/process.h — Task structure and kernel thread creation.
 *
 * Each task has a unique ID, a saved kernel RSP for context switching,
 * and a linked-list pointer for the scheduler's ready queue.
 */

#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    TASK_CREATED,
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

typedef struct task {
    uint64_t       id;
    char           name[32];
    task_state_t   state;

    uint64_t       kernel_rsp;         /* Saved stack pointer              */

    uint64_t       kernel_stack_base;  /* Bottom of allocated stack        */
    uint64_t       kernel_stack_size;  /* Size in bytes                    */

    /* Future milestones — unused for now. */
    uint64_t      *page_table;         /* PML4 phys addr (NULL = kernel)   */
    uint64_t       user_stack_base;    /* User stack (0 = kernel thread)   */

    struct task   *next;               /* Ready-queue linked list          */
} task_t;

/*
 * Create a kernel thread.  Allocates a task_t and a 16 KB kernel stack,
 * sets up the initial stack frame so that context_switch will jump to
 * entry_point on first switch.  Returns the new task (state = CREATED).
 */
task_t *task_create_kernel(const char *name, void (*entry_point)(void));

#endif /* PROCESS_H */
