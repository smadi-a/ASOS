/*
 * kernel/isr.c — C-side interrupt/exception dispatcher.
 *
 * isr_handler() is called from the assembly common stub (isr_stubs.asm).
 *
 * Dispatch logic
 * ──────────────
 *  • If a handler is registered for the vector, call it, send EOI for
 *    hardware IRQs (vectors 32–47), and return.
 *  • For unregistered hardware IRQs (32–47): send EOI and return silently.
 *  • For unregistered CPU exceptions (0–31): display a panic screen and halt.
 *
 * EOI is always sent AFTER the registered callback completes so that the
 * same IRQ cannot re-fire and nest while the handler is still running.
 */

#include "isr.h"
#include "pic.h"
#include "serial.h"
#include "framebuffer.h"
#include <stdint.h>

/* ── Registered handler table ────────────────────────────────────────────*/

static isr_handler_func g_handlers[256];

void isr_register_handler(uint8_t vector, isr_handler_func handler)
{
    g_handlers[vector] = handler;
}

/* ── Low-level output helpers (exception panic screen) ───────────────────*/

static const char hex_chars[] = "0123456789ABCDEF";

static void fmt_hex64(uint64_t v, char *buf)
{
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex_chars[(v >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
}

static void fmt_hex16(uint16_t v, char *buf)
{
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 4; i++)
        buf[2 + i] = hex_chars[(v >> (12 - i * 4)) & 0xF];
    buf[6] = '\0';
}

static void kputs(const char *s)
{
    serial_puts(s);
    fb_puts(s, COLOR_WHITE, COLOR_BLACK);
}

static void kputhex64(uint64_t v)
{
    char buf[19];
    fmt_hex64(v, buf);
    kputs(buf);
}

static void kputhex16(uint16_t v)
{
    char buf[7];
    fmt_hex16(v, buf);
    kputs(buf);
}

__attribute__((noreturn))
static void panic_halt(void)
{
    __asm__ volatile (
        "cli\n"
        "1: hlt\n"
        "jmp 1b\n"
        ::: "memory"
    );
    __builtin_unreachable();
}

static uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

/* ── Exception name table ─────────────────────────────────────────────────*/

static const char *exception_names[32] = {
    "#DE  Division Error",
    "#DB  Debug",
    "NMI  Non-Maskable Interrupt",
    "#BP  Breakpoint",
    "#OF  Overflow",
    "#BR  Bound Range Exceeded",
    "#UD  Invalid Opcode",
    "#NM  Device Not Available",
    "#DF  Double Fault",
    "     Coprocessor Segment Overrun",
    "#TS  Invalid TSS",
    "#NP  Segment Not Present",
    "#SS  Stack-Segment Fault",
    "#GP  General Protection Fault",
    "#PF  Page Fault",
    "     Reserved",
    "#MF  x87 FPU Floating-Point Error",
    "#AC  Alignment Check",
    "#MC  Machine Check",
    "#XM  SIMD Floating-Point Exception",
    "#VE  Virtualisation Exception",
    "#CP  Control Protection Exception",
    "     Reserved", "     Reserved", "     Reserved",
    "     Reserved", "     Reserved", "     Reserved",
    "     Reserved",
    "#VC  VMM Communication Exception",
    "#SX  Security Exception",
    "     Reserved",
};

/* ── Register dump ────────────────────────────────────────────────────────*/

static void print_register_dump(const InterruptFrame *f)
{
    kputs("  RAX="); kputhex64(f->rax); kputs("  RBX="); kputhex64(f->rbx); kputs("\n");
    kputs("  RCX="); kputhex64(f->rcx); kputs("  RDX="); kputhex64(f->rdx); kputs("\n");
    kputs("  RSI="); kputhex64(f->rsi); kputs("  RDI="); kputhex64(f->rdi); kputs("\n");
    kputs("  RBP="); kputhex64(f->rbp); kputs("  RSP="); kputhex64(f->rsp); kputs("\n");
    kputs("  R8 ="); kputhex64(f->r8);  kputs("  R9 ="); kputhex64(f->r9);  kputs("\n");
    kputs("  R10="); kputhex64(f->r10); kputs("  R11="); kputhex64(f->r11); kputs("\n");
    kputs("  R12="); kputhex64(f->r12); kputs("  R13="); kputhex64(f->r13); kputs("\n");
    kputs("  R14="); kputhex64(f->r14); kputs("  R15="); kputhex64(f->r15); kputs("\n");
    kputs("  RIP="); kputhex64(f->rip); kputs("  RFLAGS="); kputhex64(f->rflags); kputs("\n");
    kputs("  CS=");  kputhex16((uint16_t)f->cs);
    kputs("  SS=");  kputhex16((uint16_t)f->ss);
    kputs("  ERR="); kputhex64(f->error_code); kputs("\n");
}

static void print_pf_info(const InterruptFrame *f)
{
    uint64_t ec = f->error_code;
    kputs("  CR2="); kputhex64(read_cr2()); kputs(" (fault address)\n");
    kputs("  PF flags: ");
    kputs((ec & 1) ? "PROTECTION-VIOLATION" : "NOT-PRESENT");
    kputs(" | "); kputs((ec & 2) ? "WRITE" : "READ");
    kputs(" | "); kputs((ec & 4) ? "USER" : "SUPERVISOR");
    if (ec & 8)  kputs(" | RESERVED-BIT");
    if (ec & 16) kputs(" | INSTRUCTION-FETCH");
    kputs("\n");
}

static void print_gp_info(const InterruptFrame *f)
{
    uint64_t ec = f->error_code;
    if (ec == 0) {
        kputs("  GP source: non-segment (or undetected)\n");
    } else {
        static const char *tbl[] = { "GDT", "IDT", "LDT", "IDT" };
        kputs("  GP error code="); kputhex64(ec);
        kputs(" table="); kputs(tbl[ec & 0x3]);
        kputs(" index="); kputhex16((uint16_t)((ec >> 3) & 0x1FFF));
        kputs("\n");
    }
}

/* ── Public dispatcher ────────────────────────────────────────────────────*/

void isr_handler(InterruptFrame *frame)
{
    uint64_t vec = frame->vector;

    /* ── Registered handler path ── */
    if (g_handlers[vec]) {
        g_handlers[vec](frame);
        /* Send EOI after the callback for hardware IRQ vectors. */
        if (vec >= 32 && vec <= 47)
            pic_send_eoi((uint8_t)(vec - 32));
        return;
    }

    /* ── Unregistered hardware IRQ: acknowledge and return silently ── */
    if (vec >= 32) {
        if (vec <= 47)
            pic_send_eoi((uint8_t)(vec - 32));
        return;
    }

    /* ── Unregistered CPU exception: panic screen ── */
    fb_clear(COLOR_BLACK);
    fb_set_cursor(0, 0);

    kputs("=====================================\n");
    kputs("         KERNEL EXCEPTION\n");
    kputs("=====================================\n");

    kputs(exception_names[vec]);
    kputs("  [vector="); kputhex64(vec); kputs("]\n\n");

    switch (vec) {
    case 13: print_gp_info(frame); break;
    case 14: print_pf_info(frame); break;
    default: break;
    }

    print_register_dump(frame);
    kputs("\nSystem halted.\n");
    kputs("=====================================\n");

    panic_halt();
}
