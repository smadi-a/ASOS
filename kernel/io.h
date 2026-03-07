/*
 * kernel/io.h — x86-64 port I/O inline wrappers.
 *
 * These must be static inline so the compiler emits the actual in/out
 * instructions directly at each call site.  Calling through a function
 * pointer or external linkage would break timing-sensitive sequences
 * (e.g., PIC initialisation) where consecutive port writes must not be
 * reordered or batched.
 *
 * io_wait() pulses port 0x80 (the POST-code port, unused on modern
 * hardware) to insert a ~1 µs delay between successive PIC/PIT writes.
 */

#ifndef IO_H
#define IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* ~1 µs delay by writing to the ISA POST-code port (safe to discard). */
static inline void io_wait(void)
{
    __asm__ volatile ("outb %%al, $0x80" :: "a"((uint8_t)0) : "memory");
}

#endif /* IO_H */
