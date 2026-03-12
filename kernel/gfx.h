/*
 * kernel/gfx.h — Kernel graphics framebuffer library (public API).
 *
 * Provides a double-buffered 2D drawing layer on top of the UEFI GOP
 * framebuffer.  All draw calls write to an internal back buffer;
 * gfx_flush() copies it to the real hardware framebuffer in one shot.
 *
 * Initialise with gfx_init() after heap_init() has been called.
 */

#ifndef KERNEL_GFX_H
#define KERNEL_GFX_H

#include <stdint.h>

/*
 * gfx_init — Initialise the graphics library.
 *
 *   fb_addr      : virtual address of the hardware framebuffer
 *                  (physical address cast to void* via identity map)
 *   width        : display width in pixels
 *   height       : display height in pixels
 *   pitch        : bytes per scanline of the hardware framebuffer
 *   bpp          : bits per pixel (must be 32)
 *   pixel_format : 0 = BGR (UEFI default on QEMU), 1 = RGB
 *
 * Allocates the back buffer from the kernel heap (height * width * 4 bytes).
 * Must be called after heap_init().
 */
void gfx_init(void *fb_addr, uint32_t width, uint32_t height,
              uint32_t pitch, uint8_t bpp, uint32_t pixel_format);

/*
 * gfx_flush — Copy the entire back buffer to the hardware framebuffer.
 *
 * Must be called with the kernel page tables active (kernel CR3), since the
 * hardware framebuffer is accessed via the identity map.
 */
void gfx_flush(void);

/* ── Drawing primitives ───────────────────────────────────────────────── */

/* Fill the entire back buffer with color. */
void gfx_clear(uint32_t color);

/* Set a single pixel.  Clipped to screen bounds. */
void gfx_put_pixel(int x, int y, uint32_t color);

/* Draw a filled rectangle.  Clipped. */
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);

/* Draw an outlined (hollow) rectangle.  Clipped. */
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);

/* Draw a horizontal line of `len` pixels starting at (x, y). */
void gfx_hline(int x, int y, int len, uint32_t color);

/* Draw a vertical line of `len` pixels starting at (x, y). */
void gfx_vline(int x, int y, int len, uint32_t color);

/*
 * gfx_blit — Copy a src_w × src_h block of 32-bit pixels into the back
 * buffer at (dst_x, dst_y).  Source pixels are row-major, no padding
 * (stride = src_w * 4).  Colors in src must already be in native format
 * or ARGB — the blit copies them as-is (no per-pixel conversion).
 * Clipped to screen bounds.
 */
void gfx_blit(int dst_x, int dst_y,
              const uint32_t *src, int src_w, int src_h);

/* ── Bitmap font rendering ─────────────────────────────────────────────── */

/*
 * Draw one character from the embedded 8×16 bitmap font.
 * fg / bg are 0xAARRGGBB colours (alpha ignored).
 * Clipped to screen bounds.
 */
void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);

/*
 * Draw a NUL-terminated string using the 8×16 bitmap font.
 * Advances x by 8 per character; '\n' resets x to its initial value and
 * increments y by 16.  Does NOT scroll — characters past the bottom edge
 * are clipped.
 */
void gfx_draw_string(int x, int y, const char *s, uint32_t fg, uint32_t bg);

#endif /* KERNEL_GFX_H */
