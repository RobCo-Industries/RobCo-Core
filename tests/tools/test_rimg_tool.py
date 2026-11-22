"""Unit tests for tools/rimg.py on synthetic images built in memory."""

import os
import re
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "rimg.py")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import rimg  # noqa: E402

BRANCH = 0xEA000016  # b 0x60


def make_image(magic=b"BOOT\0\0\0\0", load_addr=0x8000, size=0x100, mem_size=None,
               subtype=7, abi=2 << 16, flags=0, hdr_version=1, hdr_size=64,
               reserved=0, file_size=None, name=b"test"):
    data = bytearray(size)
    struct.pack_into("<I", data, 0, BRANCH)
    struct.pack_into(rimg.HDR_FMT, data, 0x20, magic, hdr_version, hdr_size, flags,
                     load_addr, size if file_size is None else file_size,
                     size + 0x1000 if mem_size is None else mem_size, 0, abi, 0x10203,
                     subtype, name, reserved)
    for i in range(0x60, size):
        data[i] = i & 0xFF
    return data


def sealed(**kw):
    data = make_image(**kw)
    struct.pack_into("<I", data, 0x3C, rimg.image_crc(data, len(data)))
    return bytes(data)


def run(*args):
    return subprocess.run([sys.executable, TOOL, *map(str, args)],
                          capture_output=True, text=True)


def write(tmp_path, data, name="img"):
    p = tmp_path / name
    p.write_bytes(data)
    return p


def test_crc_check_value():
    assert zlib.crc32(b"123456789") == 0xCBF43926


def test_crc_ignores_its_own_field():
    data = sealed()
    patched = bytearray(data)
    struct.pack_into("<I", patched, 0x3C, 0x12345678)
    assert rimg.image_crc(patched, len(patched)) == rimg.image_crc(data, len(data))


def test_seal_in_place_then_verify(tmp_path):
    p = write(tmp_path, make_image())
    assert run("seal", p).returncode == 0
    r = run("verify", p)
    assert (r.returncode, r.stdout.strip()) == (0, "ok")


def test_seal_fixes_zero_file_size(tmp_path):
    p = write(tmp_path, make_image(file_size=0))
    assert run("seal", p).returncode == 0
    h = rimg.parse_header(p.read_bytes())
    assert h["file_size"] == 0x100
    assert run("verify", p).returncode == 0


def test_seal_rejects_other_file_size(tmp_path):
    p = write(tmp_path, make_image(file_size=0x200))
    before = p.read_bytes()
    r = run("seal", p)
    assert r.returncode == 1
    assert "size" in r.stderr
    assert p.read_bytes() == before


def test_seal_output_and_fields(tmp_path):
    p = write(tmp_path, make_image())
    out = tmp_path / "out.img"
    assert run("seal", p, "-o", out, "--name", "chain", "--image-version", "1.2.3").returncode == 0
    h = rimg.parse_header(out.read_bytes())
    assert h["name"].rstrip(b"\0") == b"chain"
    assert h["image_version"] == 0x010203
    assert h["crc32"] == rimg.image_crc(out.read_bytes(), 0x100)
    assert rimg.parse_header(p.read_bytes())["crc32"] == 0


def test_seal_rejects_non_branch(tmp_path):
    data = make_image()
    struct.pack_into("<I", data, 0, 0xE1A00000)  # mov r0, r0
    assert run("seal", write(tmp_path, data)).returncode == 1


def test_seal_rejects_bounds(tmp_path):
    assert run("seal", write(tmp_path, make_image(load_addr=0x9000))).returncode == 1


def test_info(tmp_path):
    p = write(tmp_path, sealed())
    r = run("info", p)
    assert r.returncode == 0
    kv = dict(line.split("=", 1) for line in r.stdout.splitlines())
    for key in rimg.FIELDS:
        assert key in kv
    assert kv["magic"] == "BOOT"
    assert kv["load_addr"] == "0x8000"
    assert kv["file_size"] == "0x100"
    assert kv["subtype"] == "0x7"
    assert kv["name"] == "test"
    assert kv["crc_ok"] == "1"
    bad = bytearray(p.read_bytes())
    bad[0x80] ^= 1
    r = run("info", write(tmp_path, bad, "bad"))
    assert r.returncode == 0
    assert "crc_ok=0" in r.stdout.splitlines()


def corrupt_payload():
    data = bytearray(sealed())
    data[0x90] ^= 0x40
    return bytes(data)


def short_file():
    return sealed()[:0x40]


# (id, image bytes, extra verify args, expected code). One case per error code.
CASES = [
    ("ok", lambda: sealed(), [], rimg.OK),
    ("io", short_file, [], rimg.E_IO),
    ("magic", lambda: sealed(magic=b"BOGUS\0\0\0"), [], rimg.E_MAGIC),
    ("magic_not_expected", lambda: sealed(), ["--magic", "ROM1"], rimg.E_MAGIC),
    ("version", lambda: sealed(hdr_version=2), [], rimg.E_VERSION),
    ("hdr_size", lambda: sealed(hdr_size=60), [], rimg.E_VERSION),
    ("flags", lambda: sealed(flags=2), [], rimg.E_VERSION),
    ("reserved", lambda: sealed(reserved=1), [], rimg.E_VERSION),
    ("size_fat", lambda: sealed(), ["--fat-size", "0x200"], rimg.E_SIZE),
    ("size_header", lambda: sealed(file_size=0x80), [], rimg.E_SIZE),
    ("size_meta", lambda: sealed(magic=b"ROM1\0\0\0\0", flags=1, size=0x100), [],
     rimg.E_SIZE),
    ("version_meta_boot", lambda: sealed(flags=1), [], rimg.E_VERSION),
    ("version_meta_rom", lambda: sealed(magic=b"ROM1\0\0\0\0", subtype=0, abi=1 << 16,
                                        load_addr=0x400000), [], rimg.E_VERSION),
    ("bounds_subtype", lambda: sealed(subtype=1), [], rimg.E_BOUNDS),
    ("bounds_data_holo", lambda: sealed(magic=b"ROBCO78\0", subtype=2, abi=1 << 16, flags=1,
                                        size=0x200, load_addr=0x800000), [], rimg.E_BOUNDS),
    ("bounds_load_addr", lambda: sealed(load_addr=0x9000), [], rimg.E_BOUNDS),
    ("bounds_mem_small", lambda: sealed(mem_size=0x80), [], rimg.E_BOUNDS),
    ("bounds_slot_end", lambda: sealed(mem_size=0xF8001), [], rimg.E_BOUNDS),
    ("bounds_slot_arg", lambda: sealed(), ["--slot-base", "0x200000",
                                           "--slot-size", "0x200000"], rimg.E_BOUNDS),
    ("abi", lambda: sealed(abi=1 << 16), [], rimg.E_ABI),
    ("abi_arg", lambda: sealed(), ["--abi-major", "3"], rimg.E_ABI),
    ("crc", corrupt_payload, [], rimg.E_CRC),
]


@pytest.mark.parametrize("make,extra,code", [c[1:] for c in CASES], ids=[c[0] for c in CASES])
def test_verify_codes(tmp_path, make, extra, code):
    r = run("verify", write(tmp_path, make()), *extra)
    assert r.returncode == code, r.stderr
    assert r.stdout.strip() == rimg.ERR_NAMES[code]


def test_verify_nofile(tmp_path):
    r = run("verify", tmp_path / "missing.img")
    assert (r.returncode, r.stdout.strip()) == (rimg.E_NOFILE, "nofile")


def test_verify_order_magic_before_crc(tmp_path):
    data = bytearray(sealed(magic=b"XXXX\0\0\0\0", hdr_version=9))
    data[0x90] ^= 1
    assert run("verify", write(tmp_path, data)).returncode == rimg.E_MAGIC


def test_verify_order_bounds_before_abi(tmp_path):
    data = sealed(load_addr=0x9000, abi=5 << 16)
    assert run("verify", write(tmp_path, data)).returncode == rimg.E_BOUNDS


def test_verify_agent_slot(tmp_path):
    data = sealed(load_addr=0x200000, subtype=4)
    r = run("verify", write(tmp_path, data), "--magic", "BOOT", "--abi-major", "2",
            "--slot-base", "0x200000", "--slot-size", "0x200000")
    assert r.returncode == 0, r.stdout


def test_usage_error_exit_code(tmp_path):
    assert run("verify").returncode == rimg.EX_USAGE
    assert run("verify", write(tmp_path, sealed()), "--slot-base", "0x8000").returncode \
        == rimg.EX_USAGE


def truncated(keep=0xC0, size=0x100):
    """Header says `size` bytes, only `keep` exist, CRC over the prefix."""
    data = make_image(size=size)[:keep]
    struct.pack_into("<I", data, 0x3C, rimg.image_crc(data, size))
    return bytes(data)


@pytest.mark.parametrize("extra", [[], ["--fat-size", "0x100"], ["--fat-size", "0xc0"]],
                         ids=["no_fat", "fat_header", "fat_read"])
def test_verify_truncated(tmp_path, extra):
    r = run("verify", write(tmp_path, truncated()), *extra)
    assert (r.returncode, r.stdout.strip()) == (rimg.E_SIZE, "size"), r.stderr


def test_verify_zero_means_default(tmp_path):
    """0 selects the default, like the zero fields of rimg_expect_t."""
    r = run("verify", write(tmp_path, sealed()), "--fat-size", "0", "--abi-major", "0",
            "--slot-base", "0", "--slot-size", "0")
    assert r.returncode == 0, r.stderr


# ---- META, pack and the layout shared with include/robco/rimg.h ----

ICON_PBM = os.path.join(ROOT, "tests", "host", "icon.pbm")
RIMG_H = os.path.join(ROOT, "include", "robco", "rimg.h")


def frame_icon():
    out = bytearray(128)
    for y in range(32):
        for x in range(32):
            if x in (0, 31) or y in (0, 31) or x == y:
                out[y * 4 + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


def rom_image(flags=1, size=0x200):
    """An unsealed ROM1 image with a zeroed META block, as crt0.S emits it."""
    data = make_image(magic=b"ROM1\0\0\0\0", load_addr=0x400000, subtype=0, abi=1 << 16,
                      flags=flags, size=size)
    data[0x60:0x120] = bytes(0xC0)
    return data


def test_layout_matches_rimg_h():
    with open(RIMG_H) as f:
        text = f.read()
    defines = dict(re.findall(r"^#define\s+(RIMG_\w+)\s+(.+?)\s*$", text, re.M))

    def num(name):
        return eval(defines[name].replace("(", " (").strip())  # "(1 << 0)" and hex

    assert num("RIMG_HDR_OFFSET") == rimg.HDR_OFFSET
    assert num("RIMG_META_OFFSET") == rimg.META_OFFSET
    assert num("RIMG_HDR_SIZE") == rimg.HDR_SIZE == struct.calcsize(rimg.HDR_FMT)
    assert num("RIMG_META_SIZE") == rimg.META_SIZE == rimg.TITLE_SIZE + rimg.ICON_SIZE
    assert num("RIMG_FLAG_META") == rimg.FLAG_META
    for kind, raw in rimg.MAGICS.items():
        c = defines[f"RIMG_MAGIC_{kind}"].strip('"').encode().decode("unicode_escape")
        assert c.encode("latin-1") == raw
    # The header struct: the same fields, sizes and order as HDR_FMT.
    body = re.search(r"typedef struct __attribute__\(\(packed\)\) rimg_header \{(.*?)\}",
                     text, re.S).group(1)
    sizes = {"uint16_t": "H", "uint32_t": "I"}
    fmt, names = "<", []
    for ctype, name, arr in re.findall(r"(\w+)\s+(\w+)(?:\[(\d+)\])?;", body):
        fmt += f"{arr}s" if arr else sizes[ctype]
        names.append(name)
    assert fmt == rimg.HDR_FMT
    assert tuple(names) == rimg.FIELDS
    meta = re.search(r"rimg_meta \{(.*?)\}", text, re.S).group(1)
    assert re.findall(r"\[(\d+)\]", meta) == [str(rimg.TITLE_SIZE), str(rimg.ICON_SIZE)]
    # The ROM1 and holotape subtypes pack uses.
    assert num("RIMG_ROM1_SYSTEM") == 0 and num("RIMG_HOLO_UTILITY") == 1


def test_parse_icon_formats(tmp_path):
    want = frame_icon()
    with open(ICON_PBM, "rb") as f:
        assert rimg.parse_icon(f.read()) == want
    p4 = b"P4\n# comment\n32 32\n" + want
    assert rimg.parse_icon(p4) == want
    assert rimg.parse_icon(want) == want
    for bad in (b"P1\n16 16\n" + b"0 " * 256, b"P4\n32 32\n" + want[:10], b"\0" * 127,
                b"P1\n32\n"):
        with pytest.raises(rimg.RimgError):
            rimg.parse_icon(bad)


def test_seal_sets_title_and_icon(tmp_path):
    p = write(tmp_path, rom_image())
    r = run("seal", p, "--title", "Deitrix", "--icon", ICON_PBM)
    assert r.returncode == 0, r.stderr
    data = p.read_bytes()
    assert data[0x60:0x60 + 64] == b"Deitrix".ljust(64, b"\0")
    assert data[0xA0:0x120] == frame_icon()
    assert run("verify", p).stdout.strip() == "ok"
    info = run("info", p).stdout.splitlines()
    assert "title=Deitrix" in info and "icon=set" in info and info[-1] == "crc_ok=1"


def test_seal_keeps_meta_without_options(tmp_path):
    data = rom_image()
    data[0x60:0x63] = b"Old"
    p = write(tmp_path, data)
    assert run("seal", p).returncode == 0
    lines = run("info", p).stdout.splitlines()
    assert "title=Old" in lines and "icon=blank" in lines


@pytest.mark.parametrize("args, msg", [
    (["--title", "x" * 65], "longer than 64"),
    (["--title", "café"], "not ASCII"),
])
def test_seal_rejects_bad_title(tmp_path, args, msg):
    p = write(tmp_path, rom_image())
    before = p.read_bytes()
    r = run("seal", p, *args)
    assert r.returncode == 1 and msg in r.stderr
    assert p.read_bytes() == before


def test_seal_meta_options_need_the_flag(tmp_path):
    p = write(tmp_path, make_image())
    r = run("seal", p, "--title", "x")
    assert r.returncode == 1 and "META flag" in r.stderr
    raw = write(tmp_path, bytes(128), "icon.bin")
    r = run("seal", p, "--icon", raw)
    assert r.returncode == 1 and "META flag" in r.stderr


def test_seal_rejects_bad_icon_file(tmp_path):
    p = write(tmp_path, rom_image())
    bad = write(tmp_path, b"\1" * 100, "icon.bin")
    r = run("seal", p, "--icon", bad)
    assert r.returncode == 1 and "100 bytes" in r.stderr


def test_pack_rom_round_trip(tmp_path):
    payload = write(tmp_path, b"\x01\x02\x03\x04" * 10, "payload.bin")
    out = tmp_path / "rom.img"
    r = run("pack", payload, "-o", out, "--magic", "ROM1", "--subtype", "0",
            "--load-addr", "0x400000", "--name", "DEITRIX", "--image-version", "3.0.3",
            "--title", "Deitrix test ROM", "--icon", ICON_PBM, "--mem-size", "0x8000")
    assert r.returncode == 0, r.stderr
    data = out.read_bytes()
    assert len(data) == 0x120 + 40
    assert struct.unpack_from("<I", data, 0)[0] == 0xEA000046  # b 0x120
    h = rimg.validate(data, magic="ROM1")
    assert (h["flags"], h["load_addr"], h["file_size"], h["mem_size"]) == \
        (1, 0x400000, len(data), 0x8000)
    assert (h["abi_version"], h["image_version"], h["subtype"]) == (1 << 16, 0x30003, 0)
    assert h["name"].rstrip(b"\0") == b"DEITRIX"
    assert data[0xA0:0x120] == frame_icon()
    assert data[0x120:] == b"\x01\x02\x03\x04" * 10
    assert run("verify", out, "--magic", "ROM1").stdout.strip() == "ok"


def test_pack_defaults_follow_the_magic(tmp_path):
    payload = write(tmp_path, b"abcd", "payload.bin")
    holo, boot = tmp_path / "holo.img", tmp_path / "boot.img"
    assert run("pack", payload, "-o", holo, "--magic", "ROBCO78", "--subtype", "1",
               "--load-addr", "0x800000").returncode == 0
    assert run("pack", payload, "-o", boot, "--magic", "BOOT", "--subtype", "7",
               "--load-addr", "0x8000", "--abi-version", "2.5", "--entry", "0x0").returncode == 0
    lines = run("info", holo).stdout.splitlines()
    assert "flags=0x1" in lines and "title=" in lines and "icon=blank" in lines
    h = rimg.validate(boot.read_bytes())
    assert (h["flags"], h["abi_version"], h["file_size"]) == (0, (2 << 16) | 5, 0x64)
    assert struct.unpack_from("<I", boot.read_bytes(), 0)[0] == 0xEA000016  # b 0x60
    assert "title=" not in "\n".join(run("info", boot).stdout.splitlines())


def test_pack_entry_offset(tmp_path):
    payload = write(tmp_path, bytes(64), "payload.bin")
    out = tmp_path / "boot.img"
    assert run("pack", payload, "-o", out, "--magic", "BOOT", "--subtype", "7",
               "--load-addr", "0x8000", "--entry", "0x20").returncode == 0
    assert struct.unpack_from("<I", out.read_bytes(), 0)[0] == 0xEA00001E  # b 0x80


@pytest.mark.parametrize("args, msg", [
    (["--magic", "BOOT", "--subtype", "7", "--load-addr", "0x8000", "--title", "x"], "META"),
    (["--magic", "BOOT", "--subtype", "7", "--load-addr", "0x8000", "--meta"], "META flag"),
    (["--magic", "ROM1", "--subtype", "0", "--load-addr", "0x400000", "--no-meta"], "META flag"),
    (["--magic", "ROM1", "--subtype", "0", "--load-addr", "0x500000"], "bounds"),
])
def test_pack_refuses_bad_images(tmp_path, args, msg):
    payload = write(tmp_path, b"abcd", "payload.bin")
    out = tmp_path / "out.img"
    r = run("pack", payload, "-o", out, *args)
    assert r.returncode == 1 and msg in r.stderr, r.stderr
    assert not out.exists()


@pytest.mark.parametrize("entry", ["-4", "2", "0x40", "0x44", "0x1000"])
def test_pack_rejects_bad_entry(tmp_path, entry):
    payload = write(tmp_path, bytes(64), "payload.bin")
    out = tmp_path / "out.img"
    r = run("pack", payload, "-o", out, "--magic", "BOOT", "--subtype", "7",
            "--load-addr", "0x8000", "--entry", entry)
    assert r.returncode == rimg.EX_USAGE and "entry offset" in r.stderr, r.stderr
    assert not out.exists()
    with pytest.raises(rimg.RimgError):
        rimg.pack(bytes(64), "BOOT", 7, 0x8000, entry=int(entry, 0))


@pytest.mark.parametrize("entry, word", [(0, 0xEA000016), (0x3C, 0xEA000025)])
def test_pack_entry_bounds_that_work(tmp_path, entry, word):
    data = rimg.pack(bytes(64), "BOOT", 7, 0x8000, entry=entry)
    assert struct.unpack_from("<I", data, 0)[0] == word  # b 0x60 + entry


def test_branch_range():
    assert rimg.branch_to(8) == 0xEA000000
    assert rimg.branch_to(0) == 0xEAFFFFFE
    assert rimg.branch_to(8 + 4 * ((1 << 23) - 1)) == 0xEA7FFFFF
    assert rimg.branch_to(8 - 4 * (1 << 23)) == 0xEA800000
    for bad in (8 + 4 * (1 << 23), 8 - 4 * (1 << 23) - 4, 6):
        with pytest.raises(rimg.RimgError):
            rimg.branch_to(bad)


def test_pack_usage(tmp_path):
    payload = write(tmp_path, b"abcd", "payload.bin")
    assert run("pack", payload, "--magic", "ROM1").returncode == rimg.EX_USAGE
    assert run("pack", payload, "-o", tmp_path / "x", "--magic", "ROM1", "--subtype", "0",
               "--load-addr", "0x400000", "--abi-version", "1").returncode == rimg.EX_USAGE
