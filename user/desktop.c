/*
 * desktop.c — ASOS Desktop Environment (init process, PID 2).
 *
 * Responsibilities:
 *   1. Drive the WM compositor via gfx_flush_display() in a render loop.
 *   2. Manage the wallpaper and taskbar (drawn by the kernel compositor).
 *   3. Spawn TERMINAL.ELF — the GUI terminal emulator.
 *
 * The desktop process does not own any window — it is a pure background
 * manager and compositor driver.  Keyboard input belongs to whichever
 * window is currently focused.
 *
 * Build: part of the normal `make` run; installed as DESKTOP.ELF.
 * Boot : kernel/main.c loads it as the first user process.
 */

#include <stdint.h>
#include <unistd.h>
#include <gfx.h>
#include <sys/syscall.h>

int main(void)
{
    /* First compositor flush: wallpaper + taskbar visible immediately. */
    gfx_flush_display();

    /* Spawn the terminal emulator. */
    spawn("/TERMINAL.ELF");

    /* ── Compositor render loop ──────────────────────────────────────── */
    /*
     * Drive the WM compositor continuously.  The desktop process does
     * not own any window — it only ensures the wallpaper, taskbar, and
     * mouse cursor are composited and blitted to the framebuffer each
     * frame.
     *
     * Do NOT call key_poll() here; keyboard input belongs to the
     * focused window's owner process.
     */
    for (;;) {
        gfx_flush_display();
        yield();
    }

    return 0;
}
