/*
 * asos_compat.h — POSIX/Linux compatibility shims for DOOM on ASOS.
 *
 * Force-included via -include asos_compat.h so that original DOOM source
 * files compile against the ASOS freestanding C library (libasos.a).
 *
 * Strategy:
 *   1. Include ASOS headers and save real function references via inlines.
 *   2. Redefine conflicting names as macros that route to the wrappers.
 */

#ifndef ASOS_COMPAT_H
#define ASOS_COMPAT_H

/* ── Include ASOS headers FIRST ─────────────────────────────────────── */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/syscall.h>
#include <unistd.h>

/* ── Save real ASOS functions before any macros ─────────────────────── */
/* These inline wrappers capture the real function addresses at compile
 * time.  After this block, we freely redefine the public names.        */

static inline int  _asos_fopen_real(const char *p)
{ return fopen(p); }

static inline long _asos_fread_real(int fd, void *b, long n)
{ return fread(fd, b, n); }

static inline int  _asos_fseek_real(int fd, long off, int wh)
{ return fseek(fd, off, wh); }

static inline int  _asos_fclose_real(int fd)
{ return fclose(fd); }

static inline long _asos_fsize_real(int fd)
{ return fsize(fd); }

static inline int  _asos_mkdir_real(const char *p)
{ return mkdir(p); }

/* ── Platform identification ─────────────────────────────────────────── */

#undef NORMALUNIX
#undef LINUX
#undef SNDSERV
#undef SNDINTR

/* ── sprintf ─────────────────────────────────────────────────────────── */

#define sprintf(buf, fmt, ...) \
    snprintf((buf), 65536, (fmt), ##__VA_ARGS__)

/* ── C stdio stubs ───────────────────────────────────────────────────── */

#define FILE      void
#define stderr    ((void *)0)
#define stdout    ((void *)0)
#define fprintf(f, fmt, ...)   printf((fmt), ##__VA_ARGS__)
#define vfprintf(f, fmt, ap)   vsnprintf(_asos_vfp_buf, sizeof(_asos_vfp_buf), (fmt), (ap))
#define fflush(f)              ((void)0)
#define setbuf(f, b)           ((void)0)
#define perror(s)              printf("%s: error\n", (s))
#define feof(f)                1
#define popen(cmd, mode)       ((void *)0)

/* vfprintf shim buffer */
#ifndef _ASOS_COMPAT_IMPL
extern char _asos_vfp_buf[512];
#endif

static inline int _doom_fscanf(long f, const char *fmt, ...)
{ (void)f; (void)fmt; return 0; }
#define fscanf(f, fmt, ...)  _doom_fscanf(0, (fmt))

/* ── Redefine ASOS VFS names to accept C-stdio-style args ───────────── */

/* fopen: C stdio takes (path, mode); ASOS takes (path).
 * Accept variable args, ignore mode. */
#undef fopen
#define fopen(path, ...)  _asos_fopen_real(path)

/* fread: C stdio takes (buf, elemsize, nmemb, stream) — 4 args.
 * ASOS takes (fd, buf, count) — 3 args.
 * Redefine to the 4-arg C stdio form (DOOM uses both; POSIX read is
 * mapped separately below). */
#undef fread
#define fread(buf, sz, n, fd) \
    _asos_fread_real((int)(long)(fd), (void *)(buf), (long)(sz) * (long)(n))

/* fclose: accept void* or int — cast through long. */
#undef fclose
#define fclose(f)  _asos_fclose_real((int)(long)(f))

/* fseek: same signature, just route through wrapper. */
#undef fseek
#define fseek(fd, off, wh)  _asos_fseek_real((int)(long)(fd), (off), (wh))

/* fsize: route through wrapper. */
#undef fsize
#define fsize(fd)  _asos_fsize_real((int)(long)(fd))

/* mkdir: ASOS takes 1 arg, POSIX takes 2.  Accept variable args. */
#undef mkdir
#define mkdir(path, ...)  _asos_mkdir_real(path)

/* ── alloca ──────────────────────────────────────────────────────────── */

#define alloca  __builtin_alloca

/* ── POSIX file I/O ──────────────────────────────────────────────────── */

#define O_RDONLY   0
#define O_BINARY   0
#define O_WRONLY   1
#define O_CREAT    0x40
#define O_TRUNC    0x200
#define R_OK       0
#define X_OK       0

/* POSIX open(path, flags) → ASOS fopen(path) */
static inline int _doom_open(const char *path, int flags)
{ (void)flags; return _asos_fopen_real(path); }
#define open(path, flags, ...)  _doom_open((path), (flags))

/* POSIX read(fd, buf, count) → ASOS fread(fd, buf, count)
 * Uses the saved wrapper, NOT the fread macro (which expects 4 args). */
#define read(fd, buf, count)  _asos_fread_real((fd), (buf), (count))

/* POSIX lseek(fd, off, whence) → ASOS fseek */
#define lseek(fd, off, wh)  _asos_fseek_real((fd), (off), (wh))

/* POSIX close(fd) → ASOS fclose */
#define close(fd)  _asos_fclose_real((fd))

/* POSIX fstat → ASOS fsize */
struct stat { long st_size; };
static inline int _doom_fstat(int fd, struct stat *buf)
{
    long sz = _asos_fsize_real(fd);
    if (sz < 0) return -1;
    buf->st_size = sz;
    return 0;
}
#define fstat(fd, buf)  _doom_fstat((fd), (buf))

/* POSIX access — check if file exists */
static inline int _doom_access(const char *path, int mode)
{
    (void)mode;
    int fd = _asos_fopen_real(path);
    if (fd < 0) return -1;
    _asos_fclose_real(fd);
    return 0;
}
#define access(path, mode)  _doom_access((path), (mode))

/* ── getenv / signal ─────────────────────────────────────────────────── */

static inline char *_doom_getenv(const char *name)
{ (void)name; return (char *)0; }
#define getenv(s)  _doom_getenv(s)

#define SIGINT   2
#define signal(sig, handler)  ((void)0)

/* ── String utilities ────────────────────────────────────────────────── */

static inline int _doom_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
#define strcasecmp(a, b)  _doom_strcasecmp((a), (b))
#define strcmpi(a, b)     _doom_strcasecmp((a), (b))

static inline int _doom_strncasecmp(const char *a, const char *b, unsigned long n)
{
    while (n-- && *a && *b) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    if (n == (unsigned long)-1) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}
#define strncasecmp(a, b, n)  _doom_strncasecmp((a), (b), (n))

static inline char *_doom_strerror(int err)
{ (void)err; return "error"; }
#define strerror(e)  _doom_strerror(e)

/* ── errno stub ──────────────────────────────────────────────────────── */

#ifndef errno
static int _doom_errno;
#define errno _doom_errno
#endif

/* ── sscanf stub ─────────────────────────────────────────────────────── */

static inline int _doom_sscanf(const char *s, const char *fmt, ...)
{ (void)s; (void)fmt; return 0; }
#define sscanf  _doom_sscanf

/* ── Misc stubs ──────────────────────────────────────────────────────── */

static inline int getuid(void) { return 0; }

static inline double _doom_pow(double base, double exp)
{ (void)base; (void)exp; return 1.0; }
#define pow(b, e)  _doom_pow((b), (e))

#ifndef abs
#define abs(x)  ((x) < 0 ? -(x) : (x))
#endif

#endif /* ASOS_COMPAT_H */
