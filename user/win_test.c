/*
 * win_test.c — ASOS GUI toolkit demo application.
 *
 * Creates a window using the gui.h toolkit with a button that changes
 * colour when clicked.  Demonstrates EVENT_WIN_CLOSE handling.
 *
 * Build: part of the normal `make` run; installed as WINTEST.ELF.
 * Run  : from the shell, type: WINTEST
 */

#include <stdint.h>
#include <unistd.h>
#include <gfx.h>
#include <asos/gui.h>
#include <event.h>
#include <sys/syscall.h>

/* ── Colour palette for the cycling button ─────────────────────────────── */

#define NUM_COLORS 6

static const uint32_t s_colors[NUM_COLORS] = {
    0x00CC4444U,   /* Red        */
    0x0044AA44U,   /* Green      */
    0x004466CCU,   /* Blue       */
    0x00CC8800U,   /* Orange     */
    0x008844CCU,   /* Purple     */
    0x0044AAAAU,   /* Teal       */
};

static const char * const s_color_names[NUM_COLORS] = {
    "Red", "Green", "Blue", "Orange", "Purple", "Teal",
};

static int s_color_idx = 0;
static gui_widget_t *s_btn    = 0;
static gui_widget_t *s_status = 0;

/* ── Helpers ───────────────────────────────────────────────────────────── */

static void copy_label(char *dst, const char *src)
{
    int i = 0;
    while (src[i] && i < GUI_LABEL_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void update_status(void)
{
    if (!s_status) return;
    char buf[GUI_LABEL_MAX];
    const char *prefix = "Color: ";
    const char *name = s_color_names[s_color_idx];
    int i = 0, j = 0;
    while (prefix[j] && i < GUI_LABEL_MAX - 1) buf[i++] = prefix[j++];
    j = 0;
    while (name[j] && i < GUI_LABEL_MAX - 1)   buf[i++] = name[j++];
    buf[i] = '\0';
    copy_label(s_status->label, buf);
}

/* ── Button click handler ──────────────────────────────────────────────── */

static void on_color_click(gui_window_t *win, gui_widget_t *w)
{
    (void)win;
    s_color_idx = (s_color_idx + 1) % NUM_COLORS;

    /* Update button label and face colour. */
    copy_label(w->label, s_color_names[s_color_idx]);
    w->face_color = s_colors[s_color_idx];
    w->text_color = 0x00FFFFFFU;   /* White text on coloured face */

    update_status();
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    gui_window_t *win = gui_init_window("Color Demo", 220, 140);
    if (!win)
        return 1;

    /* Title label. */
    gui_add_label(win, 30, 16, "Click the button!");

    /* Colour-cycling button. */
    s_btn = gui_add_button(win, 50, 50, 120, 32, "Red");
    if (s_btn) {
        s_btn->on_click   = on_color_click;
        s_btn->face_color = s_colors[0];
        s_btn->text_color = 0x00FFFFFFU;
    }

    /* Status label. */
    s_status = gui_add_label(win, 50, 100, "Color: Red");

    for (;;) {
        int key = gui_poll_events(win);

        /* gui_poll_events returns -1 on EVENT_WIN_CLOSE. */
        if (key == -1)
            break;

        /* Also quit on 'q'/'Q'. */
        if (key == 'q' || key == 'Q')
            break;

        gui_draw(win);
        gfx_flush_display();
        yield();
    }

    gui_destroy_window(win);
    return 0;
}
