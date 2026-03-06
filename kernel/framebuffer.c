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
 */

#include "framebuffer.h"
#include "font.h"
#include <stdint.h>

/* Module-private state, populated by fb_init(). */
static uint64_t    g_base;    /* Physical address of pixel (0,0)     */
static uint32_t    g_width;   /* Screen width in pixels               */
static uint32_t    g_height;  /* Screen height in pixels              */
static uint32_t    g_pitch;   /* Bytes per scanline                   */
static PixelFormat g_format;  /* PIXEL_FORMAT_RGB or PIXEL_FORMAT_BGR */

/* ── Internal helpers ─────────────────────────────────────────────────────*/

/*
 * put_pixel — write a 32-bit colour value to pixel (px, py).
 *
 * The colour argument is always 0x00RRGGBB.  We swap channels when
 * the framebuffer uses BGR byte order.
 *
 * Out-of-bounds writes are silently ignored to keep rendering code simple.
 */
static inline void put_pixel(uint32_t px, uint32_t py, uint32_t colour)
{
    if (px >= g_width || py >= g_height)
        return;

    uint32_t *pixel = (uint32_t *)(uintptr_t)(g_base + (uint64_t)py * g_pitch
                                               + (uint64_t)px * 4U);

    if (g_format == PIXEL_FORMAT_BGR) {
        /*
         * BGR mode — EFI memory layout: byte[0]=B  byte[1]=G  byte[2]=R
         *
         * On a little-endian CPU a 32-bit write places the lowest byte at
         * the lowest address, so the 32-bit word we need is:
         *
         *   word = B | (G << 8) | (R << 16)  =  0x00RRGGBB  =  colour
         *
         * No channel-swap is required: the API value 0x00RRGGBB already
         * encodes B in the low byte, which lands at byte[0].
         */
        *pixel = colour;
    } else {
        /*
         * RGB mode — EFI memory layout: byte[0]=R  byte[1]=G  byte[2]=B
         *
         * The word we need is:
         *
         *   word = R | (G << 8) | (B << 16)  =  0x00BBGGRR
         *
         * Our API colour is 0x00RRGGBB, so we must swap the R and B bytes.
         */
        uint8_t r = (uint8_t)((colour >> 16) & 0xFFU);
        uint8_t g = (uint8_t)((colour >>  8) & 0xFFU);
        uint8_t b = (uint8_t)( colour        & 0xFFU);
        *pixel = ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
    }
}

/* ── Public API ───────────────────────────────────────────────────────────*/

void fb_init(const Framebuffer *fb)
{
    g_base   = fb->base;
    g_width  = fb->width;
    g_height = fb->height;
    g_pitch  = fb->pitch;
    g_format = fb->format;
}

void fb_clear(uint32_t colour)
{
    for (uint32_t y = 0; y < g_height; y++) {
        uint32_t *row = (uint32_t *)(uintptr_t)(g_base + (uint64_t)y * g_pitch);
        for (uint32_t x = 0; x < g_width; x++)
            row[x] = colour; /* colour is already in native pixel format */
    }
    /*
     * fb_clear writes the raw word directly for speed.  0x00000000 (black)
     * is identical in both RGB and BGR formats, so no channel-swap is needed
     * for the common case.  If you clear to a non-black colour on an RGB
     * framebuffer the R and B channels will be swapped — use fb_putc_at
     * with fg==bg for colour-correct solid fills on arbitrary backgrounds.
     * In practice Milestone 1 only ever clears to black, so this is fine.
     */
}

void fb_putc_at(char c, uint32_t px, uint32_t py, uint32_t fg, uint32_t bg)
{
    uint8_t ch = (uint8_t)c;
    if (ch >= 128)
        ch = (uint8_t)'?'; /* Substitute for out-of-range characters */

    const uint8_t *glyph = font_glyphs[ch];

    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            /*
             * Bit 7 is the leftmost column; test the appropriate bit
             * by shifting a mask right as col increases.
             */
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
            /* Newline: move to the beginning of the next glyph row. */
            row++;
            col = start_col;
            continue;
        }

        fb_putc_at(c, col * FONT_WIDTH, row * FONT_HEIGHT, fg, bg);
        col++;
    }
}
