# FatFs

ChaN's FatFs, a FAT file system module. The files here are copied from the
release archive without changes, so an upgrade is a file replacement.

| Field | Value |
|---|---|
| Version | R0.16 (`FF_DEFINED` 80386) |
| Download | http://elm-chan.org/fsw/ff/arc/ff16.zip |
| Downloaded | 2026-11-22 |
| Archive SHA-256 | `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e` |
| Licence | FatFs licence, a 1-clause BSD-style licence, in `LICENSE.txt` |

## Files

| File | From the archive |
|---|---|
| `ff.c`, `ff.h` | `source/` |
| `diskio.h` | `source/` |
| `ffunicode.c` | `source/` |
| `LICENSE.txt` | top level |

Not copied: `ffconf.h`, `diskio.c` and `ffsystem.c`. Our configuration is
`src/fs/ffconf.h`, and the media access functions are in
`src/fs/fs_glue.c`. `ffsystem.c` is only needed for heap-based long file
names or a reentrant build, and we use neither. `ff.h` includes
`"ffconf.h"`; because this directory has none, the compiler finds ours
through `-I src/fs`.

## Upgrade

1. Download the new archive and check its SHA-256.
2. Replace the five files above.
3. Set `FFCONF_DEF` in `src/fs/ffconf.h` to the new `FF_DEFINED` value
   and compare the options with the new upstream `ffconf.h`.
4. Run `make test-host` and `.venv/bin/pytest tests/tools/test_fs_interop.py`.
5. Update this table.
