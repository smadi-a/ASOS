/*
 * desktop.c — ASOS Desktop Environment (init process, PID 2).
 *
 * Responsibilities:
 *   1. Create a Terminal window in the kernel WM.
 *   2. Spawn SHELL.ELF as PID 3 (the interactive text shell).
 *   3. Drive the WM compositor via gfx_flush_display() in a render loop.
 *
 * The shell reads stdin from the PS/2 keyboard and writes stdout to the
 * serial port (COM1 @ 115200).  The Terminal window provides its visual
 * frame within the desktop environment.
 *
 * Build: part of the normal `make` run; installed as DESKTOP.ELF.
 * Boot : kernel/main.c loads it as the first user process.
 */

#include <stdint.h>
#include <unistd.h>
#include <gfx.h>
#include <sys/syscall.h>

/* ── Terminal window client-area dimensions ─────────────────────────────── */

#define TERM_W  600
#define TERM_H  380

/*
 * Pixel buffer stored in BSS (static) — keeps it off the stack.
 * 600 × 380 × 4 = 912 000 bytes ≈ 891 KB.
 */
static uint32_t s_term_pixels[TERM_W * TERM_H];

/* ── Syscall wrappers ───────────────────────────────────────────────────── */

static inline long win_create(const char *title, int x, int y, int w, int h)
{
    return __syscall5(SYS_WIN_CREATE,
                      (uint64_t)(uintptr_t)title,
                      (uint64_t)(int64_t)x,
                      (uint64_t)(int64_t)y,
                      (uint64_t)(int64_t)w,
                      (uint64_t)(int64_t)h);
}

static inline long win_update(int win_id, const uint32_t *pixels)
{
    return __syscall2(SYS_WIN_UPDATE,
                      (uint64_t)(uint32_t)win_id,
                      (uint64_t)(uintptr_t)pixels);
}

/* ── Pixel buffer helpers ───────────────────────────────────────────────── */

static void buf_fill(uint32_t color)
{
    for (int i = 0; i < TERM_W * TERM_H; i++)
        s_term_pixels[i] = color;
}

static void buf_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || row >= TERM_H) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || col >= TERM_W) continue;
            s_term_pixels[row * TERM_W + col] = color;
        }
    }
}

static void buf_hline(int x, int y, int len, uint32_t color)
{
    if (y < 0 || y >= TERM_H) return;
    for (int col = x; col < x + len; col++) {
        if (col >= 0 && col < TERM_W)
            s_term_pixels[y * TERM_W + col] = color;
    }
}

static void buf_vline(int x, int y, int len, uint32_t color)
{
    if (x < 0 || x >= TERM_W) return;
    for (int row = y; row < y + len; row++) {
        if (row >= 0 && row < TERM_H)
            s_term_pixels[row * TERM_W + x] = color;
    }
}

/* ── Terminal window appearance ─────────────────────────────────────────── */

/*
 * render_terminal — paint a terminal-style interface into s_term_pixels.
 *
 * Layout (top → bottom):
 *   0–24 px  : header band (dark, status dot, tab indicators)
 *   25 px    : separator line
 *   26–357px : content area (dark bg, faux prompt/output lines)
 *   358–379px: status bar (blue)
 *
 * Right 120 px are partitioned off as a mini "sidebar" with coloured bars.
 *
 * All colours use 0x00RRGGBB (alpha ignored by the compositor).
 */
static void render_terminal(void)
{
    /* ── Base background ─────────────────────────────────────────────── */
    buf_fill(0x0D1117);

    /* ── Header band (24 px) ─────────────────────────────────────────── */
    buf_fill_rect(0, 0, TERM_W, 24, 0x161B22);
    buf_hline(0, 24, TERM_W, 0x30363D);

    /* Green "connected" indicator */
    buf_fill_rect(10, 8, 8, 8, 0x3FB950);

    /* Tab placeholders */
    for (int i = 0; i < 3; i++)
        buf_fill_rect(26 + i * 14, 9, 8, 6, 0x21262D);
    /* Active tab highlight */
    buf_fill_rect(26, 9, 8, 6, 0x388BFD);

    /* ── Sidebar separator (right 120 px) ────────────────────────────── */
    buf_vline(TERM_W - 121, 25, TERM_H - 47, 0x21262D);
    buf_fill_rect(TERM_W - 120, 25, 120, TERM_H - 47, 0x0D1117);

    /* Sidebar: coloured progress-bar rows */
    static const uint32_t bar_colors[] = {
        0x1F6FEB, 0x388BFD, 0x1F6FEB, 0x388BFD,
        0x1F6FEB, 0x3FB950, 0xF78166, 0xE3B341
    };
    static const int bar_widths[] = { 90, 70, 80, 50, 95, 65, 40, 75 };
    for (int i = 0; i < 8; i++) {
        int sy = 40 + i * 14;
        if (sy + 6 >= TERM_H - 22) break;
        buf_fill_rect(TERM_W - 112, sy, bar_widths[i], 6, bar_colors[i]);
    }

    /* ── Content area: faux shell prompts and output ─────────────────── */

    /* Helper lambda-style: draw one prompt row */
    /*   col 10: 6×12 green block ($ character placeholder)              */
    /*   col 20: blue rectangle  (typed command)                         */
    /*   col 20+cmd_w: yellow block (cursor)                             */

    int y = 40;

    /* Prompt 1 — completed command */
    buf_fill_rect(10, y,      6, 12, 0x3FB950);
    buf_fill_rect(20, y + 3, 60,  2, 0x58A6FF);
    y += 18;
    buf_fill_rect(10, y, 200, 2, 0x8B949E);
    y +=  7;
    buf_fill_rect(10, y, 160, 2, 0x8B949E);
    y +=  7;
    buf_fill_rect(10, y, 240, 2, 0x8B949E);

    /* Prompt 2 — completed command */
    y += 18;
    buf_fill_rect(10, y,      6, 12, 0x3FB950);
    buf_fill_rect(20, y + 3, 80,  2, 0x58A6FF);
    y += 18;
    buf_fill_rect(10, y, 180, 2, 0x8B949E);
    y +=  7;
    buf_fill_rect(10, y, 120, 2, 0x8B949E);
    y +=  7;
    buf_fill_rect(10, y,  90, 2, 0x8B949E);

    /* Prompt 3 — completed command */
    y += 18;
    buf_fill_rect(10, y,      6, 12, 0x3FB950);
    buf_fill_rect(20, y + 3, 45,  2, 0x58A6FF);
    y += 18;
    buf_fill_rect(10, y, 210, 2, 0x8B949E);
    y +=  7;
    buf_fill_rect(10, y, 140, 2, 0x8B949E);

    /* Prompt 4 — active prompt with blinking cursor block */
    y += 18;
    buf_fill_rect(10, y,     6, 12, 0x3FB950);
    buf_fill_rect(20, y,     8, 12, 0xE3B341);  /* cursor */

    /* ── Status bar (22 px at bottom) ───────────────────────────────── */
    buf_fill_rect(0, TERM_H - 22, TERM_W, 22, 0x1F6FEB);
    buf_hline(0, TERM_H - 22, TERM_W, 0x388BFD);

    /* Traffic-light dots (right-aligned) */
    buf_fill_rect(TERM_W - 38, TERM_H - 15, 6, 6, 0xF78166);
    buf_fill_rect(TERM_W - 28, TERM_H - 15, 6, 6, 0xE3B341);
    buf_fill_rect(TERM_W - 18, TERM_H - 15, 6, 6, 0x3FB950);
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    /* Query screen dimensions; fall back to 1024 × 768. */
    uint32_t sw = 1024, sh = 768;
    gfx_screen_info(&sw, &sh);

    /*
     * Centre the terminal window horizontally, slightly above centre
     * vertically.  Clamp so it stays below the taskbar + title bar.
     */
    int tx = ((int)sw - TERM_W) / 2;
    int ty = ((int)sh - TERM_H) / 2 - 20;
    if (ty < 48) ty = 48;   /* taskbar (28 px) + title bar (20 px) */

    /* Register the terminal window with the kernel WM. */
    long term_id = win_create("Terminal", tx, ty, TERM_W, TERM_H);

    /* Render the terminal content into the pixel buffer and upload. */
    render_terminal();
    if (term_id >= 0)
        win_update((int)term_id, s_term_pixels);

    /* First compositor flush: desktop is visible before the shell starts. */
    gfx_flush_display();

    /* Spawn the interactive shell (becomes PID 3). */
    spawn("/SHELL.ELF");

    /* ── Render loop ─────────────────────────────────────────────────── */
    /*
     * Drive the WM compositor continuously.  Do NOT call SYS_KEY_POLL
     * here — that would steal key events from the shell.  All keyboard
     * input is consumed exclusively by the shell's sys_read calls.
     */
    for (;;) {
        gfx_flush_display();
        yield();
    }

    return 0;
}
