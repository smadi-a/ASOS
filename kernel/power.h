/*
 * kernel/power.h — System power management (shutdown / reset).
 */

#ifndef KERNEL_POWER_H
#define KERNEL_POWER_H

/*
 * sys_shutdown — Attempt to power off the machine.
 *
 * Tries, in order:
 *   1. QEMU ACPI PM1a_CNT (port 0x604)
 *   2. Bochs/old-QEMU (port 0xB004)
 *   3. VirtualBox ACPI (port 0x4004)
 *   4. Keyboard controller reset (pulse CPU reset line via port 0x64)
 *   5. Triple-fault (IDT limit=0, then INT 0 — guaranteed reset)
 *
 * This function does not return.
 */
__attribute__((noreturn))
void sys_shutdown(void);

#endif /* KERNEL_POWER_H */
