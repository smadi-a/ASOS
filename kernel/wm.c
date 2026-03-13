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
#include "keyboard.h"
#include "pit.h"
#include "scheduler.h"
#include "process.h"
#include "vmm.h"
#include "vfs.h"
#include "serial.h"
#include "io.h"
#include "power.h"
#include "string.h"
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
#define COL_CURSOR_FILL       0xFFFFFFUL   /* White cursor fill              */
#define COL_CURSOR_BORDER     0x000000UL   /* Black cursor outline           */
#define COL_TASKBAR           0x1E1E1EUL   /* Dark-charcoal taskbar          */
#define COL_TASKBAR_BORDER    0x444444UL   /* 1-px bottom edge of taskbar    */
#define COL_TASKBAR_TEXT      0xFFFFFFUL
#define COL_TAB_ACTIVE        0x3A6DB5UL   /* Blue active-window tab         */
#define COL_TAB_INACTIVE      0x3C3C3CUL   /* Dark inactive-window tab       */

/* Width (px) of each taskbar window tab. */
#define TASKBAR_TAB_W  108
#define TASKBAR_TAB_GAP  4

/* ── Launcher (System menu) ───────────────────────────────────────────── */

#define COL_LAUNCHER_BTN      0x2A5A9BUL   /* Blue system button          */
#define COL_LAUNCHER_BTN_HI   0x3A7ACBUL   /* Hover highlight             */
#define COL_LAUNCHER_MENU     0x2B2B2BUL   /* Menu background             */
#define COL_LAUNCHER_HOVER    0x3A6DB5UL   /* Hovered menu item           */
#define COL_LAUNCHER_TEXT     0xFFFFFFUL   /* Menu text                   */
#define COL_LAUNCHER_BORDER   0x555555UL   /* Menu border                 */

#define LAUNCHER_BTN_W   60              /* "ASOS" button width           */
#define LAUNCHER_MENU_W 140              /* Drop-down menu width          */
#define LAUNCHER_ITEM_H  24              /* Menu item height              */
#define LAUNCHER_NUM_ITEMS  6            /* Terminal, Calculator, Pencil, Note, Doom, Shutdown */

/* ── Global state ──────────────────────────────────────────────────────── */

window_t *g_windows[MAX_WINDOWS];
int       g_window_count = 0;
int       g_mouse_x      = 0;
int       g_mouse_y      = 0;
int       g_window_stack[MAX_WINDOWS];

/* Drag tracking */
static window_t *s_dragging = NULL;
static int       s_last_x   = 0;
static int       s_last_y   = 0;
static bool      s_btn_prev = false;

/* Launcher state */
static bool      s_launcher_open = false;

/* Forward declarations for launcher actions. */
static void launcher_spawn_terminal(void);
static void launcher_spawn_calculator(void);
static void launcher_spawn_pencil(void);
static void launcher_spawn_note(void);
static void launcher_spawn_doom(void);
static void launcher_shutdown(void);

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
    for (int i = 0; i < MAX_WINDOWS; i++) {
        g_windows[i] = NULL;
        g_window_stack[i] = -1;
    }
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
    g_window_stack[0] = 0;   /* Root pinned at bottom of Z-order */
}

/* ── wm_bring_to_front ─────────────────────────────────────────────────── */

void wm_bring_to_front(int win_id)
{
    /* Root window (id 0) is pinned at stack[0] — never move it. */
    if (win_id <= 0 || win_id >= MAX_WINDOWS) return;

    /* Find win_id in the stack (skip position 0 which is always root). */
    int pos = -1;
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (g_window_stack[i] == win_id) { pos = i; break; }
    }
    if (pos < 0) return;   /* not in the stack */

    /* Find the highest active slot (last non-(-1) entry). */
    int top = 0;
    for (int i = MAX_WINDOWS - 1; i >= 1; i--) {
        if (g_window_stack[i] != -1) { top = i; break; }
    }

    /* Already at the top — nothing to do. */
    if (pos == top) return;

    /* Shift everything above pos down by one, then place win_id at top. */
    for (int i = pos; i < top; i++)
        g_window_stack[i] = g_window_stack[i + 1];
    g_window_stack[top] = win_id;
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

    /* Push new window to the top of the Z-order stack. */
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (g_window_stack[i] == -1) {
            g_window_stack[i] = slot;
            break;
        }
    }

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

    /* Remove from Z-order stack and compact. */
    for (int i = 1; i < MAX_WINDOWS; i++) {
        if (g_window_stack[i] == win_id) {
            /* Shift everything above down by one. */
            for (int j = i; j < MAX_WINDOWS - 1; j++)
                g_window_stack[j] = g_window_stack[j + 1];
            g_window_stack[MAX_WINDOWS - 1] = -1;
            break;
        }
    }

    /* Focus the topmost remaining window using Z-order stack. */
    for (int i = MAX_WINDOWS - 1; i >= 1; i--) {
        int id = g_window_stack[i];
        if (id != -1 && g_windows[id] && g_windows[id]->in_use) {
            g_windows[id]->focused = true;
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

/* ── wm_get_focused_owner ───────────────────────────────────────────────── */

uint32_t wm_get_focused_owner(void)
{
    for (int i = MAX_WINDOWS - 1; i >= 1; i--) {
        int id = g_window_stack[i];
        if (id < 1) continue;
        window_t *w = g_windows[id];
        if (w && w->in_use && w->focused)
            return w->owner_pid;
    }
    return 0;   /* no focused user window */
}

/* ── Event queue helpers ────────────────────────────────────────────────── */

/*
 * Push an event into a task's ring buffer.
 * Caller must ensure interrupts are disabled.
 */
static void task_push_event(task_t *task, const event_t *evt)
{
    uint8_t next = (task->event_tail + 1) & (EVENT_QUEUE_SIZE - 1);
    if (next == task->event_head)
        return;   /* Queue full — drop event */
    task->event_queue[task->event_tail] = *evt;
    task->event_tail = next;
}

void wm_push_event_to_focused(const event_t *evt)
{
    uint32_t owner = wm_get_focused_owner();
    if (owner == 0) return;

    task_t *task = scheduler_find_task_by_pid((uint64_t)owner);
    if (!task) return;

    task_push_event(task, evt);
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
            /* ── Launcher menu item hit-test ────────────────────── */
            if (s_launcher_open) {
                int menu_x = 0;
                int menu_y = TASKBAR_HEIGHT;
                if (x >= menu_x && x < menu_x + LAUNCHER_MENU_W) {
                    for (int mi = 0; mi < LAUNCHER_NUM_ITEMS; mi++) {
                        int item_y = menu_y + 1 + mi * LAUNCHER_ITEM_H;
                        if (y >= item_y && y < item_y + LAUNCHER_ITEM_H) {
                            s_launcher_open = false;
                            if (mi == 0) launcher_spawn_terminal();
                            if (mi == 1) launcher_spawn_calculator();
                            if (mi == 2) launcher_spawn_pencil();
                            if (mi == 3) launcher_spawn_note();
                            if (mi == 4) launcher_spawn_doom();
                            if (mi == 5) launcher_shutdown();
                            goto mouse_done;
                        }
                    }
                }
                /* Clicked outside the menu — close it. */
                s_launcher_open = false;
            }

            /* ── System button toggle ──────────────────────────── */
            if (x >= 0 && x < LAUNCHER_BTN_W &&
                y >= 0 && y < TASKBAR_HEIGHT)
            {
                s_launcher_open = !s_launcher_open;
                goto mouse_done;
            }

            /* Search Z-order stack from top (highest) downward; skip root. */
            for (int si = MAX_WINDOWS - 1; si >= 1; si--) {
                int id = g_window_stack[si];
                if (id < 1) continue;
                window_t *w = g_windows[id];
                if (!w || !w->in_use) continue;

                /* ── Close button hit-test ─────────────────────────── */
                if (!(w->flags & WM_FLAG_NO_CLOSE)) {
                    int close_x = w->x + w->w - 18;
                    if (close_x > w->x &&
                        x >= close_x && x < close_x + 16 &&
                        y >= w->y + 2 && y < w->y + WM_TITLEBAR_H - 2)
                    {
                        /* Send EVENT_WIN_CLOSE to the owning process
                         * instead of destroying immediately.  The app
                         * can then clean up and exit gracefully. */
                        task_t *owner = scheduler_find_task_by_pid(
                                            (uint64_t)w->owner_pid);
                        if (owner) {
                            event_t ce;
                            ce.type = EVENT_WIN_CLOSE;
                            ce._pad = 0;
                            ce.x    = 0;
                            ce.y    = 0;
                            ce.code = (uint16_t)w->id;
                            task_push_event(owner, &ce);
                        } else {
                            /* Owner already dead — destroy directly. */
                            wm_destroy(w->id);
                        }
                        break;
                    }
                }

                /* ── Title-bar drag hit-test ───────────────────────── */
                if (!(w->flags & WM_FLAG_NO_MOVE) &&
                    x >= w->x && x < w->x + w->w &&
                    y >= w->y && y < w->y + WM_TITLEBAR_H)
                {
                    s_dragging = w;
                    /* Focus + bring to front. */
                    for (int j = 1; j < MAX_WINDOWS; j++) {
                        if (g_windows[j]) g_windows[j]->focused = false;
                    }
                    w->focused = true;
                    wm_bring_to_front(id);
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
                    wm_bring_to_front(id);
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

    /* ── Push mouse events to the focused window's owner task ────── */
    {
        event_t evt;
        evt._pad = 0;

        /* Convert absolute screen coordinates to client-area-relative
         * coordinates so user-space widget hit-testing works correctly. */
        int16_t rel_x = (int16_t)x;
        int16_t rel_y = (int16_t)y;
        {
            uint32_t owner = wm_get_focused_owner();
            if (owner != 0) {
                for (int i = MAX_WINDOWS - 1; i >= 1; i--) {
                    int fid = g_window_stack[i];
                    if (fid < 1) continue;
                    window_t *fw = g_windows[fid];
                    if (fw && fw->in_use && fw->focused) {
                        rel_x = (int16_t)(x - fw->x);
                        rel_y = (int16_t)(y - fw->y - WM_TITLEBAR_H);
                        break;
                    }
                }
            }
        }
        evt.x = rel_x;
        evt.y = rel_y;

        /* Button transitions. */
        if (clicked && !s_btn_prev) {
            evt.type = EVENT_MOUSE_DOWN;
            evt.code = 1;   /* left button */
            wm_push_event_to_focused(&evt);
        } else if (!clicked && s_btn_prev) {
            evt.type = EVENT_MOUSE_UP;
            evt.code = 1;
            wm_push_event_to_focused(&evt);
        }

        /* Movement. */
        if (dx || dy) {
            evt.type = EVENT_MOUSE_MOVE;
            evt.code = clicked ? 1 : 0;
            wm_push_event_to_focused(&evt);
        }
    }

mouse_done:
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

/* ── Launcher actions ──────────────────────────────────────────────────── */

/*
 * Spawn a new terminal: load DESKTOP.ELF from the VFS and create a process.
 * Called from the WM compositor context (kernel CR3 is already active or we
 * switch to it).  This mirrors the pattern used in kernel/main.c boot init.
 */
static void launcher_spawn_terminal(void)
{
    uint64_t kernel_pml4 = vmm_get_kernel_pml4();
    vmm_switch_address_space(kernel_pml4);

    vfs_file_t elf_file;
    if (vfs_open("/DESKTOP.ELF", &elf_file) != 0) {
        /* Fall back to a standalone shell. */
        if (vfs_open("/SHELL.ELF", &elf_file) != 0) {
            serial_puts("[LAUNCHER] No DESKTOP.ELF or SHELL.ELF found\n");
            return;
        }
    }

    uint32_t fsz = vfs_size(&elf_file);
    void *elf_buf = kmalloc(fsz);
    uint32_t got = 0;
    if (vfs_read(&elf_file, elf_buf, fsz, &got) != 0 || got == 0) {
        kfree(elf_buf);
        vfs_close(&elf_file);
        serial_puts("[LAUNCHER] Failed to read ELF\n");
        return;
    }
    vfs_close(&elf_file);

    task_t *child = task_create_from_elf("desktop", elf_buf, got);
    kfree(elf_buf);

    if (!child) {
        serial_puts("[LAUNCHER] Failed to create process\n");
        return;
    }

    child->parent_pid = 0;
    strncpy(child->cwd, "/", sizeof(child->cwd) - 1);
    child->cwd[sizeof(child->cwd) - 1] = '\0';
    scheduler_add_task(child);

    serial_puts("[LAUNCHER] Spawned terminal\n");
}

/*
 * Spawn the calculator application (CALC.ELF).
 */
static void launcher_spawn_calculator(void)
{
    uint64_t kernel_pml4 = vmm_get_kernel_pml4();
    vmm_switch_address_space(kernel_pml4);

    vfs_file_t elf_file;
    if (vfs_open("/CALC.ELF", &elf_file) != 0) {
        serial_puts("[LAUNCHER] CALC.ELF not found\n");
        return;
    }

    uint32_t fsz = vfs_size(&elf_file);
    void *elf_buf = kmalloc(fsz);
    uint32_t got = 0;
    if (vfs_read(&elf_file, elf_buf, fsz, &got) != 0 || got == 0) {
        kfree(elf_buf);
        vfs_close(&elf_file);
        serial_puts("[LAUNCHER] Failed to read CALC.ELF\n");
        return;
    }
    vfs_close(&elf_file);

    task_t *child = task_create_from_elf("calc", elf_buf, got);
    kfree(elf_buf);

    if (!child) {
        serial_puts("[LAUNCHER] Failed to create calculator process\n");
        return;
    }

    child->parent_pid = 0;
    strncpy(child->cwd, "/", sizeof(child->cwd) - 1);
    child->cwd[sizeof(child->cwd) - 1] = '\0';
    scheduler_add_task(child);

    serial_puts("[LAUNCHER] Spawned calculator\n");
}

static void launcher_spawn_pencil(void)
{
    uint64_t kernel_pml4 = vmm_get_kernel_pml4();
    vmm_switch_address_space(kernel_pml4);

    vfs_file_t elf_file;
    if (vfs_open("/PENCIL.ELF", &elf_file) != 0) {
        serial_puts("[LAUNCHER] PENCIL.ELF not found\n");
        return;
    }

    uint32_t fsz = vfs_size(&elf_file);
    void *elf_buf = kmalloc(fsz);
    uint32_t got = 0;
    if (vfs_read(&elf_file, elf_buf, fsz, &got) != 0 || got == 0) {
        kfree(elf_buf);
        vfs_close(&elf_file);
        serial_puts("[LAUNCHER] Failed to read PENCIL.ELF\n");
        return;
    }
    vfs_close(&elf_file);

    task_t *child = task_create_from_elf("pencil", elf_buf, got);
    kfree(elf_buf);

    if (!child) {
        serial_puts("[LAUNCHER] Failed to create pencil process\n");
        return;
    }

    child->parent_pid = 0;
    strncpy(child->cwd, "/", sizeof(child->cwd) - 1);
    child->cwd[sizeof(child->cwd) - 1] = '\0';
    scheduler_add_task(child);

    serial_puts("[LAUNCHER] Spawned pencil\n");
}

/*
 * Spawn the note-taking application (NOTE.ELF).
 */
static void launcher_spawn_note(void)
{
    uint64_t kernel_pml4 = vmm_get_kernel_pml4();
    vmm_switch_address_space(kernel_pml4);

    vfs_file_t elf_file;
    if (vfs_open("/NOTE.ELF", &elf_file) != 0) {
        serial_puts("[LAUNCHER] NOTE.ELF not found\n");
        return;
    }

    uint32_t fsz = vfs_size(&elf_file);
    void *elf_buf = kmalloc(fsz);
    uint32_t got = 0;
    if (vfs_read(&elf_file, elf_buf, fsz, &got) != 0 || got == 0) {
        kfree(elf_buf);
        vfs_close(&elf_file);
        serial_puts("[LAUNCHER] Failed to read NOTE.ELF\n");
        return;
    }
    vfs_close(&elf_file);

    task_t *child = task_create_from_elf("note", elf_buf, got);
    kfree(elf_buf);

    if (!child) {
        serial_puts("[LAUNCHER] Failed to create note process\n");
        return;
    }

    child->parent_pid = 0;
    strncpy(child->cwd, "/", sizeof(child->cwd) - 1);
    child->cwd[sizeof(child->cwd) - 1] = '\0';
    scheduler_add_task(child);

    serial_puts("[LAUNCHER] Spawned note\n");
}

/*
 * Spawn DOOM (APPS/DOOM/DOOM.ELF).
 */
static void launcher_spawn_doom(void)
{
    uint64_t kernel_pml4 = vmm_get_kernel_pml4();
    vmm_switch_address_space(kernel_pml4);

    vfs_file_t elf_file;
    if (vfs_open("/APPS/DOOM/DOOM.ELF", &elf_file) != 0) {
        serial_puts("[LAUNCHER] DOOM.ELF not found\n");
        return;
    }

    uint32_t fsz = vfs_size(&elf_file);
    void *elf_buf = kmalloc(fsz);
    uint32_t got = 0;
    if (vfs_read(&elf_file, elf_buf, fsz, &got) != 0 || got == 0) {
        kfree(elf_buf);
        vfs_close(&elf_file);
        serial_puts("[LAUNCHER] Failed to read DOOM.ELF\n");
        return;
    }
    vfs_close(&elf_file);

    task_t *child = task_create_from_elf("doom", elf_buf, got);
    kfree(elf_buf);

    if (!child) {
        serial_puts("[LAUNCHER] Failed to create doom process\n");
        return;
    }

    child->parent_pid = 0;
    strncpy(child->cwd, "/APPS/DOOM", sizeof(child->cwd) - 1);
    child->cwd[sizeof(child->cwd) - 1] = '\0';
    scheduler_add_task(child);

    serial_puts("[LAUNCHER] Spawned doom\n");
}

/*
 * Power off the machine.  Uses the QEMU/Bochs debug-exit port first,
 * then tries ACPI PM1a_CNT (works on QEMU + VirtualBox with ACPI).
 * As a last resort, triple-fault to force a reset.
 */
static void launcher_shutdown(void)
{
    sys_shutdown();   /* does not return */
}

/* ── Launcher menu drawing ────────────────────────────────────────────── */

static void wm_draw_launcher_menu(void)
{
    if (!s_launcher_open) return;

    /* Menu appears just below the taskbar, aligned with the system button. */
    int menu_x = 0;
    int menu_y = TASKBAR_HEIGHT;
    int menu_h = LAUNCHER_ITEM_H * LAUNCHER_NUM_ITEMS + 2;  /* +2 for border */

    /* Border */
    gfx_fill_rect(menu_x, menu_y, LAUNCHER_MENU_W, menu_h, COL_LAUNCHER_BORDER);
    /* Inner background */
    gfx_fill_rect(menu_x + 1, menu_y + 1,
                  LAUNCHER_MENU_W - 2, menu_h - 2, COL_LAUNCHER_MENU);

    /* Menu items */
    static const char *items[LAUNCHER_NUM_ITEMS] = { "Terminal", "Calculator", "Pencil", "Note", "Doom", "Shutdown" };
    for (int i = 0; i < LAUNCHER_NUM_ITEMS; i++) {
        int item_y = menu_y + 1 + i * LAUNCHER_ITEM_H;

        /* Highlight if mouse is hovering. */
        bool hover = (g_mouse_x >= menu_x &&
                      g_mouse_x < menu_x + LAUNCHER_MENU_W &&
                      g_mouse_y >= item_y &&
                      g_mouse_y < item_y + LAUNCHER_ITEM_H);
        uint32_t bg = hover ? COL_LAUNCHER_HOVER : COL_LAUNCHER_MENU;
        gfx_fill_rect(menu_x + 1, item_y,
                      LAUNCHER_MENU_W - 2, LAUNCHER_ITEM_H, bg);

        /* Label centred vertically in the item. */
        int text_y = item_y + (LAUNCHER_ITEM_H - 16) / 2;
        gfx_draw_string(menu_x + 12, text_y, items[i],
                        COL_LAUNCHER_TEXT, bg);
    }
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

    /* ── System (ASOS) button at far left ─────────────────────────── */
    {
        uint32_t btn_col = s_launcher_open ? COL_LAUNCHER_BTN_HI
                                           : COL_LAUNCHER_BTN;
        gfx_fill_rect(0, 0, LAUNCHER_BTN_W, TASKBAR_HEIGHT, btn_col);
        int label_cx = (LAUNCHER_BTN_W - 4 * 8) / 2;  /* centre "ASOS" */
        int label_cy = (TASKBAR_HEIGHT - 16) / 2;
        gfx_draw_string(label_cx, label_cy, "ASOS",
                        COL_TASKBAR_TEXT, btn_col);
    }

    /* ── Window tabs (after system button) ─────────────────────────── */
    int tab_x   = LAUNCHER_BTN_W + 4;
    int tab_h   = TASKBAR_HEIGHT - 6;
    int tab_y   = 3;
    int label_y = (TASKBAR_HEIGHT - 16) / 2;

    for (int si = 1; si < MAX_WINDOWS; si++) {
        int id = g_window_stack[si];
        if (id < 1) continue;
        window_t *w = g_windows[id];
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

    /* 1b. Keyboard events are NOT drained here.  Processes that use the
     *     legacy key_poll() syscall read directly from the keyboard ring
     *     buffer.  GUI-toolkit apps should use SYS_GET_EVENT instead;
     *     keyboard events will be routed there once a per-window input
     *     model is in place. */

    /* 2. Root window — fill entire screen with wallpaper colour. */
    gfx_fill_rect(0, 0, (int)sw, (int)sh, COL_WALLPAPER);

    /* 3. Draw normal windows back-to-front using Z-order stack.
     *    Stack[0] is the root window (already drawn); user windows from stack[1]. */
    for (int si = 1; si < MAX_WINDOWS; si++) {
        int id = g_window_stack[si];
        if (id < 1) continue;
        window_t *w = g_windows[id];
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

    /* 4b. Launcher menu — drawn above taskbar but below cursor. */
    wm_draw_launcher_menu();

    /* 5. Mouse cursor — topmost element.
     *    Classic arrow pointer: 12 wide × 18 tall.
     *    0 = transparent, 1 = black outline, 2 = white fill.           */
    static const uint8_t cursor[18][12] = {
        {1,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0,0,0,0},
        {1,2,2,2,2,1,0,0,0,0,0,0},
        {1,2,2,2,2,2,1,0,0,0,0,0},
        {1,2,2,2,2,2,2,1,0,0,0,0},
        {1,2,2,2,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,2,2,2,1,0,0},
        {1,2,2,2,2,2,2,2,2,2,1,0},
        {1,2,2,2,2,2,2,2,2,2,2,1},
        {1,2,2,2,2,2,1,1,1,1,1,1},
        {1,2,2,2,1,2,1,0,0,0,0,0},
        {1,2,2,1,0,1,2,1,0,0,0,0},
        {1,2,1,0,0,1,2,1,0,0,0,0},
        {1,1,0,0,0,0,1,2,1,0,0,0},
        {1,0,0,0,0,0,1,1,1,0,0,0},
    };
    for (int cy = 0; cy < 18; cy++) {
        for (int cx = 0; cx < 12; cx++) {
            uint8_t v = cursor[cy][cx];
            if (v == 0) continue;
            uint32_t col = (v == 1) ? COL_CURSOR_BORDER : COL_CURSOR_FILL;
            gfx_put_pixel(g_mouse_x + cx, g_mouse_y + cy, col);
        }
    }
}
