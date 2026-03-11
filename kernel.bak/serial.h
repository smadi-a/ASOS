/*
 * kernel/serial.h — COM1 serial port interface.
 *
 * Provides a simple polled (busy-wait) UART driver for COM1 (0x3F8).
 * Useful for debugging before the framebuffer is available, and
 * throughout early kernel bringup.
 */

#ifndef SERIAL_H
#define SERIAL_H

/* Initialise COM1 at 115200 baud, 8 data bits, no parity, 1 stop bit. */
void serial_init(void);

/* Transmit a single byte, blocking until the UART is ready. */
void serial_putc(char c);

/*
 * Transmit a NUL-terminated string.
 * '\n' is automatically preceded by '\r' for CRLF-aware terminals.
 */
void serial_puts(const char *s);

#endif /* SERIAL_H */
