#!/usr/bin/env python3
"""Convert an 8x16 BDF font into the C glyph table used by src/gfx/font8x16.c.

Usage: tools/bdf2c.py third_party/spleen/spleen-8x16.bdf > src/gfx/font8x16.c

Emits code points 0x20 to 0x7E, then the block and box-drawing glyphs
listed in EXTRAS that the font has. Each extra also gets its CP437 byte
so a console that stores 8-bit cells can reach it. Stdlib only.
"""

import os
import sys

W, H = 8, 16

# Unicode code point -> CP437 byte.
EXTRAS = [
    (0x2588, 0xDB),  # full block
    (0x2591, 0xB0),  # light shade
    (0x2592, 0xB1),  # medium shade
    (0x2593, 0xB2),  # dark shade
    (0x2580, 0xDF),  # upper half block
    (0x2584, 0xDC),  # lower half block
    (0x2500, 0xC4),  # light horizontal
    (0x2502, 0xB3),  # light vertical
    (0x250C, 0xDA),  # light down and right
    (0x2510, 0xBF),  # light down and left
    (0x2514, 0xC0),  # light up and right
    (0x2518, 0xD9),  # light up and left
    (0x251C, 0xC3),  # light vertical and right
    (0x2524, 0xB4),  # light vertical and left
    (0x252C, 0xC2),  # light down and horizontal
    (0x2534, 0xC1),  # light up and horizontal
    (0x253C, 0xC5),  # light vertical and horizontal
    (0x2550, 0xCD),  # double horizontal
    (0x2551, 0xBA),  # double vertical
    (0x2554, 0xC9),  # double down and right
    (0x2557, 0xBB),  # double down and left
    (0x255A, 0xC8),  # double up and right
    (0x255D, 0xBC),  # double up and left
]


def parse_bdf(path):
    """Return {code point: 16 row bytes} for every 8x16 glyph in the font."""
    glyphs = {}
    ascent = None
    enc = None
    bbx = None
    rows = None
    with open(path, encoding="ascii") as f:
        for raw in f:
            parts = raw.split()
            if not parts:
                continue
            key = parts[0]
            if key == "FONT_ASCENT":
                ascent = int(parts[1])
            elif key == "STARTCHAR":
                enc, bbx, rows = None, None, None
            elif key == "ENCODING":
                enc = int(parts[1])
            elif key == "BBX":
                bbx = tuple(int(v) for v in parts[1:5])
            elif key == "BITMAP":
                rows = []
            elif key == "ENDCHAR":
                if enc is not None and enc >= 0 and bbx is not None:
                    glyphs[enc] = place(bbx, rows, ascent)
                rows = None
            elif rows is not None:
                rows.append(int(key, 16))
    return glyphs


def place(bbx, rows, ascent):
    """Put a glyph bitmap into an 8x16 cell using its bounding box."""
    w, h, xoff, yoff = bbx
    if ascent is None:
        ascent = H + yoff if yoff < 0 else H
    cell = [0] * H
    if w > W:
        raise ValueError(f"glyph wider than {W} pixels")
    top = ascent - (yoff + h)
    for i, bits in enumerate(rows[:h]):
        y = top + i
        if 0 <= y < H:
            # One byte per row, bit 7 is the left pixel; xoff moves it right.
            cell[y] = (bits >> xoff) & 0xFF if xoff >= 0 else (bits << -xoff) & 0xFF
    return cell


def emit(src, glyphs, out):
    name = os.path.relpath(src)
    ascii_rows = []
    for cp in range(0x20, 0x7F):
        if cp not in glyphs:
            sys.exit(f"{src}: missing U+{cp:04X}")
        ascii_rows.append(glyphs[cp])
    extras = [(u, c) for u, c in EXTRAS if u in glyphs]

    w = out.write
    w("/*\n")
    w(" * Generated file. Do not edit.\n")
    w(f" * Source: {name}, Spleen 8x16 by Frederic Cambus.\n")
    w(" * Licence: BSD 2-Clause, see third_party/spleen/LICENSE.\n")
    w(f" * Generator: tools/bdf2c.py {name} > src/gfx/font8x16.c\n")
    w(" */\n\n")
    w("#include <stddef.h>\n#include <stdint.h>\n\n#include <robco/font.h>\n\n")
    w("/* U+0020 to U+007E, 16 rows each, bit 7 is the left pixel. */\n")
    w(f"static const uint8_t font_ascii[{len(ascii_rows)}][FONT_H] = {{\n")
    for cp, rows in zip(range(0x20, 0x7F), ascii_rows):
        w("    {" + ", ".join(f"0x{b:02x}" for b in rows) + f"}}, /* U+{cp:04X} */\n")
    w("};\n\n")
    w("typedef struct {\n    uint16_t ucs;\n    uint8_t cp437;\n    uint8_t rows[FONT_H];\n} font_extra_t;\n\n")
    w("/* Block and box-drawing glyphs: Unicode code point, CP437 byte, rows. */\n")
    w(f"static const font_extra_t font_extra[{len(extras)}] = {{\n")
    for u, c in extras:
        w(f"    {{0x{u:04x}, 0x{c:02x}, {{" + ", ".join(f"0x{b:02x}" for b in glyphs[u]) + "}},\n")
    w("};\n\n")
    w("/* Shown for characters the table does not have: an outlined box. */\n")
    w("static const uint8_t font_replacement[FONT_H] = {\n")
    w("    0x00, 0x00, 0x7e, 0x42, 0x42, 0x42, 0x42, 0x42,\n")
    w("    0x42, 0x42, 0x42, 0x42, 0x7e, 0x00, 0x00, 0x00,\n")
    w("};\n\n")
    w("const uint8_t *font8x16_glyph(uint32_t ch)\n{\n    size_t i;\n\n")
    w("    if (ch >= 0x20 && ch <= 0x7e) {\n        return font_ascii[ch - 0x20];\n    }\n")
    w("    for (i = 0; i < sizeof(font_extra) / sizeof(font_extra[0]); i++) {\n")
    w("        if (font_extra[i].ucs == ch || (ch >= 0x80 && ch <= 0xff && font_extra[i].cp437 == ch)) {\n")
    w("            return font_extra[i].rows;\n        }\n    }\n")
    w("    return font_replacement;\n}\n")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: bdf2c.py FONT.bdf > font8x16.c")
    emit(sys.argv[1], parse_bdf(sys.argv[1]), sys.stdout)


if __name__ == "__main__":
    main()
