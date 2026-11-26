#!/usr/bin/env python3
"""Write the rimg seal and pack test vectors. See docs/spec/IMAGE_FORMAT.md.

    generate.py [OUT_DIR]

OUT_DIR defaults to the folder of this script. The inputs are built here
byte by byte. The outputs and the expected codes come from tools/rimg.py,
so the vectors hold what the tool does today.
tests/tools/test_rimg_vectors.py runs this into a temporary folder and
fails when any file differs from the committed one.
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rimg  # noqa: E402

MAGIC = {"BOOT": b"BOOT\0\0\0\0", "ROM1": b"ROM1\0\0\0\0", "ROBCO78": b"ROBCO78\0"}


def pattern(n, seed):
    return bytes((i * 7 + seed) & 0xFF for i in range(n))


def raw_image(magic="ROBCO78", subtype=0, load_addr=0x00800000, meta=True, code=0x40,
              file_size=0, mem_size=None, abi=0x00010003, flags=None, hdr_version=1,
              hdr_size=64, word0=None, name=b"", icon_fill=0, length=None):
    """An image as objcopy leaves it: stub, header from the startup code, code."""
    start = rimg.HDR_END + (rimg.META_SIZE if meta else 0)
    size = start + code
    data = bytearray(size)
    data[start:] = pattern(code, 3)
    if word0 is None:
        word0 = rimg.branch_to(start)
    struct.pack_into("<I", data, 0, word0)
    struct.pack_into(rimg.HDR_FMT, data, rimg.HDR_OFFSET, MAGIC.get(magic, magic), hdr_version,
                     hdr_size, (1 if meta else 0) if flags is None else flags, load_addr,
                     file_size, size + 0x1000 if mem_size is None else mem_size, 0, abi,
                     0, subtype, name, 0)
    if meta and icon_fill:
        data[rimg.META_OFFSET + rimg.TITLE_SIZE:start] = bytes([icon_fill]) * rimg.ICON_SIZE
    return bytes(data if length is None else data[:length])


def pbm_p1(w=32, h=32):
    rows = []
    for y in range(h):
        rows.append(" ".join("1" if x in (0, w - 1) or y in (0, h - 1) or x == y else "0"
                             for x in range(w)))
    return ("P1\n# frame and diagonal\n%d %d\n" % (w, h) + "\n".join(rows) + "\n").encode()


def pbm_p4():
    body = bytearray(rimg.ICON_SIZE)
    for y in range(32):
        for x in range(32):
            if (x + y) % 3 == 0:
                body[y * 4 + x // 8] |= 0x80 >> (x % 8)
    return b"P4\n32 32\n" + bytes(body)


ICONS = {
    "icon_p1.pbm": pbm_p1(),
    "icon_p4.pbm": pbm_p4(),
    "icon_raw.bin": pattern(128, 11),
    "icon_16.pbm": pbm_p1(16, 16),
    "icon_badchar.pbm": pbm_p1().replace(b"0 0 0", b"0 2 0", 1),
    "icon_100.bin": pattern(100, 5),
    "icon_num_plus.pbm": pbm_p1().replace(b"32 32", b"+32 32", 1),
    "icon_num_minus.pbm": pbm_p1().replace(b"32 32", b"-32 32", 1),
    "icon_num_sep.pbm": pbm_p1().replace(b"32 32", b"3_2 32", 1),
    "icon_num_long.pbm": pbm_p1().replace(b"32 32", b"0000000032 32", 1),
    "icon_num_vt.pbm": pbm_p1().replace(b"32 32", b"32\x0b 32", 1),
    "icon_num_height.pbm": pbm_p1().replace(b"32 32", b"32 +32", 1),
    "icon_num_zeros.pbm": pbm_p1().replace(b"32 32", b"032 000000032", 1),
}

PAYLOAD = pattern(0x40, 1)
SHORT_PAYLOAD = pattern(8, 2)

# (name, op, input bytes, options). Options are the vector keys.
CASES = [
    ("seal_holo_plain", "seal", raw_image(), {}),
    ("seal_holo_full", "seal", raw_image(),
     {"name": "PIPES", "image_version": "1.2.3", "title": "Pipes", "icon": "icon_p1.pbm"}),
    ("seal_holo_icon_p4", "seal", raw_image(subtype=1), {"icon": "icon_p4.pbm"}),
    ("seal_holo_icon_raw", "seal", raw_image(), {"icon": "icon_raw.bin", "icon_format": "raw"}),
    ("seal_holo_icon_auto_raw", "seal", raw_image(), {"icon": "icon_raw.bin"}),
    ("seal_holo_title_only", "seal", raw_image(icon_fill=0x5A), {"title": "Keeps its icon"}),
    ("seal_holo_title_empty", "seal", raw_image(), {"title": ""}),
    ("seal_holo_size_set", "seal", raw_image(file_size=0x160), {}),
    ("seal_holo_name_16", "seal", raw_image(name=b"OLDNAME"), {"name": "ABCDEFGHIJKLMNOP"}),
    ("seal_holo_name_empty", "seal", raw_image(name=b"OLDNAME"), {"name": ""}),
    ("seal_holo_version_max", "seal", raw_image(), {"image_version": "65535.255.255"}),
    ("seal_rom", "seal", raw_image("ROM1", 0, 0x00400000, abi=0x00010000),
     {"name": "DEITRIX", "title": "Deitrix", "icon": "icon_p1.pbm"}),
    ("seal_boot", "seal", raw_image("BOOT", 7, 0x8000, meta=False, abi=0x00020000),
     {"name": "robco"}),
    ("seal_boot_agent", "seal", raw_image("BOOT", 4, 0x00200000, meta=False, abi=0x00020000), {}),
    ("seal_err_short", "seal", raw_image(length=0x5F), {}),
    ("seal_err_not_branch", "seal", raw_image(word0=0xEB000046), {}),
    ("seal_err_magic", "seal", raw_image(magic=b"ROBCO79\0"), {}),
    ("seal_err_hdr_version", "seal", raw_image(hdr_version=2), {}),
    ("seal_err_hdr_size", "seal", raw_image(hdr_size=60), {}),
    ("seal_err_file_size", "seal", raw_image(file_size=0x200), {}),
    ("seal_err_name_17", "seal", raw_image(), {"name": "ABCDEFGHIJKLMNOPQ"}),
    ("seal_err_title_65", "seal", raw_image(), {"title": "T" * 65}),
    ("seal_err_title_ascii", "seal", raw_image(), {"title": "Café"}),
    ("seal_err_title_no_meta", "seal", raw_image("BOOT", 7, 0x8000, meta=False,
                                                 abi=0x00020000), {"title": "x"}),
    ("seal_err_meta_short", "seal", raw_image(code=0, length=0x100, file_size=0x100), {"title": "x"}),
    ("seal_err_flags", "seal", raw_image(flags=3), {}),
    ("seal_err_meta_on_boot", "seal", raw_image("BOOT", 7, 0x8000, meta=False, flags=1,
                                                abi=0x00020000), {}),
    ("seal_err_data_tape", "seal", raw_image(subtype=2), {}),
    ("seal_err_load_addr", "seal", raw_image(load_addr=0x00810000), {}),
    ("seal_err_mem_size", "seal", raw_image(mem_size=0x00400001), {}),
    ("seal_err_mem_small", "seal", raw_image(mem_size=0x100), {}),
    ("seal_err_abi", "seal", raw_image(abi=0x00020000), {}),
    ("seal_err_icon_16", "seal", raw_image(), {"icon": "icon_16.pbm"}),
    ("seal_err_icon_char", "seal", raw_image(), {"icon": "icon_badchar.pbm"}),
    ("seal_err_icon_raw_len", "seal", raw_image(), {"icon": "icon_100.bin", "icon_format": "raw"}),
    ("seal_err_icon_pbm_128", "seal", raw_image(), {"icon": "icon_raw.bin", "icon_format": "pbm"}),
    ("seal_err_icon_num_plus", "seal", raw_image(), {"icon": "icon_num_plus.pbm"}),
    ("seal_err_icon_num_minus", "seal", raw_image(), {"icon": "icon_num_minus.pbm"}),
    ("seal_err_icon_num_sep", "seal", raw_image(), {"icon": "icon_num_sep.pbm"}),
    ("seal_err_icon_num_long", "seal", raw_image(), {"icon": "icon_num_long.pbm"}),
    ("seal_err_icon_num_vt", "seal", raw_image(), {"icon": "icon_num_vt.pbm"}),
    ("seal_err_icon_num_height", "seal", raw_image(), {"icon": "icon_num_height.pbm"}),
    ("seal_holo_icon_num_zeros", "seal", raw_image(), {"icon": "icon_num_zeros.pbm"}),
    ("pack_holo_plain", "pack", PAYLOAD,
     {"magic": "ROBCO78", "subtype": "0", "load_addr": "0x00800000"}),
    ("pack_holo_full", "pack", PAYLOAD,
     {"magic": "ROBCO78", "subtype": "1", "load_addr": "0x00800000", "name": "HOLO",
      "image_version": "0.9.1", "abi_version": "1.4", "mem_size": "0x10000", "entry": "0x8",
      "title": "Holo utility", "icon": "icon_p4.pbm"}),
    ("pack_rom", "pack", PAYLOAD,
     {"magic": "ROM1", "subtype": "0", "load_addr": "0x00400000", "name": "DEITRIX",
      "image_version": "3.0.3", "title": "Deitrix test ROM", "icon": "icon_p1.pbm"}),
    ("pack_boot", "pack", PAYLOAD, {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000"}),
    ("pack_boot_entry_last", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "5", "load_addr": "0x8000", "entry": "0x3c"}),
    ("pack_holo_meta_title", "pack", PAYLOAD,
     {"magic": "ROBCO78", "subtype": "0", "load_addr": "0x00800000", "meta": "yes",
      "title": "Explicit META"}),
    ("pack_err_boot_meta", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000", "meta": "yes"}),
    ("pack_err_rom_no_meta", "pack", PAYLOAD,
     {"magic": "ROM1", "subtype": "0", "load_addr": "0x00400000", "meta": "no"}),
    ("pack_err_title_no_meta", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000", "title": "x"}),
    ("pack_err_entry_align", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000", "entry": "0x2"}),
    ("pack_err_entry_end", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000", "entry": "0x40"}),
    ("pack_err_empty", "pack", b"",
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000"}),
    ("pack_err_data_tape", "pack", PAYLOAD,
     {"magic": "ROBCO78", "subtype": "2", "load_addr": "0"}),
    ("pack_err_load_addr", "pack", PAYLOAD,
     {"magic": "ROBCO78", "subtype": "0", "load_addr": "0x00400000"}),
    ("pack_err_mem_small", "pack", SHORT_PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000", "mem_size": "0x40"}),
    ("pack_err_abi", "pack", PAYLOAD,
     {"magic": "ROM1", "subtype": "0", "load_addr": "0x00400000", "abi_version": "2.0"}),
    ("pack_err_name_17", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000", "name": "ABCDEFGHIJKLMNOPQ"}),
    ("pack_err_magic", "pack", PAYLOAD,
     {"magic": "ROM2", "subtype": "0", "load_addr": "0x00400000"}),
    ("seal_err_name_ascii", "seal", raw_image(), {"name": "PIPÉ"}),
    ("pack_err_name_ascii", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000", "name": "Ünit"}),
    ("pack_err_subtype_neg", "pack", PAYLOAD,
     {"magic": "ROBCO78", "subtype": "-1", "load_addr": "0x00800000"}),
    ("pack_err_subtype_big", "pack", PAYLOAD,
     {"magic": "ROBCO78", "subtype": "0x100000000", "load_addr": "0x00800000"}),
    ("pack_err_load_addr_neg", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "-0x8000"}),
    ("pack_err_load_addr_big", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x100008000"}),
    ("pack_err_mem_size_neg", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000", "mem_size": "-1"}),
    ("pack_err_mem_size_big", "pack", PAYLOAD,
     {"magic": "BOOT", "subtype": "7", "load_addr": "0x8000", "mem_size": "0x100000000"}),
]

KEY_ORDER = ("op", "input", "magic", "subtype", "load_addr", "name", "image_version",
             "abi_version", "mem_size", "entry", "meta", "title", "icon", "icon_format",
             "expect", "output")


def run_case(op, data, opts, icons):
    """Runs one case through tools/rimg.py. Returns (code name, output bytes or None)."""
    try:
        icon = None
        if "icon" in opts:
            icon = rimg.parse_icon(icons[opts["icon"]], opts.get("icon_format", "auto"))
        version = opts.get("image_version")
        version = None if version is None else rimg.parse_version(version)
        if op == "seal":
            out = rimg.seal(data, opts.get("name"), version, opts.get("title"), icon)
        else:
            meta = {"yes": True, "no": False}.get(opts.get("meta"))
            abi = opts.get("abi_version")
            mem = opts.get("mem_size")
            out = rimg.pack(data, opts["magic"], int(opts["subtype"], 0),
                            int(opts["load_addr"], 0), opts.get("name"),
                            version or 0, None if abi is None else rimg.parse_abi(abi),
                            None if mem is None else int(mem, 0), meta, opts.get("title"),
                            icon, int(opts.get("entry", "0"), 0))
        return "ok", out
    except rimg.RimgError as e:
        return rimg.ERR_NAMES[e.code], None


def ini_value(text):
    """Quotes a value the way ini.h reads it back unchanged."""
    if text == "" or text != text.strip() or any(c in text for c in "#;\"\\"):
        return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return text


def main(argv):
    out_dir = argv[1] if len(argv) > 1 else HERE
    os.makedirs(out_dir, exist_ok=True)
    for name, data in ICONS.items():
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(data)
    lines = ["# rimg seal and pack vectors. Written by generate.py, do not edit.",
             "# See docs/spec/IMAGE_FORMAT.md, section Test vectors.", ""]
    for name, op, data, opts in CASES:
        assert len(name) <= 24, name  # ini.h: 31 bytes per section name
        code, out = run_case(op, data, opts, ICONS)
        with open(os.path.join(out_dir, name + ".in"), "wb") as f:
            f.write(data)
        keys = dict(opts, op=op, input=name + ".in", expect=code)
        if out is not None:
            keys["output"] = name + ".out"
            with open(os.path.join(out_dir, name + ".out"), "wb") as f:
                f.write(out)
        lines.append(f"[vector {name}]")
        lines += [f"{k} = {ini_value(keys[k])}" for k in KEY_ORDER if k in keys]
        lines.append("")
    with open(os.path.join(out_dir, "vectors.ini"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
