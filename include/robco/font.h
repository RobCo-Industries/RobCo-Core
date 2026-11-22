#ifndef ROBCO_FONT_H
#define ROBCO_FONT_H

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

/*
 * Built-in font. Set ROBCO_FONT in robco_config.h to pick one; the default
 * is Fixedsys Excelsior. A project that links its own table from
 * tools/font2c.py replaces whichever one is selected.
 */
#define ROBCO_FONT_FIXEDSYS 1
#define ROBCO_FONT_SPLEEN   2

#ifndef ROBCO_FONT
#define ROBCO_FONT ROBCO_FONT_FIXEDSYS
#endif

#if ROBCO_FONT != ROBCO_FONT_FIXEDSYS && ROBCO_FONT != ROBCO_FONT_SPLEEN
#error "ROBCO_FONT must be ROBCO_FONT_FIXEDSYS or ROBCO_FONT_SPLEEN"
#endif

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
 * Takes printable ASCII, a CP437 byte from the list in the generated table or the
 * matching Unicode code point. Anything else gives a replacement box.
 */
const uint8_t *font8x16_glyph(uint32_t ch);

/* Short name of the font the table was made from, such as "fixedsys-excelsior". */
const char *font8x16_name(void);

#endif
