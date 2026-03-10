/*
 * user/syscall.h — Standalone user-space syscall wrappers.
 *
 * Completely freestanding — no kernel headers, no host libc.
 * Included by user programs compiled outside the kernel.
 */

#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

typedef __UINT64_TYPE__  uint64_t;
typedef __INT64_TYPE__   int64_t;

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

static inline int64_t write(int fd, const void *buf, uint64_t count)
{
    return syscall3(SYS_WRITE, (uint64_t)fd, (uint64_t)buf, count);
}

static inline int64_t read(int fd, void *buf, uint64_t count)
{
    return syscall3(SYS_READ, (uint64_t)fd, (uint64_t)buf, count);
}

static inline void exit(int status)
{
    syscall1(SYS_EXIT, (uint64_t)status);
    while (1) {}
}

static inline int64_t getpid(void)
{
    return syscall0(SYS_GETPID);
}

static inline void yield(void)
{
    syscall0(SYS_YIELD);
}

#endif /* USER_SYSCALL_H */
