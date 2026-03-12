/*
 * gfxtest.c — ASOS graphics library demo.
 *
 * Draws a colourful scene into the GFX back buffer and flushes it to the
 * framebuffer.  Then waits for the user to press 'q' to quit.
 *
 * Build: part of the normal `make` run; installed as GFXTEST.ELF.
 * Run  : from the shell, type: GFXTEST
 */

#include <stdint.h>
#include <unistd.h>
#include <gfx.h>
#include <gfx_colors.h>

int main(void)
{
    /* 1. Dark-blue background. */
    gfx_clear(COL_DKBLUE);

    /* 2. A lighter panel in the centre. */
    gfx_fill_rect(40, 40, 560, 400, COL_GRAY);

    /* 3. Outlined rectangles inside the panel. */
    gfx_draw_rect(50,  50, 200, 140, COL_WHITE);
    gfx_fill_rect(55,  55, 190, 130, COL_DKBLUE);
    gfx_draw_rect(270, 50, 200, 140, COL_YELLOW);
    gfx_fill_rect(275, 55, 190, 130, COL_RED);

    /* 4. Horizontal and vertical accent lines. */
    gfx_hline(50,  210, 420, COL_CYAN);
    gfx_hline(50,  212, 420, COL_CYAN);
    gfx_vline(150, 220, 200, COL_GREEN);
    gfx_vline(490, 220, 200, COL_RED);

    /* 5. A nested rectangle in the lower panel. */
    gfx_fill_rect(60, 230, 510, 180, COL_LGRAY);
    gfx_draw_rect(60, 230, 510, 180, COL_WHITE);

    /* 6. Individual pixels — a small diagonal line. */
    for (int i = 0; i < 20; i++)
        gfx_put_pixel(70 + i, 240 + i, COL_YELLOW);

    /* 7. Text labels. */
    gfx_puts( 60,  60, "ASOS GFX TEST", COL_WHITE,  COL_DKBLUE);
    gfx_puts( 60,  80, "Back-buffer + 8x16 font",
              COL_CYAN,   COL_DKBLUE);
    gfx_puts(280,  60, "Filled rect",   COL_WHITE,  COL_DKBLUE);
    gfx_puts(280,  80, "Draw rect",     COL_YELLOW, COL_DKBLUE);

    gfx_puts( 70, 250, "Press 'q' to quit.", COL_BLACK, COL_LGRAY);

    /* 8. Flush back buffer to hardware framebuffer. */
    gfx_flush_display();

    /* 9. Wait for 'q'. */
    char buf[2];
    for (;;) {
        long n = read(0, buf, 1);
        if (n > 0 && (buf[0] == 'q' || buf[0] == 'Q'))
            break;
    }

    return 0;
}
