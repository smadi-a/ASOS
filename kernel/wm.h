/*
 * kernel/wm.h — Kernel window manager and compositor.
 *
 * Manages a list of windows, each with its own pixel buffer.
 * wm_compose() paints the desktop into the GFX back buffer using the
 * Painter's Algorithm (back-to-front).  Call it before gfx_flush().
 */

#ifndef KERNEL_WM_H
#define KERNEL_WM_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_WINDOWS   16
#define WM_TITLEBAR_H 20   /* Title bar height in pixels */
#define WM_CURSOR_SZ   8   /* Mouse cursor size in pixels */

typedef struct {
    uint32_t  id;
    int       x, y;    /* Top-left corner of entire window (including title bar) */
    int       w, h;    /* Width and height of the client/content area */
    char      title[64];
    uint32_t *buffer;  /* Client pixel buffer: w*h 0xAARRGGBB pixels (kernel heap) */
    bool      focused;
    bool      in_use;
} window_t;

/* Global window list and current mouse position. */
extern window_t *g_windows[MAX_WINDOWS];
extern int       g_window_count;
extern int       g_mouse_x;
extern int       g_mouse_y;

/* Initialise the window manager (clears tables, centers cursor). */
void wm_init(void);

/*
 * Create a new window.
 *   title : NUL-terminated string (truncated to 63 chars)
 *   x, y  : screen position of top-left corner (including title bar)
 *   w, h  : client area dimensions in pixels
 * Returns the window ID (>= 0) on success, -1 on failure.
 */
int wm_create(const char *title, int x, int y, int w, int h);

/*
 * Composite all windows into the GFX back buffer.
 * Also polls pending PS/2 mouse events and updates cursor / drag state.
 * Must be called before gfx_flush().
 */
void wm_compose(void);

/*
 * Update mouse state with the current absolute cursor position and
 * left-button state.  Handles window focusing and dragging.
 */
void wm_handle_mouse(int x, int y, bool clicked);

/*
 * Copy pixels into a window's kernel buffer.
 *   win_id : window ID returned by wm_create
 *   pixels : w*h 0xAARRGGBB pixels already in kernel-accessible address space
 * Returns 0 on success, -1 on error.
 */
int wm_update_window(int win_id, const uint32_t *pixels);

#endif /* KERNEL_WM_H */
