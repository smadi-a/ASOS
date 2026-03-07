/*
 * kernel/framebuffer.h — Linear framebuffer text renderer.
 *
 * Provides character and string rendering using the 8×8 bitmap font
 * (font.h / font.c).  Colours are expressed as 0x00RRGGBB 32-bit values;
 * the driver handles BGR ↔ RGB swapping transparently.
 *
 * The cursor-based API (fb_set_cursor / fb_puts) supports '\n' for
 * newlines, '\b' for backspace, and automatic scrolling when the cursor
 * reaches the bottom of the screen.
 */

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include "../shared/boot_info.h"

/* Predefined colours (0x00RRGGBB). */
#define COLOR_BLACK   0x00000000U
#define COLOR_WHITE   0x00FFFFFFU
#define COLOR_RED     0x00FF0000U
#define COLOR_GREEN   0x0000FF00U
#define COLOR_BLUE    0x000000FFU
#define COLOR_GRAY    0x00AAAAAAU
#define COLOR_YELLOW  0x00FFFF00U

/* Bind the framebuffer module to the GOP descriptor from BootInfo.
 * Must be called before any other fb_* function.                    */
void fb_init(const Framebuffer *fb);

/* Fill the entire screen with colour (0x00RRGGBB). */
void fb_clear(uint32_t colour);

/* Draw a single character at pixel position (px, py). */
void fb_putc_at(char c, uint32_t px, uint32_t py, uint32_t fg, uint32_t bg);

/* Draw a string starting at glyph cell (col, row).
 * '\n' advances to the next row and resets col to the initial column. */
void fb_puts_at(const char *s, uint32_t col, uint32_t row,
                uint32_t fg, uint32_t bg);

/* ── Cursor-based terminal output ──────────────────────────────────────── */

/* Set the internal glyph-cell cursor. */
void fb_set_cursor(uint32_t col, uint32_t row);

/* Get the current cursor column / row. */
uint32_t fb_get_cursor_col(void);
uint32_t fb_get_cursor_row(void);

/*
 * Print a string at the current cursor position, advancing the cursor.
 *   '\n'  — move to column 0 of the next row; scroll if at bottom.
 *   '\b'  — erase the previous character (move back, draw space, move back).
 *   Other — draw character and advance column; wrap+scroll at right edge.
 */
void fb_puts(const char *s, uint32_t fg, uint32_t bg);

#endif /* FRAMEBUFFER_H */
