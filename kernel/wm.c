/*
 * kernel/wm.c — Window manager and compositor.
 *
 * Compositor layer order (bottom → top):
 *   1. Root window (slot 0) — fills the screen with the wallpaper colour.
 *   2. Normal windows (slots 1–15) — Painter's Algorithm, back to front.
 *   3. Taskbar (TASKBAR_HEIGHT px at top) — always on top of all windows.
 *   4. Mouse cursor.
 *
 * Window lifecycle:
 *   wm_init()    → creates the root window in slot 0.
 *   wm_create()  → allocates slots 1–15; enforces y >= TASKBAR_HEIGHT.
 *   wm_destroy() → frees pixel buffer + descriptor; cannot destroy root.
 */

#include "wm.h"
#include "gfx.h"
#include "heap.h"
#include "mouse.h"
#include "pit.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Colours (0x00RRGGBB) ─────────────────────────────────────────────── */

#define COL_WALLPAPER         0x223344UL   /* Midnight Blue desktop          */
#define COL_TITLEBAR_FOCUS    0x1C559EUL   /* Blue  — focused title bar      */
#define COL_TITLEBAR_UNFOCUS  0x6B6B6BUL   /* Grey  — unfocused title bar    */
#define COL_TITLEBAR_HI       0x5B8FDEUL   /* 1-px top highlight, focused    */
#define COL_CLOSE_BTN         0xCC2222UL   /* Red close button               */
#define COL_CLOSE_BTN_TEXT    0xFFFFFFUL
#define COL_TITLE_TEXT        0xFFFFFFUL
#define COL_WIN_PLACEHOLDER   0xC0C0C0UL   /* Grey fill when buffer is NULL  */
#define COL_CURSOR            0xFFFFFFUL   /* White mouse cursor             */
#define COL_TASKBAR           0x1E1E1EUL   /* Dark-charcoal taskbar          */
#define COL_TASKBAR_BORDER    0x444444UL   /* 1-px bottom edge of taskbar    */
#define COL_TASKBAR_TEXT      0xFFFFFFUL
#define COL_TAB_ACTIVE        0x3A6DB5UL   /* Blue active-window tab         */
#define COL_TAB_INACTIVE      0x3C3C3CUL   /* Dark inactive-window tab       */

/* Width (px) of each taskbar window tab. */
#define TASKBAR_TAB_W  108
#define TASKBAR_TAB_GAP  4

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

/* Format a two-digit decimal into buf[0..1] (no NUL). */
static void fmt2(char *buf, uint32_t val)
{
    buf[0] = (char)('0' + (val / 10) % 10);
    buf[1] = (char)('0' + val % 10);
}

/* ── wm_init ───────────────────────────────────────────────────────────── */

void wm_init(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        g_windows[i] = NULL;
    g_window_count = 0;

    /* Centre cursor. */
    uint32_t sw = gfx_screen_width();
    uint32_t sh = gfx_screen_height();
    g_mouse_x  = (sw > 0) ? (int)(sw / 2) : 0;
    g_mouse_y  = (sh > 0) ? (int)(sh / 2) : 0;
    s_last_x   = g_mouse_x;
    s_last_y   = g_mouse_y;
    s_dragging = NULL;
    s_btn_prev = false;

    /* ── Create the Root Window (slot 0) ─────────────────────────────── */
    window_t *root = (window_t *)kmalloc(sizeof(window_t));
    if (!root) return;   /* fatal, but don't crash boot */
    {
        uint8_t *p = (uint8_t *)root;
        for (uint32_t i = 0; i < sizeof(window_t); i++) p[i] = 0;
    }
    root->id      = 0;
    root->x       = 0;
    root->y       = 0;
    root->w       = (int)sw;
    root->h       = (int)sh;
    root->buffer  = NULL;   /* compositor fills it with COL_WALLPAPER */
    root->flags   = WM_FLAG_NO_MOVE | WM_FLAG_NO_CLOSE | WM_FLAG_BOTTOM;
    root->focused = false;
    root->in_use  = true;
    root->title[0] = '\0';

    g_windows[0]   = root;
    g_window_count = 1;
}

/* ── wm_create ─────────────────────────────────────────────────────────── */

int wm_create(const char *title, int x, int y, int w, int h, uint32_t owner_pid)
{
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return -1;
    /* Slots 1–15 are available for user windows; slot 0 = root. */
    if (g_window_count >= MAX_WINDOWS) return -1;

    /* Find first free slot starting at 1. */
    int slot = -1;
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (!g_windows[i]) { slot = i; break; }
    }
    if (slot < 0) return -1;

    /* Enforce work-area constraint: title bar must stay below taskbar. */
    if (y < TASKBAR_HEIGHT) y = TASKBAR_HEIGHT;

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

    win->id        = (uint32_t)slot;
    win->x         = x;
    win->y         = y;
    win->w         = w;
    win->h         = h;
    win->flags     = 0;
    win->focused   = true;
    win->in_use    = true;
    win->owner_pid = owner_pid;
    wm_str_copy(win->title, title, (int)sizeof(win->title));

    /* Unfocus all existing windows when a new one is created on top. */
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i]) g_windows[i]->focused = false;
    }
    win->focused = true;   /* re-focus the new window */

    g_windows[slot] = win;
    g_window_count++;
    return slot;
}

/* ── wm_destroy ────────────────────────────────────────────────────────── */

void wm_destroy(int win_id)
{
    /* Root window (slot 0) is immortal. */
    if (win_id < 1 || win_id >= MAX_WINDOWS) return;

    window_t *w = g_windows[win_id];
    if (!w || !w->in_use) return;

    /* Clear drag state if this window is being dragged — prevents
     * use-after-free on the next mouse move. */
    if (s_dragging == w)
        s_dragging = NULL;

    if (w->buffer) {
        kfree(w->buffer);
        w->buffer = NULL;
    }
    kfree(w);
    g_windows[win_id] = NULL;
    g_window_count--;

    /* If any window was focused, focus the topmost remaining one. */
    for (int i = MAX_WINDOWS - 1; i >= 1; i--) {
        if (g_windows[i] && g_windows[i]->in_use) {
            g_windows[i]->focused = true;
            break;
        }
    }
}

/* ── wm_destroy_by_owner ───────────────────────────────────────────────── */

void wm_destroy_by_owner(uint32_t pid)
{
    for (int i = 1; i < MAX_WINDOWS; i++) {
        window_t *w = g_windows[i];
        if (w && w->in_use && w->owner_pid == pid)
            wm_destroy(i);
    }
}

/* ── wm_handle_mouse ───────────────────────────────────────────────────── */

void wm_handle_mouse(int x, int y, bool clicked)
{
    int dx = x - s_last_x;
    int dy = y - s_last_y;
    s_last_x = x;
    s_last_y = y;

    if (clicked) {
        /* On button-down transition: hit-test windows topmost-first. */
        if (!s_btn_prev) {
            /* Search from the top (highest slot index) downward; skip root. */
            for (int i = MAX_WINDOWS - 1; i >= 1; i--) {
                window_t *w = g_windows[i];
                if (!w || !w->in_use) continue;

                /* ── Close button hit-test ─────────────────────────── */
                if (!(w->flags & WM_FLAG_NO_CLOSE)) {
                    int close_x = w->x + w->w - 18;
                    if (close_x > w->x &&
                        x >= close_x && x < close_x + 16 &&
                        y >= w->y + 2 && y < w->y + WM_TITLEBAR_H - 2)
                    {
                        wm_destroy(w->id);
                        break;
                    }
                }

                /* ── Title-bar drag hit-test ───────────────────────── */
                if (!(w->flags & WM_FLAG_NO_MOVE) &&
                    x >= w->x && x < w->x + w->w &&
                    y >= w->y && y < w->y + WM_TITLEBAR_H)
                {
                    s_dragging = w;
                    /* Focus this window, unfocus all others (skip root). */
                    for (int j = 1; j < MAX_WINDOWS; j++) {
                        if (g_windows[j]) g_windows[j]->focused = false;
                    }
                    w->focused = true;
                    break;
                }

                /* ── Client-area focus ─────────────────────────────── */
                int client_y = w->y + WM_TITLEBAR_H;
                if (x >= w->x && x < w->x + w->w &&
                    y >= client_y && y < client_y + w->h)
                {
                    for (int j = 1; j < MAX_WINDOWS; j++) {
                        if (g_windows[j]) g_windows[j]->focused = false;
                    }
                    w->focused = true;
                    break;
                }
            }
        }

        /* While button held: move the dragged window. */
        if (s_dragging && (dx || dy)) {
            s_dragging->x += dx;
            s_dragging->y += dy;

            /* Clamp: keep title bar on-screen and below the taskbar. */
            if (s_dragging->x < 0) s_dragging->x = 0;
            if (s_dragging->y < TASKBAR_HEIGHT)
                s_dragging->y = TASKBAR_HEIGHT;

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

/* ── Taskbar ───────────────────────────────────────────────────────────── */

static void wm_draw_taskbar(uint32_t sw)
{
    /* Background bar. */
    gfx_fill_rect(0, 0, (int)sw, TASKBAR_HEIGHT, COL_TASKBAR);

    /* 1-px bottom border for subtle depth. */
    gfx_fill_rect(0, TASKBAR_HEIGHT - 1, (int)sw, 1, COL_TASKBAR_BORDER);

    /* ── System clock (far right) ──────────────────────────────────── */
    uint64_t ms   = pit_get_ticks();
    uint32_t secs = (uint32_t)(ms / 1000U);
    uint32_t mins = secs / 60U;
    uint32_t hrs  = mins / 60U;
    secs %= 60U;
    mins %= 60U;
    hrs  %= 24U;

    /* "HH:MM:SS" — 8 chars × 8 px wide = 64 px + 6 px padding */
    char clk[9];
    fmt2(clk + 0, hrs);
    clk[2] = ':';
    fmt2(clk + 3, mins);
    clk[5] = ':';
    fmt2(clk + 6, secs);
    clk[8] = '\0';

    int clk_text_w = 8 * 8;   /* 8 chars × 8 px (VGA font width) */
    int clk_x = (int)sw - clk_text_w - 8;
    int clk_y = (TASKBAR_HEIGHT - 16) / 2;   /* vertically centred */
    gfx_draw_string(clk_x, clk_y, clk, COL_TASKBAR_TEXT, COL_TASKBAR);

    /* ── Window tabs (left side) ───────────────────────────────────── */
    int tab_x   = 4;
    int tab_h   = TASKBAR_HEIGHT - 6;
    int tab_y   = 3;
    int label_y = (TASKBAR_HEIGHT - 16) / 2;

    for (int i = 1; i < MAX_WINDOWS; i++) {
        window_t *w = g_windows[i];
        if (!w || !w->in_use) continue;

        /* Stop before overlapping the clock. */
        if (tab_x + TASKBAR_TAB_W > clk_x - 8) break;

        uint32_t tab_col = w->focused ? COL_TAB_ACTIVE : COL_TAB_INACTIVE;
        gfx_fill_rect(tab_x, tab_y, TASKBAR_TAB_W, tab_h, tab_col);

        /* 1-px top highlight on active tab. */
        if (w->focused) {
            gfx_fill_rect(tab_x, tab_y, TASKBAR_TAB_W, 1, COL_TITLEBAR_HI);
        }

        /* Truncated title label. */
        int max_chars = (TASKBAR_TAB_W - 8) / 8;
        int title_len = wm_str_len(w->title);
        if (title_len > max_chars) title_len = max_chars;

        char disp[16];
        for (int c = 0; c < title_len; c++) disp[c] = w->title[c];
        disp[title_len] = '\0';
        gfx_draw_string(tab_x + 4, label_y, disp, COL_TASKBAR_TEXT, tab_col);

        tab_x += TASKBAR_TAB_W + TASKBAR_TAB_GAP;
    }
}

/* ── wm_compose ────────────────────────────────────────────────────────── */

void wm_compose(void)
{
    uint32_t sw = gfx_screen_width();
    uint32_t sh = gfx_screen_height();

    /* 1. Poll all pending PS/2 mouse events. */
    {
        mouse_event_t evt;
        while (mouse_read_event(&evt)) {
            g_mouse_x += evt.dx;
            g_mouse_y -= evt.dy;   /* PS/2: +dy = up; screen: +y = down */

            if (g_mouse_x < 0) g_mouse_x = 0;
            if (g_mouse_y < 0) g_mouse_y = 0;
            if (sw > 0 && (uint32_t)g_mouse_x >= sw) g_mouse_x = (int)sw - 1;
            if (sh > 0 && (uint32_t)g_mouse_y >= sh) g_mouse_y = (int)sh - 1;

            bool left_btn = (evt.buttons & 1) != 0;
            wm_handle_mouse(g_mouse_x, g_mouse_y, left_btn);
        }
    }

    /* 2. Root window — fill entire screen with wallpaper colour. */
    gfx_fill_rect(0, 0, (int)sw, (int)sh, COL_WALLPAPER);

    /* 3. Draw normal windows back-to-front (Painter's Algorithm).
     *    Slot 0 is the root window; user windows start at slot 1. */
    for (int i = 1; i < MAX_WINDOWS; i++) {
        window_t *w = g_windows[i];
        if (!w || !w->in_use) continue;

        /* ── Title bar ────────────────────────────────────────────── */
        uint32_t tb_col = w->focused ? COL_TITLEBAR_FOCUS : COL_TITLEBAR_UNFOCUS;
        gfx_fill_rect(w->x, w->y, w->w, WM_TITLEBAR_H, tb_col);

        /* 1-px highlight at the very top of the focused title bar. */
        if (w->focused) {
            gfx_fill_rect(w->x, w->y, w->w, 1, COL_TITLEBAR_HI);
        }

        /* Title text — leave 20 px on the right for the close button. */
        int max_chars = (w->w - 20) / 8;
        if (max_chars > 0) {
            int title_len = wm_str_len(w->title);
            if (title_len > max_chars) title_len = max_chars;
            char disp[64];
            for (int c = 0; c < title_len; c++) disp[c] = w->title[c];
            disp[title_len] = '\0';
            gfx_draw_string(w->x + 4, w->y + 2, disp, COL_TITLE_TEXT, tb_col);
        }

        /* Close button — 16×(TITLEBAR-4) px red box on the far right. */
        if (!(w->flags & WM_FLAG_NO_CLOSE)) {
            int close_x = w->x + w->w - 18;
            if (close_x > w->x) {
                gfx_fill_rect(close_x, w->y + 2, 16, WM_TITLEBAR_H - 4,
                              COL_CLOSE_BTN);
                gfx_draw_string(close_x + 4, w->y + 2, "X",
                                COL_CLOSE_BTN_TEXT, COL_CLOSE_BTN);
            }
        }

        /* ── Client area ──────────────────────────────────────────── */
        int client_y = w->y + WM_TITLEBAR_H;
        if (w->buffer) {
            gfx_blit(w->x, client_y, w->buffer, w->w, w->h);
        } else {
            gfx_fill_rect(w->x, client_y, w->w, w->h, COL_WIN_PLACEHOLDER);
        }
    }

    /* 4. Taskbar — drawn after all windows so it is always visible. */
    wm_draw_taskbar(sw);

    /* 5. Mouse cursor — topmost element. */
    gfx_fill_rect(g_mouse_x, g_mouse_y, WM_CURSOR_SZ, WM_CURSOR_SZ, COL_CURSOR);
}
