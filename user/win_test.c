/*
 * win_test.c — ASOS window manager test application.
 *
 * Creates a window via SYS_WIN_CREATE, then animates a bouncing orange
 * square inside it, calling SYS_WIN_UPDATE + SYS_GFX_FLUSH each frame.
 *
 * Build: part of the normal `make` run; installed as WINTEST.ELF.
 * Run  : from the shell, type: WINTEST
 */

#include <stdint.h>
#include <unistd.h>
#include <gfx.h>
#include <sys/syscall.h>

/* ── Window client dimensions ──────────────────────────────────────────── */

#define WIN_W  200
#define WIN_H  150

/*
 * Pixel buffer lives in BSS (static) so it does not eat stack space.
 * 200 * 150 * 4 = 120,000 bytes ≈ 117 KB.
 */
static uint32_t s_pixels[WIN_W * WIN_H];

/* ── Syscall wrappers ───────────────────────────────────────────────────── */

/*
 * win_create — create a WM window.
 *   title : NUL-terminated window title
 *   x, y  : top-left position (includes title bar)
 *   w, h  : client area size in pixels
 * Returns window ID on success, < 0 on failure.
 */
static inline long win_create(const char *title, int x, int y, int w, int h)
{
    return __syscall5(SYS_WIN_CREATE,
                      (uint64_t)(uintptr_t)title,
                      (uint64_t)(int64_t)x,
                      (uint64_t)(int64_t)y,
                      (uint64_t)(int64_t)w,
                      (uint64_t)(int64_t)h);
}

/*
 * win_update — upload pixel data to a window's kernel buffer.
 *   win_id : ID returned by win_create
 *   pixels : array of w*h 0xAARRGGBB values
 */
static inline long win_update(int win_id, const uint32_t *pixels)
{
    return __syscall2(SYS_WIN_UPDATE,
                      (uint64_t)(uint32_t)win_id,
                      (uint64_t)(uintptr_t)pixels);
}

/* ── Rendering helpers ──────────────────────────────────────────────────── */

/* Fill the entire pixel buffer with a solid colour. */
static void buf_fill(uint32_t color)
{
    for (int i = 0; i < WIN_W * WIN_H; i++)
        s_pixels[i] = color;
}

/* Draw a filled rectangle into the pixel buffer.  Clips to buffer bounds. */
static void buf_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= WIN_H) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= WIN_W) continue;
            s_pixels[row * WIN_W + col] = color;
        }
    }
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    /* Create the window.  Title string is in .rodata, which is mapped
     * into user pages by the ELF loader — safe to pass to the kernel. */
    long win_id = win_create("ASOS Test Window", 100, 80, WIN_W, WIN_H);
    if (win_id < 0)
        return 1;

    /* Bouncing square state. */
    int bx = 10, by = 10;
    int dx = 2,  dy = 2;
    int sq = 20;   /* square side length */

    for (;;) {
        /* Check for quit key ('q' or 'Q'). */
        int64_t key = __syscall0(SYS_KEY_POLL);
        if (key == 'q' || key == 'Q')
            break;

        /* Compute a slowly shifting background colour using bx/by. */
        uint32_t r = (uint32_t)((bx * 255) / WIN_W);
        uint32_t b = (uint32_t)((by * 255) / WIN_H);
        uint32_t bg = (r << 16) | 0x001800UL | b;   /* dark teal gradient */

        /* 1. Clear to background. */
        buf_fill(bg);

        /* 2. Draw the bouncing square in orange. */
        buf_fill_rect(bx, by, sq, sq, 0xFF8800UL);

        /* 3. Advance and bounce off edges. */
        bx += dx;
        by += dy;
        if (bx <= 0      || bx + sq >= WIN_W) { dx = -dx; bx += dx; }
        if (by <= 0      || by + sq >= WIN_H) { dy = -dy; by += dy; }

        /* 4. Push pixels to the kernel window buffer. */
        win_update((int)win_id, s_pixels);

        /* 5. Trigger the compositor and blit to screen. */
        gfx_flush_display();

        /* 6. Yield so other tasks get CPU time. */
        yield();
    }

    return 0;
}
