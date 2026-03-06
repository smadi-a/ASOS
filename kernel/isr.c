/*
 * kernel/isr.c — C-side interrupt/exception dispatcher.
 *
 * isr_handler() is called from the assembly common stub (isr_stubs.asm)
 * with a pointer to the saved InterruptFrame on the stack.
 *
 * For known fatal exceptions it prints a detailed register dump to both
 * the serial port and the framebuffer, then halts.
 *
 * For unrecognised vectors it prints a brief notice and halts.
 */

#include "isr.h"
#include "serial.h"
#include "framebuffer.h"
#include <stdint.h>

/* ── Low-level output helpers ─────────────────────────────────────────────
 *
 * We cannot use printf (no libc), so we roll a minimal hex formatter and
 * a dual-output helper that writes to both serial and framebuffer.
 *
 * The framebuffer cursor is managed via fb_set_cursor() / fb_puts()
 * declared in framebuffer.h.
 */

static const char hex_chars[] = "0123456789ABCDEF";

/* Write a 64-bit value as "0x" + 16 hex digits into buf[19]. */
static void fmt_hex64(uint64_t v, char *buf)
{
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex_chars[(v >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
}

/* Write a 16-bit value as "0x" + 4 hex digits into buf[7]. */
static void fmt_hex16(uint16_t v, char *buf)
{
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 4; i++)
        buf[2 + i] = hex_chars[(v >> (12 - i * 4)) & 0xF];
    buf[6] = '\0';
}

/* Print a string to both serial and framebuffer (at current fb cursor). */
static void kputs(const char *s)
{
    serial_puts(s);
    fb_puts(s, COLOR_WHITE, COLOR_BLACK);
}

/* Print a 64-bit hex value to both outputs. */
static void kputhex64(uint64_t v)
{
    char buf[19];
    fmt_hex64(v, buf);
    kputs(buf);
}

/* Print a 16-bit hex value to both outputs. */
static void kputhex16(uint16_t v)
{
    char buf[7];
    fmt_hex16(v, buf);
    kputs(buf);
}

/* Spin forever with interrupts disabled — the kernel is unrecoverable. */
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

/* Read CR2 (page-fault linear address). */
static uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

/* ── Exception name table ─────────────────────────────────────────────────*/

static const char *exception_names[32] = {
    "#DE  Division Error",                     /* 0  */
    "#DB  Debug",                              /* 1  */
    "NMI  Non-Maskable Interrupt",             /* 2  */
    "#BP  Breakpoint",                         /* 3  */
    "#OF  Overflow",                           /* 4  */
    "#BR  Bound Range Exceeded",               /* 5  */
    "#UD  Invalid Opcode",                     /* 6  */
    "#NM  Device Not Available",               /* 7  */
    "#DF  Double Fault",                       /* 8  */
    "     Coprocessor Segment Overrun",        /* 9  */
    "#TS  Invalid TSS",                        /* 10 */
    "#NP  Segment Not Present",                /* 11 */
    "#SS  Stack-Segment Fault",                /* 12 */
    "#GP  General Protection Fault",           /* 13 */
    "#PF  Page Fault",                         /* 14 */
    "     Reserved",                           /* 15 */
    "#MF  x87 FPU Floating-Point Error",       /* 16 */
    "#AC  Alignment Check",                    /* 17 */
    "#MC  Machine Check",                      /* 18 */
    "#XM  SIMD Floating-Point Exception",      /* 19 */
    "#VE  Virtualisation Exception",           /* 20 */
    "#CP  Control Protection Exception",       /* 21 */
    "     Reserved",                           /* 22 */
    "     Reserved",                           /* 23 */
    "     Reserved",                           /* 24 */
    "     Reserved",                           /* 25 */
    "     Reserved",                           /* 26 */
    "     Reserved",                           /* 27 */
    "     Reserved",                           /* 28 */
    "#VC  VMM Communication Exception",        /* 29 */
    "#SX  Security Exception",                 /* 30 */
    "     Reserved",                           /* 31 */
};

/* ── Register dump ────────────────────────────────────────────────────────*/

static void print_register_dump(const InterruptFrame *f)
{
    char buf[19];

    kputs("  RAX="); kputhex64(f->rax);
    kputs("  RBX="); kputhex64(f->rbx);
    kputs("\n");

    kputs("  RCX="); kputhex64(f->rcx);
    kputs("  RDX="); kputhex64(f->rdx);
    kputs("\n");

    kputs("  RSI="); kputhex64(f->rsi);
    kputs("  RDI="); kputhex64(f->rdi);
    kputs("\n");

    kputs("  RBP="); kputhex64(f->rbp);
    kputs("  RSP="); kputhex64(f->rsp);
    kputs("\n");

    kputs("  R8 ="); kputhex64(f->r8);
    kputs("  R9 ="); kputhex64(f->r9);
    kputs("\n");

    kputs("  R10="); kputhex64(f->r10);
    kputs("  R11="); kputhex64(f->r11);
    kputs("\n");

    kputs("  R12="); kputhex64(f->r12);
    kputs("  R13="); kputhex64(f->r13);
    kputs("\n");

    kputs("  R14="); kputhex64(f->r14);
    kputs("  R15="); kputhex64(f->r15);
    kputs("\n");

    kputs("  RIP="); kputhex64(f->rip);
    kputs("  RFLAGS="); kputhex64(f->rflags);
    kputs("\n");

    kputs("  CS=");  kputhex16((uint16_t)f->cs);
    kputs("  SS=");  kputhex16((uint16_t)f->ss);
    kputs("  ERR="); kputhex64(f->error_code);
    kputs("\n");

    (void)buf; /* suppress unused warning */
}

/* ── Page-fault error code decoder ───────────────────────────────────────*/

static void print_pf_info(const InterruptFrame *f)
{
    uint64_t ec = f->error_code;
    kputs("  CR2="); kputhex64(read_cr2());
    kputs(" (fault address)\n");

    kputs("  PF flags: ");
    kputs((ec & (1 << 0)) ? "PROTECTION-VIOLATION" : "NOT-PRESENT");
    kputs(" | ");
    kputs((ec & (1 << 1)) ? "WRITE" : "READ");
    kputs(" | ");
    kputs((ec & (1 << 2)) ? "USER" : "SUPERVISOR");
    if (ec & (1 << 3)) kputs(" | RESERVED-BIT");
    if (ec & (1 << 4)) kputs(" | INSTRUCTION-FETCH");
    kputs("\n");
}

/* ── General-protection fault decoder ────────────────────────────────────*/

static void print_gp_info(const InterruptFrame *f)
{
    uint64_t ec = f->error_code;
    if (ec == 0) {
        kputs("  GP source: non-segment (or undetected)\n");
    } else {
        kputs("  GP error code="); kputhex64(ec);
        /* bits 2:0 describe the table: 00=GDT, 01=IDT, 10=LDT, 11=IDT */
        static const char *tbl[] = { "GDT", "IDT", "LDT", "IDT" };
        kputs(" table="); kputs(tbl[ec & 0x3]);
        kputs(" index="); kputhex16((uint16_t)((ec >> 3) & 0x1FFF));
        kputs("\n");
    }
}

/* ── Public dispatcher ────────────────────────────────────────────────────*/

void isr_handler(InterruptFrame *frame)
{
    uint64_t vec = frame->vector;

    /* Reset framebuffer to top-left for the panic screen. */
    fb_clear(COLOR_BLACK);
    fb_set_cursor(0, 0);

    /* ── Header ── */
    kputs("=====================================\n");
    kputs("         KERNEL EXCEPTION\n");
    kputs("=====================================\n");

    if (vec < 32) {
        kputs(exception_names[vec]);
    } else {
        kputs("Interrupt vector ");
        kputhex64(vec);
    }
    kputs("  [vector="); kputhex64(vec); kputs("]\n\n");

    /* ── Vector-specific extra information ── */
    switch (vec) {
    case 13: /* #GP */
        print_gp_info(frame);
        break;
    case 14: /* #PF */
        print_pf_info(frame);
        break;
    default:
        break;
    }

    /* ── Register dump ── */
    print_register_dump(frame);

    kputs("\nSystem halted.\n");
    kputs("=====================================\n");

    panic_halt();
}
