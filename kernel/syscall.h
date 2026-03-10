/*
 * kernel/syscall.h — Syscall interface (MSR setup + dispatch).
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* Syscall numbers. */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_GETPID  3
#define SYS_YIELD   4
#define SYS_SBRK    5
#define SYS_WAITPID 6
#define SYS_SPAWN   7
#define SYS_READDIR 8
#define SYS_PIDOF    9
#define SYS_KILL     10
#define SYS_PROCLIST 11
#define SYS_GETCWD   12
#define SYS_CHDIR    13
#define SYS_FSSTAT   14

/* Initialise the syscall/sysret mechanism (STAR, LSTAR, FMASK MSRs). */
void syscall_init(void);

/* C dispatcher — called from syscall_entry.asm. */
int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5);

#endif /* SYSCALL_H */
