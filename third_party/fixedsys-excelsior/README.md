# Fixedsys Excelsior 3.02

Pixel font by Darien Valentine. The default console font of RobCo-Core.

- Upstream: https://github.com/kika/fixedsys
- Release: `v3.09.10`
- Commit: `3f37fa8855bdbc542c817947ede82bef7c4049eb`
- File: `FSEX302.ttf`, unchanged
- SHA-256: `842f8fbf80f57d867aeb1d2988140d3ea8b4718e5f687035b0a3b66756df3899`
- Vendored: 2026-11-22

## Terms

The font file has no licence fields, and the upstream repository has no
licence file. There is no formal licence text beyond these two statements.
`PROVENANCE` holds the same text.

The upstream [README](https://github.com/kika/fixedsys) says:

> The creator(s) of this content have released it to the public domain. For jurisdictions where this is not permitted, it is released under the terms of the Creative Commons Zero Dedication.

The original author, Darien Valentine (GitHub user `bathos`), wrote on
2019-07-13 in
[kika/fixedsys#21](https://github.com/kika/fixedsys/issues/21#issuecomment-511143610):

> I made the original Fixedsys Excelsior when I was a teenager. Yes, the site is gone now and probably won't be back. The font is intended to be free to use and alter as you see fit. Many people have used it in other software and media over the years (kinda cool!). Possibly notable: the letterforms for the ASCII-range characters are based on the original Fixedsys bitmap font, which is much older and was not created by me. It is not derived from the older font in the software sense though.

The font is kept as a separate asset under `third_party/`, and one setting
in `robco_config.h` swaps it for Spleen (see `docs/FONTS.md`).

## Generated table

`src/gfx/font8x16_fixedsys.c` is generated from the TTF (needs Pillow):

    .venv/bin/python tools/font2c.py third_party/fixedsys-excelsior/FSEX302.ttf --name fixedsys-excelsior --select ROBCO_FONT_FIXEDSYS -o src/gfx/font8x16_fixedsys.c
