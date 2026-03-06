/*
 * kernel/panic.c — Kernel panic implementation.
 *
 * Prints "KERNEL PANIC: <message>" with the caller's return address to
 * both serial and framebuffer, then disables interrupts and halts.
 */

#include "panic.h"
#include "serial.h"
#include "framebuffer.h"
#include <stdint.h>

static const char hex_chars[] = "0123456789ABCDEF";

static void put_hex64(uint64_t v)
{
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex_chars[(v >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
    serial_puts(buf);
    fb_puts(buf, COLOR_RED, COLOR_BLACK);
}

__attribute__((noreturn))
void kpanic(const char *message)
{
    void *caller = __builtin_return_address(0);

    serial_puts("\n[PANIC] KERNEL PANIC: ");
    serial_puts(message);
    serial_puts("\n[PANIC] caller=");

    fb_puts("\nKERNEL PANIC: ", COLOR_RED, COLOR_BLACK);
    fb_puts(message, COLOR_RED, COLOR_BLACK);
    fb_puts("\ncaller=", COLOR_RED, COLOR_BLACK);

    put_hex64((uint64_t)(uintptr_t)caller);

    serial_puts("\n[PANIC] System halted.\n");
    fb_puts("\nSystem halted.\n", COLOR_RED, COLOR_BLACK);

    __asm__ volatile (
        "cli\n"
        "1: hlt\n"
        "jmp 1b\n"
        ::: "memory"
    );
    __builtin_unreachable();
}
