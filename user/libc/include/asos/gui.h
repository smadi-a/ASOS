/*
 * asos/gui.h — ASOS GUI Toolkit for user-space window manager clients.
 *
 * Provides a widget-based API on top of the raw WM syscalls.  Each window
 * is represented by a gui_window_t that owns a local pixel buffer and a
 * list of widgets.  Drawing is done directly into the pixel buffer; the
 * toolkit pushes it to the kernel via SYS_WIN_UPDATE when needed.
 *
 * Usage:
 *   gui_window_t *win = gui_init_window("My App", 300, 200);
 *   gui_button_t *btn = gui_add_button(win, 10, 10, 80, 24, "Click me");
 *   btn->on_click = my_handler;
 *   for (;;) {
 *       gui_poll_events(win);
 *       gui_draw(win);
 *       gfx_flush_display();
 *       yield();
 *   }
 */

#ifndef _ASOS_GUI_H
#define _ASOS_GUI_H

#include <stdint.h>

/* ── Limits ──────────────────────────────────────────────────────────── */

#define GUI_MAX_WIDGETS   64
#define GUI_LABEL_MAX     48

/* ── Colour palette (classic 3D look) ────────────────────────────────── */

#define GUI_COL_FACE      0x00C0C0C0U  /* Button / widget face          */
#define GUI_COL_LIGHT     0x00FFFFFFU  /* Top / left highlight edge     */
#define GUI_COL_SHADOW    0x00808080U  /* Bottom / right shadow edge    */
#define GUI_COL_DARK      0x00404040U  /* Outer shadow (pressed state)  */
#define GUI_COL_TEXT      0x00000000U  /* Default text colour (black)   */
#define GUI_COL_BG        0x00C0C0C0U  /* Default window background     */
#define GUI_COL_PRESSED   0x00A0A0A0U  /* Face colour when pressed      */

/* ── Widget types ────────────────────────────────────────────────────── */

#define GUI_WIDGET_BUTTON  1
#define GUI_WIDGET_LABEL   2

/* ── Callback types ──────────────────────────────────────────────────── */

struct gui_window;
struct gui_widget;

typedef void (*gui_click_fn)(struct gui_window *win, struct gui_widget *w);

/* ── Widget ──────────────────────────────────────────────────────────── */

typedef struct gui_widget {
    uint8_t   type;                   /* GUI_WIDGET_* constant          */
    int16_t   x, y;                   /* Position within client area    */
    int16_t   w, h;                   /* Widget dimensions in pixels    */
    char      label[GUI_LABEL_MAX];   /* Display text                   */
    uint8_t   pressed;                /* Currently held down?           */
    uint8_t   in_use;                 /* Slot occupied?                 */
    uint32_t  face_color;             /* Custom face colour (0 = default) */
    uint32_t  text_color;             /* Custom text colour (0 = default) */
    gui_click_fn on_click;            /* Click callback (buttons only)  */
} gui_widget_t;

/* ── Window context ──────────────────────────────────────────────────── */

typedef struct gui_window {
    int          win_id;              /* Kernel WM window ID            */
    int          width;               /* Client area width  (pixels)    */
    int          height;              /* Client area height (pixels)    */
    uint32_t    *buffer;              /* Local pixel buffer (w * h)     */
    uint32_t     bg_color;            /* Background fill colour         */

    gui_widget_t widgets[GUI_MAX_WIDGETS];
    int          widget_count;

    /* Internal mouse tracking (from events). */
    int16_t      mouse_x;
    int16_t      mouse_y;
    uint8_t      mouse_down;          /* Left button currently held     */
} gui_window_t;

/* ── API ─────────────────────────────────────────────────────────────── */

/*
 * gui_init_window — Create a WM window and return a toolkit context.
 *
 *   title : NUL-terminated window title (up to 63 chars).
 *   w, h  : Client area dimensions.
 *
 * Allocates a pixel buffer via malloc(w * h * 4).  The window is
 * positioned at a default location chosen by the toolkit.
 * Returns NULL on failure (no free WM slot or OOM).
 */
gui_window_t *gui_init_window(const char *title, int w, int h);

/*
 * gui_destroy_window — Free all resources associated with a window.
 *
 * Does NOT destroy the kernel-side WM window (that happens on process
 * exit automatically), but frees the pixel buffer and context.
 */
void gui_destroy_window(gui_window_t *win);

/*
 * gui_add_button — Add a 3D beveled push-button widget.
 *
 *   x, y   : position relative to client area top-left
 *   w, h   : button dimensions in pixels
 *   label  : NUL-terminated text (centred on the button face)
 *
 * Returns a pointer to the widget (for setting on_click), or NULL if
 * the widget table is full.
 */
gui_widget_t *gui_add_button(gui_window_t *win,
                              int x, int y, int w, int h,
                              const char *label);

/*
 * gui_add_label — Add a static text label widget.
 *
 *   x, y   : position relative to client area top-left
 *   label  : NUL-terminated display text
 *
 * Returns a pointer to the widget, or NULL if the table is full.
 */
gui_widget_t *gui_add_label(gui_window_t *win,
                             int x, int y,
                             const char *label);

/*
 * gui_poll_events — Process pending events for this window.
 *
 * Drains the process event queue (SYS_GET_EVENT) and updates internal
 * mouse state.  Detects button clicks and fires on_click callbacks.
 * Also drains key_poll so keyboard events are consumed.
 *
 * Returns the most recent key character pressed (> 0), or 0 if none.
 */
int gui_poll_events(gui_window_t *win);

/*
 * gui_draw — Redraw the entire window into the local pixel buffer
 * and push it to the kernel via SYS_WIN_UPDATE.
 *
 * Clears to bg_color, then draws every widget.  Call this once per
 * frame after gui_poll_events().
 */
void gui_draw(gui_window_t *win);

/*
 * draw_button — Low-level: render a single 3D beveled button into
 * the window's pixel buffer.  Called automatically by gui_draw(),
 * but exposed for custom rendering.
 */
void draw_button(gui_window_t *ctx, int x, int y, int w, int h,
                 const char *label, int pressed);

/*
 * draw_button_ex — Like draw_button but with custom face and text colours.
 * Pass 0 for face_color / text_color to use the default palette.
 */
void draw_button_ex(gui_window_t *ctx, int x, int y, int w, int h,
                    const char *label, int pressed,
                    uint32_t face_color, uint32_t text_color);

#endif /* _ASOS_GUI_H */
