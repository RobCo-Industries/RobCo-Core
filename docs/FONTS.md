# Console fonts

The text console draws 8x16 glyphs from a C table made by
`tools/font2c.py`. RobCo-Core ships two tables:

| Font | Table | Source | Name |
| --- | --- | --- | --- |
| Fixedsys Excelsior 3.02 (default) | `src/gfx/font8x16_fixedsys.c` | `third_party/fixedsys-excelsior/FSEX302.ttf` | `fixedsys-excelsior` |
| Spleen 8x16 | `src/gfx/font8x16_spleen.c` | `third_party/spleen/spleen-8x16.bdf` | `spleen` |

Every table holds printable ASCII (U+0020 to U+007E), the block and shade
characters, the single and double box-drawing set, and a replacement box.
The block and box glyphs are also reachable through their CP437 bytes.
`font8x16_name()` returns the name in the table above.

## Where the fonts come from

### Fixedsys Excelsior

Pixel font by Darien Valentine, from
<https://github.com/kika/fixedsys>, release `v3.09.10`. The file, its
SHA-256 and the commit are listed in
[`third_party/fixedsys-excelsior/README.md`](../third_party/fixedsys-excelsior/README.md).

The font file has no licence fields, and the upstream repository has no
licence file. There is no formal licence text beyond these two
statements, which are also kept in
`third_party/fixedsys-excelsior/PROVENANCE`.

The upstream [README](https://github.com/kika/fixedsys) says:

> The creator(s) of this content have released it to the public domain. For jurisdictions where this is not permitted, it is released under the terms of the Creative Commons Zero Dedication.

The original author wrote on 2019-07-13 in
[kika/fixedsys#21](https://github.com/kika/fixedsys/issues/21#issuecomment-511143610):

> I made the original Fixedsys Excelsior when I was a teenager. Yes, the site is gone now and probably won't be back. The font is intended to be free to use and alter as you see fit. Many people have used it in other software and media over the years (kinda cool!). Possibly notable: the letterforms for the ASCII-range characters are based on the original Fixedsys bitmap font, which is much older and was not created by me. It is not derived from the older font in the software sense though.

### Spleen

Bitmap font by Frederic Cambus, under the BSD 2-Clause licence. The BDF
source, the licence and the upstream tag are in `third_party/spleen/`.

## Switching to Spleen

Set `ROBCO_FONT` in your project's `robco_config.h`:

    #define ROBCO_FONT ROBCO_FONT_SPLEEN

`ROBCO_FONT_FIXEDSYS` is the default when `ROBCO_FONT` is not defined.
`include/robco/font.h` defines both values and rejects any other.

Both tables are compiled into `librobco.a`, but each body sits in
`#if ROBCO_FONT == ...`, so only the selected one has any code or data.
The console example builds the Spleen version as the `spleen` variant
listed in `examples/console/VARIANTS`:

    make -C examples/console BUILD=build/spleen EXTRA_CFLAGS=-DROBCO_FONT=ROBCO_FONT_SPLEEN

The example prints `@RC stage=console ev=font name=<name>` at boot.

## Regenerating the tables

TTF input needs Pillow, which `make venv` installs.

    .venv/bin/python tools/font2c.py third_party/fixedsys-excelsior/FSEX302.ttf --name fixedsys-excelsior --select ROBCO_FONT_FIXEDSYS -o src/gfx/font8x16_fixedsys.c
    .venv/bin/python tools/font2c.py third_party/spleen/spleen-8x16.bdf --name spleen --select ROBCO_FONT_SPLEEN -o src/gfx/font8x16_spleen.c

`--select` adds the `#if` guard. `make test-tools` checks that both
commands reproduce the committed files byte for byte. Each table's header
names its source file, its SHA-256 and the command that made it. A table
inherits the licence of its source font.

## Using another font

A project can replace the built-in table with one it generates from its
own font. Leave out `--select`:

    .venv/bin/python tools/font2c.py fonts/myfont.ttf --name myfont -o fonts/font8x16_myfont.c

Then add the file to `SRCS`:

    SRCS := main.c fonts/font8x16_myfont.c

This works with either `ROBCO_FONT` value. The project table defines
`font8x16_glyph()` and `font8x16_name()`, so the linker resolves them
from the project object and never pulls a built-in table from the
archive. The image holds one table. To check, run `arm-none-eabi-nm` on
the ELF: `font_ascii`, `font8x16_glyph` and `font8x16_name` each appear
once.

The console example takes such a table with `FONT_C=`:

    make -C examples/console FONT_C=$PWD/fonts/font8x16_myfont.c BUILD=build/myfont

The top-level `local/` directory is ignored by git. It is a good place
for downloaded fonts and the tables made from them.

`tools/font2c.py` reads two kinds of input:

- BDF fonts with 8x16 cells. Standard library only.
- TTF or OTF fonts drawn on a pixel grid. Needs Pillow. Pass `--size` if
  the font is not 16 px tall (default 16).

The tool draws each character on its own, so programming ligatures never
apply. It fails and names the glyph if a glyph is not pure black and
white, does not fit an 8x16 cell, or does not advance by 8 px. A glyph the
font lacks is drawn as the replacement box and listed in the summary the
tool prints.

## Known quirk: gaps in Fixedsys vertical lines

In Fixedsys Excelsior the full block, the shades and every box-drawing
glyph with a vertical stroke stop one pixel short of the top of the
16-pixel cell. The font is 16 px tall (13 px ascent, 3 px descent), but
these outlines span only 15 px, from 12 px above the baseline to 3 px
below. So stacked vertical lines show a one-pixel gap at each row
boundary. The gap is easiest to see on double-line frames. The table
matches the font; the converter does not drop a row. Spleen fills the
whole cell.

## Specimens

`tools/font_specimen.py` boots the console example in QEMU, switches the
colour scheme, draws a specimen page and saves a screenshot plus a 2x copy:

    .venv/bin/python tools/font_specimen.py --scheme amber -o local/fixedsys-amber.png
    .venv/bin/python tools/font_specimen.py --font spleen --scheme amber -o local/spleen-amber.png
    .venv/bin/python tools/font_specimen.py --font-c local/font8x16_myfont.c -o local/myfont-green.png

Schemes are `green`, `amber`, `blue` and `white`. `--board` picks the
QEMU machine (default `raspi2b`). The page is also on command `p` of the
console example.
