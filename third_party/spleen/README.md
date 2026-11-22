# Spleen 8x16

Bitmap font by Frederic Cambus, BSD 2-Clause (see `LICENSE`).

- Upstream: https://github.com/fcambus/spleen
- Tag: `2.3.0`
- Commit: `b2d04a6180d44f47396dc530f3b2ca46b87e618b`
- Files: `spleen-8x16.bdf`, `LICENSE`, unchanged.

`src/gfx/font8x16.c` is generated from the BDF:

    tools/bdf2c.py third_party/spleen/spleen-8x16.bdf > src/gfx/font8x16.c
