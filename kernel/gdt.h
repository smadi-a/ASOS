/*
 * kernel/gdt.h — Global Descriptor Table.
 *
 * GDT layout (selector = index × 8):
 *
 *   Selector  Index  Description
 *   ────────  ─────  ─────────────────────────────────────────────────
 *   0x00        0    Null descriptor (required; CPU ignores it)
 *   0x08        1    Kernel code — 64-bit, ring 0, execute/read
 *   0x10        2    Kernel data — ring 0, read/write
 *   0x18        3    User data   — ring 3, read/write  ← before user code
 *   0x20        4    User code   — 64-bit, ring 3, execute/read
 *   0x28        5    TSS low     — 16-byte TSS descriptor (slots 5+6)
 *   0x30        6    TSS high    — upper 32 bits of TSS base address
 *
 * The user data descriptor MUST precede user code in the table.
 * The SYSCALL/SYSRET instruction derives the ring-3 code and data
 * selectors from a single MSR (STAR) by adding 8 to a base selector,
 * so the layout must be: kernel_code | kernel_data | user_data | user_code.
 *
 * In 64-bit mode segment bases and limits are mostly ignored for
 * code/data segments; the CPU uses a flat memory model.  Only the
 * TSS descriptor carries a meaningful base address.
 */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* Segment selectors */
#define SEL_KERNEL_CODE  0x08
#define SEL_KERNEL_DATA  0x10
#define SEL_USER_DATA    0x18
#define SEL_USER_CODE    0x20
#define SEL_TSS          0x28

/*
 * GDTDescriptor — the 10-byte operand for the LGDT instruction.
 * The base must be the linear address of the first GDT entry.
 */
typedef struct __attribute__((packed)) {
    uint16_t limit;   /* Byte length of GDT minus 1                      */
    uint64_t base;    /* Linear (= physical here) address of GDT[0]      */
} GDTDescriptor;

/*
 * Set up the GDT with the entries above, install the TSS descriptor,
 * flush the CPU's segment register cache with LGDT, reload CS via a
 * far-return, reload DS/ES/FS/GS/SS, then execute LTR to install the
 * TSS into the task register.
 */
void gdt_init(void);

#endif /* GDT_H */
