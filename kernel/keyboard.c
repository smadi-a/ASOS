/*
 * kernel/keyboard.c — PS/2 keyboard driver (Scan Code Set 1, US QWERTY).
 *
 * Make codes (key down) below 0x80 are translated to ASCII using two
 * tables: one for unshifted, one for shifted.  Break codes (key up) have
 * bit 7 set.
 *
 * All key presses and releases are posted to the focused window's process
 * event queue as EVENT_KEY_PRESS / EVENT_KEY_RELEASE.  Non-ASCII keys
 * (arrows, F-keys, modifiers) use ASOS_KEY_* codes (0x80+).  ASCII keys
 * are also written to the legacy ring buffer for shell/sys_read compat.
 *
 * The scancode MUST be read from 0x60 on every IRQ 1 even if we discard
 * the result — the keyboard controller will not send another interrupt
 * until the byte is consumed.
 */

#include "keyboard.h"
#include "ring_buffer.h"
#include "scheduler.h"
#include "framebuffer.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "serial.h"
#include "wm.h"
#include <stdint.h>
#include <stdbool.h>

/* ── PS/2 ports ───────────────────────────────────────────────────────────*/

#define KBD_DATA   0x60   /* read: scancode, write: command */
#define KBD_STATUS 0x64   /* read: status,   write: command */

#define KBD_STATUS_IBF  (1 << 1)   /* input buffer full — wait until clear */

/* ── ASOS extended key codes (must match i_video_asos.c) ─────────────────*/

#define ASOS_KEY_UP      0x80
#define ASOS_KEY_DOWN    0x81
#define ASOS_KEY_LEFT    0x82
#define ASOS_KEY_RIGHT   0x83
#define ASOS_KEY_F1      0x84
#define ASOS_KEY_F2      0x85
#define ASOS_KEY_F3      0x86
#define ASOS_KEY_F4      0x87
#define ASOS_KEY_F5      0x88
#define ASOS_KEY_F6      0x89
#define ASOS_KEY_F7      0x8A
#define ASOS_KEY_F8      0x8B
#define ASOS_KEY_F9      0x8C
#define ASOS_KEY_F10     0x8D
#define ASOS_KEY_F11     0x8E
#define ASOS_KEY_F12     0x8F
#define ASOS_KEY_LSHIFT  0x90
#define ASOS_KEY_RSHIFT  0x91
#define ASOS_KEY_LCTRL   0x92
#define ASOS_KEY_RCTRL   0x93
#define ASOS_KEY_LALT    0x94
#define ASOS_KEY_RALT    0x95
#define ASOS_KEY_PAUSE   0x96

/* ── Scan Code Set 1 → ASCII tables ──────────────────────────────────────
 *
 * Index = make code (0x00–0x58).  0x00 = unmapped (no ASCII output).
 * Backspace → '\b', Enter → '\n', Tab → '\t'.
 */

static const char g_sc_normal[89] = {
/*00*/  0,    27,   '1',  '2',  '3',  '4',  '5',  '6',
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
/*00*/  0,    27,   '!',  '@',  '#',  '$',  '%',  '^',
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

/*
 * Scancode → ASOS_KEY_* for non-ASCII keys (normal scancodes, no E0).
 * Returns 0 if the scancode has an ASCII mapping instead.
 */
static uint16_t scancode_to_asos_key(uint8_t make)
{
    switch (make) {
    case 0x3B: return ASOS_KEY_F1;
    case 0x3C: return ASOS_KEY_F2;
    case 0x3D: return ASOS_KEY_F3;
    case 0x3E: return ASOS_KEY_F4;
    case 0x3F: return ASOS_KEY_F5;
    case 0x40: return ASOS_KEY_F6;
    case 0x41: return ASOS_KEY_F7;
    case 0x42: return ASOS_KEY_F8;
    case 0x43: return ASOS_KEY_F9;
    case 0x44: return ASOS_KEY_F10;
    case 0x57: return ASOS_KEY_F11;
    case 0x58: return ASOS_KEY_F12;
    case 0x38: return ASOS_KEY_LALT;
    default:   return 0;
    }
}

/*
 * E0-prefixed scancode → ASOS_KEY_* for extended keys.
 * Returns 0 for unknown E0 codes.
 */
static uint16_t e0_scancode_to_asos_key(uint8_t make)
{
    switch (make) {
    case 0x48: return ASOS_KEY_UP;
    case 0x50: return ASOS_KEY_DOWN;
    case 0x4B: return ASOS_KEY_LEFT;
    case 0x4D: return ASOS_KEY_RIGHT;
    case 0x1D: return ASOS_KEY_RCTRL;
    case 0x38: return ASOS_KEY_RALT;
    default:   return 0;
    }
}

/* ── Driver state ─────────────────────────────────────────────────────────*/

static ring_buffer_t g_kbd_buf;
static bool g_shift  = false;
static bool g_ctrl   = false;
static bool g_e0     = false;  /* Next scancode is E0-prefixed */

/*
 * Post a keyboard event to the focused window's process event queue.
 * Called from IRQ context (interrupts already disabled).
 */
static void post_key_event(uint8_t type, uint16_t code)
{
    event_t evt;
    evt.type = type;
    evt._pad = 0;
    evt.x    = 0;
    evt.y    = 0;
    evt.code = code;
    wm_push_event_to_focused(&evt);
}

/* ── IRQ 1 handler ────────────────────────────────────────────────────────*/

static void kbd_irq_handler(InterruptFrame *frame)
{
    (void)frame;

    uint8_t sc = inb(KBD_DATA);   /* MUST read even if unused */

    /* E0 prefix: set flag and wait for the actual scancode next IRQ. */
    if (sc == 0xE0) {
        g_e0 = true;
        return;
    }

    /* E1 prefix (Pause key): consume and ignore the remaining bytes.
     * Pause sends E1 1D 45 E1 9D C5 — we just discard them. */
    if (sc == 0xE1) {
        return;
    }

    bool is_break = (sc & 0x80) != 0;
    uint8_t make  = sc & 0x7F;

    /* ── Handle E0-prefixed extended scancodes ─────────────────────── */
    if (g_e0) {
        g_e0 = false;

        uint16_t asos_key = e0_scancode_to_asos_key(make);

        /* Track right Ctrl state. */
        if (make == 0x1D) {
            g_ctrl = !is_break;
        }

        if (asos_key) {
            post_key_event(is_break ? EVENT_KEY_RELEASE : EVENT_KEY_PRESS,
                           asos_key);
        }
        return;
    }

    /* ── Track modifier keys and post events for them ─────────────── */

    /* Left Shift (0x2A). */
    if (make == 0x2A) {
        g_shift = !is_break;
        post_key_event(is_break ? EVENT_KEY_RELEASE : EVENT_KEY_PRESS,
                       ASOS_KEY_LSHIFT);
        return;
    }

    /* Right Shift (0x36). */
    if (make == 0x36) {
        g_shift = !is_break;
        post_key_event(is_break ? EVENT_KEY_RELEASE : EVENT_KEY_PRESS,
                       ASOS_KEY_RSHIFT);
        return;
    }

    /* Left Ctrl (0x1D). */
    if (make == 0x1D) {
        g_ctrl = !is_break;
        post_key_event(is_break ? EVENT_KEY_RELEASE : EVENT_KEY_PRESS,
                       ASOS_KEY_LCTRL);
        return;
    }

    /* ── Non-ASCII special keys (F-keys, Alt, etc.) ───────────────── */

    uint16_t asos_key = scancode_to_asos_key(make);
    if (asos_key) {
        post_key_event(is_break ? EVENT_KEY_RELEASE : EVENT_KEY_PRESS,
                       asos_key);
        return;
    }

    /* ── ASCII-mapped keys ────────────────────────────────────────── */

    if (make >= sizeof(g_sc_normal))
        return;

    char ascii = g_shift ? g_sc_shifted[make] : g_sc_normal[make];
    if (ascii == 0)
        return;   /* unmapped key */

    /* Post press/release event with ASCII code. */
    post_key_event(is_break ? EVENT_KEY_RELEASE : EVENT_KEY_PRESS,
                   (uint16_t)(uint8_t)ascii);

    /* For key presses only: legacy ring buffer + Ctrl+C handling. */
    if (!is_break) {
        /* Ctrl+C detection: scancode 0x2E = 'c' key. */
        if (g_ctrl && make == 0x2E) {
            keyboard_signal_interrupt();
            return;
        }

        ring_buffer_write(&g_kbd_buf, (uint8_t)ascii);
    }
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

void keyboard_signal_interrupt(void)
{
    /* Echo ^C to framebuffer and serial. */
    serial_puts("^C\n");
    fb_puts("^C\n", COLOR_WHITE, COLOR_BLACK);

    /* Find the foreground process and set its pending signal. */
    /* Walk PIDs — we're in IRQ context so keep it simple. */
    for (uint64_t pid = 1; pid < 1000; pid++) {
        task_t *t = scheduler_find_task_by_pid(pid);
        if (!t) continue;
        if (t->state == TASK_DEAD) continue;
        if (t->is_foreground) {
            t->pending_signal = 0x03;  /* ETX / SIGINT */
            serial_puts("[KBD] Ctrl+C sent to foreground process\n");
            return;
        }
    }

    /* No foreground process — put ETX in the keyboard buffer so
     * the shell's gets_s can handle it. */
    ring_buffer_write(&g_kbd_buf, 0x03);
}
