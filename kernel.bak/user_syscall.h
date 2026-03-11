/*
 * kernel/user_syscall.h — Inline syscall wrappers for user programs.
 *
 * Included by user program source files compiled into the kernel binary.
 * In a future milestone this will be part of the user-space C runtime.
 *
 * CRITICAL: The clobber list must include rcx and r11 because the syscall
 * instruction stores RIP in RCX and RFLAGS in R11, destroying whatever the
 * compiler had in those registers.
 */

#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include <stdint.h>

#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_EXIT    2
#define SYS_GETPID  3
#define SYS_YIELD   4

static inline int64_t syscall0(uint64_t num)
{
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall1(uint64_t num, uint64_t arg1)
{
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2,
                                uint64_t a3)
{
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t user_write(int fd, const void *buf, uint64_t count)
{
    return syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, count);
}

static inline int64_t user_read(int fd, void *buf, uint64_t count)
{
    return syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, count);
}

static inline void user_exit(int status)
{
    syscall1(SYS_EXIT, (uint64_t)status);
    while (1) {}
}

static inline int64_t user_getpid(void)
{
    return syscall0(SYS_GETPID);
}

static inline void user_yield(void)
{
    syscall0(SYS_YIELD);
}

#endif /* USER_SYSCALL_H */
