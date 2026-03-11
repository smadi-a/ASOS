/*
 * kernel/keyboard.h — PS/2 keyboard driver interface.
 *
 * Handles Scan Code Set 1 (the default set on x86 POST).
 * Key events are translated to ASCII and stored in a ring buffer.
 * The IRQ 1 handler is registered inside keyboard_init().
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdbool.h>

/* Initialise the PS/2 keyboard and register the IRQ 1 handler. */
void keyboard_init(void);

/*
 * Non-blocking read: retrieve one ASCII character from the input buffer.
 * Returns false if no character is waiting.
 */
bool keyboard_read_char(char *c);

/*
 * Blocking read: halts (with interrupts enabled) until a character
 * arrives, then returns it.
 */
char keyboard_wait_char(void);

#endif /* KEYBOARD_H */
