/*
 * kernel/keyboard.c — PS/2 keyboard driver (Scan Code Set 1, US QWERTY).
 *
 * Make codes (key down) below 0x80 are translated to ASCII using two
 * tables: one for unshifted, one for shifted.  Break codes (key up) have
 * bit 7 set; we only use them to track shift state.
 *
 * The scancode MUST be read from 0x60 on every IRQ 1 even if we discard
 * the result — the keyboard controller will not send another interrupt
 * until the byte is consumed.
 */

#include "keyboard.h"
#include "ring_buffer.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>

/* ── PS/2 ports ───────────────────────────────────────────────────────────*/

#define KBD_DATA   0x60   /* read: scancode, write: command */
#define KBD_STATUS 0x64   /* read: status,   write: command */

#define KBD_STATUS_IBF  (1 << 1)   /* input buffer full — wait until clear */

/* ── Scan Code Set 1 → ASCII tables ──────────────────────────────────────
 *
 * Index = make code (0x00–0x58).  0x00 = unmapped (no ASCII output).
 * Backspace → '\b', Enter → '\n', Tab → '\t'.
 */

static const char g_sc_normal[89] = {
/*00*/  0,    0,    '1',  '2',  '3',  '4',  '5',  '6',
/*08*/  '7',  '8',  '9',  '0',  '-',  '=',  '\b', '\t',
/*10*/  'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',
/*18*/  'o',  'p',  '[',  ']',  '\n', 0,    'a',  's',
/*20*/  'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',
/*28*/  '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',
/*30*/  'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',
/*38*/  0,    ' ',  0,    0,    0,    0,    0,    0,
/*40*/  0,    0,    0,    0,    0,    0,    0,    '7',
/*48*/  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
/*50*/  '2',  '3',  '0',  '.',  0,    0,    0,    0,
/*58*/  0
};

static const char g_sc_shifted[89] = {
/*00*/  0,    0,    '!',  '@',  '#',  '$',  '%',  '^',
/*08*/  '&',  '*',  '(',  ')',  '_',  '+',  '\b', '\t',
/*10*/  'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',
/*18*/  'O',  'P',  '{',  '}',  '\n', 0,    'A',  'S',
/*20*/  'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',
/*28*/  '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',
/*30*/  'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',
/*38*/  0,    ' ',  0,    0,    0,    0,    0,    0,
/*40*/  0,    0,    0,    0,    0,    0,    0,    '7',
/*48*/  '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',
/*50*/  '2',  '3',  '0',  '.',  0,    0,    0,    0,
/*58*/  0
};

/* ── Driver state ─────────────────────────────────────────────────────────*/

static ring_buffer_t g_kbd_buf;
static bool g_shift = false;

/* ── IRQ 1 handler ────────────────────────────────────────────────────────*/

static void kbd_irq_handler(InterruptFrame *frame)
{
    (void)frame;

    uint8_t sc = inb(KBD_DATA);   /* MUST read even if unused */

    bool is_break = (sc & 0x80) != 0;
    uint8_t make  = sc & 0x7F;

    /* Track shift keys (make = 0x2A left shift, 0x36 right shift). */
    if (make == 0x2A || make == 0x36) {
        g_shift = !is_break;
        return;
    }

    /* Ignore all break codes. */
    if (is_break)
        return;

    /* Translate make code to ASCII. */
    if (make >= sizeof(g_sc_normal))
        return;

    char ascii = g_shift ? g_sc_shifted[make] : g_sc_normal[make];
    if (ascii == 0)
        return;   /* unmapped key */

    ring_buffer_write(&g_kbd_buf, (uint8_t)ascii);
    /* EOI sent by isr_handler() after this returns. */
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void keyboard_init(void)
{
    /* Drain any stale byte in the output buffer. */
    if (inb(KBD_STATUS) & 0x01)
        (void)inb(KBD_DATA);

    /* Wait for input buffer empty, then send "enable scanning". */
    while (inb(KBD_STATUS) & KBD_STATUS_IBF)
        ;
    outb(KBD_DATA, 0xF4);

    isr_register_handler(33, kbd_irq_handler);   /* IRQ 1 = vector 33 */
    pic_unmask_irq(1);

    serial_puts("[KBD] PS/2 keyboard initialized.\n");
}

bool keyboard_read_char(char *c)
{
    uint8_t b;
    if (!ring_buffer_read(&g_kbd_buf, &b))
        return false;
    *c = (char)b;
    return true;
}

char keyboard_wait_char(void)
{
    char c;
    while (!keyboard_read_char(&c))
        __asm__ volatile ("hlt");
    return c;
}
