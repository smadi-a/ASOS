/*
 * unistd.h — POSIX-like system call wrappers.
 */

#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>

long write(int fd, const void *buf, size_t count);
long read(int fd, void *buf, size_t count);
int  getpid(void);
void yield(void);
void _exit(int status) __attribute__((noreturn));
void *sbrk(intptr_t increment);
long spawn(const char *path);
long waitpid(long pid, int *status);

#endif /* _UNISTD_H */
