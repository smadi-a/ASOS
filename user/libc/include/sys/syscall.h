/*
 * sys/syscall.h — Syscall numbers and raw wrappers.
 */

#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

#include <stdint.h>

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
#define SYS_FOPEN    15
#define SYS_FREAD    16
#define SYS_FCLOSE   17
#define SYS_FSIZE    18
#define SYS_FSEEK    19
#define SYS_FTELL    20
#define SYS_FWRITE   21
#define SYS_FCREATE  22
#define SYS_FDELETE  23
#define SYS_MKDIR    24
#define SYS_RENAME   25
#define SYS_COPY     26
#define SYS_RMDIR    27
#define SYS_GFX_DRAW   28
#define SYS_GFX_FLUSH  29
#define SYS_GFX_INFO   30
#define SYS_WIN_CREATE 31
#define SYS_WIN_UPDATE 32
#define SYS_KEY_POLL   33
#define SYS_GET_EVENT  34
#define SYS_SHUTDOWN   35
#define SYS_UPTIME     36

static inline int64_t __syscall0(uint64_t num)
{
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t __syscall1(uint64_t num, uint64_t arg1)
{
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t __syscall2(uint64_t num, uint64_t a1, uint64_t a2)
{
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t __syscall3(uint64_t num, uint64_t a1, uint64_t a2,
                                  uint64_t a3)
{
    int64_t ret;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return ret;
}

/*
 * __syscall5 — invoke a syscall with five arguments.
 *
 * x86-64 syscall convention:
 *   rax = num, rdi = a1, rsi = a2, rdx = a3, r10 = a4, r8 = a5
 * (rcx is clobbered by the syscall instruction, so r10 replaces rcx.)
 */
static inline int64_t __syscall5(uint64_t num,
                                  uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5)
{
    int64_t ret;
    register uint64_t _r10 __asm__("r10") = a4;
    register uint64_t _r8  __asm__("r8")  = a5;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(_r10), "r"(_r8)
        : "rcx", "r11", "memory");
    return ret;
}

#endif /* _SYS_SYSCALL_H */
