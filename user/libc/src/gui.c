/*
 * gui.c — ASOS GUI Toolkit implementation.
 *
 * Draws widgets directly into a per-window pixel buffer, then pushes
 * the buffer to the kernel WM via SYS_WIN_UPDATE.  Uses a built-in
 * 8x8 bitmap font for text rendering (independent of the kernel font).
 */

#include <asos/gui.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <event.h>

/* ── Embedded 8x8 bitmap font (printable ASCII 0x20–0x7E) ───────────── */

/*
 * Each glyph is 8 rows of 8 pixels, stored as one byte per row.
 * Bit 7 = leftmost pixel.  Covers ' ' (0x20) through '~' (0x7E).
 */
static const uint8_t g_font8x8[95][8] = {
    /* 0x20 ' ' */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 0x21 '!' */ { 0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00 },
    /* 0x22 '"' */ { 0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00 },
    /* 0x23 '#' */ { 0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00 },
    /* 0x24 '$' */ { 0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00 },
    /* 0x25 '%' */ { 0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00 },
    /* 0x26 '&' */ { 0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00 },
    /* 0x27 ''' */ { 0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00 },
    /* 0x28 '(' */ { 0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00 },
    /* 0x29 ')' */ { 0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00 },
    /* 0x2A '*' */ { 0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00 },
    /* 0x2B '+' */ { 0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00 },
    /* 0x2C ',' */ { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30 },
    /* 0x2D '-' */ { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 },
    /* 0x2E '.' */ { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00 },
    /* 0x2F '/' */ { 0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00 },
    /* 0x30 '0' */ { 0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00 },
    /* 0x31 '1' */ { 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 },
    /* 0x32 '2' */ { 0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00 },
    /* 0x33 '3' */ { 0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00 },
    /* 0x34 '4' */ { 0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x00 },
    /* 0x35 '5' */ { 0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00 },
    /* 0x36 '6' */ { 0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00 },
    /* 0x37 '7' */ { 0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00 },
    /* 0x38 '8' */ { 0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00 },
    /* 0x39 '9' */ { 0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00 },
    /* 0x3A ':' */ { 0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00 },
    /* 0x3B ';' */ { 0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30 },
    /* 0x3C '<' */ { 0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00 },
    /* 0x3D '=' */ { 0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00 },
    /* 0x3E '>' */ { 0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00 },
    /* 0x3F '?' */ { 0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00 },
    /* 0x40 '@' */ { 0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x7C,0x00 },
    /* 0x41 'A' */ { 0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00 },
    /* 0x42 'B' */ { 0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00 },
    /* 0x43 'C' */ { 0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00 },
    /* 0x44 'D' */ { 0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00 },
    /* 0x45 'E' */ { 0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xFE,0x00 },
    /* 0x46 'F' */ { 0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00 },
    /* 0x47 'G' */ { 0x7C,0xC6,0xC0,0xCE,0xC6,0xC6,0x7E,0x00 },
    /* 0x48 'H' */ { 0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00 },
    /* 0x49 'I' */ { 0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00 },
    /* 0x4A 'J' */ { 0x1E,0x06,0x06,0x06,0xC6,0xC6,0x7C,0x00 },
    /* 0x4B 'K' */ { 0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x00 },
    /* 0x4C 'L' */ { 0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFE,0x00 },
    /* 0x4D 'M' */ { 0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00 },
    /* 0x4E 'N' */ { 0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00 },
    /* 0x4F 'O' */ { 0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00 },
    /* 0x50 'P' */ { 0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0x00 },
    /* 0x51 'Q' */ { 0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06 },
    /* 0x52 'R' */ { 0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00 },
    /* 0x53 'S' */ { 0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00 },
    /* 0x54 'T' */ { 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 },
    /* 0x55 'U' */ { 0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00 },
    /* 0x56 'V' */ { 0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00 },
    /* 0x57 'W' */ { 0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00 },
    /* 0x58 'X' */ { 0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00 },
    /* 0x59 'Y' */ { 0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00 },
    /* 0x5A 'Z' */ { 0xFE,0x06,0x0C,0x18,0x30,0x60,0xFE,0x00 },
    /* 0x5B '[' */ { 0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00 },
    /* 0x5C '\' */ { 0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00 },
    /* 0x5D ']' */ { 0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00 },
    /* 0x5E '^' */ { 0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00 },
    /* 0x5F '_' */ { 0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00 },
    /* 0x60 '`' */ { 0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00 },
    /* 0x61 'a' */ { 0x00,0x00,0x7C,0x06,0x7E,0xC6,0x7E,0x00 },
    /* 0x62 'b' */ { 0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xFC,0x00 },
    /* 0x63 'c' */ { 0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00 },
    /* 0x64 'd' */ { 0x06,0x06,0x7E,0xC6,0xC6,0xC6,0x7E,0x00 },
    /* 0x65 'e' */ { 0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00 },
    /* 0x66 'f' */ { 0x1C,0x36,0x30,0x7C,0x30,0x30,0x30,0x00 },
    /* 0x67 'g' */ { 0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x7C },
    /* 0x68 'h' */ { 0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x00 },
    /* 0x69 'i' */ { 0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00 },
    /* 0x6A 'j' */ { 0x06,0x00,0x06,0x06,0x06,0xC6,0xC6,0x7C },
    /* 0x6B 'k' */ { 0xC0,0xC0,0xCC,0xD8,0xF0,0xD8,0xCC,0x00 },
    /* 0x6C 'l' */ { 0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00 },
    /* 0x6D 'm' */ { 0x00,0x00,0xEC,0xFE,0xD6,0xC6,0xC6,0x00 },
    /* 0x6E 'n' */ { 0x00,0x00,0xFC,0xC6,0xC6,0xC6,0xC6,0x00 },
    /* 0x6F 'o' */ { 0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00 },
    /* 0x70 'p' */ { 0x00,0x00,0xFC,0xC6,0xC6,0xFC,0xC0,0xC0 },
    /* 0x71 'q' */ { 0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x06 },
    /* 0x72 'r' */ { 0x00,0x00,0xDC,0xE6,0xC0,0xC0,0xC0,0x00 },
    /* 0x73 's' */ { 0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00 },
    /* 0x74 't' */ { 0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00 },
    /* 0x75 'u' */ { 0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x7E,0x00 },
    /* 0x76 'v' */ { 0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00 },
    /* 0x77 'w' */ { 0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00 },
    /* 0x78 'x' */ { 0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00 },
    /* 0x79 'y' */ { 0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0x7C },
    /* 0x7A 'z' */ { 0x00,0x00,0xFE,0x0C,0x38,0x60,0xFE,0x00 },
    /* 0x7B '{' */ { 0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00 },
    /* 0x7C '|' */ { 0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00 },
    /* 0x7D '}' */ { 0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00 },
    /* 0x7E '~' */ { 0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00 },
};

#define FONT_W 8
#define FONT_H 8

/* ── Syscall wrappers (local, matching win_test.c pattern) ───────────── */

static inline long _win_create(const char *title, int x, int y, int w, int h)
{
    return (long)__syscall5(SYS_WIN_CREATE,
                            (uint64_t)(uintptr_t)title,
                            (uint64_t)(int64_t)x,
                            (uint64_t)(int64_t)y,
                            (uint64_t)(int64_t)w,
                            (uint64_t)(int64_t)h);
}

static inline long _win_update(int win_id, const uint32_t *pixels,
                                uint64_t buf_size)
{
    return (long)__syscall3(SYS_WIN_UPDATE,
                            (uint64_t)(uint32_t)win_id,
                            (uint64_t)(uintptr_t)pixels,
                            buf_size);
}

static inline long _key_poll(void)
{
    return (long)__syscall0(SYS_KEY_POLL);
}

/* ── Pixel buffer helpers ────────────────────────────────────────────── */

static inline void _put_pixel(gui_window_t *win, int x, int y, uint32_t color)
{
    if (x >= 0 && x < win->width && y >= 0 && y < win->height)
        win->buffer[y * win->width + x] = color;
}

static void _fill_rect(gui_window_t *win,
                        int rx, int ry, int rw, int rh, uint32_t color)
{
    /* Clip to buffer bounds. */
    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < 0 ? 0 : ry;
    int x1 = rx + rw > win->width  ? win->width  : rx + rw;
    int y1 = ry + rh > win->height ? win->height : ry + rh;

    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            win->buffer[y * win->width + x] = color;
}

static void _hline(gui_window_t *win, int x, int y, int len, uint32_t color)
{
    for (int i = 0; i < len; i++)
        _put_pixel(win, x + i, y, color);
}

static void _vline(gui_window_t *win, int x, int y, int len, uint32_t color)
{
    for (int i = 0; i < len; i++)
        _put_pixel(win, x, y + i, color);
}

/* Draw a single 8x8 character into the pixel buffer. */
static void _draw_char(gui_window_t *win, int cx, int cy,
                        char ch, uint32_t fg)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *glyph = g_font8x8[ch - 0x20];

    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (0x80 >> col))
                _put_pixel(win, cx + col, cy + row, fg);
        }
    }
}

/* Draw a NUL-terminated string. Returns the width in pixels. */
static int _draw_string(gui_window_t *win, int sx, int sy,
                          const char *s, uint32_t fg)
{
    int x = sx;
    while (*s) {
        _draw_char(win, x, sy, *s, fg);
        x += FONT_W;
        s++;
    }
    return x - sx;
}

/* Measure string width in pixels. */
static int _text_width(const char *s)
{
    int len = 0;
    while (*s++) len++;
    return len * FONT_W;
}

/* ── Public API ──────────────────────────────────────────────────────── */

gui_window_t *gui_init_window(const char *title, int w, int h)
{
    /* Default position: slightly offset from top-left. */
    long wid = _win_create(title, 80, 60, w, h);
    if (wid < 0) return (void *)0;

    uint32_t *buf = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
    if (!buf) return (void *)0;

    gui_window_t *win = (gui_window_t *)malloc(sizeof(gui_window_t));
    if (!win) {
        free(buf);
        return (void *)0;
    }

    /* Zero the struct to clear all widget slots. */
    for (unsigned i = 0; i < sizeof(gui_window_t); i++)
        ((uint8_t *)win)[i] = 0;

    win->win_id   = (int)wid;
    win->width    = w;
    win->height   = h;
    win->buffer   = buf;
    win->bg_color = GUI_COL_BG;

    return win;
}

void gui_destroy_window(gui_window_t *win)
{
    if (!win) return;
    if (win->buffer) free(win->buffer);
    free(win);
}

gui_widget_t *gui_add_button(gui_window_t *win,
                              int x, int y, int w, int h,
                              const char *label)
{
    if (!win || win->widget_count >= GUI_MAX_WIDGETS) return (void *)0;

    gui_widget_t *wgt = (void *)0;
    for (int i = 0; i < GUI_MAX_WIDGETS; i++) {
        if (!win->widgets[i].in_use) {
            wgt = &win->widgets[i];
            break;
        }
    }
    if (!wgt) return (void *)0;

    wgt->type    = GUI_WIDGET_BUTTON;
    wgt->x       = (int16_t)x;
    wgt->y       = (int16_t)y;
    wgt->w       = (int16_t)w;
    wgt->h       = (int16_t)h;
    wgt->pressed = 0;
    wgt->in_use  = 1;
    wgt->on_click = (void *)0;

    /* Copy label. */
    int i = 0;
    while (label[i] && i < GUI_LABEL_MAX - 1) {
        wgt->label[i] = label[i];
        i++;
    }
    wgt->label[i] = '\0';

    win->widget_count++;
    return wgt;
}

gui_widget_t *gui_add_label(gui_window_t *win,
                             int x, int y,
                             const char *label)
{
    if (!win || win->widget_count >= GUI_MAX_WIDGETS) return (void *)0;

    gui_widget_t *wgt = (void *)0;
    for (int i = 0; i < GUI_MAX_WIDGETS; i++) {
        if (!win->widgets[i].in_use) {
            wgt = &win->widgets[i];
            break;
        }
    }
    if (!wgt) return (void *)0;

    int tw = _text_width(label);

    wgt->type    = GUI_WIDGET_LABEL;
    wgt->x       = (int16_t)x;
    wgt->y       = (int16_t)y;
    wgt->w       = (int16_t)tw;
    wgt->h       = (int16_t)FONT_H;
    wgt->pressed = 0;
    wgt->in_use  = 1;
    wgt->on_click = (void *)0;

    int i = 0;
    while (label[i] && i < GUI_LABEL_MAX - 1) {
        wgt->label[i] = label[i];
        i++;
    }
    wgt->label[i] = '\0';

    win->widget_count++;
    return wgt;
}

/* ── draw_button — render a 3D beveled push button ──────────────────── */

void draw_button(gui_window_t *ctx, int x, int y, int w, int h,
                 const char *label, int pressed)
{
    draw_button_ex(ctx, x, y, w, h, label, pressed, 0, 0);
}

void draw_button_ex(gui_window_t *ctx, int x, int y, int w, int h,
                    const char *label, int pressed,
                    uint32_t face_color, uint32_t text_color)
{
    if (!ctx || w < 4 || h < 4) return;

    uint32_t base  = face_color ? face_color : GUI_COL_FACE;
    uint32_t face  = pressed ? (base == GUI_COL_FACE ? GUI_COL_PRESSED
                                : ((base >> 1) & 0x7F7F7FU)) : base;
    uint32_t lt    = pressed ? GUI_COL_SHADOW  : GUI_COL_LIGHT;   /* top/left  */
    uint32_t rb    = pressed ? GUI_COL_LIGHT   : GUI_COL_SHADOW;  /* bot/right */
    uint32_t outer = pressed ? GUI_COL_LIGHT   : GUI_COL_DARK;    /* outer rim */
    uint32_t tcol  = text_color ? text_color : GUI_COL_TEXT;

    /* Face fill. */
    _fill_rect(ctx, x + 2, y + 2, w - 4, h - 4, face);

    /* Outer bevel — top and left. */
    _hline(ctx, x, y, w, lt);
    _vline(ctx, x, y, h, lt);

    /* Inner highlight — one pixel inward. */
    _hline(ctx, x + 1, y + 1, w - 2, lt);
    _vline(ctx, x + 1, y + 1, h - 2, lt);

    /* Outer bevel — bottom and right. */
    _hline(ctx, x, y + h - 1, w, outer);
    _vline(ctx, x + w - 1, y, h, outer);

    /* Inner shadow — one pixel inward from bottom/right. */
    _hline(ctx, x + 1, y + h - 2, w - 2, rb);
    _vline(ctx, x + w - 2, y + 1, h - 2, rb);

    /* Centre the label text. */
    int tw = _text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - FONT_H) / 2;

    /* Pressed buttons shift text 1px down-right. */
    if (pressed) { tx++; ty++; }

    _draw_string(ctx, tx, ty, label, tcol);
}

/* ── gui_poll_events ─────────────────────────────────────────────────── */

int gui_poll_events(gui_window_t *win)
{
    if (!win) return 0;

    int last_key = 0;
    event_t evt;

    /* Drain the per-process event queue. */
    while (get_event(&evt) == 0) {
        switch (evt.type) {
        case EVENT_KEY_PRESS:
            last_key = (int)evt.code;
            break;

        case EVENT_WIN_CLOSE:
            /* Return -1 to signal that the window should close. */
            return -1;

        case EVENT_MOUSE_MOVE:
            win->mouse_x = evt.x;
            win->mouse_y = evt.y;
            break;

        case EVENT_MOUSE_DOWN: {
            win->mouse_down = 1;
            win->mouse_x = evt.x;
            win->mouse_y = evt.y;

            /* Hit-test all button widgets. */
            for (int i = 0; i < GUI_MAX_WIDGETS; i++) {
                gui_widget_t *w = &win->widgets[i];
                if (!w->in_use || w->type != GUI_WIDGET_BUTTON) continue;

                int mx = evt.x, my = evt.y;
                if (mx >= w->x && mx < w->x + w->w &&
                    my >= w->y && my < w->y + w->h) {
                    w->pressed = 1;
                }
            }
            break;
        }

        case EVENT_MOUSE_UP: {
            win->mouse_down = 0;
            win->mouse_x = evt.x;
            win->mouse_y = evt.y;

            /* Release all buttons; fire callback if still inside. */
            for (int i = 0; i < GUI_MAX_WIDGETS; i++) {
                gui_widget_t *w = &win->widgets[i];
                if (!w->in_use || w->type != GUI_WIDGET_BUTTON) continue;

                if (w->pressed) {
                    w->pressed = 0;
                    int mx = evt.x, my = evt.y;
                    if (mx >= w->x && mx < w->x + w->w &&
                        my >= w->y && my < w->y + w->h) {
                        if (w->on_click)
                            w->on_click(win, w);
                    }
                }
            }
            break;
        }

        default:
            break;
        }
    }

    /* Also drain key_poll for compatibility (keyboard events may come
     * through the old path if this process doesn't receive event_t
     * keyboard events). */
    for (;;) {
        long k = _key_poll();
        if (k < 0) break;
        if (!last_key) last_key = (int)k;
    }

    return last_key;
}

/* ── gui_draw ────────────────────────────────────────────────────────── */

void gui_draw(gui_window_t *win)
{
    if (!win) return;

    /* Clear to background colour. */
    _fill_rect(win, 0, 0, win->width, win->height, win->bg_color);

    /* Draw all widgets. */
    for (int i = 0; i < GUI_MAX_WIDGETS; i++) {
        gui_widget_t *w = &win->widgets[i];
        if (!w->in_use) continue;

        switch (w->type) {
        case GUI_WIDGET_BUTTON:
            draw_button_ex(win, w->x, w->y, w->w, w->h,
                           w->label, w->pressed,
                           w->face_color, w->text_color);
            break;

        case GUI_WIDGET_LABEL: {
            uint32_t tc = w->text_color ? w->text_color : GUI_COL_TEXT;
            _draw_string(win, w->x, w->y, w->label, tc);
            break;
        }

        default:
            break;
        }
    }

    /* Push to kernel WM. */
    uint64_t sz = (uint64_t)win->width * (uint64_t)win->height * 4ULL;
    _win_update(win->win_id, win->buffer, sz);
}
