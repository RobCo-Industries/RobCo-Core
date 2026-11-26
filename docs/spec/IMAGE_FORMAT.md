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
| ROBCO78 | 0 | `RIMG_HOLO_GAME` | Game | `0x00800000`-`0x00BFFFFF` | `/PIPBOY/HOLO/*.HTP`, or `*.HTP` in the root of a holotape slot card |
| ROBCO78 | 1 | `RIMG_HOLO_UTILITY` | Utility | `0x00800000`-`0x00BFFFFF` | `/PIPBOY/HOLO/*.HTP`, or `*.HTP` in the root of a holotape slot card |
| ROBCO78 | 2 | `RIMG_HOLO_DATA` | Data. Not executable. | none | `/PIPBOY/HOLO/*.HTP`, `*.HTP` in the root of a holotape slot card, `/UOS/HOLO/*.HTP` |

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
| 2 | `hdr_version == 1`, `hdr_size == 64`, unknown `flags` bits are 0, `reserved == 0`. `flags` bit 0 is set for `ROM1` and `ROBCO78` and clear for `BOOT`. | `RIMG_E_VERSION` |
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

`rimg_validate()` in `src/lib/rimg.c` runs steps 1 to 6 on an image already in memory. `rimg_expect_t` gives the accepted magic, the slot, the ABI major and the FAT size. Step 3 also fails when `file_size` is larger than the readable bytes. `rimg_strerror()` returns the `@RC` name. `tools/rimg.py verify` runs the same steps.

A loader that fails at step 6 has already overwritten the slot. That is fine, because the slot belongs to the image being loaded.

The loader does not zero BSS. The image's own startup code does that.

An image can check its own CRC after start with `rimg_self_check()`. The CRC then holds only while the image has not written its `.data`. The firmware loads some images with no validation, so the header itself may be corrupt. `rimg_self_check()` first compares the header with the values the image was linked with, in step order: the magic (step 1), `hdr_version` and `hdr_size` (step 2), `file_size` against the linked size (step 3), and `load_addr` against the linked start (step 4). It reports the code of the first mismatch and scans nothing. Only then does it run step 6 over the linked size. RobCo-Core's startup code writes nothing into the file bytes: it saves the entry registers in BSS, after it zeroes BSS.

## Files on the SD card

The boot partition is FAT32. Names are 8.3 and upper case. Each name is 1 to 8 characters, then optionally a dot and 1 to 3 characters. The characters are `A-Z`, `0-9`, `_`, `-` and `~`, a deliberate subset of what FAT allows. A path has at most 8 components and 63 characters. The `path` rule of [BOOT.CFG](./BOOT_PROTOCOL.md#bootcfg) uses the same rule and accepts lower case letters, which it upper-cases.

| Chain | Path | Kind |
|---|---|---|
| Terminal | `/RETROS.IMG` | BOOT 5, loaded by firmware |
| Terminal | `/ROBCO/MFBOOT.SYS` | BOOT 4 |
| Terminal | `/ROBCO/MFBOOT.BAK` | BOOT 4, fallback copy |
| Terminal | `/ROBCO/BOOT.CFG` | Boot menu config, INI text |
| Terminal | `/ROBCO/UOS.SYS` | BOOT 0 |
| Terminal | `/ROBCO/MAINT.SYS` | BOOT 2 |
| Terminal | `/ROBCO/DIAG.SYS` | BOOT 3 |
| Terminal | `/ROBCO/MAINT.REQ` | Maintenance request, text, see [BOOT_PROTOCOL.md](./BOOT_PROTOCOL.md#maintenance-request) |
| Terminal | `/UOS/...` | UOS data files |
| Terminal | `/UOS/SYSUAF.DEF` | Default user file, copied by the maintenance image's restore users tool |
| Terminal | `/UOS/HOLO/*.HTP` | ROBCO78 2, read by UOS |
| Pip-Boy | `/PIPOS.IMG` | BOOT 6, loaded by firmware |
| Pip-Boy | `/PIPBOY/PIPOS.CFG` | Pip-OS config, INI text |
| Pip-Boy | `/PIPBOY/ROM1.ROM` | ROM1 0 |
| Pip-Boy | `/PIPBOY/HOLO/*.HTP` | ROBCO78 0, 1 or 2 |
| Holotape slot card | `/*.HTP` | ROBCO78 0, 1 or 2, read-only, see [PIPOS_ABI.md](./PIPOS_ABI.md#holotapes) |
| Pip-Boy | `/PIPBOY/SAVE/*.SAV` | Save files, not `rimg` |
| Both | `/bootcode.bin`, `/start.elf`, `/fixup.dat` | Firmware, fetched by `tools/fetch_firmware.sh` at a pinned release with pinned SHA-256 values, never committed |
| Both | `/config.txt` | See [BOARD_SUPPORT.md](./BOARD_SUPPORT.md). `tools/mkbootdir.sh` writes it with the firmware and the image into a boot directory. |

| Extension | Meaning |
|---|---|
| `.IMG` | Stage loaded by the firmware |
| `.SYS` | Stage or OS loaded by another stage |
| `.BAK` | Fallback copy of a `.SYS` file |
| `.ROM` | Pip-Boy ROM |
| `.HTP` | Holotape |
| `.REQ` | Request file, read once by the next boot |

## tools/rimg.py

`tools/rimg.py` builds and checks images. It needs only the Python standard library.

### seal

```
rimg.py seal FILE [-o OUT] [--name NAME] [--image-version X.Y.Z] [--title TEXT] [--icon FILE] [--icon-format auto|raw|pbm]
```

The input is the raw binary from `objcopy`. The startup code already wrote the stub, magic, `hdr_version`, `hdr_size`, `load_addr`, `file_size`, `mem_size`, `abi_version` and `subtype`. `seal` writes the result in place, or to `OUT` with `-o`. It:

1. Checks that word 0 is an ARM `b` instruction, then checks the magic, `hdr_version` and `hdr_size`. It fails on anything else.
2. Sets `file_size` to the file length if the header holds 0. It fails if the header holds another non-zero value.
3. Sets `name` and `image_version` if given.
4. Sets the META title and icon if given. Both need `flags` bit 0 in the header, and `seal` fails without it. The title is ASCII, at most 64 bytes, NUL-padded. The icon file is a 32x32 PBM, plain (`P1`) or raw (`P4`), or 128 raw bytes in the META layout. `--icon-format` picks the format. The default, `auto`, reads a file of exactly 128 bytes as raw and any other length as PBM. A 32x32 PBM is never 128 bytes long, so a raw icon that starts with `P1` or `P4` stays raw. `raw` needs exactly 128 bytes. `pbm` rejects a 128-byte file. Errors name the format assumed and why. PBM 1 (black) is a lit pixel and 0 (white) is unlit. `P1` allows `#` comments anywhere, to the end of the line, and digits with or without whitespace between them. `P4` needs exactly 128 bytes after the single whitespace byte that ends the header. `seal` rejects other dimensions, a wrong pixel count and any other character. Without these options the META bytes stay as the startup code wrote them, blank for a zeroed block.
5. Computes and writes `crc32`.
6. Runs validation steps 1 to 6 on the result, with the default slot for the subtype. It fails and writes nothing if one fails.

`seal` exits 0 on success and 1 on a bad image.

`mk/robco.mk` seals every image it builds.

### pack

```
rimg.py pack PAYLOAD -o OUT --magic BOOT|ROM1|ROBCO78 --subtype N --load-addr A
             [--name NAME] [--image-version X.Y.Z] [--abi-version MAJOR.MINOR]
             [--mem-size N] [--entry OFFSET] [--meta | --no-meta]
             [--title TEXT] [--icon FILE] [--icon-format auto|raw|pbm]
```

Wraps a raw payload into a complete image, for tests and for builds that do not use `start.S`. The payload is code linked for `load_addr + 0x60`, or `load_addr + 0x120` with a META block. `pack`:

1. Writes word 0 as an ARM `b` to the payload start plus `--entry`, 0 by default. `--entry` must be a multiple of 4 and point at a whole word inside the payload. `pack` rejects any other value as a usage error. `verify` and the C validator do not check the branch, since the stub encoding is not fixed.
2. Writes the header: the magic, `hdr_version` 1, `hdr_size` 64, `load_addr`, `subtype`, `name`, `image_version` and `abi_version`. The ABI version defaults to the major of the magic with minor 0. `file_size` is the image length. `mem_size` defaults to it.
3. Adds the META block when the magic needs one, `ROM1` and `ROBCO78`, or as `--meta` and `--no-meta` say. The title is empty and the icon blank unless given, with the same rules as `seal`.
4. Seals the result as `seal` does, so validation steps 1 to 6 must pass. It writes nothing otherwise.

`pack` exits 0 on success, 64 on a usage error such as a bad `--entry`, and 1 on a bad input.

### info

```
rimg.py info FILE
```

Prints every header field, one per line as `field=value`. Numbers are in hex with `0x`. `magic` and `name` print as text without the NUL padding. With `flags` bit 0 set it also prints `title=` with the META title and `icon=set` or `icon=blank`, blank meaning all 128 icon bytes are 0. The last line is `crc_ok=1` or `crc_ok=0`. Exits 0 if the header parses.

### verify

```
rimg.py verify FILE [--fat-size N] [--slot-base A --slot-size N] [--magic BOOT|ROM1|ROBCO78] [--abi-major N]
```

Runs validation steps 1 to 6. Step 2 also checks that the META flag matches the magic.

- Step 1 accepts any of the three magics, or only `--magic`.
- Step 3 compares with `--fat-size`, or with the file's own size if absent.
- Step 4 uses the slot from the subtype table, or `--slot-base` and `--slot-size`.
- Step 5 uses the ABI major from the magic table, or `--abi-major`.

Prints `ok` or the `@RC` error name. The exit code is the error code from the table above, so 0 on success. A file shorter than `0x60` bytes gives `io`. Bad usage exits 64.

`tools/rimg.py` replaces `mkbootimg.py` and `sign_payload.py` from MFBootAgent. Images carry no signature.

## Sealing and packing

Sealing fills in the fields that only exist once the image is linked and writes the CRC. Packing wraps a raw payload into a complete image, then seals it. Two implementations follow these rules:

| Implementation | State |
|---|---|
| `seal()` and `pack()` in `tools/rimg.py`, behind the `seal` and `pack` commands | Exists |
| `rimg_seal()` and `rimg_pack()` in C, beside `rimg_validate()` | Exists, in `src/lib/rimg_seal.c`, with the signatures of [C functions](#c-functions) |

Both give the same bytes for the same inputs and fail with the same code at the same step. The [test vectors](#test-vectors) prove it. The code is a `rimg_err_t` value and its `@RC` name, as in [Validation](#validation).

An implementation writes nothing when a step fails. It returns the whole sealed file when every step passes.

### Inputs

| Input | Type | Rule | Code when broken |
|---|---|---|---|
| Image, for seal | Bytes | The raw output of `objcopy` | See the steps |
| Payload, for pack | Bytes | Code linked for `load_addr + 0x60`, or `load_addr + 0x120` with META | See the steps |
| `name` | Text or absent | ASCII, 0 to 16 bytes. Absent leaves the header's `name` as it is. | `version` when not ASCII or longer than 16 bytes, as for a bad `title`: both are header and META text |
| `image_version` | `X.Y.Z` or absent | Exactly three parts, each one or more of the ASCII digits `0` to `9`. X at most 65535, Y and Z at most 255. Stored as `(X << 16) \| (Y << 8) \| Z`. | A usage error, before any step |
| `title` | Text or absent | ASCII, 0 to 64 bytes, no NUL. Stored NUL-padded to 64 bytes. | `version` |
| `icon` | File bytes and a format, or absent | See [Icon decoding](#icon-decoding) | See there |
| `magic`, for pack | `BOOT`, `ROM1` or `ROBCO78` | | `magic` |
| `subtype`, `load_addr`, `mem_size`, for pack | Integers | Each from 0 to `0xFFFFFFFF`. `mem_size` absent means the file size. | `bounds`, at pack step 2 |
| `abi_version`, for pack | `MAJOR.MINOR` or absent | Two parts, each one or more of the ASCII digits `0` to `9`, each at most 65535. Absent means the ABI major of the magic, minor 0. | A usage error, before any step |
| `meta`, for pack | Yes, no or absent | Absent follows the magic: yes for `ROM1` and `ROBCO78`, no for `BOOT` | See the steps |
| `entry`, for pack | Integer | Byte offset into the payload. Default 0. | `size`, at pack step 4 |

A usage error is not a `rimg_err_t` code. `rimg.py` exits 64 for it. The C functions take `image_version` and `abi_version` as numbers, so the caller parses the text and reports the error itself.

### Icon decoding

The icon is decoded before any other step. The decoding follows the `--icon` rules of [seal](#seal). The codes are:

| Case | Code |
|---|---|
| Format `raw`, or `auto` with a 128-byte file: the file is not 128 bytes | `size` |
| Format `pbm` with a 128-byte file | `io` |
| The file does not start with `P1` or `P4`, or its header does not parse. [Numbers in a PBM header](#numbers-in-a-pbm-header) gives the form of the width and the height. | `io` |
| The PBM is not 32x32 | `size` |
| `P4`: the height is not followed by one whitespace byte | `io` |
| `P4`: not exactly 128 bytes after that byte | `size` |
| `P1`: a character other than `0`, `1`, whitespace or a comment | `io` |
| `P1`: not exactly 1024 pixels | `size` |

The result is the 128 bytes of the META icon layout.

#### Numbers in a PBM header

A PBM header is three tokens: the magic, the width and the height. In the header, whitespace is a space, a tab, a carriage return or a line feed, and a `#` starts a comment that runs to the next carriage return or line feed. A token is the bytes up to the next whitespace byte or `#`.

The width and the height each have exactly this form:

| Rule | Detail |
|---|---|
| Bytes | The ASCII digits `0` to `9`, bytes `0x30` to `0x39`, and nothing else |
| Length | 1 to 9 digits. Leading zeros count toward the length. |
| Value | The decimal number. Leading zeros are allowed and ignored: `032` and `0032` are 32, and `0` and `00` are 0. |
| A token in any other form | The header does not parse: `io` |
| A token in this form whose value is not 32 | `size` |

Nine digits fit a 32-bit integer, so an implementation in C or Rust reads the value with no overflow check.

These forms are refused with `io`, each because of one byte outside the set or one digit too many:

| Token | Why |
|---|---|
| `+32` | A sign |
| `-32` | A sign. The code is `io`, not `size`: the token is no number in this form. |
| `3_2` | A digit separator |
| `0x20`, `32.0`, `1e1` | A letter or a dot |
| A digit outside ASCII, such as U+FF13 | Not a byte from `0x30` to `0x39` |
| `0000000032` | Ten digits |
| `32` with a vertical tab, `0x0B`, or a form feed, `0x0C`, before or after it | Those two bytes are not whitespace in the header, so they are part of the token |
| An empty token, at the end of the file | No digit |

State: `tools/rimg.py` and `src/lib/rimg_seal.c` both follow this section. `_pbm_header()` in `rimg.py` checks each token with `ascii_digits()`, the check that `parse_version()` uses for its parts, and with the limit of 9 digits. Only then does it read the value with `int()`. Before this, `rimg.py` read both tokens with `int()` alone and differed in five cases:

| Token | `rimg.py` before | This section, and both tools now |
|---|---|---|
| `+32` | Read as 32, and the icon is accepted | `io` |
| `3_2` | Read as 32, and the icon is accepted | `io` |
| `-32` | `size` | `io` |
| `0000000032` | Read as 32, and the icon is accepted | `io` |
| `32` with a vertical tab or a form feed before or after it | Read as 32, and the icon is accepted: `int()` drops those bytes | `io` |

These vectors in `tests/vectors/rimg/` check the rule. Each vector is a seal of a holotape image with an icon in the `P1` form, whose header is `P1`, a line feed, the width, a space, the height and a line feed:

1. Vector `seal_err_icon_num_plus`: width `+32`, height `32`. Expect `io`.
2. Vector `seal_err_icon_num_minus`: width `-32`, height `32`. Expect `io`.
3. Vector `seal_err_icon_num_sep`: width `3_2`, height `32`. Expect `io`.
4. Vector `seal_err_icon_num_long`: width `0000000032`, height `32`. Expect `io`.
5. Vector `seal_err_icon_num_vt`: width `32` followed by the byte `0x0B`, then a space, height `32`. Expect `io`.
6. Vector `seal_err_icon_num_height`: width `32`, height `+32`. Expect `io`. It shows that the rule holds for both tokens.
7. Vector `seal_holo_icon_num_zeros`: width `032`, height `000000032`, which has nine digits. Expect `ok`, with the output of the same seal with `32` and `32`.

### Seal steps

| Step | Action | Code on failure |
|---|---|---|
| 1 | The image is at least `0x60` bytes. | `io` |
| 2 | Bits 27 to 24 of word 0 are `1010`, an ARM `b` with any condition. | `magic` |
| 3 | `magic` is one of the three magics. | `magic` |
| 4 | `hdr_version` is 1 and `hdr_size` is 64. | `version` |
| 5 | If `file_size` is 0, set it to the image length. Else it must equal the image length. | `size` |
| 6 | If `name` is given: every byte is below `0x80` and it is at most 16 bytes. Write it NUL-padded to all 16 bytes of the field. | `version` |
| 7 | If `image_version` is given, write it. | |
| 8 | If `title` or `icon` is given, `flags` bit 0 is set. | `version` |
| 9 | If `title` or `icon` is given, the image is at least `0x120` bytes. | `size` |
| 10 | Write the title into META bytes `0x00` to `0x3F` if given. Write the icon into META bytes `0x40` to `0xBF` if given. The other part stays as it is. | `version` for a bad title |
| 11 | Compute the CRC over the whole image with the `crc32` field read as zero, and write it. | |
| 12 | Run [validation](#validation) steps 1 to 6 on the result. Step 1 accepts the three magics. Step 3 compares with the image length. Step 4 uses the slot of the subtype from [Subtypes](#subtypes). Step 5 uses the ABI major of the magic. Step 2 also checks that `flags` bit 0 is set for `ROM1` and `ROBCO78` and clear for `BOOT`. | That step's code |

Seal never changes the stub, `flags`, `load_addr`, `mem_size`, `abi_version`, `subtype`, `reserved` or the code bytes. Step 12 catches what the startup code got wrong: a data tape fails there with `bounds`, because subtype 2 has no slot.

### Pack steps

| Step | Action | Code on failure |
|---|---|---|
| 1 | `magic` is one of the three magics. | `magic` |
| 2 | `subtype`, `load_addr` and, if given, `mem_size` are each from 0 to `0xFFFFFFFF`, checked in that order. | `bounds` |
| 3 | Resolve `meta`. Without META, a `title` or `icon` is refused. | `version` |
| 4 | `entry` is at least 0 and a multiple of 4, and `entry + 4` is at most the payload length. An empty payload always fails here. | `size` |
| 5 | The code offset is `0x60`, plus `0xC0` with META. The file size is the code offset plus the payload length. | |
| 6 | Start from that many zero bytes. Word 0 is `0xEA000000 \| (((code offset + entry - 8) >> 2) & 0x00FFFFFF)`, a `b` to the entry. A target more than 32 MiB away fails. | `version` |
| 7 | Write the header: the magic, `hdr_version` 1, `hdr_size` 64, `flags` 1 with META or 0 without, `load_addr`, `file_size`, `mem_size` or the file size, `crc32` 0, `abi_version`, `image_version` or 0, `subtype`, 16 NUL bytes of `name`, `reserved` 0. The META block stays zero. | |
| 8 | Copy the payload to the code offset. | |
| 9 | Seal the result with `name`, `title` and `icon` and no `image_version`, by the seal steps. | That step's code |

`rimg.py pack` on the command line checks `entry` before step 1 and exits 64, a usage error, where the function returns `size`. The C function returns `size`.

Seal never writes `abi_version`, and a Studio build does not pack. The startup code writes the project's target ABI into the header when it is compiled. See [Startup code](./SDK_PACKAGE.md#startup-code).

### C functions

`rimg_seal()` and `rimg_pack()` follow the steps above. Their declarations go into `include/robco/rimg.h`, beside `rimg_validate()`. Their code goes into a new file, `src/lib/rimg_seal.c`, beside `src/lib/rimg.c`, so a program that only validates links no sealing code. They touch no hardware. These are their declarations:

```c
/* Icon file formats, as rimg.py --icon-format. */
typedef enum rimg_icon_format {
    RIMG_ICON_AUTO = 0,     /* 128 bytes is raw, any other length is PBM */
    RIMG_ICON_RAW  = 1,     /* exactly 128 bytes in the META layout */
    RIMG_ICON_PBM  = 2,     /* a 32x32 PBM, P1 or P4 */
} rimg_icon_format_t;

/*
 * Seal inputs. A NULL pointer, or a has_ flag of 0, is an absent input.
 *   name           NUL-terminated text.
 *   image_version  (X << 16) | (Y << 8) | Z, already parsed by the caller.
 *   title          NUL-terminated text.
 *   icon           The icon file's bytes, icon_len of them. The function
 *                  decodes them by icon_format before any step.
 */
typedef struct rimg_seal_opts {
    const char        *name;
    int                has_image_version;
    uint32_t           image_version;
    const char        *title;
    const uint8_t     *icon;
    size_t             icon_len;
    rimg_icon_format_t icon_format;
} rimg_seal_opts_t;

/*
 * Pack inputs. seal holds name, image_version, title and icon. Pack writes
 * image_version into the header at step 7, 0 when absent, and seals with
 * the rest. The numbers are signed and 64 bits wide so that step 2 and
 * step 4 see a value out of range, as rimg.py does.
 *   magic        NUL-terminated: "BOOT", "ROM1" or "ROBCO78".
 *   abi_version  (MAJOR << 16) | MINOR. Absent: the ABI major of the magic, minor 0.
 *   meta         -1 follows the magic, 0 no META block, 1 a META block.
 */
typedef struct rimg_pack_opts {
    rimg_seal_opts_t seal;
    const char      *magic;
    int64_t          subtype;
    int64_t          load_addr;
    int              has_mem_size;
    int64_t          mem_size;
    int              has_abi_version;
    uint32_t         abi_version;
    int              meta;
    int64_t          entry;
} rimg_pack_opts_t;

/*
 * Seals the len bytes at image into out, which holds len bytes and does
 * not overlap image. Runs icon decoding, then seal steps 1 to 12.
 * Returns RIMG_OK, or the code of the first failed step. On a failure the
 * bytes of out are undefined and the caller writes no file.
 * A NULL image, opts or out returns RIMG_E_IO before any step.
 */
int rimg_seal(const void *image, size_t len, const rimg_seal_opts_t *opts, void *out);

/*
 * Packs payload_len bytes into out, which holds out_cap bytes and does not
 * overlap payload. Runs icon decoding, then pack steps 1 to 9, with the
 * size and capacity checks below at step 5.
 * Returns RIMG_OK and sets *out_len to the image length, or the code of
 * the first failed check. On a failure *out_len is 0 and the bytes of out
 * are undefined. A NULL opts, out or out_len, a NULL payload with a
 * non-zero payload_len, or a meta other than -1, 0 or 1, returns
 * RIMG_E_IO before any step. *out_len is 0 then too, if out_len is not NULL.
 */
int rimg_pack(const void *payload, size_t payload_len, const rimg_pack_opts_t *opts,
              void *out, size_t out_cap, size_t *out_len);
```

An `icon_format` outside the enum returns `RIMG_E_IO`, as an unknown `--icon-format` does in `rimg.py`.

`rimg_pack()` writes an image of exactly this many bytes:

| META block, as resolved at step 3 | Image length |
|---|---|
| No | `payload_len + 0x60` |
| Yes | `payload_len + 0x120` |

Step 3 resolves `meta` from the options alone: 1 means yes, 0 means no, and -1 means yes for `ROM1` and `ROBCO78` and no for `BOOT`. A caller computes the length by this rule before it allocates. No call returns it. The other headers in `include/robco/` state sizes as rules and take no NULL buffer as a size query, so `rimg.h` does the same.

Step 5 runs two checks, in this order, after steps 1 to 4 pass:

1. `payload_len` is at most `0xFFFFFFFF` minus the code offset, so the length fits the 32-bit `file_size`. Else the code is `RIMG_E_SIZE`. The C function compares `payload_len` with that difference and never adds first, so the sum cannot wrap on a 32-bit target. `rimg.py` cannot build such a payload, and no vector covers this case.
2. `out_cap` is at least the image length. Else the code is `RIMG_E_IO`. A larger `out_cap` is fine: the function writes only the image length and reports it in `*out_len`.

So a buffer that is too small fails after the checks of steps 1 to 4, and their codes come first. `rimg_seal()` has no capacity check: `out` holds `len` bytes, and seal step 5 fails with `RIMG_E_SIZE` when `len` is above `0xFFFFFFFF`, since `file_size` cannot equal it.

Either function may write into `out` before a later step fails. The caller then treats `out` as garbage and writes no file.

Both functions decode the PBM themselves. A C host test then runs every vector as it stands: it reads the `icon` file named in `vectors.ini` and passes its bytes with its `icon_format`. The caller parses only `image_version` and `abi_version`, by the rules of [Inputs](#inputs). It maps the other keys straight to the options: `meta` `yes` to 1, `no` to 0 and absent to -1, a quoted empty `name` or `title` to an empty text, not NULL, and each number to its `int64_t` field. It sizes `out` by the rule above, or by the input length for seal, and compares `*out_len` bytes of `out` with `output`, or the code with `expect`.

## Test vectors

`tests/vectors/rimg/` holds the vectors that every sealer must reproduce byte for byte.

| File | Content |
|---|---|
| `vectors.ini` | One section per vector |
| `*.in` | The input of each vector: an image for seal, a payload for pack |
| `*.out` | The expected output of each vector that succeeds |
| `icon_*` | Icon files that vectors name |
| `generate.py` | Writes every other file |

`vectors.ini` is INI text in the syntax of [`ini.h`](../guide/api/buses.md#configuration-files-inih), so a C host test reads it with the library's own parser. Each section is `[vector ID]`. Its keys:

| Key | Meaning |
|---|---|
| `op` | `seal` or `pack` |
| `input` | The input file, in the same folder |
| `magic`, `subtype`, `load_addr`, `mem_size`, `entry` | Pack inputs. Numbers are C literals, decimal or `0x` hex. |
| `name`, `image_version`, `abi_version`, `title` | Inputs as text. A quoted empty value is an empty text, which is not the same as an absent key. |
| `meta` | `yes` or `no`. Absent follows the magic. |
| `icon`, `icon_format` | The icon file and its format. `icon_format` absent means `auto`. |
| `expect` | `ok` or the `@RC` name of the code |
| `output` | The expected file. Present only when `expect` is `ok`. |

A key that is absent is an input that is not given. A vector that expects a failure expects that code and no output.

`generate.py` builds every input byte by byte and runs it through `tools/rimg.py`, which writes the outputs and the codes. `tests/tools/test_rimg_vectors.py` checks three things:

1. `tools/rimg.py` gives every expected output and code.
2. `generate.py` run again writes the same files, byte for byte.
3. The `seal` and `pack` commands give the same bytes as the functions for every vector that succeeds.

A change to `rimg.py` that changes a vector is a change to this spec. The vectors cover each code a sealer returns: `io`, `magic`, `version`, `size`, `bounds` and `abi`. Seal computes the CRC, so it never returns `crc`.
