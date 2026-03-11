/*
 * kernel/tss.h — 64-bit Task State Segment.
 *
 * In 64-bit long mode the CPU no longer uses the TSS for hardware task
 * switching, but it still reads two fields from it:
 *
 *   RSP0   — kernel stack pointer loaded when an interrupt or exception
 *             causes a ring 3→0 privilege-level change.  Not used until
 *             we have user-mode processes, but must be set to a valid
 *             value so the hardware does not load a garbage RSP.
 *
 *   IST1–7 — "Interrupt Stack Table" entries.  When an IDT gate has a
 *             non-zero IST field, the CPU unconditionally switches to the
 *             corresponding IST stack regardless of the current ring.
 *             IST1 is dedicated here to the double-fault (#DF) handler
 *             so that a corrupted kernel stack cannot prevent us from
 *             diagnosing the fault.
 *
 * The TSS must be installed in the GDT as a 16-byte system descriptor
 * and loaded into TR with the `ltr` instruction.
 */

#ifndef TSS_H
#define TSS_H

#include <stdint.h>

/*
 * 64-bit TSS layout (Intel SDM Vol.3A §8.7).
 * All fields are packed; there is no implicit padding.
 */
typedef struct __attribute__((packed)) {
    uint32_t reserved0;    /* Always 0                                      */
    uint64_t rsp0;         /* Ring-0 stack pointer (ring 3 → 0 transitions) */
    uint64_t rsp1;         /* Ring-1 stack pointer (unused here)            */
    uint64_t rsp2;         /* Ring-2 stack pointer (unused here)            */
    uint64_t reserved1;    /* Always 0                                      */
    uint64_t ist1;         /* IST slot 1: double-fault dedicated stack      */
    uint64_t ist2;         /* IST slots 2–7: unused for now                 */
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;    /* Always 0                                      */
    uint16_t reserved3;    /* Always 0                                      */
    uint16_t iomap_base;   /* Byte offset of I/O permission bitmap from TSS */
                           /* base.  Set to sizeof(TSS) → no IOPB present. */
} TSS;

/* sizeof(TSS) must be 104 — checked at compile time in tss.c */

/* Initialise the TSS and its IST stacks. */
void tss_init(void);

/* Return a pointer to the single kernel TSS instance. */
TSS *tss_get(void);

/* Update TSS RSP0 — the kernel stack loaded on ring 3→0 transitions. */
void tss_set_rsp0(uint64_t rsp0);

#endif /* TSS_H */
