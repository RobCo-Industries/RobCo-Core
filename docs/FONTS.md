# Console fonts

The text console draws 8x16 glyphs from a C table in `src/gfx/font8x16.c`.
A project can swap that table for one it generates from its own font file.

## The built-in font

RobCo-Core ships Spleen 8x16 by Frederic Cambus, under the BSD 2-Clause
licence. The BDF source and the licence are in `third_party/spleen/`.
The table is generated with:

    tools/font2c.py third_party/spleen/spleen-8x16.bdf --name spleen -o src/gfx/font8x16.c

Every table holds printable ASCII (U+0020 to U+007E), the block and shade
characters, the single and double box-drawing set, and a replacement box.
The block and box glyphs are also reachable through their CP437 bytes.
`font8x16_name()` returns the name given with `--name`.

## How the override works

The built-in table is its own object in `librobco.a`. It defines only
`font8x16_glyph()` and `font8x16_name()`. A project that lists its own
generated table in `SRCS` defines the same two symbols, so the linker
resolves them from the project object and never pulls the built-in one
from the archive. No library option is needed, and the image holds one
table.

To check, run `arm-none-eabi-nm` on the ELF. `font_ascii`,
`font8x16_glyph` and `font8x16_name` each appear once.

Put the generated file anywhere under your project and add it to `SRCS`:

    SRCS := main.c fonts/font8x16_myfont.c

## Local fonts

The top-level `local/` directory is ignored by git. Put downloaded fonts
and the tables made from them there. Nothing in `local/` is committed.

A generated table inherits the licence of its source font. Its header
names the source file, its SHA-256 and the command that made it.

## Fixedsys Excelsior

To get the Fixedsys Excelsior look on your own machine:

1. Download `FSEX302.ttf` yourself from the release page of
   <https://github.com/kika/fixedsys> and save it as `local/FSEX302.ttf`.
2. Generate the table (needs Pillow, which `make venv` installs):

       .venv/bin/python tools/font2c.py local/FSEX302.ttf --name fixedsys-excelsior -o local/font8x16_fixedsys.c

3. Build your project with `local/font8x16_fixedsys.c` in `SRCS`, or build
   the console example with it:

       make -C examples/console FONT_C=$PWD/local/font8x16_fixedsys.c BUILD=build/fixedsys

The console example prints `@RC stage=console ev=font name=fixedsys-excelsior`
at boot when the override is in place.

### Why the repository does not include it

Its licence status is unclear. The public-domain claim comes from a
third-party repository, not from the author. The font file has no licence
fields. Its basic Latin glyphs are Microsoft's original Fixedsys bitmaps.

So RobCo-Core does not ship the font, any table made from it, or any image
built with it. If you redistribute an image built with it, you take on
that question yourself.

## Other fonts

`tools/font2c.py` reads two kinds of input:

- BDF fonts with 8x16 cells. Standard library only.
- TTF or OTF fonts drawn on a pixel grid. Needs Pillow. Pass `--size` if
  the font is not 16 px tall (default 16).

The tool draws each character on its own, so programming ligatures never
apply. It fails and names the glyph if a glyph is not pure black and
white, does not fit an 8x16 cell, or does not advance by 8 px. A glyph the
font lacks is drawn as the replacement box and listed in the summary the
tool prints.

## Specimens

`tools/font_specimen.py` boots the console example in QEMU, switches the
colour scheme, draws a specimen page and saves a screenshot plus a 2x copy:

    .venv/bin/python tools/font_specimen.py --scheme amber -o local/spleen-amber.png
    .venv/bin/python tools/font_specimen.py --font-c local/font8x16_fixedsys.c --scheme amber -o local/fixedsys-amber.png

Schemes are `green`, `amber`, `blue` and `white`. `--board` picks the
QEMU machine (default `raspi2b`). The page is also on command `p` of the
console example.
