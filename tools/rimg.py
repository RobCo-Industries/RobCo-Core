#!/usr/bin/env python3
"""Seal, inspect and verify rimg images. See docs/spec/IMAGE_FORMAT.md.

    rimg.py seal FILE [-o OUT] [--name NAME] [--image-version X.Y.Z]
                      [--title TEXT] [--icon FILE] [--icon-format F]
    rimg.py pack PAYLOAD -o OUT --magic BOOT|ROM1|ROBCO78 --subtype N
                      --load-addr A [--name NAME] [--image-version X.Y.Z]
                      [--abi-version MAJOR.MINOR] [--mem-size N] [--entry OFFSET]
                      [--meta | --no-meta] [--title TEXT] [--icon FILE] [--icon-format F]
    rimg.py info FILE
    rimg.py verify FILE [--fat-size N] [--slot-base A --slot-size N]
                        [--magic BOOT|ROM1|ROBCO78] [--abi-major N]

verify exits with the error code of the first failed step, 0 on success.
seal and pack exit 1 on a bad image or input. Usage errors exit 64.

The layout constants below copy include/robco/rimg.h. tests/tools/
test_rimg_tool.py fails when the two disagree.
"""

import argparse
import struct
import sys
import zlib

HDR_OFFSET = 0x20
HDR_SIZE = 64
HDR_END = HDR_OFFSET + HDR_SIZE
META_OFFSET = HDR_END
META_SIZE = 192
TITLE_SIZE = 64
ICON_SIZE = 128
ICON_W = ICON_H = 32
CRC_OFFSET = 0x3C
FLAG_META = 1
HDR_FMT = "<8sHHIIIIIIII16sI"

FIELDS = ("magic", "hdr_version", "hdr_size", "flags", "load_addr", "file_size",
          "mem_size", "crc32", "abi_version", "image_version", "subtype", "name",
          "reserved")

MAGICS = {
    "BOOT": b"BOOT\0\0\0\0",
    "ROM1": b"ROM1\0\0\0\0",
    "ROBCO78": b"ROBCO78\0",
}
ABI_MAJOR = {"BOOT": 2, "ROM1": 1, "ROBCO78": 1}
# Whether images of a magic carry the META block (flags bit 0).
HAS_META = {"BOOT": False, "ROM1": True, "ROBCO78": True}

# (magic, subtype) -> (slot base, slot size). Subtypes not listed have no slot.
SLOTS = {
    ("BOOT", 0): (0x00400000, 0x00C00000),
    ("BOOT", 2): (0x00400000, 0x00C00000),
    ("BOOT", 3): (0x00400000, 0x00C00000),
    ("BOOT", 4): (0x00200000, 0x00200000),
    ("BOOT", 5): (0x00008000, 0x000F8000),
    ("BOOT", 6): (0x00008000, 0x003F8000),
    ("BOOT", 7): (0x00008000, 0x000F8000),
    ("ROM1", 0): (0x00400000, 0x00400000),
    ("ROBCO78", 0): (0x00800000, 0x00400000),
    ("ROBCO78", 1): (0x00800000, 0x00400000),
}

OK, E_NOFILE, E_IO, E_MAGIC, E_VERSION, E_SIZE, E_BOUNDS, E_ABI, E_CRC = range(9)
ERR_NAMES = ["ok", "nofile", "io", "magic", "version", "size", "bounds", "abi", "crc"]
EX_USAGE = 64


class RimgError(Exception):
    def __init__(self, code, msg):
        super().__init__(msg)
        self.code = code


def parse_header(data):
    if len(data) < HDR_END:
        raise RimgError(E_IO, f"file is {len(data)} bytes, shorter than the header")
    return dict(zip(FIELDS, struct.unpack_from(HDR_FMT, data, HDR_OFFSET)))


def magic_kind(magic):
    for kind, raw in MAGICS.items():
        if raw == magic:
            return kind
    return None


def image_crc(data, file_size):
    """CRC-32/ISO-HDLC over [0, file_size) with the crc32 field read as zero."""
    crc = zlib.crc32(data[:CRC_OFFSET])
    crc = zlib.crc32(b"\0\0\0\0", crc)
    return zlib.crc32(data[CRC_OFFSET + 4:file_size], crc) & 0xFFFFFFFF


def validate(data, fat_size=None, magic=None, slot=None, abi_major=None):
    """Runs validation steps 1 to 6 in order. Returns the header or raises RimgError.

    Mirrors rimg_validate() in src/lib/rimg.c check for check. A fat_size,
    slot size or abi_major of 0 or None means "use the default".
    """
    h = parse_header(data)
    kind = magic_kind(h["magic"])
    # 1. Magic.
    if kind is None or (magic is not None and kind != magic):
        raise RimgError(E_MAGIC, f"magic {h['magic']!r} not accepted")
    # 2. Header version, size, flags, reserved, and META as the magic requires.
    if (h["hdr_version"] != 1 or h["hdr_size"] != HDR_SIZE
            or h["flags"] & ~FLAG_META or h["reserved"] != 0):
        raise RimgError(E_VERSION, "unsupported header version, size, flags or reserved")
    if bool(h["flags"] & FLAG_META) != HAS_META[kind]:
        raise RimgError(E_VERSION, f"{kind} images {'need' if HAS_META[kind] else 'must not have'}"
                                   " the META flag")
    # 3. File size. Like rimg_validate(), file_size must fit the bytes read
    # in every mode. Without --fat-size the file length is the expected size.
    expected = len(data) if not fat_size else fat_size
    min_size = HDR_END + (META_SIZE if h["flags"] & FLAG_META else 0)
    if (h["file_size"] < min_size or h["file_size"] > len(data)
            or h["file_size"] != expected):
        raise RimgError(E_SIZE, f"file_size {h['file_size']} but expected {expected},"
                                f" at least {min_size}, at most {len(data)} read")
    # 4. Subtype, slot and memory bounds. A slot size of 0 uses the default.
    default = SLOTS.get((kind, h["subtype"]))
    if default is None:
        raise RimgError(E_BOUNDS, f"subtype {h['subtype']} has no slot for {kind}")
    base, size = slot if slot is not None and slot[1] else default
    if (h["load_addr"] != base or h["mem_size"] < h["file_size"]
            or h["load_addr"] + h["mem_size"] > base + size):
        raise RimgError(E_BOUNDS, f"image does not fit the slot {base:#010x}+{size:#x}")
    # 5. ABI major.
    # An ABI major of 0 uses the default, as in rimg_validate().
    want = abi_major if abi_major else ABI_MAJOR[kind]
    if h["abi_version"] >> 16 != want:
        raise RimgError(E_ABI, f"ABI major {h['abi_version'] >> 16}, expected {want}")
    # 6. CRC.
    if image_crc(data, h["file_size"]) != h["crc32"]:
        raise RimgError(E_CRC, "CRC mismatch")
    return h


def read_file(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        raise RimgError(E_NOFILE, f"{path}: no such file")
    except OSError as e:
        raise RimgError(E_IO, f"{path}: {e}")


def ascii_digits(text):
    """True for one or more of the ASCII digits 0 to 9, and nothing else."""
    return text != "" and all("0" <= c <= "9" for c in text)


def parse_version(text):
    parts = text.split(".")
    if len(parts) != 3 or not all(ascii_digits(p) for p in parts):
        raise ValueError(f"bad version {text!r}, expected X.Y.Z")
    major, minor, patch = (int(p) for p in parts)
    if major > 0xFFFF or minor > 0xFF or patch > 0xFF:
        raise ValueError(f"version {text!r} out of range")
    return (major << 16) | (minor << 8) | patch


def title_bytes(title):
    try:
        raw = title.encode("ascii")
    except UnicodeEncodeError:
        raise RimgError(E_VERSION, "title is not ASCII")
    if len(raw) > TITLE_SIZE:
        raise RimgError(E_VERSION, f"title is longer than {TITLE_SIZE} bytes")
    if b"\0" in raw:
        raise RimgError(E_VERSION, "title holds a NUL byte")
    return raw.ljust(TITLE_SIZE, b"\0")


def _pbm_header(data):
    """Returns (kind, width, height, offset after the height) of a PBM file."""
    tokens = []
    i = 0
    while len(tokens) < 3:
        while i < len(data) and (data[i] in b" \t\r\n" or data[i] == ord("#")):
            if data[i] == ord("#"):
                while i < len(data) and data[i] not in b"\r\n":
                    i += 1
            else:
                i += 1
        start = i
        while i < len(data) and data[i] not in b" \t\r\n#":
            i += 1
        if start == i:
            raise RimgError(E_IO, "bad PBM header")
        tokens.append(data[start:i])
    try:
        return tokens[0], int(tokens[1]), int(tokens[2]), i
    except ValueError:
        raise RimgError(E_IO, "bad PBM header")


def _p1_bits(body):
    """Returns the pixel digits of a P1 raster, comments removed."""
    bits = []
    i = 0
    while i < len(body):
        c = body[i]
        if c == ord("#"):
            while i < len(body) and body[i] not in b"\r\n":
                i += 1
            continue
        if c in b"01":
            bits.append(c == ord("1"))
        elif c not in b" \t\r\n\v\f":
            raise RimgError(E_IO, f"bad character {chr(c)!r} in PBM pixel data")
        i += 1
    return bits


ICON_FORMATS = ("auto", "raw", "pbm")


def _parse_pbm_icon(data):
    """Returns the 128 META icon bytes from a 32x32 PBM (P1 or P4)."""
    if data[:2] not in (b"P1", b"P4"):
        raise RimgError(E_IO, "bad PBM header: expected P1 or P4")
    kind, w, h, end = _pbm_header(data)
    if (w, h) != (ICON_W, ICON_H):
        raise RimgError(E_SIZE, f"icon is {w}x{h}, expected {ICON_W}x{ICON_H}")
    if kind == b"P4":
        # One whitespace byte after the height, then 4 bytes per row, MSB first.
        if end >= len(data) or data[end] not in b" \t\r\n\v\f":
            raise RimgError(E_IO, "bad PBM header")
        raw = data[end + 1:]
        if len(raw) != ICON_SIZE:
            raise RimgError(E_SIZE, f"PBM pixel data is {len(raw)} bytes,"
                                    f" expected {ICON_SIZE}")
        return bytes(raw)
    bits = _p1_bits(data[end:])
    if len(bits) != ICON_W * ICON_H:
        raise RimgError(E_SIZE, f"PBM has {len(bits)} pixels,"
                                f" expected {ICON_W * ICON_H}")
    out = bytearray(ICON_SIZE)
    for i, b in enumerate(bits):
        if b:
            out[i // 8] |= 0x80 >> (i % 8)
    return bytes(out)


def parse_icon(data, fmt="auto"):
    """Returns the 128 META icon bytes from a 32x32 PBM (P1 or P4) or 128 raw bytes.

    PBM 1 (black) is a lit pixel and 0 (white) is unlit. With fmt "auto" a
    file of exactly 128 bytes is raw and any other length is PBM. A 32x32 PBM
    is never 128 bytes long, so the rule has no ambiguity.
    """
    if fmt not in ICON_FORMATS:
        raise RimgError(E_IO, f"unknown icon format {fmt!r}")
    if fmt == "raw" or (fmt == "auto" and len(data) == ICON_SIZE):
        if len(data) != ICON_SIZE:
            raise RimgError(E_SIZE, f"icon file is {len(data)} bytes (--icon-format raw):"
                                    f" a raw icon must be {ICON_SIZE} bytes")
        return bytes(data)
    if fmt == "pbm" and len(data) == ICON_SIZE:
        raise RimgError(E_IO, f"icon is {ICON_SIZE} bytes, which is the raw size:"
                              " a 32x32 PBM cannot be that long")
    why = "--icon-format pbm" if fmt == "pbm" else f"not {ICON_SIZE} bytes, so read as PBM"
    try:
        return _parse_pbm_icon(data)
    except RimgError as e:
        raise RimgError(e.code, f"icon file is {len(data)} bytes ({why}): {e.args[-1]}")


def seal(data, name=None, image_version=None, title=None, icon=None):
    """Returns the sealed image. Raises RimgError on a bad input.

    title (str) and icon (128 bytes) fill the META block. They need the
    META flag in the header. Without them the META bytes stay as they are.
    """
    data = bytearray(data)
    h = parse_header(data)
    word0 = struct.unpack_from("<I", data, 0)[0]
    if (word0 >> 24) & 0x0F != 0x0A:
        raise RimgError(E_MAGIC, f"word 0 ({word0:#010x}) is not an ARM b instruction")
    if magic_kind(h["magic"]) is None:
        raise RimgError(E_MAGIC, f"magic {h['magic']!r} not known")
    if h["hdr_version"] != 1 or h["hdr_size"] != HDR_SIZE:
        raise RimgError(E_VERSION, "unsupported header version or size")
    if h["file_size"] == 0:
        struct.pack_into("<I", data, HDR_OFFSET + 0x14, len(data))
    elif h["file_size"] != len(data):
        raise RimgError(E_SIZE, f"header file_size {h['file_size']} but the file has"
                                f" {len(data)} bytes")
    if name is not None:
        try:
            raw = name.encode("ascii")
        except UnicodeEncodeError:
            raise RimgError(E_VERSION, "name is not ASCII")
        if len(raw) > 16:
            raise RimgError(E_VERSION, "name is longer than 16 bytes")
        struct.pack_into("16s", data, HDR_OFFSET + 0x2C, raw)
    if image_version is not None:
        struct.pack_into("<I", data, HDR_OFFSET + 0x24, image_version)
    if title is not None or icon is not None:
        if not h["flags"] & FLAG_META:
            raise RimgError(E_VERSION, "a title or an icon needs the META flag in the header")
        if len(data) < META_OFFSET + META_SIZE:
            raise RimgError(E_SIZE, "the file is too short for a META block")
        if title is not None:
            data[META_OFFSET:META_OFFSET + TITLE_SIZE] = title_bytes(title)
        if icon is not None:
            if len(icon) != ICON_SIZE:
                raise RimgError(E_SIZE, f"icon must be {ICON_SIZE} bytes")
            data[META_OFFSET + TITLE_SIZE:META_OFFSET + META_SIZE] = icon
    struct.pack_into("<I", data, CRC_OFFSET, image_crc(data, len(data)))
    # Steps 1 to 6 must pass on the result, slot bounds included.
    validate(bytes(data))
    return bytes(data)


def branch_to(offset):
    """The ARM instruction `b offset`, placed at address 0."""
    if offset % 4:
        raise RimgError(E_VERSION, f"branch target {offset:#x} is not word aligned")
    words = (offset - 8) >> 2
    # B holds a signed 24-bit word offset from PC + 8.
    if not -(1 << 23) <= words < (1 << 23):
        raise RimgError(E_VERSION, f"branch target {offset:#x} is out of range for b")
    return 0xEA000000 | (words & 0x00FFFFFF)


def check_entry(entry, payload_len):
    """Raises ValueError unless entry is a word offset inside the payload."""
    if entry < 0 or entry % 4 or entry + 4 > payload_len:
        raise ValueError(f"entry offset {entry:#x} must be a multiple of 4 and point at"
                         f" a word inside the {payload_len}-byte payload")


def pack(payload, magic, subtype, load_addr, name=None, image_version=0, abi_version=None,
         mem_size=None, meta=None, title=None, icon=None, entry=0):
    """Wraps a raw payload into a sealed image and returns it.

    The payload is code linked for load_addr + 0x60, or load_addr + 0x120
    with META. Word 0 branches to `entry`, an offset into the payload.
    meta None follows the magic: ROM1 and ROBCO78 carry META, BOOT does not.
    The icon is blank unless given. mem_size None uses the file size.
    """
    if magic not in MAGICS:
        raise RimgError(E_MAGIC, f"magic {magic!r} not known")
    for key, value in (("subtype", subtype), ("load_addr", load_addr), ("mem_size", mem_size)):
        if value is not None and not 0 <= value <= 0xFFFFFFFF:
            raise RimgError(E_BOUNDS, f"{key} {value:#x} is outside 0 to 0xffffffff")
    if meta is None:
        meta = HAS_META[magic]
    if not meta and (title is not None or icon is not None):
        raise RimgError(E_VERSION, "a title or an icon needs META")
    try:
        check_entry(entry, len(payload))
    except ValueError as e:
        raise RimgError(E_SIZE, str(e))
    code = HDR_END + (META_SIZE if meta else 0)
    size = code + len(payload)
    if abi_version is None:
        abi_version = ABI_MAJOR[magic] << 16
    data = bytearray(size)
    struct.pack_into("<I", data, 0, branch_to(code + entry))
    struct.pack_into(HDR_FMT, data, HDR_OFFSET, MAGICS[magic], 1, HDR_SIZE,
                     FLAG_META if meta else 0, load_addr, size,
                     size if mem_size is None else mem_size, 0, abi_version,
                     image_version, subtype, b"", 0)
    data[code:] = payload
    return seal(bytes(data), name, None, title, icon)


def parse_abi(text):
    parts = text.split(".")
    if len(parts) != 2 or not all(ascii_digits(p) for p in parts):
        raise ValueError(f"bad ABI version {text!r}, expected MAJOR.MINOR")
    major, minor = (int(p) for p in parts)
    if major > 0xFFFF or minor > 0xFFFF:
        raise ValueError(f"ABI version {text!r} out of range")
    return (major << 16) | minor


def text_field(raw):
    return raw.rstrip(b"\0").decode("ascii", "replace")


def info_lines(data):
    h = parse_header(data)
    lines = []
    for key in FIELDS:
        v = h[key]
        if isinstance(v, bytes):
            lines.append(f"{key}={text_field(v)}")
        else:
            lines.append(f"{key}={v:#x}")
    if h["flags"] & FLAG_META and len(data) >= META_OFFSET + META_SIZE:
        lines.append(f"title={text_field(data[META_OFFSET:META_OFFSET + TITLE_SIZE])}")
        icon = data[META_OFFSET + TITLE_SIZE:META_OFFSET + META_SIZE]
        lines.append(f"icon={'set' if any(icon) else 'blank'}")
    ok = h["file_size"] <= len(data) and image_crc(data, h["file_size"]) == h["crc32"]
    lines.append(f"crc_ok={int(ok)}")
    return lines


class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.print_usage(sys.stderr)
        print(f"{self.prog}: error: {message}", file=sys.stderr)
        sys.exit(EX_USAGE)


def int_arg(text):
    return int(text, 0)


def main(argv=None):
    p = Parser(prog="rimg.py", description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", parser_class=Parser)
    sub.required = True

    s = sub.add_parser("seal", help="set file_size and the CRC")
    s.add_argument("file")
    s.add_argument("-o", "--output", help="write here instead of in place")
    s.add_argument("--name")
    s.add_argument("--image-version", type=parse_version)
    s.add_argument("--title", help="META title, ASCII, at most 64 bytes")
    s.add_argument("--icon", help="META icon: a 32x32 PBM (P1 or P4, 1 is lit) or 128 raw bytes")
    s.add_argument("--icon-format", choices=ICON_FORMATS, default="auto",
                   help="auto: 128 bytes is raw, any other length is PBM")

    k = sub.add_parser("pack", help="wrap a raw payload into a sealed image")
    k.add_argument("file", metavar="PAYLOAD")
    k.add_argument("-o", "--output", required=True)
    k.add_argument("--magic", required=True, choices=sorted(MAGICS))
    k.add_argument("--subtype", required=True, type=int_arg)
    k.add_argument("--load-addr", required=True, type=int_arg)
    k.add_argument("--name")
    k.add_argument("--image-version", type=parse_version, default=0)
    k.add_argument("--abi-version", type=parse_abi)
    k.add_argument("--mem-size", type=int_arg)
    k.add_argument("--entry", type=int_arg, default=0,
                   help="entry point as an offset into the payload: a multiple of 4,"
                        " at least 4 bytes before its end")
    m = k.add_mutually_exclusive_group()
    m.add_argument("--meta", dest="meta", action="store_true", default=None)
    m.add_argument("--no-meta", dest="meta", action="store_false")
    k.add_argument("--title")
    k.add_argument("--icon", help="META icon: a 32x32 PBM (P1 or P4, 1 is lit)"
                                  " or 128 raw bytes")
    k.add_argument("--icon-format", choices=ICON_FORMATS, default="auto",
                   help="auto: 128 bytes is raw, any other length is PBM")

    i = sub.add_parser("info", help="print the header fields")
    i.add_argument("file")

    v = sub.add_parser("verify", help="run validation steps 1 to 6")
    v.add_argument("file")
    v.add_argument("--fat-size", type=int_arg)
    v.add_argument("--slot-base", type=int_arg)
    v.add_argument("--slot-size", type=int_arg)
    v.add_argument("--magic", choices=sorted(MAGICS))
    v.add_argument("--abi-major", type=int_arg)

    args = p.parse_args(argv)
    try:
        data = read_file(args.file)
        if args.cmd in ("seal", "pack"):
            icon = None if args.icon is None else parse_icon(read_file(args.icon),
                                                               args.icon_format)
            if args.cmd == "pack":
                try:
                    check_entry(args.entry, len(data))
                except ValueError as e:
                    p.error(str(e))
            if args.cmd == "seal":
                out = seal(data, args.name, args.image_version, args.title, icon)
            else:
                out = pack(data, args.magic, args.subtype, args.load_addr, args.name,
                           args.image_version, args.abi_version, args.mem_size, args.meta,
                           args.title, icon, args.entry)
            with open(args.output or args.file, "wb") as f:
                f.write(out)
            return 0
        if args.cmd == "info":
            print("\n".join(info_lines(data)))
            return 0
        if (args.slot_base is None) != (args.slot_size is None):
            p.error("--slot-base and --slot-size go together")
        slot = None if args.slot_base is None else (args.slot_base, args.slot_size)
        validate(data, args.fat_size, args.magic, slot, args.abi_major)
        print("ok")
        return 0
    except RimgError as e:
        if args.cmd == "verify":
            print(ERR_NAMES[e.code])
            print(f"rimg.py: {e}", file=sys.stderr)
            return e.code
        print(f"rimg.py: {args.cmd}: {ERR_NAMES[e.code]}: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
