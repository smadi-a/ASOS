/*
 * unistd.h — POSIX-like system call wrappers.
 */

#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     name[12];        /* "FILENAME.EXT\0" (8.3 display format) */
    uint32_t size;            /* File size in bytes                    */
    uint8_t  is_directory;    /* 1 if directory, 0 if file             */
    uint8_t  padding[3];
} dirent_t;

long write(int fd, const void *buf, size_t count);
long read(int fd, void *buf, size_t count);
int  getpid(void);
void yield(void);
void _exit(int status) __attribute__((noreturn));
void *sbrk(intptr_t increment);
long spawn(const char *path);
long waitpid(long pid, int *status);
int  readdir(const char *path, dirent_t *entries, int max_entries);
long pidof(const char *name);

#endif /* _UNISTD_H */
