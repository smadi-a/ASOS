/*
 * unistd.c — POSIX-like system call wrappers.
 */

#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>

long write(int fd, const void *buf, size_t count)
{
    int64_t ret = __syscall3(SYS_WRITE, (uint64_t)fd,
                             (uint64_t)(uintptr_t)buf, (uint64_t)count);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (long)ret;
}

long read(int fd, void *buf, size_t count)
{
    int64_t ret = __syscall3(SYS_READ, (uint64_t)fd,
                             (uint64_t)(uintptr_t)buf, (uint64_t)count);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (long)ret;
}

int getpid(void)
{
    return (int)__syscall0(SYS_GETPID);
}

void yield(void)
{
    __syscall0(SYS_YIELD);
}

void _exit(int status)
{
    __syscall1(SYS_EXIT, (uint64_t)status);
    while (1) {}
}

void *sbrk(intptr_t increment)
{
    int64_t ret = __syscall1(SYS_SBRK, (uint64_t)increment);
    if (ret == -1) {
        errno = ENOMEM;
        return (void *)-1;
    }
    return (void *)(uintptr_t)ret;
}
