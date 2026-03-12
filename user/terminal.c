/*
 * user/terminal.c — ASOS Terminal Emulator.
 *
 * A GUI terminal emulator running as a WM client.  Renders an 80×25
 * character grid into a pixel buffer using an embedded 8×16 VGA font,
 * polls keyboard via SYS_KEY_POLL, and implements a built-in command
 * processor (since there is no PTY layer to redirect shell I/O).
 *
 * Build: part of `make`; installed as TERMINAL.ELF on the FAT32 image.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <gfx.h>

/* ── Terminal grid dimensions ───────────────────────────────────────────── */

#define COLS        80
#define ROWS        25
#define FONT_W       8
#define FONT_H      16
#define WIN_W       (COLS * FONT_W)   /* 640 */
#define WIN_H       (ROWS * FONT_H)   /* 400 */

/* ── Colours ────────────────────────────────────────────────────────────── */

#define COL_BG      0x00000000U   /* black background   */
#define COL_FG      0x00BBBBBBU   /* light grey text     */
#define COL_PROMPT  0x0022CC22U   /* green prompt        */
#define COL_CMD     0x0058A6FFU   /* blue typed text     */
#define COL_ERR     0x00CC2222U   /* red error text      */
#define COL_CURSOR  0x00BBBBBBU   /* cursor block colour */
#define COL_INFO    0x0000CCCCU   /* cyan info text      */

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

static inline long key_poll(void)
{
    return __syscall0(SYS_KEY_POLL);
}

/* ── Embedded 8×16 VGA bitmap font ─────────────────────────────────────── *
 *
 * 128 glyphs × 16 bytes each.  Printable ASCII 0x20–0x7E populated;
 * everything else is blank.  Each byte = one pixel row, MSB = leftmost.
 */

#define _Z  0x00
#define _R(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p) \
    a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p
#define _BL _R(_Z,_Z,_Z,_Z,_Z,_Z,_Z,_Z,_Z,_Z,_Z,_Z,_Z,_Z,_Z,_Z)

static const uint8_t g_font[128 * 16] = {
    /* 0x00–0x1F: control characters (blank) */
    _BL, _BL, _BL, _BL, _BL, _BL, _BL, _BL,
    _BL, _BL, _BL, _BL, _BL, _BL, _BL, _BL,
    _BL, _BL, _BL, _BL, _BL, _BL, _BL, _BL,
    _BL, _BL, _BL, _BL, _BL, _BL, _BL, _BL,

    /* 0x20 ' ' */ _BL,
    /* 0x21 '!' */ _R(0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00),
    /* 0x22 '"' */ _R(0x00,0x00,0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x23 '#' */ _R(0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x24 '$' */ _R(0x18,0x18,0x7C,0xC6,0xC0,0x7C,0x06,0x06,0xC6,0x7C,0x18,0x18,0x00,0x00,0x00,0x00),
    /* 0x25 '%' */ _R(0x00,0x00,0xC6,0xCC,0xD8,0x38,0x6C,0xD6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x26 '&' */ _R(0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x27 '\''*/ _R(0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x28 '(' */ _R(0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x29 ')' */ _R(0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x2A '*' */ _R(0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x2B '+' */ _R(0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x2C ',' */ _R(0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x2D '-' */ _R(0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x2E '.' */ _R(0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x2F '/' */ _R(0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x30 '0' */ _R(0x00,0x00,0x3C,0x66,0x6E,0x76,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x31 '1' */ _R(0x00,0x00,0x18,0x38,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x32 '2' */ _R(0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x33 '3' */ _R(0x00,0x00,0x3C,0x66,0x06,0x1C,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x34 '4' */ _R(0x00,0x00,0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x35 '5' */ _R(0x00,0x00,0x7E,0x60,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x36 '6' */ _R(0x00,0x00,0x1C,0x30,0x60,0x7C,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x37 '7' */ _R(0x00,0x00,0x7E,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x38 '8' */ _R(0x00,0x00,0x3C,0x66,0x66,0x3C,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x39 '9' */ _R(0x00,0x00,0x3C,0x66,0x66,0x3E,0x06,0x0C,0x18,0x38,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x3A ':' */ _R(0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x3B ';' */ _R(0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x3C '<' */ _R(0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00),
    /* 0x3D '=' */ _R(0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x3E '>' */ _R(0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00),
    /* 0x3F '?' */ _R(0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x40 '@' */ _R(0x00,0x00,0x3C,0x66,0x6E,0x6E,0x6E,0x60,0x62,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x41 'A' */ _R(0x00,0x00,0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x42 'B' */ _R(0x00,0x00,0x7C,0x66,0x66,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x43 'C' */ _R(0x00,0x00,0x3C,0x66,0x60,0x60,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x44 'D' */ _R(0x00,0x00,0x78,0x6C,0x66,0x66,0x66,0x66,0x6C,0x78,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x45 'E' */ _R(0x00,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x46 'F' */ _R(0x00,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x47 'G' */ _R(0x00,0x00,0x3C,0x66,0x60,0x60,0x6E,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x48 'H' */ _R(0x00,0x00,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x49 'I' */ _R(0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x4A 'J' */ _R(0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x4B 'K' */ _R(0x00,0x00,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x4C 'L' */ _R(0x00,0x00,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x4D 'M' */ _R(0x00,0x00,0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x4E 'N' */ _R(0x00,0x00,0x66,0x76,0x7E,0x6E,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x4F 'O' */ _R(0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x50 'P' */ _R(0x00,0x00,0x7C,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x51 'Q' */ _R(0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x6E,0x3C,0x06,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x52 'R' */ _R(0x00,0x00,0x7C,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x53 'S' */ _R(0x00,0x00,0x3C,0x66,0x60,0x30,0x18,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x54 'T' */ _R(0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x55 'U' */ _R(0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x56 'V' */ _R(0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x57 'W' */ _R(0x00,0x00,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0xEE,0xC6,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x58 'X' */ _R(0x00,0x00,0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x59 'Y' */ _R(0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x5A 'Z' */ _R(0x00,0x00,0x7E,0x06,0x0C,0x18,0x30,0x60,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x5B '[' */ _R(0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x5C '\' */ _R(0x00,0x00,0xC0,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x5D ']' */ _R(0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x5E '^' */ _R(0x00,0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x5F '_' */ _R(0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x60 '`' */ _R(0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x61 'a' */ _R(0x00,0x00,0x00,0x00,0x3C,0x06,0x3E,0x66,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x62 'b' */ _R(0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x63 'c' */ _R(0x00,0x00,0x00,0x00,0x3C,0x66,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x64 'd' */ _R(0x00,0x00,0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x65 'e' */ _R(0x00,0x00,0x00,0x00,0x3C,0x66,0x7E,0x60,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x66 'f' */ _R(0x00,0x00,0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x67 'g' */ _R(0x00,0x00,0x00,0x00,0x3E,0x66,0x66,0x66,0x3E,0x06,0x3C,0x00,0x00,0x00,0x00,0x00),
    /* 0x68 'h' */ _R(0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x69 'i' */ _R(0x00,0x00,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x6A 'j' */ _R(0x00,0x00,0x0C,0x00,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x6B 'k' */ _R(0x00,0x00,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x6C 'l' */ _R(0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x6D 'm' */ _R(0x00,0x00,0x00,0x00,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x6E 'n' */ _R(0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x6F 'o' */ _R(0x00,0x00,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x70 'p' */ _R(0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x00,0x00,0x00,0x00),
    /* 0x71 'q' */ _R(0x00,0x00,0x00,0x00,0x3E,0x66,0x66,0x66,0x3E,0x06,0x06,0x06,0x00,0x00,0x00,0x00),
    /* 0x72 'r' */ _R(0x00,0x00,0x00,0x00,0x6C,0x76,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x73 's' */ _R(0x00,0x00,0x00,0x00,0x3C,0x60,0x3C,0x06,0x06,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x74 't' */ _R(0x00,0x00,0x30,0x30,0x7C,0x30,0x30,0x30,0x30,0x1C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x75 'u' */ _R(0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3E,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x76 'v' */ _R(0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x77 'w' */ _R(0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x78 'x' */ _R(0x00,0x00,0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x79 'y' */ _R(0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x7A 'z' */ _R(0x00,0x00,0x00,0x00,0x7E,0x0C,0x18,0x30,0x60,0x7E,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x7B '{' */ _R(0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00,0x00),
    /* 0x7C '|' */ _R(0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00),
    /* 0x7D '}' */ _R(0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00,0x00),
    /* 0x7E '~' */ _R(0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00),
    /* 0x7F DEL */ _BL,
};

#undef _Z
#undef _R
#undef _BL

/* ── Terminal state ─────────────────────────────────────────────────────── */

static char     s_grid[ROWS][COLS];
static uint32_t s_color[ROWS][COLS];
static int      s_cx, s_cy;          /* cursor position (col, row) */
static int      s_dirty;             /* redraw needed? */

/* Pixel buffer for the WM window — stored in BSS. */
static uint32_t s_pixels[WIN_W * WIN_H];

/* Input line buffer */
#define INPUT_MAX 256
static char s_input[INPUT_MAX];
static int  s_input_len;

/* Prompt tracking — where the current input line starts */
static int s_prompt_cx, s_prompt_cy;

/* ── Pixel buffer rendering ─────────────────────────────────────────────── */

static void render_char(int col, int row, char ch, uint32_t fg)
{
    int px = col * FONT_W;
    int py = row * FONT_H;
    unsigned int c = (unsigned char)ch;
    if (c >= 128) c = '?';
    const uint8_t *glyph = &g_font[c * 16];

    for (int y = 0; y < FONT_H; y++) {
        uint8_t bits = glyph[y];
        int off = (py + y) * WIN_W + px;
        for (int x = 0; x < FONT_W; x++) {
            s_pixels[off + x] = (bits & (0x80 >> x)) ? fg : COL_BG;
        }
    }
}

static void render_cursor(void)
{
    if (s_cy < 0 || s_cy >= ROWS || s_cx < 0 || s_cx >= COLS) return;
    int px = s_cx * FONT_W;
    int py = s_cy * FONT_H;
    /* Draw a solid block cursor */
    for (int y = 0; y < FONT_H; y++) {
        int off = (py + y) * WIN_W + px;
        for (int x = 0; x < FONT_W; x++)
            s_pixels[off + x] = COL_CURSOR;
    }
}

static void term_redraw(void)
{
    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            char ch = s_grid[row][col];
            uint32_t fg = s_color[row][col];
            if (ch == 0) ch = ' ';
            if (fg == 0) fg = COL_FG;
            render_char(col, row, ch, fg);
        }
    }
    render_cursor();
}

/* ── Grid manipulation ──────────────────────────────────────────────────── */

static void scroll_up(void)
{
    memmove(s_grid[0], s_grid[1], (ROWS - 1) * COLS);
    memmove(s_color[0], s_color[1], (ROWS - 1) * COLS * sizeof(uint32_t));
    memset(s_grid[ROWS - 1], 0, COLS);
    memset(s_color[ROWS - 1], 0, COLS * sizeof(uint32_t));
}

static void term_putc_color(char c, uint32_t fg)
{
    if (c == '\n') {
        s_cx = 0;
        s_cy++;
        if (s_cy >= ROWS) {
            scroll_up();
            s_cy = ROWS - 1;
        }
        s_dirty = 1;
        return;
    }

    if (c == '\b') {
        if (s_cx > 0) {
            s_cx--;
            s_grid[s_cy][s_cx] = 0;
            s_color[s_cy][s_cx] = 0;
        }
        s_dirty = 1;
        return;
    }

    if (c == '\t') {
        int spaces = 4 - (s_cx % 4);
        for (int i = 0; i < spaces; i++)
            term_putc_color(' ', fg);
        return;
    }

    if (s_cx >= COLS) {
        s_cx = 0;
        s_cy++;
        if (s_cy >= ROWS) {
            scroll_up();
            s_cy = ROWS - 1;
        }
    }

    s_grid[s_cy][s_cx] = c;
    s_color[s_cy][s_cx] = fg;
    s_cx++;
    s_dirty = 1;
}

static void term_putc(char c)
{
    term_putc_color(c, COL_FG);
}

static void term_puts_color(const char *s, uint32_t fg)
{
    while (*s)
        term_putc_color(*s++, fg);
}

static void term_puts(const char *s)
{
    term_puts_color(s, COL_FG);
}

static void term_clear(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    memset(s_color, 0, sizeof(s_color));
    s_cx = 0;
    s_cy = 0;
    s_dirty = 1;
}

/* ── Simple number-to-string for the terminal ───────────────────────────── */

static void term_put_dec(long v, uint32_t fg)
{
    if (v < 0) {
        term_putc_color('-', fg);
        v = -v;
    }
    if (v == 0) {
        term_putc_color('0', fg);
        return;
    }
    char buf[20];
    int i = 0;
    while (v) {
        buf[i++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (i--)
        term_putc_color(buf[i], fg);
}

static void term_put_size(uint64_t bytes, uint32_t fg)
{
    uint64_t kb = bytes / 1024;
    term_put_dec((long)kb, fg);
    term_puts_color(" KB", fg);
}

/* ── Prompt ─────────────────────────────────────────────────────────────── */

static void show_prompt(void)
{
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) != 0)
        cwd[0] = '/', cwd[1] = '\0';
    term_puts_color("asos", COL_PROMPT);
    term_puts_color(":", COL_FG);
    term_puts_color(cwd, COL_INFO);
    term_puts_color("> ", COL_FG);
    s_prompt_cx = s_cx;
    s_prompt_cy = s_cy;
    s_input_len = 0;
    s_input[0] = '\0';
    s_dirty = 1;
}

/* ── Command helpers ────────────────────────────────────────────────────── */

static void trim(char *str)
{
    char *start = str;
    while (*start && (*start == ' ' || *start == '\t')) start++;
    if (start != str) memmove(str, start, strlen(start) + 1);
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t'))
        str[--len] = '\0';
}

/* ── Built-in commands ──────────────────────────────────────────────────── */

static void cmd_help(void)
{
    term_puts_color("ASOS Terminal v1.0\n\n", COL_INFO);
    term_puts("Navigation:\n");
    term_puts("  l [dir]              List directory\n");
    term_puts("  go [dir]             Change directory\n");
    term_puts("  path                 Show current directory\n");
    term_puts("\nFile viewing:\n");
    term_puts("  show <file>          Display file contents\n");
    term_puts("  top [n] <file>       First n lines (default 10)\n");
    term_puts("\nFile operations:\n");
    term_puts("  new <file>           Create empty file\n");
    term_puts("  copy <src> <dst>     Copy file\n");
    term_puts("  move <src> <dst>     Rename/move file\n");
    term_puts("  delete <file>        Delete file\n");
    term_puts("  newdir <name>        Create directory\n");
    term_puts("  deletedir <name>     Remove empty directory\n");
    term_puts("\nSystem:\n");
    term_puts("  say <text>           Print text\n");
    term_puts("  clear                Clear screen\n");
    term_puts("  pid                  Show terminal PID\n");
    term_puts("  proc                 List processes\n");
    term_puts("  end <pid>            Kill a process\n");
    term_puts("  disk                 Filesystem usage\n");
    term_puts("  help                 This help\n");
    term_puts("  exit                 Exit terminal\n");
    term_puts("\nType any program name to run it.\n");
}

static void cmd_l(const char *args)
{
    const char *path;
    char cwd[256];
    if (args && *args) {
        path = args;
    } else {
        getcwd(cwd, sizeof(cwd));
        path = cwd;
    }

    dirent_t entries[64];
    int count = readdir(path, entries, 64);
    if (count < 0) {
        term_puts_color("l: cannot list '", COL_ERR);
        term_puts_color(path, COL_ERR);
        term_puts_color("'\n", COL_ERR);
        return;
    }
    if (count == 0) {
        term_puts("(empty)\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        if (entries[i].is_directory) {
            term_puts("  [DIR]       ");
            term_puts_color(entries[i].name, COL_INFO);
            term_putc('\n');
        } else {
            term_puts("  ");
            /* Right-align file size in a 10-char field */
            char sizebuf[16];
            snprintf(sizebuf, sizeof(sizebuf), "%10d", entries[i].size);
            term_puts(sizebuf);
            term_puts("  ");
            term_puts(entries[i].name);
            term_putc('\n');
        }
    }
    char countbuf[32];
    snprintf(countbuf, sizeof(countbuf), "\n%d item%s\n", count, count == 1 ? "" : "s");
    term_puts(countbuf);
}

static void cmd_go(const char *args)
{
    const char *path = (args && *args) ? args : "/";
    if (chdir(path) != 0) {
        term_puts_color("go: '", COL_ERR);
        term_puts_color(path, COL_ERR);
        term_puts_color("': no such directory\n", COL_ERR);
    }
}

static void cmd_path(void)
{
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd)) == 0)
        term_puts(cwd);
    else
        term_puts_color("path: unable to determine", COL_ERR);
    term_putc('\n');
}

static void cmd_show(const char *args)
{
    if (!args || !*args) {
        term_puts("Usage: show <filename>\n");
        return;
    }
    int fd = fopen(args);
    if (fd < 0) {
        term_puts_color("show: '", COL_ERR);
        term_puts_color(args, COL_ERR);
        term_puts_color("': not found\n", COL_ERR);
        return;
    }
    char buf[512];
    long n;
    while ((n = fread(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        term_puts(buf);
    }
    term_putc('\n');
    fclose(fd);
}

static void cmd_top(const char *args)
{
    if (!args || !*args) {
        term_puts("Usage: top [n] <filename>\n");
        return;
    }

    int lines = 10;
    const char *filename = args;

    if (isdigit(args[0])) {
        lines = atoi(args);
        if (lines <= 0) lines = 10;
        filename = strchr(args, ' ');
        if (!filename) {
            term_puts("Usage: top [n] <filename>\n");
            return;
        }
        filename++;
        while (*filename == ' ') filename++;
    }

    int fd = fopen(filename);
    if (fd < 0) {
        term_puts_color("top: '", COL_ERR);
        term_puts_color(filename, COL_ERR);
        term_puts_color("': not found\n", COL_ERR);
        return;
    }

    int line_count = 0;
    char buf[512];
    long n;
    while (line_count < lines && (n = fread(fd, buf, sizeof(buf) - 1)) > 0) {
        for (long i = 0; i < n && line_count < lines; i++) {
            term_putc(buf[i]);
            if (buf[i] == '\n') line_count++;
        }
    }
    if (line_count == 0) term_putc('\n');
    fclose(fd);
}

static void cmd_new(const char *args)
{
    if (!args || !*args) {
        term_puts("Usage: new <filename>\n");
        return;
    }
    if (fcreate(args) != 0)
        term_puts_color("new: cannot create file\n", COL_ERR);
}

static void cmd_copy(const char *args)
{
    if (!args || !*args) {
        term_puts("Usage: copy <source> <destination>\n");
        return;
    }
    char argbuf[512];
    strncpy(argbuf, args, sizeof(argbuf) - 1);
    argbuf[sizeof(argbuf) - 1] = '\0';
    char *dst = strchr(argbuf, ' ');
    if (!dst) { term_puts("Usage: copy <source> <destination>\n"); return; }
    *dst++ = '\0';
    while (*dst == ' ') dst++;
    if (!*dst) { term_puts("Usage: copy <source> <destination>\n"); return; }
    if (copy(argbuf, dst) != 0)
        term_puts_color("copy: failed\n", COL_ERR);
}

static void cmd_move(const char *args)
{
    if (!args || !*args) {
        term_puts("Usage: move <source> <destination>\n");
        return;
    }
    char argbuf[512];
    strncpy(argbuf, args, sizeof(argbuf) - 1);
    argbuf[sizeof(argbuf) - 1] = '\0';
    char *dst = strchr(argbuf, ' ');
    if (!dst) { term_puts("Usage: move <source> <destination>\n"); return; }
    *dst++ = '\0';
    while (*dst == ' ') dst++;
    if (!*dst) { term_puts("Usage: move <source> <destination>\n"); return; }
    if (rename(argbuf, dst) != 0)
        term_puts_color("move: failed\n", COL_ERR);
}

static void cmd_delete(const char *args)
{
    if (!args || !*args) { term_puts("Usage: delete <filename>\n"); return; }
    if (fdelete(args) != 0)
        term_puts_color("delete: cannot delete file\n", COL_ERR);
}

static void cmd_md(const char *args)
{
    if (!args || !*args) { term_puts("Usage: newdir <name>\n"); return; }
    if (mkdir(args) != 0)
        term_puts_color("newdir: cannot create directory\n", COL_ERR);
}

static void cmd_deletedir(const char *args)
{
    if (!args || !*args) { term_puts("Usage: deletedir <name>\n"); return; }
    if (rmdir(args) != 0)
        term_puts_color("deletedir: cannot remove (not found or not empty)\n", COL_ERR);
}

static void cmd_say(const char *args)
{
    if (args && *args)
        term_puts(args);
    term_putc('\n');
}

static void cmd_pid(void)
{
    term_puts("Terminal PID: ");
    term_put_dec(getpid(), COL_FG);
    term_putc('\n');
}

static void cmd_proc(void)
{
    proc_info_t procs[32];
    int count = proclist(procs, 32);
    if (count < 0) {
        term_puts_color("proc: failed\n", COL_ERR);
        return;
    }
    term_puts_color("PID   PPID  STATE    NAME\n", COL_INFO);
    static const char *states[] = {"CREATED","RUNNING","READY","BLOCKED","DEAD"};
    for (int i = 0; i < count; i++) {
        char line[80];
        const char *st = procs[i].state <= 4 ? states[procs[i].state] : "?";
        snprintf(line, sizeof(line), "%-5ld %-5ld %-8s %s\n",
                 (long)procs[i].pid, (long)procs[i].parent_pid,
                 st, procs[i].name);
        term_puts(line);
    }
}

static void cmd_end(const char *args)
{
    if (!args || !*args) { term_puts("Usage: end <pid>\n"); return; }
    long pid = atol(args);
    if (pid <= 0) { term_puts_color("end: invalid PID\n", COL_ERR); return; }
    if (kill(pid) != 0) {
        term_puts_color("end: process not found\n", COL_ERR);
    } else {
        term_puts("Terminated PID ");
        term_put_dec(pid, COL_FG);
        term_putc('\n');
    }
}

static void cmd_disk(void)
{
    fs_stat_t stat;
    if (fsstat(&stat) != 0) {
        term_puts_color("disk: cannot read stats\n", COL_ERR);
        return;
    }
    uint64_t pct = stat.total_bytes > 0
                   ? (stat.used_bytes * 100) / stat.total_bytes : 0;

    term_puts_color("Filesystem    Total     Used     Free   Use%\n", COL_INFO);
    term_puts("/          ");
    term_put_size(stat.total_bytes, COL_FG);
    term_puts("  ");
    term_put_size(stat.used_bytes, COL_FG);
    term_puts("  ");
    term_put_size(stat.free_bytes, COL_FG);
    term_puts("   ");
    term_put_dec((long)pct, COL_FG);
    term_puts("%\n");
}

/* ── External program execution ─────────────────────────────────────────── */

static void run_program(const char *name)
{
    char path[256];
    if (name[0] == '/')
        strncpy(path, name, 255);
    else {
        path[0] = '/';
        strncpy(path + 1, name, 254);
    }
    path[255] = '\0';

    /* Uppercase for FAT32 8.3 */
    for (int i = 0; path[i]; i++)
        path[i] = (char)toupper(path[i]);

    /* Append .ELF if no extension */
    if (!strchr(path + 1, '.')) {
        size_t len = strlen(path);
        if (len + 4 < 256) strcat(path, ".ELF");
    }

    long pid = spawn(path);
    if (pid < 0) {
        term_puts_color(name, COL_ERR);
        term_puts_color(": command not found\n", COL_ERR);
        return;
    }

    /* Wait for child — yield while it runs, keeping compositor alive. */
    int status;
    waitpid(pid, &status);
    if (status != 0) {
        term_puts_color("Process exited with status ", COL_ERR);
        term_put_dec(status, COL_ERR);
        term_putc('\n');
    }
}

/* ── Command dispatch ───────────────────────────────────────────────────── */

static void process_command(char *cmd)
{
    trim(cmd);
    if (cmd[0] == '\0') return;

    /* Split command and args */
    char *args = strchr(cmd, ' ');
    if (args) {
        *args++ = '\0';
        while (*args == ' ') args++;
        if (*args == '\0') args = NULL;
    }

    if      (strcmp(cmd, "help") == 0)       cmd_help();
    else if (strcmp(cmd, "exit") == 0)       exit(0);
    else if (strcmp(cmd, "clear") == 0 ||
             strcmp(cmd, "clean") == 0)      term_clear();
    else if (strcmp(cmd, "say") == 0 ||
             strcmp(cmd, "echo") == 0)       cmd_say(args);
    else if (strcmp(cmd, "l") == 0 ||
             strcmp(cmd, "ls") == 0)         cmd_l(args);
    else if (strcmp(cmd, "go") == 0 ||
             strcmp(cmd, "cd") == 0)         cmd_go(args);
    else if (strcmp(cmd, "path") == 0 ||
             strcmp(cmd, "pwd") == 0)        cmd_path();
    else if (strcmp(cmd, "show") == 0 ||
             strcmp(cmd, "cat") == 0)        cmd_show(args);
    else if (strcmp(cmd, "top") == 0 ||
             strcmp(cmd, "head") == 0)       cmd_top(args);
    else if (strcmp(cmd, "new") == 0 ||
             strcmp(cmd, "touch") == 0)      cmd_new(args);
    else if (strcmp(cmd, "copy") == 0 ||
             strcmp(cmd, "cp") == 0)         cmd_copy(args);
    else if (strcmp(cmd, "move") == 0 ||
             strcmp(cmd, "mv") == 0)         cmd_move(args);
    else if (strcmp(cmd, "delete") == 0 ||
             strcmp(cmd, "rm") == 0)         cmd_delete(args);
    else if (strcmp(cmd, "newdir") == 0 ||
             strcmp(cmd, "mkdir") == 0)      cmd_md(args);
    else if (strcmp(cmd, "deletedir") == 0 ||
             strcmp(cmd, "rmdir") == 0)      cmd_deletedir(args);
    else if (strcmp(cmd, "pid") == 0)        cmd_pid();
    else if (strcmp(cmd, "proc") == 0)       cmd_proc();
    else if (strcmp(cmd, "end") == 0 ||
             strcmp(cmd, "kill") == 0)       cmd_end(args);
    else if (strcmp(cmd, "disk") == 0 ||
             strcmp(cmd, "df") == 0)         cmd_disk();
    else if (strcmp(cmd, "halt") == 0)       { term_puts("System halting.\n"); exit(0); }
    else if (strcmp(cmd, "exec") == 0) {
        if (args) run_program(args);
        else term_puts("Usage: exec <program>\n");
    }
    else run_program(cmd);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    /* Clear grid */
    term_clear();

    /* Query screen dimensions */
    uint32_t sw = 1024, sh = 768;
    gfx_screen_info(&sw, &sh);

    /* Centre window */
    int wx = ((int)sw - WIN_W) / 2;
    int wy = ((int)sh - WIN_H) / 2 - 10;
    if (wy < 48) wy = 48;   /* below taskbar + title bar */

    long win_id = win_create("Terminal", wx, wy, WIN_W, WIN_H);

    /* Welcome banner */
    term_puts_color("  ___   ___  ___  ___\n", COL_PROMPT);
    term_puts_color(" / _ | / __// _ \\/ __/\n", COL_PROMPT);
    term_puts_color("/ __ |\\__ \\/ , _/\\ \\\n", COL_PROMPT);
    term_puts_color("/_/ |_/___//_/|_/___/\n\n", COL_PROMPT);
    term_puts_color("ASOS Terminal v1.0\n", COL_INFO);
    term_puts("Type 'help' for available commands.\n\n");

    show_prompt();

    /* Initial render */
    term_redraw();
    if (win_id >= 0)
        win_update((int)win_id, s_pixels);
    gfx_flush_display();

    /* ── Event loop ─────────────────────────────────────────────────────── */

    for (;;) {
        long k = key_poll();

        if (k >= 0) {
            char c = (char)k;

            if (c == '\n' || c == '\r') {
                /* Echo newline */
                term_putc('\n');

                /* Execute the command */
                s_input[s_input_len] = '\0';
                char cmd_copy[INPUT_MAX];
                memcpy(cmd_copy, s_input, (size_t)s_input_len + 1);
                process_command(cmd_copy);
                show_prompt();
            } else if (c == '\b' || c == 127) {
                /* Backspace */
                if (s_input_len > 0) {
                    s_input_len--;
                    s_input[s_input_len] = '\0';
                    term_putc('\b');
                }
            } else if (c >= 32 && c < 127) {
                /* Printable character */
                if (s_input_len < INPUT_MAX - 1) {
                    s_input[s_input_len++] = c;
                    s_input[s_input_len] = '\0';
                    term_putc_color(c, COL_CMD);
                }
            }
            /* Ignore other control characters */
        }

        if (s_dirty) {
            term_redraw();
            if (win_id >= 0)
                win_update((int)win_id, s_pixels);
            gfx_flush_display();
            s_dirty = 0;
        } else {
            /* No input and no dirty state — just drive compositor and yield */
            gfx_flush_display();
            yield();
        }
    }

    return 0;
}
