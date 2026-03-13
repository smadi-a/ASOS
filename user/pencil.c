/*
 * pencil.c — ASOS Pencil drawing application.
 *
 * A simple pixel drawing app with pencil and eraser modes.
 * Uses the asos/gui.h toolkit for windowing and event handling.
 *
 * Build: part of the normal `make` run; installed as PENCIL.ELF.
 * Run  : from the shell, type: PENCIL
 */

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <gfx.h>
#include <asos/gui.h>
#include <event.h>
#include <sys/syscall.h>

/* ── Layout constants ──────────────────────────────────────────────────── */

#define WIN_W       400
#define WIN_H       300

#define TOOLBAR_H   24
#define BTN_W       32
#define BTN_H       20
#define BTN_PAD     4
#define BTN_Y       2

/* Canvas area starts just below the toolbar. */
#define CANVAS_Y    TOOLBAR_H

/* ── Colours ───────────────────────────────────────────────────────────── */

#define COL_CANVAS_BG   0x00FFFFFFU   /* White canvas background         */
#define COL_TOOLBAR_BG  0x00808080U   /* Grey toolbar background         */
#define COL_ACTIVE_FACE 0x004488CCU   /* Blue — active tool button       */
#define COL_BTN_TEXT    0x00FFFFFFU   /* White text on buttons           */

/* ── Palette ──────────────────────────────────────────────────────────── */

#define PALETTE_COUNT   6
#define PAL_SWATCH_W    16
#define PAL_SWATCH_H    16
#define PAL_PAD         3
#define PAL_START_X     (BTN_PAD + (BTN_W + BTN_PAD) * 2 + 8)
#define PAL_Y           4

static const uint32_t s_palette[PALETTE_COUNT] = {
    0x00000000U,   /* Black  */
    0x0000AA00U,   /* Green  */
    0x00FF0000U,   /* Blue (BGRA) */
    0x000000FFU,   /* Red  (BGRA) */
    0x0000FFFFU,   /* Yellow (BGRA: G+R) */
    0x00808080U,   /* Grey   */
};

/* ── Drawing modes ─────────────────────────────────────────────────────── */

#define MODE_PENCIL  0
#define MODE_ERASER  1

/* ── State ─────────────────────────────────────────────────────────────── */

static gui_window_t  *s_win;
static gui_widget_t  *s_btn_pencil;
static gui_widget_t  *s_btn_eraser;

static int      s_mode       = MODE_PENCIL;
static int      s_pal_idx    = 0;            /* Default: black           */
static bool     s_drawing    = false;
static int      s_last_x     = -1;
static int      s_last_y     = -1;
static bool     s_dirty      = true;

/* ── Helpers ───────────────────────────────────────────────────────────── */

static uint32_t current_color(void)
{
    return (s_mode == MODE_PENCIL) ? s_palette[s_pal_idx] : COL_CANVAS_BG;
}

/* Plot a 2x2 square at (x, y) in the pixel buffer, clipped to canvas. */
static void plot(int x, int y)
{
    if (!s_win) return;
    uint32_t col = current_color();
    int w = s_win->width;
    int h = s_win->height;
    uint32_t *buf = s_win->buffer;

    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < w && py >= CANVAS_Y && py < h)
                buf[py * w + px] = col;
        }
    }
}

/* Bresenham line of 2x2 plots from (x0,y0) to (x1,y1). */
static void draw_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = (dx >= 0) ? 1 : -1;
    int sy = (dy >= 0) ? 1 : -1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    int err = dx - dy;

    for (;;) {
        plot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Update button highlight to reflect active mode. */
static void update_button_colors(void)
{
    if (s_btn_pencil) {
        s_btn_pencil->face_color = (s_mode == MODE_PENCIL) ? COL_ACTIVE_FACE : 0;
        s_btn_pencil->text_color = (s_mode == MODE_PENCIL) ? COL_BTN_TEXT    : 0;
    }
    if (s_btn_eraser) {
        s_btn_eraser->face_color = (s_mode == MODE_ERASER) ? COL_ACTIVE_FACE : 0;
        s_btn_eraser->text_color = (s_mode == MODE_ERASER) ? COL_BTN_TEXT    : 0;
    }
}

/* ── Button callbacks ──────────────────────────────────────────────────── */

static void on_pencil(gui_window_t *win, gui_widget_t *w)
{
    (void)win; (void)w;
    s_mode = MODE_PENCIL;
    update_button_colors();
    s_dirty = true;
}

static void on_eraser(gui_window_t *win, gui_widget_t *w)
{
    (void)win; (void)w;
    s_mode = MODE_ERASER;
    update_button_colors();
    s_dirty = true;
}

/* ── Render ─────────────────────────────────────────────────────────────── */

/*
 * render_frame — Redraw toolbar only (canvas pixels are persistent),
 * then push the whole buffer to the kernel WM.
 */
static void render_frame(void)
{
    uint32_t *buf = s_win->buffer;
    int w = s_win->width;

    /* Redraw toolbar background. */
    for (int y = 0; y < TOOLBAR_H; y++)
        for (int x = 0; x < w; x++)
            buf[y * w + x] = COL_TOOLBAR_BG;

    /* Draw toolbar buttons via the gui toolkit (3D beveled look). */
    draw_button_ex(s_win, BTN_PAD, BTN_Y, BTN_W, BTN_H, "P",
                   s_btn_pencil ? s_btn_pencil->pressed : 0,
                   s_btn_pencil ? s_btn_pencil->face_color : 0,
                   s_btn_pencil ? s_btn_pencil->text_color : 0);

    draw_button_ex(s_win, BTN_PAD + BTN_W + BTN_PAD, BTN_Y, BTN_W, BTN_H, "E",
                   s_btn_eraser ? s_btn_eraser->pressed : 0,
                   s_btn_eraser ? s_btn_eraser->face_color : 0,
                   s_btn_eraser ? s_btn_eraser->text_color : 0);

    /* Draw colour palette swatches. */
    for (int i = 0; i < PALETTE_COUNT; i++) {
        int sx = PAL_START_X + i * (PAL_SWATCH_W + PAL_PAD);
        int sy = PAL_Y;
        uint32_t col = s_palette[i];

        /* Fill swatch */
        for (int py = sy; py < sy + PAL_SWATCH_H; py++)
            for (int px = sx; px < sx + PAL_SWATCH_W; px++)
                buf[py * w + px] = col;

        /* Border: highlight selected swatch with white, others dark grey */
        uint32_t border = (i == s_pal_idx) ? 0x00FFFFFFU : 0x00404040U;
        for (int px = sx - 1; px <= sx + PAL_SWATCH_W; px++) {
            if (px >= 0 && px < w) {
                if (sy - 1 >= 0)              buf[(sy - 1) * w + px] = border;
                if (sy + PAL_SWATCH_H < TOOLBAR_H) buf[(sy + PAL_SWATCH_H) * w + px] = border;
            }
        }
        for (int py = sy - 1; py <= sy + PAL_SWATCH_H; py++) {
            if (py >= 0 && py < TOOLBAR_H) {
                if (sx - 1 >= 0)             buf[py * w + (sx - 1)] = border;
                if (sx + PAL_SWATCH_W < w)   buf[py * w + (sx + PAL_SWATCH_W)] = border;
            }
        }
    }

    /* Push pixel buffer to kernel. */
    __syscall3(SYS_WIN_UPDATE,
               (uint64_t)s_win->win_id,
               (uint64_t)(uintptr_t)s_win->buffer,
               (uint64_t)((unsigned)w * (unsigned)s_win->height * 4));
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    s_win = gui_init_window("Pencil", WIN_W, WIN_H);
    if (!s_win)
        return 1;

    /* Fill canvas area with black. */
    {
        uint32_t *buf = s_win->buffer;
        int w = s_win->width;
        int h = s_win->height;
        for (int y = CANVAS_Y; y < h; y++)
            for (int x = 0; x < w; x++)
                buf[y * w + x] = COL_CANVAS_BG;
    }

    /* Register toolbar buttons (used for hit-testing by gui_poll_events). */
    s_btn_pencil = gui_add_button(s_win, BTN_PAD, BTN_Y,
                                   BTN_W, BTN_H, "P");
    if (s_btn_pencil)
        s_btn_pencil->on_click = on_pencil;

    s_btn_eraser = gui_add_button(s_win, BTN_PAD + BTN_W + BTN_PAD, BTN_Y,
                                   BTN_W, BTN_H, "E");
    if (s_btn_eraser)
        s_btn_eraser->on_click = on_eraser;

    update_button_colors();

    /* Main loop. */
    for (;;) {
        int key = gui_poll_events(s_win);

        if (key == -1)
            break;
        if (key == 'q' || key == 'Q')
            break;

        /* Keyboard shortcuts for mode switching. */
        if (key == 'p' || key == 'P') {
            s_mode = MODE_PENCIL;
            update_button_colors();
            s_dirty = true;
        } else if (key == 'e' || key == 'E') {
            s_mode = MODE_ERASER;
            update_button_colors();
            s_dirty = true;
        }

        /* Handle drawing based on mouse state from gui_poll_events. */
        int mx = s_win->mouse_x;
        int my = s_win->mouse_y;

        /* Palette click detection. */
        if (s_win->mouse_down && my >= PAL_Y && my < PAL_Y + PAL_SWATCH_H) {
            for (int i = 0; i < PALETTE_COUNT; i++) {
                int sx = PAL_START_X + i * (PAL_SWATCH_W + PAL_PAD);
                if (mx >= sx && mx < sx + PAL_SWATCH_W) {
                    if (s_pal_idx != i) {
                        s_pal_idx = i;
                        s_dirty = true;
                    }
                    break;
                }
            }
        }

        if (s_win->mouse_down && my >= CANVAS_Y) {
            if (!s_drawing) {
                /* Starting a new stroke. */
                s_drawing = true;
                s_last_x  = mx;
                s_last_y  = my;
                plot(mx, my);
                s_dirty = true;
            } else {
                /* Continue stroke — draw line from last point. */
                if (mx != s_last_x || my != s_last_y) {
                    draw_line(s_last_x, s_last_y, mx, my);
                    s_last_x = mx;
                    s_last_y = my;
                    s_dirty  = true;
                }
            }
        } else {
            s_drawing = false;
            s_last_x  = -1;
            s_last_y  = -1;
        }

        /* Only push to compositor when something changed. */
        if (s_dirty) {
            render_frame();
            gfx_flush_display();
            s_dirty = false;
        }

        yield();
    }

    gui_destroy_window(s_win);
    return 0;
}
