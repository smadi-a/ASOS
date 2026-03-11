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

    void         (*entry)(void);       /* Entry point (for first-run wrapper) */
    uint64_t       time_slice_remaining; /* Ticks before preemption        */

    uint64_t       pml4_phys;          /* PML4 phys addr (0 = kernel)      */
    uint64_t       user_entry_virt;    /* User-space entry VA              */
    uint64_t       user_stack_top_virt;/* Top of user stack VA             */
    int            has_been_preempted; /* Set once user task is preempted   */

    uint64_t       heap_start;         /* First usable heap VA             */
    uint64_t       heap_break;         /* Current program break            */
    uint64_t       heap_max;           /* Maximum heap VA                  */

    uint64_t       parent_pid;         /* PID of parent (0 = kernel)       */
    int            exit_status;        /* Exit code from sys_exit          */

    char           cwd[256];           /* Current working directory        */

    struct task   *next;               /* Ready-queue linked list          */
    struct task   *all_next;           /* Global task list (forward)       */
    struct task   *all_prev;           /* Global task list (backward)      */
} task_t;

/*
 * Create a kernel thread.  Allocates a task_t and a 16 KB kernel stack,
 * sets up the initial stack frame so that context_switch will jump to
 * entry_point on first switch.  Returns the new task (state = CREATED).
 */
task_t *task_create_kernel(const char *name, void (*entry_point)(void));

/*
 * Create a ring-3 user process.  Copies code_size bytes from entry_point
 * (kernel VA) to freshly mapped user pages at 0x400000.  Allocates a
 * 16 KB user stack and a 16 KB kernel stack.
 */
task_t *task_create_user(const char *name, void (*entry_point)(void),
                         size_t code_size);

/*
 * Create a ring-3 user process from an in-memory ELF64 binary.
 * Parses the ELF, maps PT_LOAD segments into a new address space,
 * and sets the entry point from the ELF header.
 * Returns NULL on failure (invalid ELF, allocation error).
 */
task_t *task_create_from_elf(const char *name,
                             const void *elf_data, size_t elf_size);

#endif /* PROCESS_H */
