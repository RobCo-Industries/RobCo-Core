#!/usr/bin/env python3
"""Convert an 8x16 font into the C glyph table used by the console.

Usage:
    tools/font2c.py FONT.bdf --name NAME -o font8x16.c
    tools/font2c.py FONT.ttf --name NAME [--size 16] -o font8x16.c

--select ROBCO_FONT_X wraps the table in #if ROBCO_FONT == ROBCO_FONT_X.
The built-in tables use it. A project table does not need it.

BDF input needs only the standard library. TTF and OTF input needs Pillow
and works only for pixel-grid fonts: every glyph must rasterise to pure
black and white, fit an 8x16 cell and advance by 8 pixels.

The table holds code points 0x20 to 0x7E, then the block and box-drawing
glyphs listed in EXTRAS. Each extra also gets its CP437 byte so a console
that stores 8-bit cells can reach it. A glyph the font lacks falls back to
the replacement box and is listed in the summary on stderr.
"""

import argparse
import hashlib
import os
import re
import struct
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

ASCII = range(0x20, 0x7F)
WANTED = list(ASCII) + [u for u, _ in EXTRAS]

# Outlined box shown for characters the table does not have.
REPLACEMENT = [0x00, 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x42,
               0x42, 0x42, 0x42, 0x42, 0x7E, 0x00, 0x00, 0x00]

SCALE = 4

NAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,32}$")
SELECT_RE = re.compile(r"^ROBCO_FONT_[A-Z0-9_]+$")


class FontError(Exception):
    pass


def glyph_label(cp):
    ch = chr(cp)
    return f"U+{cp:04X} {ch!r}" if ch.isprintable() else f"U+{cp:04X}"


# BDF


def parse_bdf(path):
    """Return {code point: 16 row bytes} for the code points in WANTED."""
    glyphs = {}
    ascent = None
    enc = bbx = rows = dwidth = None
    with open(path, encoding="ascii", errors="replace") as f:
        for raw in f:
            parts = raw.split()
            if not parts:
                continue
            key = parts[0]
            if key == "FONT_ASCENT":
                ascent = int(parts[1])
            elif key == "STARTCHAR":
                enc = bbx = rows = dwidth = None
            elif key == "ENCODING":
                enc = int(parts[1])
            elif key == "DWIDTH":
                dwidth = int(parts[1])
            elif key == "BBX":
                bbx = tuple(int(v) for v in parts[1:5])
            elif key == "BITMAP":
                rows = []
            elif key == "ENDCHAR":
                if enc in WANTED and bbx is not None:
                    if dwidth is not None and dwidth != W:
                        raise FontError(f"{glyph_label(enc)}: advance is {dwidth} px, want {W}")
                    glyphs[enc] = place(enc, bbx, rows or [], ascent)
                rows = None
            elif rows is not None:
                rows.append(int(key, 16))
    return glyphs


def place(cp, bbx, rows, ascent):
    """Put a glyph bitmap into an 8x16 cell using its bounding box."""
    w, h, xoff, yoff = bbx
    if ascent is None:
        ascent = H + yoff if yoff < 0 else H
    top = ascent - (yoff + h)
    if w > W or xoff < 0 or xoff + w > W or top < 0 or top + h > H:
        raise FontError(f"{glyph_label(cp)}: bounding box {w}x{h}+{xoff}+{yoff} "
                        f"does not fit an {W}x{H} cell")
    # Rows are padded to whole bytes; keep the leftmost W bits.
    nbytes = (w + 7) // 8
    cell = [0] * H
    for i, bits in enumerate(rows[:h]):
        bits >>= 8 * (nbytes - 1)
        cell[top + i] = (bits >> xoff) & 0xFF
    return cell


# TTF and OTF


def cmap_codepoints(path):
    """Code points mapped by the font's cmap table (formats 4 and 12)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == b"ttcf":
        data_off = struct.unpack(">I", data[12:16])[0]
    else:
        data_off = 0
    num = struct.unpack(">H", data[data_off + 4:data_off + 6])[0]
    cmap = None
    for i in range(num):
        rec = data_off + 12 + 16 * i
        tag, _, off, _ = struct.unpack(">4sIII", data[rec:rec + 16])
        if tag == b"cmap":
            cmap = off
    if cmap is None:
        raise FontError(f"{path}: no cmap table")
    found = set()
    n = struct.unpack(">H", data[cmap + 2:cmap + 4])[0]
    for i in range(n):
        plat, enc, off = struct.unpack(">HHI", data[cmap + 4 + 8 * i:cmap + 12 + 8 * i])
        if (plat, enc) not in ((3, 1), (3, 10), (0, 3), (0, 4), (0, 6)):
            continue
        sub = cmap + off
        fmt = struct.unpack(">H", data[sub:sub + 2])[0]
        if fmt == 4:
            segx2 = struct.unpack(">H", data[sub + 6:sub + 8])[0]
            seg = segx2 // 2
            ends = struct.unpack(f">{seg}H", data[sub + 14:sub + 14 + segx2])
            base = sub + 16 + segx2
            starts = struct.unpack(f">{seg}H", data[base:base + segx2])
            deltas = struct.unpack(f">{seg}h", data[base + segx2:base + 2 * segx2])
            ro_base = base + 2 * segx2
            ranges = struct.unpack(f">{seg}H", data[ro_base:ro_base + segx2])
            for k in range(seg):
                for cp in range(starts[k], ends[k] + 1):
                    if cp == 0xFFFF:
                        continue
                    if ranges[k] == 0:
                        gid = (cp + deltas[k]) & 0xFFFF
                    else:
                        at = ro_base + 2 * k + ranges[k] + 2 * (cp - starts[k])
                        gid = struct.unpack(">H", data[at:at + 2])[0]
                        if gid:
                            gid = (gid + deltas[k]) & 0xFFFF
                    if gid:
                        found.add(cp)
        elif fmt == 12:
            ngroups = struct.unpack(">I", data[sub + 12:sub + 16])[0]
            for g in range(ngroups):
                s, e, gid = struct.unpack(">III", data[sub + 16 + 12 * g:sub + 28 + 12 * g])
                for cp in range(s, e + 1):
                    if gid + (cp - s):
                        found.add(cp)
    return found


def parse_outline(path, size):
    """Rasterise each wanted glyph on its own and check it is pixel exact."""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        raise FontError("TTF and OTF input needs Pillow: pip install Pillow "
                        "(or use .venv/bin/python from 'make venv')") from None

    # Render at SCALE times the size so each font pixel is a SCALE x SCALE
    # block. Corners of touching pixels can come out grey, so only the
    # inside of each block counts. A glyph off the pixel grid gives a mixed
    # or grey block inside and fails instead of being thresholded.
    font = ImageFont.truetype(path, size * SCALE)
    mapped = cmap_codepoints(path)
    pad = 4
    cw, ch_ = (W + 2 * pad), (H + 2 * pad)
    glyphs = {}
    errors = []
    for cp in WANTED:
        if cp not in mapped:
            continue
        ch = chr(cp)
        adv = font.getlength(ch) / SCALE
        if adv != W:
            errors.append(f"{glyph_label(cp)}: advance is {adv:g} px, want {W}")
            continue
        img = Image.new("L", (cw * SCALE, ch_ * SCALE), 0)
        ImageDraw.Draw(img).text((pad * SCALE, pad * SCALE), ch, font=font, fill=255, anchor="la")
        px = img.load()
        grey = outside = False
        cell = [0] * H
        for by in range(ch_):
            for bx in range(cw):
                vals = {px[bx * SCALE + i, by * SCALE + j]
                        for i in range(1, SCALE - 1) for j in range(1, SCALE - 1)}
                if vals == {0}:
                    continue
                if vals != {255}:
                    grey = True
                    continue
                x, y = bx - pad, by - pad
                if 0 <= x < W and 0 <= y < H:
                    cell[y] |= 0x80 >> x
                else:
                    outside = True
        if grey:
            errors.append(f"{glyph_label(cp)}: not pure black and white at {size} px")
        elif outside:
            errors.append(f"{glyph_label(cp)}: does not fit an {W}x{H} cell at {size} px")
        else:
            glyphs[cp] = cell
    if errors:
        raise FontError("glyphs failed the pixel grid check:\n  " + "\n  ".join(errors))
    return glyphs


# Output


def emit(out, src, command, name, glyphs, select=None):
    digest = hashlib.sha256(open(src, "rb").read()).hexdigest()
    ascii_rows = [glyphs.get(cp, REPLACEMENT) for cp in ASCII]
    extras = [(u, c) for u, c in EXTRAS if u in glyphs]

    w = out.write
    w("/*\n")
    w(" * Generated file. Do not edit.\n")
    w(f" * Source: {os.path.basename(src)}\n")
    w(f" * SHA-256: {digest}\n")
    w(f" * Generator: {command}\n")
    w(" * This table inherits the licence of its source font.\n")
    w(" */\n\n")
    w("#include <stddef.h>\n#include <stdint.h>\n\n#include <robco/font.h>\n\n")
    if select:
        w(f"/* Built only when robco_config.h selects this font. */\n#if ROBCO_FONT == {select}\n\n")
    w("/* U+0020 to U+007E, 16 rows each, bit 7 is the left pixel. */\n")
    w(f"static const uint8_t font_ascii[{len(ascii_rows)}][FONT_H] = {{\n")
    for cp, rows in zip(ASCII, ascii_rows):
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
    w("    " + ", ".join(f"0x{b:02x}" for b in REPLACEMENT[:8]) + ",\n")
    w("    " + ", ".join(f"0x{b:02x}" for b in REPLACEMENT[8:]) + ",\n")
    w("};\n\n")
    w("const uint8_t *font8x16_glyph(uint32_t ch)\n{\n    size_t i;\n\n")
    w("    if (ch >= 0x20 && ch <= 0x7e) {\n        return font_ascii[ch - 0x20];\n    }\n")
    w("    for (i = 0; i < sizeof(font_extra) / sizeof(font_extra[0]); i++) {\n")
    w("        if (font_extra[i].ucs == ch || (ch >= 0x80 && ch <= 0xff && font_extra[i].cp437 == ch)) {\n")
    w("            return font_extra[i].rows;\n        }\n    }\n")
    w("    return font_replacement;\n}\n\n")
    w(f"const char *font8x16_name(void)\n{{\n    return \"{name}\";\n}}\n")
    if select:
        w(f"\n#endif /* ROBCO_FONT == {select} */\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Convert an 8x16 BDF or pixel TTF/OTF font into a C glyph table.")
    ap.add_argument("font", help="BDF, TTF or OTF file")
    ap.add_argument("--name", required=True, help="name returned by font8x16_name() ([A-Za-z0-9._-], up to 32)")
    ap.add_argument("--size", type=int, default=16, help="pixel size for TTF and OTF (default 16)")
    ap.add_argument("--select", metavar="MACRO",
                    help="wrap the table in #if ROBCO_FONT == MACRO (for the tables in src/gfx)")
    ap.add_argument("-o", dest="out", required=True, help="output C file")
    args = ap.parse_args(argv)

    if not NAME_RE.match(args.name):
        ap.error("--name must be 1 to 32 characters from [A-Za-z0-9._-]")
    if args.select and not SELECT_RE.match(args.select):
        ap.error("--select must be a ROBCO_FONT_* macro name")

    ext = os.path.splitext(args.font)[1].lower()
    command = f"tools/font2c.py {args.font} --name {args.name}"
    try:
        if ext == ".bdf":
            glyphs = parse_bdf(args.font)
        elif ext in (".ttf", ".otf", ".ttc"):
            glyphs = parse_outline(args.font, args.size)
            command += f" --size {args.size}"
        else:
            raise FontError(f"{args.font}: unknown font type {ext!r}, want .bdf, .ttf or .otf")
    except (FontError, OSError, ValueError) as e:
        sys.exit(f"font2c: {e}")
    if args.select:
        command += f" --select {args.select}"
    command += f" -o {args.out}"

    with open(args.out, "w", newline="\n") as out:
        emit(out, args.font, command, args.name, glyphs, args.select)

    missing = [cp for cp in WANTED if cp not in glyphs]
    print(f"font2c: {args.out}: {len(WANTED) - len(missing)} of {len(WANTED)} glyphs", file=sys.stderr)
    if missing:
        print(f"font2c: {len(missing)} missing, shown as the replacement box: "
              + ", ".join(glyph_label(cp) for cp in missing), file=sys.stderr)


if __name__ == "__main__":
    main()
