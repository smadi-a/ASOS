/*
 * kernel/serial.c — COM1 serial port driver (polled, 115200-8N1).
 *
 * The PC-compatible UART (16550A) at I/O base 0x3F8 (COM1).
 *
 * Baud rate calculation:
 *   The UART clock is 1.8432 MHz.
 *   The baud rate divisor = clock / (16 × desired_baud).
 *   For 115200 baud: divisor = 1 843 200 / (16 × 115 200) = 1.
 *
 * This driver is intentionally minimal: no interrupts, no FIFOs
 * (though we enable and flush them for stability), no flow control.
 */

#include "serial.h"
#include <stdint.h>

/* I/O base address of COM1. */
#define COM1  0x3F8U

/*
 * Register offsets from the COM1 base.
 * The meaning of offsets 0 and 1 depends on the Divisor Latch Access Bit
 * (DLAB) in the Line Control Register.
 */
#define COM_DATA  0   /* DLAB=0: receive/transmit data; DLAB=1: divisor low  */
#define COM_IER   1   /* DLAB=0: interrupt enable;      DLAB=1: divisor high */
#define COM_FCR   2   /* FIFO control (write-only)                            */
#define COM_LCR   3   /* Line control                                         */
#define COM_MCR   4   /* Modem control                                        */
#define COM_LSR   5   /* Line status (read-only)                              */

/* Line Control Register bit fields. */
#define LCR_8N1   0x03U   /* 8 data bits, no parity, 1 stop bit */
#define LCR_DLAB  0x80U   /* Enable Divisor Latch Access         */

/* Line Status Register bit fields. */
#define LSR_THRE  0x20U   /* Transmit Holding Register Empty — OK to send */

/* ── Low-level port I/O ───────────────────────────────────────────────────*/

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port) : "memory");
    return val;
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void serial_init(void)
{
    const uint16_t DIVISOR = 1; /* 115200 baud */

    outb(COM1 + COM_IER, 0x00);        /* Disable all UART interrupts         */
    outb(COM1 + COM_LCR, LCR_DLAB);   /* Enable DLAB to write baud divisor   */
    outb(COM1 + COM_DATA, DIVISOR & 0xFF);        /* Divisor low byte         */
    outb(COM1 + COM_IER,  (DIVISOR >> 8) & 0xFF); /* Divisor high byte        */
    outb(COM1 + COM_LCR, LCR_8N1);    /* 8N1 framing; also clears DLAB       */

    /*
     * FCR: enable FIFO, clear both RX and TX FIFOs, set 14-byte threshold.
     * Even though we poll, enabling the FIFO prevents the chip from
     * dropping characters if we have brief scheduling pauses later.
     */
    outb(COM1 + COM_FCR, 0xC7U);

    /*
     * MCR: assert RTS and DTR, enable OUT2 (required on PC hardware
     * to enable the IRQ line — harmless when not using interrupts).
     */
    outb(COM1 + COM_MCR, 0x0BU);

    outb(COM1 + COM_IER, 0x00);        /* Keep interrupts disabled            */
}

void serial_putc(char c)
{
    /* Spin until the transmit holding register is empty. */
    while (!(inb(COM1 + COM_LSR) & LSR_THRE))
        ;
    outb(COM1 + COM_DATA, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            serial_putc('\r'); /* Prepend CR so terminals render CRLF */
        serial_putc(*s++);
    }
}
