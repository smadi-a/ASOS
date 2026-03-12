/*
 * kernel/power.c — System power management (shutdown / reset).
 */

#include "power.h"
#include "serial.h"
#include "io.h"

__attribute__((noreturn))
void sys_shutdown(void)
{
    serial_puts("[SHUTDOWN] Powering off...\n");

    /* 1. QEMU default ACPI PM1a_CNT — SLP_TYPa=5 | SLP_EN. */
    outw(0x604, 0x2000);

    /* 2. Bochs / old QEMU shutdown port. */
    outw(0xB004, 0x2000);

    /* 3. VirtualBox ACPI. */
    outw(0x4004, 0x3400);

    /* 4. Keyboard controller reset (pulse the CPU RESET line). */
    serial_puts("[SHUTDOWN] ACPI failed, trying keyboard reset...\n");
    {
        /* Wait for the keyboard controller input buffer to drain. */
        uint8_t status;
        int timeout = 100000;
        do {
            status = inb(0x64);
        } while ((status & 0x02) && --timeout > 0);

        outb(0x64, 0xFE);   /* Pulse reset line */
    }

    /* 5. Triple-fault: load a zero-length IDT then trigger an interrupt.
     *    The CPU will triple-fault and reset. */
    serial_puts("[SHUTDOWN] Keyboard reset failed, triple-faulting...\n");
    {
        struct __attribute__((packed)) { uint16_t limit; uint64_t base; }
            null_idt = { 0, 0 };
        __asm__ volatile (
            "cli\n\t"
            "lidt %0\n\t"
            "int $0\n\t"
            :
            : "m"(null_idt)
        );
    }

    /* Should never be reached. */
    for (;;)
        __asm__ volatile ("hlt");
}
