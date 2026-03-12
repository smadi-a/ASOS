/*
 * gfx_colors.h — Shared colour palette for ASOS user-space graphics.
 *
 * All constants are in 0x00RRGGBB format, matching the contract expected
 * by color_native() in kernel/gfx.c.  Alpha byte is ignored.
 */

#ifndef _GFX_COLORS_H
#define _GFX_COLORS_H

#define COL_BLACK      0x00000000U
#define COL_WHITE      0x00FFFFFFU
#define COL_RED        0x00CC2222U
#define COL_GREEN      0x0022CC22U
#define COL_BLUE       0x002244CCU
#define COL_DKBLUE     0x00001040U
#define COL_YELLOW     0x00CCCC00U
#define COL_CYAN       0x0000CCCCU
#define COL_MAGENTA    0x00CC22CCU
#define COL_GRAY       0x00444444U
#define COL_DKGRAY     0x00222222U
#define COL_LGRAY      0x00888888U

#endif /* _GFX_COLORS_H */
