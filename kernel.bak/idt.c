/*
 * kernel/idt.c — IDT construction and loading.
 */

#include "idt.h"
#include "gdt.h"
#include <stdint.h>

#define IDT_ENTRIES 256

/* Type/attribute byte for a ring-0, 64-bit interrupt gate (clears IF). */
#define GATE_KERNEL_INT  0x8E

/* The stub table is an array of 256 function pointers defined in
 * isr_stubs.asm and exported as a symbol of type uint64_t[256].     */
extern uint64_t isr_stub_table[IDT_ENTRIES];

/* The 10-byte LIDT operand. */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} IDTDescriptor;

static IDTEntry      g_idt[IDT_ENTRIES];
static IDTDescriptor g_idt_desc;

/* ── Helper ───────────────────────────────────────────────────────────────*/

static IDTEntry make_gate(uint64_t handler, uint8_t ist)
{
    IDTEntry e;
    e.offset_lo  = (uint16_t)( handler        & 0xFFFFU);
    e.selector   = SEL_KERNEL_CODE;
    e.ist        = ist & 0x07U;
    e.type_attr  = GATE_KERNEL_INT;
    e.offset_mid = (uint16_t)((handler >> 16) & 0xFFFFU);
    e.offset_hi  = (uint32_t)((handler >> 32) & 0xFFFFFFFFU);
    e.zero       = 0;
    return e;
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void idt_init(void)
{
    for (int i = 0; i < IDT_ENTRIES; i++) {
        /*
         * Vector 8 (#DF, double fault) uses IST1 so that the CPU switches
         * to the dedicated double-fault stack even if the kernel stack is
         * corrupted.  All other vectors use IST=0 (current stack).
         */
        uint8_t ist = (i == 8) ? 1 : 0;
        g_idt[i] = make_gate(isr_stub_table[i], ist);
    }

    g_idt_desc.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idt_desc.base  = (uint64_t)g_idt;

    __asm__ volatile ("lidt %0" : : "m"(g_idt_desc));
}
