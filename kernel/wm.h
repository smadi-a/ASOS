/*
 * kernel/wm.h — Kernel window manager and compositor.
 *
 * Manages a list of windows, each with its own pixel buffer.
 * wm_compose() paints the desktop into the GFX back buffer using the
 * Painter's Algorithm (back-to-front).  Call it before gfx_flush().
 *
 * Layout:
 *   • Slot 0           : Root window (wallpaper) — always at bottom.
 *   • Slots 1..15      : Normal user windows.
 *   • Taskbar (28 px)  : Drawn last, always on top of all windows.
 */

#ifndef KERNEL_WM_H
#define KERNEL_WM_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_WINDOWS    16
#define WM_TITLEBAR_H  20   /* Title bar height in pixels                */
#define WM_CURSOR_SZ    8   /* Mouse cursor size in pixels               */
#define TASKBAR_HEIGHT 28   /* Top taskbar height; windows must stay below */

/* Window flags */
#define WM_FLAG_NO_MOVE  (1u << 0)  /* Window cannot be dragged           */
#define WM_FLAG_NO_CLOSE (1u << 1)  /* Window has no close button         */
#define WM_FLAG_BOTTOM   (1u << 2)  /* Drawn before all normal windows    */

typedef struct {
    uint32_t  id;
    int       x, y;    /* Top-left corner of entire window (incl. title bar) */
    int       w, h;    /* Width and height of the client/content area        */
    char      title[64];
    uint32_t *buffer;  /* Client pixel buffer: w*h 0xAARRGGBB (kernel heap) */
    uint32_t  flags;   /* WM_FLAG_* bitmask                                  */
    bool      focused;
    bool      in_use;
    uint32_t  owner_pid; /* PID of the process that created this window      */
} window_t;

/* Global window list and current mouse position. */
extern window_t *g_windows[MAX_WINDOWS];
extern int       g_window_count;
extern int       g_mouse_x;
extern int       g_mouse_y;

/* Initialise the window manager (creates root window, centres cursor). */
void wm_init(void);

/*
 * Create a new window (slots 1–15; slot 0 is reserved for the root window).
 *   title : NUL-terminated string (truncated to 63 chars)
 *   x, y  : screen position of top-left corner (clamped so y >= TASKBAR_HEIGHT)
 *   w, h  : client area dimensions in pixels
 * Returns the window ID (>= 1) on success, -1 on failure.
 */
int wm_create(const char *title, int x, int y, int w, int h, uint32_t owner_pid);

/*
 * Destroy a window: frees its pixel buffer and descriptor, removes it from
 * the window list.  The root window (id == 0) cannot be destroyed.
 */
void wm_destroy(int win_id);

/*
 * Composite all windows into the GFX back buffer.
 * Layer order: wallpaper → windows → taskbar → cursor.
 * Also polls pending PS/2 mouse events and updates cursor / drag state.
 * Must be called before gfx_flush().
 */
void wm_compose(void);

/*
 * Update mouse state with the current absolute cursor position and
 * left-button state.  Handles window focusing, dragging, and close-button
 * clicks.
 */
void wm_handle_mouse(int x, int y, bool clicked);

/*
 * Copy pixels into a window's kernel buffer.
 *   win_id : window ID returned by wm_create
 *   pixels : w*h 0xAARRGGBB pixels already in kernel-accessible address space
 * Returns 0 on success, -1 on error.
 */
int wm_update_window(int win_id, const uint32_t *pixels);

/*
 * Destroy all windows owned by the given PID.
 * Called automatically on process exit / kill to prevent window leaks.
 */
void wm_destroy_by_owner(uint32_t pid);

/*
 * Returns the owner PID of the currently focused user window, or 0 if
 * no user window is focused (only the root window / wallpaper exists).
 */
uint32_t wm_get_focused_owner(void);

#endif /* KERNEL_WM_H */
