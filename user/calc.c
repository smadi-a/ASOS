/*
 * calc.c — ASOS Calculator application.
 *
 * A basic integer calculator with a 4x5 button grid using the
 * asos/gui.h toolkit.  State machine tracks current value, previous
 * value, and a pending arithmetic operation.
 *
 * Build: part of the normal `make` run; installed as CALC.ELF.
 * Run  : from the shell, type: CALC
 */

#include <stdint.h>
#include <unistd.h>
#include <gfx.h>
#include <asos/gui.h>
#include <event.h>
#include <sys/syscall.h>

/* ── Layout constants ──────────────────────────────────────────────────── */

#define WIN_W       240
#define WIN_H       320

#define DISP_X      8
#define DISP_Y      8
#define DISP_W      224
#define DISP_H      40

#define GRID_X      8
#define GRID_Y      56
#define BTN_W       53
#define BTN_H       48
#define BTN_PAD     4

#define DISP_MAX    12

#define ROWS        5
#define COLS        4

/* ── Button colours ────────────────────────────────────────────────────── */

#define COL_OP_FACE   0x00FF9500U   /* Orange — operators              */
#define COL_EQ_FACE   0x004488CCU   /* Blue   — equals                 */
#define COL_CLR_FACE  0x00CC4444U   /* Red    — clear                  */
#define COL_AUX_FACE  0x00666666U   /* Dark grey — auxiliary buttons   */
#define COL_BTN_TEXT  0x00FFFFFFU   /* White text on coloured buttons  */

#define COL_DISP_FACE 0x00223322U   /* Dark green display background   */
#define COL_DISP_TEXT 0x0044FF44U   /* Bright green display text       */

/* ── Button grid labels (4 columns x 5 rows) ──────────────────────────── */

static const char *s_labels[ROWS][COLS] = {
    { "C",  "<",  "%",  "/" },
    { "7",  "8",  "9",  "*" },
    { "4",  "5",  "6",  "-" },
    { "1",  "2",  "3",  "+" },
    { "~",  "0",  ".",  "=" },
};

/* ── State machine ─────────────────────────────────────────────────────── */

static gui_window_t  *s_win;
static gui_widget_t  *s_btns[ROWS][COLS];

static char  s_display[DISP_MAX + 1];
static long  s_prev_value;
static char  s_pending_op;
static int   s_new_input;

/* ── Helpers ───────────────────────────────────────────────────────────── */

static int slen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static long parse_display(void)
{
    long val = 0;
    int  neg = 0;
    const char *p = s_display;

    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return neg ? -val : val;
}

static void set_display_value(long val)
{
    int  neg = 0;
    unsigned long uv;

    if (val < 0) { neg = 1; uv = (unsigned long)(-val); }
    else         { uv = (unsigned long)val; }

    /* Build digits in reverse into a temp buffer. */
    char tmp[DISP_MAX + 1];
    int  t = 0;
    if (uv == 0) {
        tmp[t++] = '0';
    } else {
        while (uv > 0 && t < DISP_MAX) {
            tmp[t++] = '0' + (char)(uv % 10);
            uv /= 10;
        }
    }

    /* Copy into s_display in the correct order. */
    int i = 0;
    if (neg && i < DISP_MAX) s_display[i++] = '-';
    while (t > 0 && i < DISP_MAX) s_display[i++] = tmp[--t];
    s_display[i] = '\0';
}

static long evaluate(long a, char op, long b)
{
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return (b != 0) ? a / b : 0;
    case '%': return (b != 0) ? a % b : 0;
    default:  return b;
    }
}

static void reset_state(void)
{
    s_display[0] = '0';
    s_display[1] = '\0';
    s_prev_value = 0;
    s_pending_op = 0;
    s_new_input  = 1;
}

/* ── Button click callback ─────────────────────────────────────────────── */

static void on_btn_click(gui_window_t *win, gui_widget_t *w)
{
    (void)win;
    char ch = w->label[0];

    /* ── Digit 0-9 ──────────────────────────────────────────────────── */
    if (ch >= '0' && ch <= '9') {
        if (s_new_input) {
            s_display[0] = ch;
            s_display[1] = '\0';
            s_new_input  = 0;
        } else {
            int len = slen(s_display);
            if (len == 1 && s_display[0] == '0') {
                /* Replace leading zero. */
                s_display[0] = ch;
            } else if (len < DISP_MAX) {
                s_display[len]     = ch;
                s_display[len + 1] = '\0';
            }
        }
        return;
    }

    /* ── Clear ──────────────────────────────────────────────────────── */
    if (ch == 'C') {
        reset_state();
        return;
    }

    /* ── Backspace ──────────────────────────────────────────────────── */
    if (ch == '<') {
        int len = slen(s_display);
        if (len > 1)
            s_display[len - 1] = '\0';
        else {
            s_display[0] = '0';
            s_display[1] = '\0';
        }
        return;
    }

    /* ── Negate ─────────────────────────────────────────────────────── */
    if (ch == '~') {
        if (s_display[0] == '-') {
            /* Remove minus sign. */
            int i;
            for (i = 0; s_display[i]; i++)
                s_display[i] = s_display[i + 1];
        } else if (!(s_display[0] == '0' && s_display[1] == '\0')) {
            int len = slen(s_display);
            if (len < DISP_MAX) {
                int i;
                for (i = len + 1; i > 0; i--)
                    s_display[i] = s_display[i - 1];
                s_display[0] = '-';
            }
        }
        return;
    }

    /* ── Decimal point — no-op (integer calculator) ─────────────────── */
    if (ch == '.')
        return;

    /* ── Operators: + - * / % ───────────────────────────────────────── */
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%') {
        if (!s_new_input && s_pending_op) {
            /* Chain evaluation: apply previous op first. */
            long cur    = parse_display();
            long result = evaluate(s_prev_value, s_pending_op, cur);
            set_display_value(result);
            s_prev_value = result;
        } else {
            s_prev_value = parse_display();
        }
        s_pending_op = ch;
        s_new_input  = 1;
        return;
    }

    /* ── Equals ─────────────────────────────────────────────────────── */
    if (ch == '=') {
        if (s_pending_op) {
            long cur    = parse_display();
            long result = evaluate(s_prev_value, s_pending_op, cur);
            set_display_value(result);
            s_prev_value = 0;
            s_pending_op = 0;
            s_new_input  = 1;
        }
        return;
    }
}

/* ── draw_calc — Render the entire calculator UI ───────────────────────── */

static void draw_calc(void)
{
    uint32_t *buf = s_win->buffer;
    int       w   = s_win->width;
    int       h   = s_win->height;

    /* Clear background to window face colour. */
    for (int i = 0; i < w * h; i++)
        buf[i] = GUI_COL_BG;

    /* Draw display area (sunken look: pressed=1). */
    draw_button_ex(s_win, DISP_X, DISP_Y, DISP_W, DISP_H,
                   s_display, 1, COL_DISP_FACE, COL_DISP_TEXT);

    /* Draw the 4x5 button grid. */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int bx = GRID_X + c * (BTN_W + BTN_PAD);
            int by = GRID_Y + r * (BTN_H + BTN_PAD);
            gui_widget_t *btn = s_btns[r][c];

            draw_button_ex(s_win, bx, by, BTN_W, BTN_H,
                           s_labels[r][c],
                           btn ? btn->pressed : 0,
                           btn ? btn->face_color : 0,
                           btn ? btn->text_color : 0);
        }
    }

    /* Push the pixel buffer to the kernel window manager. */
    __syscall3(SYS_WIN_UPDATE,
               (uint64_t)s_win->win_id,
               (uint64_t)(uintptr_t)s_win->buffer,
               (uint64_t)((unsigned)w * (unsigned)h * 4));
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    s_win = gui_init_window("Calculator", WIN_W, WIN_H);
    if (!s_win)
        return 1;

    reset_state();

    /* Register button widgets (for hit-testing in gui_poll_events). */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int bx = GRID_X + c * (BTN_W + BTN_PAD);
            int by = GRID_Y + r * (BTN_H + BTN_PAD);

            gui_widget_t *btn = gui_add_button(s_win, bx, by,
                                               BTN_W, BTN_H,
                                               s_labels[r][c]);
            if (btn) {
                btn->on_click = on_btn_click;

                /* Colour-code by role. */
                char ch = s_labels[r][c][0];
                if (ch == 'C') {
                    btn->face_color = COL_CLR_FACE;
                    btn->text_color = COL_BTN_TEXT;
                } else if (ch == '=') {
                    btn->face_color = COL_EQ_FACE;
                    btn->text_color = COL_BTN_TEXT;
                } else if (ch == '+' || ch == '-' || ch == '*' ||
                           ch == '/' || ch == '%') {
                    btn->face_color = COL_OP_FACE;
                    btn->text_color = COL_BTN_TEXT;
                } else if (ch == '<' || ch == '~' || ch == '.') {
                    btn->face_color = COL_AUX_FACE;
                    btn->text_color = COL_BTN_TEXT;
                }
                /* Digits keep the default GUI_COL_FACE. */
            }
            s_btns[r][c] = btn;
        }
    }

    /* Main loop: poll input, render, composite, yield. */
    for (;;) {
        int key = gui_poll_events(s_win);

        if (key == -1)              /* Window closed */
            break;
        if (key == 'q' || key == 'Q')
            break;

        draw_calc();
        gfx_flush_display();
        yield();
    }

    gui_destroy_window(s_win);
    return 0;
}
