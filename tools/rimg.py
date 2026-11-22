#!/usr/bin/env python3
"""Seal, inspect and verify rimg images. See docs/spec/IMAGE_FORMAT.md.

    rimg.py seal FILE [-o OUT] [--name NAME] [--image-version X.Y.Z]
    rimg.py info FILE
    rimg.py verify FILE [--fat-size N] [--slot-base A --slot-size N]
                        [--magic BOOT|ROM1|ROBCO78] [--abi-major N]

verify exits with the error code of the first failed step, 0 on success.
seal exits 1 on a bad image. Usage errors exit 64.
"""

import argparse
import struct
import sys
import zlib

HDR_OFFSET = 0x20
HDR_SIZE = 64
HDR_END = HDR_OFFSET + HDR_SIZE
META_SIZE = 192
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
    # 2. Header version, size, flags, reserved.
    if (h["hdr_version"] != 1 or h["hdr_size"] != HDR_SIZE
            or h["flags"] & ~FLAG_META or h["reserved"] != 0):
        raise RimgError(E_VERSION, "unsupported header version, size, flags or reserved")
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


def parse_version(text):
    parts = text.split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        raise ValueError(f"bad version {text!r}, expected X.Y.Z")
    major, minor, patch = (int(p) for p in parts)
    if major > 0xFFFF or minor > 0xFF or patch > 0xFF:
        raise ValueError(f"version {text!r} out of range")
    return (major << 16) | (minor << 8) | patch


def seal(data, name=None, image_version=None):
    """Returns the sealed image. Raises RimgError on a bad input."""
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
        raw = name.encode("ascii")
        if len(raw) > 16:
            raise RimgError(E_VERSION, "name is longer than 16 bytes")
        struct.pack_into("16s", data, HDR_OFFSET + 0x2C, raw)
    if image_version is not None:
        struct.pack_into("<I", data, HDR_OFFSET + 0x24, image_version)
    struct.pack_into("<I", data, CRC_OFFSET, image_crc(data, len(data)))
    # Steps 1 to 6 must pass on the result, slot bounds included.
    validate(bytes(data))
    return bytes(data)


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
    if h["flags"] & FLAG_META and len(data) >= HDR_END + META_SIZE:
        lines.append(f"title={text_field(data[HDR_END:HDR_END + 64])}")
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
        if args.cmd == "seal":
            out = seal(data, args.name, args.image_version)
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
