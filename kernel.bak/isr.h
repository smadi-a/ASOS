/*
 * kernel/isr.h — Interrupt/exception stack frame and C handler interface.
 *
 * Stack layout at the point isr_handler() is called
 * ──────────────────────────────────────────────────
 * (low addresses = top of stack = what RSP points to)
 *
 *   ┌──────────────┐  ← RSP  (RDI argument to isr_handler)
 *   │ r15          │  +0
 *   │ r14          │  +8
 *   │ r13          │  +16
 *   │ r12          │  +24
 *   │ r11          │  +32
 *   │ r10          │  +40
 *   │ r9           │  +48
 *   │ r8           │  +56
 *   │ rbp          │  +64
 *   │ rdi          │  +72
 *   │ rsi          │  +80
 *   │ rdx          │  +88
 *   │ rcx          │  +96
 *   │ rbx          │  +104
 *   │ rax          │  +112   ← pushed by common stub (last = highest addr)
 *   ├──────────────┤
 *   │ vector       │  +120   ← pushed by individual ISR stub
 *   │ error_code   │  +128   ← CPU-pushed (exceptions) or 0 (others)
 *   ├──────────────┤
 *   │ rip          │  +136   ┐
 *   │ cs           │  +144   │ CPU-pushed hardware interrupt frame
 *   │ rflags       │  +152   │ (always present in 64-bit mode, even
 *   │ rsp          │  +160   │ for same-privilege faults)
 *   │ ss           │  +168   ┘
 *   └──────────────┘
 */

#ifndef ISR_H
#define ISR_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
    /* General-purpose registers — pushed by common stub (push order:
     * rax, rbx, rcx, rdx, rsi, rdi, rbp, r8–r15; popped in reverse) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    /* Pushed by the per-vector stub */
    uint64_t vector;
    uint64_t error_code;  /* 0 for non-error-code vectors */

    /* Pushed by the CPU (always present in 64-bit mode) */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} InterruptFrame;

/*
 * Callback type for registered interrupt/IRQ handlers.
 * Handlers must be fast (no I/O printing, no kmalloc).
 * EOI is sent by isr_handler() after the callback returns — do not
 * send EOI inside the callback.
 */
typedef void (*isr_handler_func)(InterruptFrame *frame);

/*
 * Register a handler for the given vector (0–255).
 * For hardware IRQs, use vector = 32 + irq_number.
 * Replaces any previously installed handler.
 */
void isr_register_handler(uint8_t vector, isr_handler_func handler);

/*
 * C-side dispatcher called from the assembly common stub.
 * Not called directly; invoked via the ISR stub table.
 */
void isr_handler(InterruptFrame *frame);

#endif /* ISR_H */
