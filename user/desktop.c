/*
 * desktop.c — ASOS Desktop Environment (init process, PID 2).
 *
 * Responsibilities:
 *   1. Spawn TERMINAL.ELF — the GUI terminal emulator.
 *   2. Drive the WM compositor via gfx_flush_display() in a render loop.
 *
 * The terminal emulator creates its own WM window, handles keyboard input
 * via key_poll(), and implements a built-in command processor.
 *
 * Build: part of the normal `make` run; installed as DESKTOP.ELF.
 * Boot : kernel/main.c loads it as the first user process.
 */

#include <stdint.h>
#include <unistd.h>
#include <gfx.h>
#include <sys/syscall.h>

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    /* First compositor flush: wallpaper + taskbar visible immediately. */
    gfx_flush_display();

    /* Spawn the terminal emulator. */
    spawn("/TERMINAL.ELF");

    /* ── Render loop ─────────────────────────────────────────────────── */
    /*
     * Drive the WM compositor continuously.  The desktop process does
     * not own any window — it is a background compositor driver.
     * Do NOT call key_poll() here; keyboard input belongs to the
     * terminal emulator (or whichever window is focused).
     */
    for (;;) {
        gfx_flush_display();
        yield();
    }

    return 0;
}
