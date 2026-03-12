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
#define SYS_GFX_DRAW  28   /* rdi = ptr to GfxCmd (user VA)   */
#define SYS_GFX_FLUSH 29   /* no args — runs compositor then copies back buf to FB */
#define SYS_GFX_INFO  30   /* rdi = ptr to uint32_t[2] (user VA, out: w,h) */
#define SYS_WIN_CREATE 31  /* rdi=title(VA), rsi=x, rdx=y, r10=w, r8=h → win_id */
#define SYS_WIN_UPDATE 32  /* rdi=win_id, rsi=pixel_buf(VA), rdx=buf_size */
#define SYS_KEY_POLL   33  /* no args — returns key char (0–255) or -1 if none  */
#define SYS_GET_EVENT  34  /* rdi = ptr to event_t (user VA) → 0 if event popped, -1 if empty */

/* Initialise the syscall/sysret mechanism (STAR, LSTAR, FMASK MSRs). */
void syscall_init(void);

/* C dispatcher — called from syscall_entry.asm. */
int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5);

#endif /* SYSCALL_H */
