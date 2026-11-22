# Image format: rimg

Every loadable file in the suite uses one format, `rimg`. Stages, ROMs and holotapes all use it. A `rimg` file is a flat binary. The loader copies it to `load_addr` and jumps to byte 0. Stage images stay directly bootable by the firmware and by QEMU, because byte 0 is a branch.

## File layout

| File offset | Size | Content |
|---|---|---|
| `0x00` | `0x20` | Branch stub |
| `0x20` | `0x40` | `rimg_header_t` |
| `0x60` | `0xC0` | META block. Present only if `flags` bit 0 is set. |
| `0x60` or `0x120` | rest | Code and data |

Code follows whatever is present. Stage images have no META block, so their code starts at `0x60`. ROMs and holotapes have one, so their code starts at `0x120`.

`start.S` emits the stub and header for stages. It refuses to build with `flags` bit 0 set. `crt0.S` from the Pip-OS SDK emits the stub, the header with `flags` bit 0 set, and a zeroed META block for ROMs and holotapes.

The branch stub is a branch to the image's entry point, padded with zeros to `0x20`. The encoding is not fixed. Only word 0 runs.

All multi-byte fields are little-endian. Structures are packed with no padding. Every field has a fixed-width type.

## Header

`include/robco/rimg.h` holds these definitions.

```c
#include <stdint.h>

#define RIMG_HDR_OFFSET   0x20
#define RIMG_META_OFFSET  0x60
#define RIMG_HDR_VERSION  1
#define RIMG_HDR_SIZE     64
#define RIMG_META_SIZE    192
#define RIMG_FLAG_META    (1 << 0)

typedef struct __attribute__((packed)) rimg_header {
    char     magic[8];
    uint16_t hdr_version;
    uint16_t hdr_size;
    uint32_t flags;
    uint32_t load_addr;
    uint32_t file_size;
    uint32_t mem_size;
    uint32_t crc32;
    uint32_t abi_version;
    uint32_t image_version;
    uint32_t subtype;
    char     name[16];
    uint32_t reserved;
} rimg_header_t;

_Static_assert(sizeof(rimg_header_t) == 64, "rimg_header_t must be 64 bytes");
```

| Offset in header | File offset | Size | Field | Meaning |
|---|---|---|---|---|
| `0x00` | `0x20` | 8 | `magic` | Image kind. See magics. |
| `0x08` | `0x28` | 2 | `hdr_version` | `1` |
| `0x0A` | `0x2A` | 2 | `hdr_size` | `64` |
| `0x0C` | `0x2C` | 4 | `flags` | Bit 0: META block valid. Bits 1 to 31 must be 0. |
| `0x10` | `0x30` | 4 | `load_addr` | Address of byte 0 in memory. Also the entry point. Must equal the slot base. |
| `0x14` | `0x34` | 4 | `file_size` | Size of the file in bytes. |
| `0x18` | `0x38` | 4 | `mem_size` | Bytes used in memory from `load_addr`: file, BSS and stacks. At least `file_size`. |
| `0x1C` | `0x3C` | 4 | `crc32` | CRC of the whole file. See CRC. |
| `0x20` | `0x40` | 4 | `abi_version` | ABI the image was built for: `(major << 16) \| minor`. |
| `0x24` | `0x44` | 4 | `image_version` | Image's own version: `(major << 16) \| (minor << 8) \| patch`. Shown to users only. |
| `0x28` | `0x48` | 4 | `subtype` | Depends on the magic. See subtypes. |
| `0x2C` | `0x4C` | 16 | `name` | Short ASCII name. NUL-padded. May fill all 16 bytes with no NUL. |
| `0x3C` | `0x5C` | 4 | `reserved` | Must be 0. |
| | | 64 | | Total |

## META block

```c
typedef struct __attribute__((packed)) rimg_meta {
    char    title[64];
    uint8_t icon[128];
} rimg_meta_t;

_Static_assert(sizeof(rimg_meta_t) == 192, "rimg_meta_t must be 192 bytes");
```

| Offset in META | File offset | Size | Field | Meaning |
|---|---|---|---|---|
| `0x00` | `0x060` | 64 | `title` | Display title, ASCII, NUL-padded |
| `0x40` | `0x0A0` | 128 | `icon` | 32x32 pixels, 1 bit per pixel. 4 bytes per row, rows top to bottom. Bit 7 of each byte is the leftmost pixel. 1 is lit. |
| | | 192 | | Total |

## Magics

`rimg.h` defines them as `RIMG_MAGIC_BOOT`, `RIMG_MAGIC_ROM1` and `RIMG_MAGIC_ROBCO78`. `start.S` uses `RIMG_MAGIC_BOOT` unless the project's `robco_config.h` sets `ROBCO_IMG_MAGIC`.

| Magic bytes | Kind | Loaded by | ABI major |
|---|---|---|---|
| `"BOOT\0\0\0\0"` | Boot stage or OS | Firmware, RETROS-BIOS, MFBootAgent | 2, the `robco_bootinfo_t` version |
| `"ROM1\0\0\0\0"` | Pip-Boy ROM | Pip-OS | 1, the Pip-OS ABI |
| `"ROBCO78\0"` | Holotape | Pip-OS, UOS | 1, the Pip-OS ABI |

## Subtypes

| Magic | Value | `rimg.h` name | Meaning | Slot | File |
|---|---|---|---|---|---|
| BOOT | 0 | `RIMG_BOOT_UOS` | Operating system, UOS | `0x00400000`-`0x00FFFFFF` | `/ROBCO/UOS.SYS` |
| BOOT | 1 | | Reserved | | |
| BOOT | 2 | `RIMG_BOOT_MAINT` | Maintenance image | `0x00400000`-`0x00FFFFFF` | `/ROBCO/MAINT.SYS` |
| BOOT | 3 | `RIMG_BOOT_DIAG` | Diagnostics image | `0x00400000`-`0x00FFFFFF` | `/ROBCO/DIAG.SYS` |
| BOOT | 4 | `RIMG_BOOT_AGENT` | Boot agent, MFBootAgent | `0x00200000`-`0x003FFFFF` | `/ROBCO/MFBOOT.SYS`, `/ROBCO/MFBOOT.BAK` |
| BOOT | 5 | `RIMG_BOOT_BIOS` | BIOS, RETROS-BIOS | `0x00008000`-`0x000FFFFF` | `/RETROS.IMG` |
| BOOT | 6 | `RIMG_BOOT_PIPOS` | Pip-OS kernel | `0x00008000`-`0x003FFFFF` | `/PIPOS.IMG` |
| BOOT | 7 | `RIMG_BOOT_EXAMPLE` | Example or test image. The default in `start.S`. | `0x00008000`-`0x000FFFFF` | none, booted directly in tests |
| ROM1 | 0 | `RIMG_ROM1_SYSTEM` | System ROM, Deitrix | `0x00400000`-`0x007FFFFF` | `/PIPBOY/ROM1.ROM` |
| ROBCO78 | 0 | `RIMG_HOLO_GAME` | Game | `0x00800000`-`0x00BFFFFF` | `/PIPBOY/HOLO/*.HTP` |
| ROBCO78 | 1 | `RIMG_HOLO_UTILITY` | Utility | `0x00800000`-`0x00BFFFFF` | `/PIPBOY/HOLO/*.HTP` |
| ROBCO78 | 2 | `RIMG_HOLO_DATA` | Data. Not executable. | none | `/PIPBOY/HOLO/*.HTP`, `/UOS/HOLO/*.HTP` |

Values not listed are reserved. A loader rejects them at the bounds step.

The firmware loads subtypes 5 and 6, and tests load subtype 7 the same way. It checks nothing. These images do not check themselves either. `tools/rimg.py verify` checks them at build time.

## CRC

The `crc32` field holds CRC-32/ISO-HDLC, the same CRC as zlib `crc32()`.

| Parameter | Value |
|---|---|
| Width | 32 |
| Polynomial | `0x04C11DB7`, reflected form `0xEDB88320` |
| Init | `0xFFFFFFFF` |
| Reflect in and out | Yes |
| Final XOR | `0xFFFFFFFF` |
| Check value of `"123456789"` | `0xCBF43926` |

The CRC covers file bytes `[0, file_size)` with the four `crc32` bytes at file offset `0x3C` read as zero.

## Validation

A loader validates in this order. It stops at the first failure and reports that step's code. The loader reads the first `0x60` bytes for steps 1 to 5. It then reads the whole file into the slot and runs step 6.

| Step | Check | Error |
|---|---|---|
| 0 | The file exists and opens. | `RIMG_E_NOFILE` |
| 0 | Every read succeeds. Applies to all steps. | `RIMG_E_IO` |
| 1 | `magic` matches a magic the loader accepts for this slot. | `RIMG_E_MAGIC` |
| 2 | `hdr_version == 1`, `hdr_size == 64`, unknown `flags` bits are 0, `reserved == 0`. | `RIMG_E_VERSION` |
| 3 | `file_size` equals the size in the FAT directory entry. It is at least `0x60`, or `0x120` if `flags` bit 0 is set. | `RIMG_E_SIZE` |
| 4 | `subtype` is valid for the magic and has a slot, so data holotapes fail here. `load_addr` equals the slot base. `mem_size >= file_size`. `load_addr + mem_size` does not pass the slot end. | `RIMG_E_BOUNDS` |
| 5 | `abi_version >> 16` equals the loader's ABI major for this magic. The minor is not checked. | `RIMG_E_ABI` |
| 6 | The CRC matches. | `RIMG_E_CRC` |

```c
typedef enum rimg_err {
    RIMG_OK        = 0,
    RIMG_E_NOFILE  = 1,
    RIMG_E_IO      = 2,
    RIMG_E_MAGIC   = 3,
    RIMG_E_VERSION = 4,
    RIMG_E_SIZE    = 5,
    RIMG_E_BOUNDS  = 6,
    RIMG_E_ABI     = 7,
    RIMG_E_CRC     = 8,
} rimg_err_t;
```

| Code | Value | `@RC` name |
|---|---|---|
| `RIMG_OK` | 0 | `ok` |
| `RIMG_E_NOFILE` | 1 | `nofile` |
| `RIMG_E_IO` | 2 | `io` |
| `RIMG_E_MAGIC` | 3 | `magic` |
| `RIMG_E_VERSION` | 4 | `version` |
| `RIMG_E_SIZE` | 5 | `size` |
| `RIMG_E_BOUNDS` | 6 | `bounds` |
| `RIMG_E_ABI` | 7 | `abi` |
| `RIMG_E_CRC` | 8 | `crc` |

A loader that fails at step 6 has already overwritten the slot. That is fine, because the slot belongs to the image being loaded.

The loader does not zero BSS. The image's own startup code does that.

## Files on the SD card

The boot partition is FAT32. Names are 8.3 and upper case.

| Chain | Path | Kind |
|---|---|---|
| Terminal | `/RETROS.IMG` | BOOT 5, loaded by firmware |
| Terminal | `/ROBCO/MFBOOT.SYS` | BOOT 4 |
| Terminal | `/ROBCO/MFBOOT.BAK` | BOOT 4, fallback copy |
| Terminal | `/ROBCO/BOOT.CFG` | Boot menu config, INI text |
| Terminal | `/ROBCO/UOS.SYS` | BOOT 0 |
| Terminal | `/ROBCO/MAINT.SYS` | BOOT 2 |
| Terminal | `/ROBCO/DIAG.SYS` | BOOT 3 |
| Terminal | `/UOS/...` | UOS data files |
| Terminal | `/UOS/HOLO/*.HTP` | ROBCO78 2, read by UOS |
| Pip-Boy | `/PIPOS.IMG` | BOOT 6, loaded by firmware |
| Pip-Boy | `/PIPBOY/PIPOS.CFG` | Pip-OS config, INI text |
| Pip-Boy | `/PIPBOY/ROM1.ROM` | ROM1 0 |
| Pip-Boy | `/PIPBOY/HOLO/*.HTP` | ROBCO78 0, 1 or 2 |
| Pip-Boy | `/PIPBOY/SAVE/*.SAV` | Save files, not `rimg` |
| Both | `/bootcode.bin`, `/start.elf`, `/fixup.dat` | Firmware, fetched by `tools/fetch_firmware.sh`, never committed |
| Both | `/config.txt` | See [BOARD_SUPPORT.md](./BOARD_SUPPORT.md) |

| Extension | Meaning |
|---|---|
| `.IMG` | Stage loaded by the firmware |
| `.SYS` | Stage or OS loaded by another stage |
| `.BAK` | Fallback copy of a `.SYS` file |
| `.ROM` | Pip-Boy ROM |
| `.HTP` | Holotape |

## tools/rimg.py

`tools/rimg.py` builds and checks images. It needs only the Python standard library.

### seal

```
rimg.py seal IN.bin -o OUT.img [--name NAME] [--image-version X.Y.Z] [--title TEXT] [--icon ICON.pbm]
```

The input is the raw binary from `objcopy`. The startup code already wrote the stub, magic, `hdr_version`, `hdr_size`, `load_addr`, `mem_size`, `abi_version` and `subtype`. `seal` then:

1. Checks that word 0 is an ARM `b` instruction, then checks the magic, `hdr_version` and `hdr_size`. It fails on anything else.
2. Sets `name` and `image_version` if given.
3. With `--title` or `--icon`, fills the META block. It fails if `flags` bit 0 is clear, because the image has no room for the block. The icon must be a 32x32 PBM file.
4. Sets `file_size` to the file length.
5. Checks `mem_size >= file_size` and the slot bounds.
6. Computes and writes `crc32`.

### info

```
rimg.py info FILE
```

Prints every header field, one per line as `field=value`. Numbers are in hex with `0x`. Prints the META title if present. Exits 0 if the header parses. It does not check the CRC.

### verify

```
rimg.py verify FILE [--fat-size N]
```

Runs validation steps 1 to 6. Step 3 compares with `--fat-size`, or with the file's own size if absent. Prints `ok` or the `@RC` error name. Exits 0 on success, 1 on a validation failure, 2 on bad usage or an unreadable file.

`tools/rimg.py` replaces `mkbootimg.py` and `sign_payload.py` from MFBootAgent. Images carry no signature.
