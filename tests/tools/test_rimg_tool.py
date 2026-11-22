"""Unit tests for tools/rimg.py on synthetic images built in memory."""

import os
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
    ("size_meta", lambda: sealed(flags=1, size=0x100), [], rimg.E_SIZE),
    ("bounds_subtype", lambda: sealed(subtype=1), [], rimg.E_BOUNDS),
    ("bounds_data_holo", lambda: sealed(magic=b"ROBCO78\0", subtype=2, abi=1 << 16,
                                        load_addr=0x800000), [], rimg.E_BOUNDS),
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
