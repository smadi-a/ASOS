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

/* Convenient predefined colours (0x00RRGGBB). */
#define COLOR_BLACK   0x00000000U
#define COLOR_WHITE   0x00FFFFFFU
#define COLOR_RED     0x00FF0000U
#define COLOR_GREEN   0x0000FF00U
#define COLOR_BLUE    0x000000FFU
#define COLOR_GRAY    0x00AAAAADU

/*
 * fb_init — bind the framebuffer module to the GOP descriptor passed by
 * the bootloader.  Must be called before any other fb_* function.
 */
void fb_init(const Framebuffer *fb);

/* Fill the entire screen with the given colour (0x00RRGGBB). */
void fb_clear(uint32_t colour);

/*
 * fb_putc_at — draw a single ASCII character at pixel position (px, py).
 *
 *   px, py  — top-left pixel of the 8×8 glyph cell
 *   fg, bg  — foreground and background colours (0x00RRGGBB)
 */
void fb_putc_at(char c, uint32_t px, uint32_t py, uint32_t fg, uint32_t bg);

/*
 * fb_puts_at — draw a NUL-terminated string starting at glyph cell
 * (col, row), where each cell is FONT_WIDTH × FONT_HEIGHT pixels.
 *
 * Newlines ('\n') advance to the next row and reset the column to 0.
 */
void fb_puts_at(const char *s, uint32_t col, uint32_t row,
                uint32_t fg, uint32_t bg);

#endif /* FRAMEBUFFER_H */
