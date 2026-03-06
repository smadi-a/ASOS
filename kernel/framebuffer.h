/*
 * kernel/framebuffer.h — Linear framebuffer text renderer.
 *
 * Provides character and string rendering using the 8×8 bitmap font
 * (font.h / font.c).  Colours are expressed as 0x00RRGGBB 32-bit values;
 * the driver handles BGR ↔ RGB swapping transparently.
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

/* ── Cursor-based output (used by the exception/panic printer) ─────────── */

/* Set the internal glyph-cell cursor.  Subsequent fb_puts() calls start here. */
void fb_set_cursor(uint32_t col, uint32_t row);

/* Print a string at the current cursor position, advancing the cursor.
 * '\n' moves to column 0 of the next row.                             */
void fb_puts(const char *s, uint32_t fg, uint32_t bg);

#endif /* FRAMEBUFFER_H */
