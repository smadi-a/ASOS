/*
 * kernel/font.h — 8×8 bitmap font for printable ASCII.
 *
 * Each glyph is stored as 8 bytes, one byte per pixel row (top to bottom).
 * Within each byte the most-significant bit is the leftmost pixel:
 *
 *   bit 7 = pixel column 0 (leftmost)
 *   bit 0 = pixel column 7 (rightmost)
 *
 * Only the printable ASCII range 0x20–0x7E is populated.
 * Characters outside that range fall back to a solid question-mark glyph.
 */

#ifndef FONT_H
#define FONT_H

#include <stdint.h>

#define FONT_WIDTH   8   /* pixels per glyph row    */
#define FONT_HEIGHT  8   /* pixel rows per glyph    */

/*
 * font_glyphs[c][row] — bitmap row `row` of character `c`.
 * Index with the raw (unsigned) ASCII code; always safe for c in 0..127.
 */
extern const uint8_t font_glyphs[128][FONT_HEIGHT];

#endif /* FONT_H */
