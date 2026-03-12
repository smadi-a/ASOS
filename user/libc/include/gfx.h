/*
 * gfx.h — User-space graphics command interface.
 *
 * Mirrors shared/gfx.h so user programs can include it without needing
 * the shared/ directory on their include path.
 */

#ifndef _GFX_H
#define _GFX_H

#include <stdint.h>

/* ── GFX operation codes ───────────────────────────────────────────────── */

#define GFX_OP_CLEAR        0
#define GFX_OP_FILL_RECT    1
#define GFX_OP_DRAW_RECT    2
#define GFX_OP_HLINE        3
#define GFX_OP_VLINE        4
#define GFX_OP_PUT_PIXEL    5
#define GFX_OP_DRAW_STRING  6   /* x,y, color=fg, w=bg,
                                   ptr=char*, ptr_len=strlen+1 */
#define GFX_OP_BLIT         7   /* x,y, w=src_w, h=src_h,
                                   ptr=uint32_t pixels, ptr_len=w*h*4 */

/* ── GFX command packet ────────────────────────────────────────────────── */

typedef struct {
    uint32_t op;
    int32_t  x, y;
    int32_t  w, h;
    uint32_t color;
    uint64_t ptr;
    uint32_t ptr_len;
} GfxCmd;

/* ── User-space wrappers (syscalls 28 / 29) ────────────────────────────── */

/*
 * gfx_draw — Submit a GfxCmd to the kernel.
 * Returns 0 on success, -1 on error.
 */
int gfx_draw(const GfxCmd *cmd);

/*
 * gfx_flush_display — Copy the kernel back buffer to the hardware
 * framebuffer.  Call this after all drawing is done.
 * Returns 0 on success.
 */
int gfx_flush_display(void);

/*
 * gfx_screen_info — Query the display dimensions.
 * On success, *width and *height are filled in and 0 is returned.
 */
int gfx_screen_info(uint32_t *width, uint32_t *height);

/* ── Convenience helpers ───────────────────────────────────────────────── */

static inline int gfx_clear(uint32_t color)
{
    GfxCmd c = { GFX_OP_CLEAR, 0, 0, 0, 0, color, 0, 0 };
    return gfx_draw(&c);
}

static inline int gfx_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    GfxCmd c = { GFX_OP_FILL_RECT, x, y, w, h, color, 0, 0 };
    return gfx_draw(&c);
}

static inline int gfx_draw_rect(int x, int y, int w, int h, uint32_t color)
{
    GfxCmd c = { GFX_OP_DRAW_RECT, x, y, w, h, color, 0, 0 };
    return gfx_draw(&c);
}

static inline int gfx_put_pixel(int x, int y, uint32_t color)
{
    GfxCmd c = { GFX_OP_PUT_PIXEL, x, y, 0, 0, color, 0, 0 };
    return gfx_draw(&c);
}

static inline int gfx_hline(int x, int y, int len, uint32_t color)
{
    GfxCmd c = { GFX_OP_HLINE, x, y, len, 0, color, 0, 0 };
    return gfx_draw(&c);
}

static inline int gfx_vline(int x, int y, int len, uint32_t color)
{
    GfxCmd c = { GFX_OP_VLINE, x, y, 0, len, color, 0, 0 };
    return gfx_draw(&c);
}

/* Draw a string at (x,y) with fg color; bg defaults to black. */
static inline int gfx_puts(int x, int y, const char *s,
                            uint32_t fg, uint32_t bg)
{
    /* Compute length including NUL. */
    uint32_t len = 0;
    const char *p = s;
    while (*p++) len++;
    len++; /* include NUL */

    GfxCmd c;
    c.op      = GFX_OP_DRAW_STRING;
    c.x       = x;
    c.y       = y;
    c.w       = (int32_t)bg;   /* bg color in the w field */
    c.h       = 0;
    c.color   = fg;
    c.ptr     = (uint64_t)(uintptr_t)s;
    c.ptr_len = len;
    return gfx_draw(&c);
}

#endif /* _GFX_H */
