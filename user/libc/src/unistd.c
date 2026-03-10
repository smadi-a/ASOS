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

long spawn(const char *path)
{
    int64_t ret = __syscall2(SYS_SPAWN, (uint64_t)(uintptr_t)path, 0);
    if (ret < 0) { errno = ENOENT; return -1; }
    return (long)ret;
}

long waitpid(long pid, int *status)
{
    int64_t ret = __syscall2(SYS_WAITPID, (uint64_t)pid,
                             (uint64_t)(uintptr_t)status);
    if (ret < 0) { errno = ESRCH; return -1; }
    return (long)ret;
}

int readdir(const char *path, dirent_t *entries, int max_entries)
{
    int64_t ret = __syscall3(SYS_READDIR, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)entries,
                             (uint64_t)max_entries);
    if (ret < 0) { errno = EIO; return -1; }
    return (int)ret;
}

long pidof(const char *name)
{
    int64_t ret = __syscall1(SYS_PIDOF, (uint64_t)(uintptr_t)name);
    return (long)ret;
}

int kill(long pid)
{
    int64_t ret = __syscall1(SYS_KILL, (uint64_t)pid);
    if (ret < 0) { errno = ESRCH; return -1; }
    return 0;
}

int proclist(proc_info_t *buf, int max_entries)
{
    int64_t ret = __syscall2(SYS_PROCLIST, (uint64_t)(uintptr_t)buf,
                             (uint64_t)max_entries);
    if (ret < 0) { errno = EIO; return -1; }
    return (int)ret;
}

int chdir(const char *path)
{
    int64_t ret = __syscall1(SYS_CHDIR, (uint64_t)(uintptr_t)path);
    if (ret < 0) { errno = ENOENT; return -1; }
    return 0;
}

int getcwd(char *buf, size_t size)
{
    int64_t ret = __syscall2(SYS_GETCWD, (uint64_t)(uintptr_t)buf,
                             (uint64_t)size);
    if (ret < 0) { errno = ERANGE; return -1; }
    return 0;
}

int fsstat(fs_stat_t *stat)
{
    int64_t ret = __syscall1(SYS_FSSTAT, (uint64_t)(uintptr_t)stat);
    if (ret < 0) { errno = EIO; return -1; }
    return 0;
}

int fopen(const char *path)
{
    int64_t ret = __syscall1(SYS_FOPEN, (uint64_t)(uintptr_t)path);
    if (ret < 0) { errno = ENOENT; return -1; }
    return (int)ret;
}

long fread(int fd, void *buf, size_t count)
{
    int64_t ret = __syscall3(SYS_FREAD, (uint64_t)fd,
                             (uint64_t)(uintptr_t)buf, (uint64_t)count);
    if (ret < 0) { errno = EIO; return -1; }
    return (long)ret;
}

int fclose(int fd)
{
    int64_t ret = __syscall1(SYS_FCLOSE, (uint64_t)fd);
    if (ret < 0) { errno = EINVAL; return -1; }
    return 0;
}

long fsize(int fd)
{
    int64_t ret = __syscall1(SYS_FSIZE, (uint64_t)fd);
    if (ret < 0) { errno = EINVAL; return -1; }
    return (long)ret;
}

int fseek(int fd, long offset, int whence)
{
    int64_t ret = __syscall3(SYS_FSEEK, (uint64_t)fd,
                             (uint64_t)offset, (uint64_t)whence);
    if (ret < 0) { errno = EINVAL; return -1; }
    return 0;
}

long ftell(int fd)
{
    int64_t ret = __syscall1(SYS_FTELL, (uint64_t)fd);
    if (ret < 0) { errno = EINVAL; return -1; }
    return (long)ret;
}
