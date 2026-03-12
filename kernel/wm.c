/*
 * kernel/wm.c — Window manager and compositor.
 *
 * Manages up to MAX_WINDOWS windows.  Each window has:
 *   • A title bar (WM_TITLEBAR_H pixels tall) drawn by the compositor.
 *   • A client-area pixel buffer allocated from the kernel heap.
 *
 * Compositor pipeline (wm_compose):
 *   1. Poll PS/2 mouse events → update cursor position + drag state.
 *   2. Clear the back buffer with the desktop background colour.
 *   3. Painter's algorithm (back to front): for each window draw title
 *      bar then blit client buffer.
 *   4. Stamp the mouse cursor (8×8 white square).
 */

#include "wm.h"
#include "gfx.h"
#include "heap.h"
#include "mouse.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Colours (0xAARRGGBB) ──────────────────────────────────────────────── */

#define COL_DESKTOP          0x003A6EUL   /* Dark teal desktop              */
#define COL_TITLEBAR_FOCUS   0x1C559EUL   /* Blue  — focused title bar      */
#define COL_TITLEBAR_UNFOCUS 0x6B6B6BUL   /* Grey  — unfocused title bar    */
#define COL_CLOSE_BTN        0xCC2222UL   /* Red close button               */
#define COL_CLOSE_BTN_TEXT   0xFFFFFFUL
#define COL_TITLE_TEXT       0xFFFFFFUL
#define COL_WIN_PLACEHOLDER  0xC0C0C0UL   /* Grey fill when buffer is NULL  */
#define COL_CURSOR           0xFFFFFFUL   /* White mouse cursor             */

/* ── Global state ──────────────────────────────────────────────────────── */

window_t *g_windows[MAX_WINDOWS];
int       g_window_count = 0;
int       g_mouse_x      = 0;
int       g_mouse_y      = 0;

/* Drag tracking */
static window_t *s_dragging = NULL;
static int       s_last_x   = 0;
static int       s_last_y   = 0;
static bool      s_btn_prev = false;

/* ── Internal helpers ──────────────────────────────────────────────────── */

static void wm_str_copy(char *dst, const char *src, int max_len)
{
    int i = 0;
    if (src) {
        while (i < max_len - 1 && src[i]) {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static int wm_str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ── wm_init ───────────────────────────────────────────────────────────── */

void wm_init(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        g_windows[i] = NULL;
    g_window_count = 0;

    /* Start cursor at screen centre. */
    uint32_t sw = gfx_screen_width();
    uint32_t sh = gfx_screen_height();
    g_mouse_x = (sw > 0) ? (int)(sw / 2) : 0;
    g_mouse_y = (sh > 0) ? (int)(sh / 2) : 0;
    s_last_x  = g_mouse_x;
    s_last_y  = g_mouse_y;
    s_dragging = NULL;
    s_btn_prev = false;
}

/* ── wm_create ─────────────────────────────────────────────────────────── */

int wm_create(const char *title, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
    if (g_window_count >= MAX_WINDOWS) return -1;

    /* Find first free slot. */
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_windows[i]) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* Allocate and zero the window descriptor. */
    window_t *win = (window_t *)kmalloc(sizeof(window_t));
    if (!win) return -1;
    {
        uint8_t *p = (uint8_t *)win;
        for (uint32_t i = 0; i < sizeof(window_t); i++) p[i] = 0;
    }

    /* Allocate and zero the client pixel buffer. */
    uint32_t buf_bytes = (uint32_t)w * (uint32_t)h * 4U;
    win->buffer = (uint32_t *)kmalloc(buf_bytes);
    if (!win->buffer) {
        kfree(win);
        return -1;
    }
    {
        uint8_t *bp = (uint8_t *)win->buffer;
        for (uint32_t i = 0; i < buf_bytes; i++) bp[i] = 0;
    }

    win->id      = (uint32_t)slot;
    win->x       = x;
    win->y       = y;
    win->w       = w;
    win->h       = h;
    win->focused = true;
    win->in_use  = true;
    wm_str_copy(win->title, title, (int)sizeof(win->title));

    /* Unfocus all existing windows when a new one is created on top. */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i]) g_windows[i]->focused = false;
    }

    g_windows[slot] = win;
    g_window_count++;
    return slot;
}

/* ── wm_handle_mouse ───────────────────────────────────────────────────── */

void wm_handle_mouse(int x, int y, bool clicked)
{
    int dx = x - s_last_x;
    int dy = y - s_last_y;
    s_last_x = x;
    s_last_y = y;

    if (clicked) {
        /* On button-down transition: find a window to drag/focus. */
        if (!s_btn_prev) {
            /* Search topmost (highest index) window first. */
            for (int i = g_window_count - 1; i >= 0; i--) {
                window_t *w = g_windows[i];
                if (!w || !w->in_use) continue;
                /* Hit-test the title bar region. */
                if (x >= w->x && x < w->x + w->w &&
                    y >= w->y && y < w->y + WM_TITLEBAR_H)
                {
                    s_dragging = w;
                    /* Focus the clicked window, unfocus others. */
                    for (int j = 0; j < MAX_WINDOWS; j++) {
                        if (g_windows[j]) g_windows[j]->focused = false;
                    }
                    w->focused = true;
                    break;
                }
            }
        }
        /* While button is held: move the dragged window by the mouse delta. */
        if (s_dragging && (dx || dy)) {
            s_dragging->x += dx;
            s_dragging->y += dy;

            /* Clamp so the title bar always stays on screen. */
            if (s_dragging->x < 0) s_dragging->x = 0;
            if (s_dragging->y < 0) s_dragging->y = 0;
            uint32_t sw = gfx_screen_width();
            uint32_t sh = gfx_screen_height();
            if (sw > 0 && s_dragging->x + s_dragging->w > (int)sw)
                s_dragging->x = (int)sw - s_dragging->w;
            if (sh > 0 && s_dragging->y + WM_TITLEBAR_H > (int)sh)
                s_dragging->y = (int)sh - WM_TITLEBAR_H;
        }
    } else {
        s_dragging = NULL;
    }

    s_btn_prev = clicked;
}

/* ── wm_update_window ──────────────────────────────────────────────────── */

int wm_update_window(int win_id, const uint32_t *pixels)
{
    if (win_id < 0 || win_id >= MAX_WINDOWS) return -1;
    window_t *w = g_windows[win_id];
    if (!w || !w->in_use || !w->buffer || !pixels) return -1;

    uint32_t n = (uint32_t)w->w * (uint32_t)w->h * 4U;
    const uint8_t *src = (const uint8_t *)pixels;
    uint8_t       *dst = (uint8_t *)w->buffer;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = src[i];

    return 0;
}

/* ── wm_compose ────────────────────────────────────────────────────────── */

void wm_compose(void)
{
    uint32_t sw = gfx_screen_width();
    uint32_t sh = gfx_screen_height();

    /* 1. Poll all pending PS/2 mouse events.
     *    mouse_read_event() and the window list live in kernel heap /
     *    kernel data — accessible with any CR3. */
    {
        mouse_event_t evt;
        while (mouse_read_event(&evt)) {
            g_mouse_x += evt.dx;
            g_mouse_y -= evt.dy;   /* PS/2: positive dy = upward, screen: positive y = down */

            if (g_mouse_x < 0) g_mouse_x = 0;
            if (g_mouse_y < 0) g_mouse_y = 0;
            if (sw > 0 && (uint32_t)g_mouse_x >= sw) g_mouse_x = (int)sw - 1;
            if (sh > 0 && (uint32_t)g_mouse_y >= sh) g_mouse_y = (int)sh - 1;

            bool left_btn = (evt.buttons & 1) != 0;
            wm_handle_mouse(g_mouse_x, g_mouse_y, left_btn);
        }
    }

    /* 2. Clear back buffer — desktop background colour. */
    gfx_clear(COL_DESKTOP);

    /* 3. Draw each window back-to-front (Painter's Algorithm). */
    for (int i = 0; i < g_window_count; i++) {
        window_t *w = g_windows[i];
        if (!w || !w->in_use) continue;

        /* ── Title bar ────────────────────────────────────────────────── */
        uint32_t tb_col = w->focused ? COL_TITLEBAR_FOCUS : COL_TITLEBAR_UNFOCUS;
        gfx_fill_rect(w->x, w->y, w->w, WM_TITLEBAR_H, tb_col);

        /* Title text — leave 20 px on the right for the close button. */
        int max_chars = (w->w - 20) / 8;   /* 8 px per character */
        if (max_chars > 0) {
            int title_len = wm_str_len(w->title);
            if (title_len > max_chars) title_len = max_chars;
            /* Build a truncated, NUL-terminated copy on the stack. */
            char disp[64];
            for (int c = 0; c < title_len; c++) disp[c] = w->title[c];
            disp[title_len] = '\0';
            gfx_draw_string(w->x + 4, w->y + 2, disp, COL_TITLE_TEXT, tb_col);
        }

        /* Close button — 16×(TITLEBAR-4) px red box on the far right. */
        int close_x = w->x + w->w - 18;
        if (close_x > w->x) {
            gfx_fill_rect(close_x, w->y + 2, 16, WM_TITLEBAR_H - 4, COL_CLOSE_BTN);
            gfx_draw_string(close_x + 4, w->y + 2, "X",
                            COL_CLOSE_BTN_TEXT, COL_CLOSE_BTN);
        }

        /* ── Client area ──────────────────────────────────────────────── */
        int client_y = w->y + WM_TITLEBAR_H;
        if (w->buffer) {
            gfx_blit(w->x, client_y, w->buffer, w->w, w->h);
        } else {
            gfx_fill_rect(w->x, client_y, w->w, w->h, COL_WIN_PLACEHOLDER);
        }
    }

    /* 4. Draw mouse cursor — simple solid white square. */
    gfx_fill_rect(g_mouse_x, g_mouse_y, WM_CURSOR_SZ, WM_CURSOR_SZ, COL_CURSOR);
}
