// i_video_asos.c — DOOM video backend for ASOS.
//
// Replaces the X11 i_video.c for the ASOS operating system.
// Uses the ASOS windowing system (gui_init_window / SYS_WIN_UPDATE)
// and event system (SYS_GET_EVENT) instead of Xlib.
//
// DOOM renders 320x200 in 8-bit indexed colour.  This backend converts
// through a palette LUT to 0x00RRGGBB (the format ASOS expects) and
// optionally scales 2x to 640x400 before blitting into the WM window.

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"
#include "doomdef.h"
#include "d_event.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <asos/gui.h>
#include <sys/syscall.h>
#include <gfx.h>

// ASOS event struct (mirrors event.h but uses a different name to avoid
// collision with DOOM's event_t from d_event.h).
typedef struct {
    uint8_t  type;
    uint8_t  _pad;
    int16_t  x;
    int16_t  y;
    uint16_t code;
} asos_event_t;

static inline int asos_get_event(asos_event_t *out)
{
    return (int)__syscall1(SYS_GET_EVENT, (uint64_t)(uintptr_t)out);
}

// ASOS event type constants.
#define ASOS_EVENT_KEY_PRESS   1
#define ASOS_EVENT_KEY_RELEASE 2
#define ASOS_EVENT_MOUSE_MOVE  3
#define ASOS_EVENT_MOUSE_DOWN  4
#define ASOS_EVENT_MOUSE_UP    5
#define ASOS_EVENT_WIN_CLOSE   6

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

// Scale factor: 1 = native 320x200, 2 = 640x400.
#define DOOM_SCALE  2

#define WIN_W  (SCREENWIDTH  * DOOM_SCALE)   // 640
#define WIN_H  (SCREENHEIGHT * DOOM_SCALE)   // 400

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// ASOS window context (from gui_init_window).
static gui_window_t *doom_window;

// Palette look-up table: maps 8-bit index → 0x00RRGGBB.
static uint32_t doom_palette[256];

// Standalone 32-bit framebuffer for the scaled image.
// Points to doom_window->buffer once the window is created.
static uint32_t *framebuffer_32;

// Mouse state: track last absolute position for computing deltas,
// and current button mask for DOOM's ev_mouse events.
static int lastmousex;
static int lastmousey;
static int mouse_initialized;   // set after first MOUSE_MOVE
static int mouse_buttons;       // bitmask: bit0=left, bit1=right, bit2=middle

// ---------------------------------------------------------------------------
// Keyboard translation (ASOS event code → DOOM keycode)
// ---------------------------------------------------------------------------

// ASOS key codes that come through EVENT_KEY_PRESS / EVENT_KEY_RELEASE.
// Most printable ASCII maps 1:1 to DOOM.  Special keys use codes > 127
// that the ASOS keyboard driver emits for non-ASCII keys.
//
// ASOS scan code set 1 extended codes (from keyboard.c):
//   0x80 = Escape (already 27 in ASCII, but let's be safe)
//   Arrow keys, F-keys, etc. come through as the code field.

// ASOS sends these codes for special keys (matching keyboard.c tables).
// We define them here so the mapping is explicit.
#define ASOS_KEY_UP      0x80
#define ASOS_KEY_DOWN    0x81
#define ASOS_KEY_LEFT    0x82
#define ASOS_KEY_RIGHT   0x83
#define ASOS_KEY_F1      0x84
#define ASOS_KEY_F2      0x85
#define ASOS_KEY_F3      0x86
#define ASOS_KEY_F4      0x87
#define ASOS_KEY_F5      0x88
#define ASOS_KEY_F6      0x89
#define ASOS_KEY_F7      0x8A
#define ASOS_KEY_F8      0x8B
#define ASOS_KEY_F9      0x8C
#define ASOS_KEY_F10     0x8D
#define ASOS_KEY_F11     0x8E
#define ASOS_KEY_F12     0x8F
#define ASOS_KEY_LSHIFT  0x90
#define ASOS_KEY_RSHIFT  0x91
#define ASOS_KEY_LCTRL   0x92
#define ASOS_KEY_RCTRL   0x93
#define ASOS_KEY_LALT    0x94
#define ASOS_KEY_RALT    0x95
#define ASOS_KEY_PAUSE   0x96

static int xlatekey(uint16_t code)
{
    // Printable ASCII: DOOM wants lower-case letters.
    if (code >= 'A' && code <= 'Z')
        return code - 'A' + 'a';
    if (code >= ' ' && code <= '~')
        return code;

    switch (code) {
    case 27:              return KEY_ESCAPE;
    case '\r': case '\n': return KEY_ENTER;
    case '\t':            return KEY_TAB;
    case '\b': case 127:  return KEY_BACKSPACE;

    case ASOS_KEY_UP:     return KEY_UPARROW;
    case ASOS_KEY_DOWN:   return KEY_DOWNARROW;
    case ASOS_KEY_LEFT:   return KEY_LEFTARROW;
    case ASOS_KEY_RIGHT:  return KEY_RIGHTARROW;

    case ASOS_KEY_LSHIFT:
    case ASOS_KEY_RSHIFT: return KEY_RSHIFT;
    case ASOS_KEY_LCTRL:
    case ASOS_KEY_RCTRL:  return KEY_RCTRL;
    case ASOS_KEY_LALT:
    case ASOS_KEY_RALT:   return KEY_RALT;

    case ASOS_KEY_F1:     return KEY_F1;
    case ASOS_KEY_F2:     return KEY_F2;
    case ASOS_KEY_F3:     return KEY_F3;
    case ASOS_KEY_F4:     return KEY_F4;
    case ASOS_KEY_F5:     return KEY_F5;
    case ASOS_KEY_F6:     return KEY_F6;
    case ASOS_KEY_F7:     return KEY_F7;
    case ASOS_KEY_F8:     return KEY_F8;
    case ASOS_KEY_F9:     return KEY_F9;
    case ASOS_KEY_F10:    return KEY_F10;
    case ASOS_KEY_F11:    return KEY_F11;
    case ASOS_KEY_F12:    return KEY_F12;

    case ASOS_KEY_PAUSE:  return KEY_PAUSE;
    case '=':             return KEY_EQUALS;
    case '-':             return KEY_MINUS;

    default:              return 0;  // unknown → ignore
    }
}

// ---------------------------------------------------------------------------
// I_InitGraphics — create the ASOS window
// ---------------------------------------------------------------------------

void I_InitGraphics(void)
{
    static int firsttime = 1;
    if (!firsttime)
        return;
    firsttime = 0;

    // Create a WM window via the ASOS GUI toolkit.
    // gui_init_window allocates a local pixel buffer (WIN_W * WIN_H * 4)
    // and creates the kernel-side window via SYS_WIN_CREATE.
    doom_window = gui_init_window("DOOM", WIN_W, WIN_H);
    if (!doom_window) {
        I_Error("I_InitGraphics: gui_init_window failed");
        return;
    }

    // The pixel buffer inside doom_window is our blit target.
    framebuffer_32 = doom_window->buffer;

    // Allocate DOOM's own 8-bit screen buffer.
    // V_Init usually does this, but some code paths set screens[0]
    // inside I_InitGraphics (the original X11 code did).
    // If V_Init hasn't allocated it yet, do it here.
    if (!screens[0])
        screens[0] = (unsigned char *)malloc(SCREENWIDTH * SCREENHEIGHT);
}

// ---------------------------------------------------------------------------
// I_ShutdownGraphics
// ---------------------------------------------------------------------------

void I_ShutdownGraphics(void)
{
    if (doom_window) {
        gui_destroy_window(doom_window);
        doom_window  = 0;
        framebuffer_32 = 0;
    }
}

// ---------------------------------------------------------------------------
// I_SetPalette — convert DOOM's 768-byte RGB palette into our LUT
// ---------------------------------------------------------------------------

void I_SetPalette(byte *palette)
{
    for (int i = 0; i < 256; i++) {
        unsigned r = gammatable[usegamma][*palette++];
        unsigned g = gammatable[usegamma][*palette++];
        unsigned b = gammatable[usegamma][*palette++];

        // ASOS pixel format: 0x00RRGGBB
        doom_palette[i] = (r << 16) | (g << 8) | b;
    }
}

// ---------------------------------------------------------------------------
// I_FinishUpdate — convert, scale, and push to the WM
// ---------------------------------------------------------------------------

void I_FinishUpdate(void)
{
    if (!doom_window || !framebuffer_32 || !screens[0])
        return;

    // Source: screens[0] is 320 x 200 x 8-bit indexed.
    // Destination: framebuffer_32 is WIN_W x WIN_H x 32-bit (0x00RRGGBB).

    const byte *src = screens[0];

#if DOOM_SCALE == 1
    // No scaling: straight palette look-up.
    uint32_t *dst = framebuffer_32;
    for (int i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++)
        *dst++ = doom_palette[*src++];

#elif DOOM_SCALE == 2
    // 2x nearest-neighbour scale.
    for (int y = 0; y < SCREENHEIGHT; y++) {
        const byte *row = src + y * SCREENWIDTH;
        uint32_t *dst_row0 = framebuffer_32 + (y * 2)     * WIN_W;
        uint32_t *dst_row1 = framebuffer_32 + (y * 2 + 1) * WIN_W;

        for (int x = 0; x < SCREENWIDTH; x++) {
            uint32_t c = doom_palette[row[x]];
            dst_row0[x * 2]     = c;
            dst_row0[x * 2 + 1] = c;
            dst_row1[x * 2]     = c;
            dst_row1[x * 2 + 1] = c;
        }
    }

#else
    // Generic integer scale.
    for (int y = 0; y < SCREENHEIGHT; y++) {
        const byte *row = src + y * SCREENWIDTH;
        for (int x = 0; x < SCREENWIDTH; x++) {
            uint32_t c = doom_palette[row[x]];
            for (int sy = 0; sy < DOOM_SCALE; sy++) {
                uint32_t *dst = framebuffer_32
                    + (y * DOOM_SCALE + sy) * WIN_W
                    + x * DOOM_SCALE;
                for (int sx = 0; sx < DOOM_SCALE; sx++)
                    dst[sx] = c;
            }
        }
    }
#endif

    // Push the 32-bit pixel buffer to the kernel WM.
    // This copies our local buffer into the kernel's window backing store.
    __syscall3(SYS_WIN_UPDATE,
               (uint64_t)doom_window->win_id,
               (uint64_t)(uintptr_t)framebuffer_32,
               (uint64_t)(WIN_W * WIN_H * 4));

    // Signal the compositor to render and blit to the hardware framebuffer.
    gfx_flush_display();
}

// ---------------------------------------------------------------------------
// I_StartTic — poll ASOS events and post them to DOOM
// ---------------------------------------------------------------------------

void I_StartTic(void)
{
    event_t doom_ev;
    asos_event_t asos_ev;

    // Drain all pending ASOS events.
    while (asos_get_event(&asos_ev) == 0) {
        switch (asos_ev.type) {
        case ASOS_EVENT_KEY_PRESS:
            doom_ev.type  = ev_keydown;
            doom_ev.data1 = xlatekey(asos_ev.code);
            if (doom_ev.data1)
                D_PostEvent(&doom_ev);
            break;

        case ASOS_EVENT_KEY_RELEASE:
            doom_ev.type  = ev_keyup;
            doom_ev.data1 = xlatekey(asos_ev.code);
            if (doom_ev.data1)
                D_PostEvent(&doom_ev);
            break;

        case ASOS_EVENT_MOUSE_DOWN:
            // ASOS button mask: bit0=left, bit1=right, bit2=middle
            // DOOM expects the same layout in data1.
            mouse_buttons |= asos_ev.code;
            doom_ev.type  = ev_mouse;
            doom_ev.data1 = mouse_buttons;
            doom_ev.data2 = 0;
            doom_ev.data3 = 0;
            D_PostEvent(&doom_ev);
            break;

        case ASOS_EVENT_MOUSE_UP:
            mouse_buttons &= ~asos_ev.code;
            doom_ev.type  = ev_mouse;
            doom_ev.data1 = mouse_buttons;
            doom_ev.data2 = 0;
            doom_ev.data3 = 0;
            D_PostEvent(&doom_ev);
            break;

        case ASOS_EVENT_MOUSE_MOVE:
            if (!mouse_initialized) {
                // First move event: establish baseline, no delta.
                lastmousex = asos_ev.x;
                lastmousey = asos_ev.y;
                mouse_initialized = 1;
                break;
            }
            {
                // Compute relative motion from absolute positions.
                // Shift left by 2 to match original DOOM X11 sensitivity.
                int dx = (asos_ev.x - lastmousex) << 2;
                int dy = (lastmousey - asos_ev.y) << 2;  // inverted: up = positive
                lastmousex = asos_ev.x;
                lastmousey = asos_ev.y;

                if (dx || dy) {
                    doom_ev.type  = ev_mouse;
                    doom_ev.data1 = mouse_buttons;
                    doom_ev.data2 = dx;
                    doom_ev.data3 = dy;
                    D_PostEvent(&doom_ev);
                }
            }
            break;

        case ASOS_EVENT_WIN_CLOSE:
            I_Quit();
            break;

        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Stubs and trivial implementations
// ---------------------------------------------------------------------------

void I_StartFrame(void)
{
    // Nothing needed.
}

void I_UpdateNoBlit(void)
{
    // Nothing needed.
}

void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

