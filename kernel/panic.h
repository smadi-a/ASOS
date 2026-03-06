/*
 * kernel/panic.h — Kernel panic: print message and halt.
 */

#ifndef PANIC_H
#define PANIC_H

__attribute__((noreturn))
void kpanic(const char *message);

#endif /* PANIC_H */
