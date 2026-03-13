/* signal.h — stub for DOOM on ASOS. */
#ifndef _SIGNAL_H
#define _SIGNAL_H
/* signal() is stubbed in asos_compat.h */
#define SIGALRM  14
#define SA_RESTART  0x10000000
typedef int sigset_t;
struct sigaction { void (*sa_handler)(int); int sa_flags; sigset_t sa_mask; };
static inline int sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{ (void)sig; (void)act; (void)oact; return 0; }
#endif
