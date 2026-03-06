/*
 * kernel/framebuffer.c — Linear framebuffer text renderer implementation.
 *
 * Pixel colour format
 * ───────────────────
 * The API accepts colours as 0x00RRGGBB (the conventional "web" order).
 * When the GOP reports BGR layout the driver swaps the R and B bytes
 * before writing to memory so the caller never needs to care.
 *
 * Coordinate system
 * ─────────────────
 * Pixel (0,0) is the top-left corner.
 * Positive x goes right; positive y goes down.
 *
 * Terminal cursor
 * ───────────────
 * fb_puts() maintains a (col, row) cursor in glyph-cell coordinates.
 * '\n' moves to the next row; '\b' erases the previous character.
 * When the cursor advances past the last row, all content is scrolled
 * up by one text row (FONT_HEIGHT pixels) using memmove, and the last
 * row is cleared.
 */

#include "framebuffer.h"
#include "font.h"
#include "string.h"
#include <stdint.h>

/* Module-private state, populated by fb_init(). */
static uint64_t    g_base;    /* Physical address of pixel (0,0)     */
static uint32_t    g_width;   /* Screen width in pixels               */
static uint32_t    g_height;  /* Screen height in pixels              */
static uint32_t    g_pitch;   /* Bytes per scanline                   */
static PixelFormat g_format;  /* PIXEL_FORMAT_RGB or PIXEL_FORMAT_BGR */

/* Text grid dimensions (computed from resolution and font size). */
static uint32_t g_cols;       /* Characters per row                   */
static uint32_t g_rows;       /* Text rows on screen                  */

/* Cursor state for the terminal API. */
static uint32_t g_cursor_col = 0;
static uint32_t g_cursor_row = 0;

/* ── Internal helpers ─────────────────────────────────────────────────────*/

static inline void put_pixel(uint32_t px, uint32_t py, uint32_t colour)
{
    if (px >= g_width || py >= g_height)
        return;

    uint32_t *pixel = (uint32_t *)(uintptr_t)(g_base + (uint64_t)py * g_pitch
                                               + (uint64_t)px * 4U);

    if (g_format == PIXEL_FORMAT_BGR) {
        *pixel = colour;
    } else {
        uint8_t r = (uint8_t)((colour >> 16) & 0xFFU);
        uint8_t g = (uint8_t)((colour >>  8) & 0xFFU);
        uint8_t b = (uint8_t)( colour        & 0xFFU);
        *pixel = ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    }
}

/*
 * Scroll the entire screen up by one text row (FONT_HEIGHT pixels).
 * The vacated bottom row is cleared to black.
 * The cursor row is decremented to stay on the last visible row.
 */
static void fb_scroll(void)
{
    uint32_t row_bytes    = g_pitch;
    uint32_t scroll_bytes = (g_rows - 1) * FONT_HEIGHT * row_bytes;

    uint8_t *dst = (uint8_t *)(uintptr_t)g_base;
    uint8_t *src = dst + (uint64_t)FONT_HEIGHT * row_bytes;

    memmove(dst, src, scroll_bytes);

    /* Clear the last text row. */
    uint8_t *last = dst + (uint64_t)(g_rows - 1) * FONT_HEIGHT * row_bytes;
    memset(last, 0, (uint64_t)FONT_HEIGHT * row_bytes);

    if (g_cursor_row > 0)
        g_cursor_row--;
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void fb_init(const Framebuffer *fb)
{
    g_base   = fb->base;
    g_width  = fb->width;
    g_height = fb->height;
    g_pitch  = fb->pitch;
    g_format = fb->format;
    g_cols   = g_width  / FONT_WIDTH;
    g_rows   = g_height / FONT_HEIGHT;
}

void fb_clear(uint32_t colour)
{
    for (uint32_t y = 0; y < g_height; y++) {
        uint32_t *row = (uint32_t *)(uintptr_t)(g_base + (uint64_t)y * g_pitch);
        for (uint32_t x = 0; x < g_width; x++)
            row[x] = colour;
    }
}

void fb_putc_at(char c, uint32_t px, uint32_t py, uint32_t fg, uint32_t bg)
{
    uint8_t ch = (uint8_t)c;
    if (ch >= 128) ch = (uint8_t)'?';

    const uint8_t *glyph = font_glyphs[ch];

    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            uint32_t colour = (bits & (0x80U >> col)) ? fg : bg;
            put_pixel(px + col, py + row, colour);
        }
    }
}

void fb_puts_at(const char *s, uint32_t col, uint32_t row,
                uint32_t fg, uint32_t bg)
{
    uint32_t start_col = col;

    while (*s) {
        char c = *s++;
        if (c == '\n') {
            row++;
            col = start_col;
            continue;
        }
        fb_putc_at(c, col * FONT_WIDTH, row * FONT_HEIGHT, fg, bg);
        col++;
    }
}

void fb_set_cursor(uint32_t col, uint32_t row)
{
    g_cursor_col = col;
    g_cursor_row = row;
}

uint32_t fb_get_cursor_col(void) { return g_cursor_col; }
uint32_t fb_get_cursor_row(void) { return g_cursor_row; }

void fb_puts(const char *s, uint32_t fg, uint32_t bg)
{
    while (*s) {
        char c = *s++;

        if (c == '\n') {
            g_cursor_col = 0;
            g_cursor_row++;
            if (g_cursor_row >= g_rows)
                fb_scroll();

        } else if (c == '\b') {
            if (g_cursor_col > 0) {
                g_cursor_col--;
                fb_putc_at(' ', g_cursor_col * FONT_WIDTH,
                                g_cursor_row * FONT_HEIGHT, fg, bg);
            }

        } else {
            fb_putc_at(c, g_cursor_col * FONT_WIDTH,
                          g_cursor_row * FONT_HEIGHT, fg, bg);
            g_cursor_col++;
            if (g_cursor_col >= g_cols) {
                g_cursor_col = 0;
                g_cursor_row++;
                if (g_cursor_row >= g_rows)
                    fb_scroll();
            }
        }
    }
}
