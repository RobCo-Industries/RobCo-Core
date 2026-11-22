#ifndef ROBCO_FONT_H
#define ROBCO_FONT_H

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

/* CP437 bytes for the extra glyphs. Unicode code points work too. */
#define FONT_CH_BLOCK   0xdb
#define FONT_CH_SHADE1  0xb0
#define FONT_CH_SHADE2  0xb1
#define FONT_CH_SHADE3  0xb2
#define FONT_CH_HLINE   0xc4
#define FONT_CH_VLINE   0xb3
#define FONT_CH_TL      0xda
#define FONT_CH_TR      0xbf
#define FONT_CH_BL      0xc0
#define FONT_CH_BR      0xd9

/*
 * Returns 16 bytes, one per row, bit 7 is the left pixel.
 * Takes printable ASCII, a CP437 byte from the list in font8x16.c or the
 * matching Unicode code point. Anything else gives a replacement box.
 */
const uint8_t *font8x16_glyph(uint32_t ch);

/* Short name of the font the table was made from, such as "spleen". */
const char *font8x16_name(void);

#endif
