/*
 * kernel/mouse.c — PS/2 mouse driver (standard 3-byte protocol).
 *
 * Initialisation sequence:
 *   1. Enable the auxiliary (mouse) PS/2 port.
 *   2. Configure the PS/2 controller to enable IRQ12 and the mouse clock.
 *   3. Send "set defaults" (0xF6) and "enable data reporting" (0xF4).
 *
 * The IRQ 12 handler accumulates 3-byte packets:
 *   Byte 0: buttons + sign/overflow bits (bit 3 must be set for sync).
 *   Byte 1: X movement delta.
 *   Byte 2: Y movement delta.
 *
 * Completed packets are pushed into a ring buffer as mouse_event_t
 * structs.  The kernel polls via mouse_read_event().
 */

#include "mouse.h"
#include "isr.h"
#include "pic.h"
#include "io.h"
#include "serial.h"
#include <stdint.h>
#include <stdbool.h>

/* ── PS/2 controller ports ───────────────────────────────────────────────*/

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define PS2_STATUS_OBF  (1 << 0)   /* output buffer full — data ready   */
#define PS2_STATUS_IBF  (1 << 1)   /* input buffer full  — wait to write */

/* ── Timeout for controller polling ──────────────────────────────────────*/

#define PS2_TIMEOUT 100000

static bool ps2_wait_write(void)
{
    for (int i = 0; i < PS2_TIMEOUT; i++) {
        if (!(inb(PS2_STATUS) & PS2_STATUS_IBF))
            return true;
    }
    return false;
}

static bool ps2_wait_read(void)
{
    for (int i = 0; i < PS2_TIMEOUT; i++) {
        if (inb(PS2_STATUS) & PS2_STATUS_OBF)
            return true;
    }
    return false;
}

/* Write a command byte to the PS/2 controller (port 0x64). */
static void ps2_cmd(uint8_t cmd)
{
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}

/* Write a data byte to the PS/2 data port (port 0x60). */
static void ps2_data_write(uint8_t data)
{
    ps2_wait_write();
    outb(PS2_DATA, data);
}

/* Read a byte from the PS/2 data port (port 0x60). */
static uint8_t ps2_data_read(void)
{
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* Send a byte to the mouse via the auxiliary port (0xD4 prefix). */
static void mouse_write(uint8_t byte)
{
    ps2_cmd(0xD4);
    ps2_data_write(byte);
}

/* Send a command to the mouse and wait for ACK (0xFA). */
static bool mouse_send_cmd(uint8_t cmd)
{
    mouse_write(cmd);
    uint8_t ack = ps2_data_read();
    return ack == 0xFA;
}

/* ── Mouse event ring buffer ─────────────────────────────────────────────
 *
 * Fixed-size circular buffer of mouse_event_t.  Uses uint8_t indices into
 * a 128-entry array — wrapping handled by masking with (size - 1).
 * Single-producer (IRQ 12) / single-consumer (kernel poll), no locks.
 */

#define MOUSE_BUF_SIZE  128   /* must be power of 2 */

static volatile mouse_event_t g_mouse_buf[MOUSE_BUF_SIZE];
static volatile uint8_t       g_mouse_head;   /* read  index */
static volatile uint8_t       g_mouse_tail;   /* write index */

static bool mouse_buf_write(const mouse_event_t *evt)
{
    uint8_t next_tail = (g_mouse_tail + 1) & (MOUSE_BUF_SIZE - 1);
    if (next_tail == g_mouse_head)
        return false;   /* full — drop event */
    g_mouse_buf[g_mouse_tail] = *evt;
    g_mouse_tail = next_tail;
    return true;
}

static bool mouse_buf_read(mouse_event_t *evt)
{
    if (g_mouse_head == g_mouse_tail)
        return false;   /* empty */
    *evt = g_mouse_buf[g_mouse_head];
    g_mouse_head = (g_mouse_head + 1) & (MOUSE_BUF_SIZE - 1);
    return true;
}

/* ── IRQ 12 handler ──────────────────────────────────────────────────────*/

static uint8_t g_packet[3];
static uint8_t g_cycle;   /* 0, 1, or 2 — which byte of the 3-byte packet */

static void mouse_irq_handler(InterruptFrame *frame)
{
    (void)frame;

    uint8_t byte = inb(PS2_DATA);   /* MUST read even if discarded */

    switch (g_cycle) {
    case 0:
        /* Byte 0: bit 3 (alignment/"always set") must be 1.
         * If not, the stream is out of sync — discard and stay at cycle 0.
         * Also discard if overflow bits (6 or 7) are set. */
        if (!(byte & 0x08))
            return;   /* re-sync: wait for a valid byte 0 */
        if (byte & 0xC0)
            return;   /* overflow — discard packet */
        g_packet[0] = byte;
        g_cycle = 1;
        break;

    case 1:
        g_packet[1] = byte;
        g_cycle = 2;
        break;

    case 2: {
        g_packet[2] = byte;
        g_cycle = 0;

        /* Construct the signed deltas from raw bytes + sign bits. */
        int32_t dx = (int32_t)g_packet[1];
        int32_t dy = (int32_t)g_packet[2];

        if (g_packet[0] & 0x10)   /* X sign bit */
            dx |= (int32_t)0xFFFFFF00;
        if (g_packet[0] & 0x20)   /* Y sign bit */
            dy |= (int32_t)0xFFFFFF00;

        mouse_event_t evt;
        evt.dx      = dx;
        evt.dy      = dy;
        evt.buttons = g_packet[0] & 0x07;

        mouse_buf_write(&evt);
        break;
    }
    }

    /* EOI is sent by isr_handler() after this returns. */
}

/* ── Public API ──────────────────────────────────────────────────────────*/

void mouse_init(void)
{
    /* 1. Enable auxiliary device (second PS/2 port). */
    ps2_cmd(0xA8);

    /* 2. Read controller config byte, enable IRQ12 + mouse clock. */
    ps2_cmd(0x20);
    uint8_t config = ps2_data_read();
    config |= (1 << 1);   /* bit 1: enable IRQ12 */
    config &= ~(1 << 5);  /* bit 5: clear = enable mouse clock */
    ps2_cmd(0x60);
    ps2_data_write(config);

    /* 3. Set defaults (0xF6) then enable data reporting (0xF4). */
    if (!mouse_send_cmd(0xF6))
        serial_puts("[MOUSE] WARN: no ACK for set-defaults\n");

    if (!mouse_send_cmd(0xF4))
        serial_puts("[MOUSE] WARN: no ACK for enable-reporting\n");

    /* 4. Flush any stale bytes left in the output buffer. */
    while (inb(PS2_STATUS) & PS2_STATUS_OBF)
        (void)inb(PS2_DATA);

    /* 5. Register IRQ 12 handler (vector 44) and unmask. */
    g_cycle = 0;
    isr_register_handler(44, mouse_irq_handler);   /* IRQ 12 = vector 44 */
    pic_unmask_irq(12);

    /* IRQ 12 is on the slave PIC — also need IRQ 2 (cascade) unmasked. */
    pic_unmask_irq(2);

    serial_puts("[MOUSE] PS/2 mouse initialized.\n");
}

bool mouse_read_event(mouse_event_t *evt)
{
    return mouse_buf_read(evt);
}

bool mouse_has_event(void)
{
    return g_mouse_head != g_mouse_tail;
}
