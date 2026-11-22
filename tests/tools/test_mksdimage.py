"""Tests for tools/mksdimage.sh: layout, contents, sizes and refusals."""

import os
import pathlib
import shutil
import struct
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MKSD = ROOT / "tools/mksdimage.sh"
PART_OFF = 2048 * 512
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")

pytestmark = pytest.mark.skipif(
    not all(shutil.which(t) for t in ("mformat", "mcopy", "mdir", "mtype")),
    reason="mtools not installed")


def run(*args):
    return subprocess.run(["bash", str(MKSD), *map(str, args)], capture_output=True, text=True)


def mt(*args):
    return subprocess.run(list(args), check=True, env=ENV, capture_output=True).stdout


@pytest.fixture
def tree(tmp_path):
    d = tmp_path / "tree"
    files = {
        "config.txt": b"kernel=retros.img\n",
        "ROBCO/MFBOOT.SYS": os.urandom(70000),
        "ROBCO/SUB/DEEP.TXT": b"deep\n",
        "Long_Name_File.data": os.urandom(1000),
        ".hidden": b"h",
    }
    for name, data in files.items():
        p = d / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    (d / "EMPTY").mkdir()
    return d, files


def is_pow2(n):
    return n > 0 and n & (n - 1) == 0


@pytest.mark.parametrize("mib,want,ptype", [(1, 4, 0x0E), (5, 8, 0x0E), (16, 16, 0x0E),
                                            (33, 64, 0x0C), (100, 128, 0x0C)])
def test_layout_and_contents(tree, tmp_path, mib, want, ptype):
    d, files = tree
    out = tmp_path / "sd.img"
    r = run(out, mib, d)
    assert r.returncode == 0, r.stderr
    size = out.stat().st_size
    assert size == want << 20 and is_pow2(size)
    mbr = out.read_bytes()[:512]
    assert mbr[510:] == b"\x55\xaa"
    assert mbr[446 + 4] == ptype
    lba, count = struct.unpack_from("<II", mbr, 446 + 8)
    assert (lba, count) == (2048, size // 512 - 2048)
    assert not any(mbr[446 + 16:510])
    img = f"{out}@@{PART_OFF}"
    for name, data in files.items():
        assert mt("mtype", "-i", img, "::/" + name) == data, name
    listing = mt("mdir", "-/", "-b", "-i", img, "::").decode()
    assert "::/EMPTY/" in listing
    info = mt("minfo", "-i", img, "::").decode()
    assert ("FAT32" in info) == (ptype == 0x0C)


def test_overwrites_existing_file(tree, tmp_path):
    d, _ = tree
    out = tmp_path / "sd.img"
    out.write_bytes(b"old")
    assert run(out, 4, d).returncode == 0
    assert out.stat().st_size == 4 << 20
    assert not list(tmp_path.glob("sd.img.tmp*"))


def test_refuses_device(tree, tmp_path):
    d, _ = tree
    r = run("/dev/null", 4, d)
    assert r.returncode == 1 and "device" in r.stderr
    link = tmp_path / "dev.img"
    link.symlink_to("/dev/null")
    r = run(link, 4, d)
    assert r.returncode == 1 and "device" in r.stderr


def test_refuses_missing_dir(tmp_path):
    out = tmp_path / "sd.img"
    r = run(out, 4, tmp_path / "missing")
    assert r.returncode == 1 and "not a directory" in r.stderr
    assert not out.exists()


def test_refuses_bad_arguments(tree, tmp_path):
    d, _ = tree
    assert run(tmp_path / "a.img", "x", d).returncode == 1
    assert run(tmp_path / "a.img", 0, d).returncode == 1
    assert run(tmp_path, 4, d).returncode == 1
    assert run(tmp_path / "a.img", 4).returncode == 2
    assert not (tmp_path / "a.img").exists()


def test_too_small_fails_cleanly(tmp_path):
    d = tmp_path / "big"
    d.mkdir()
    (d / "BIG.BIN").write_bytes(os.urandom(5 << 20))
    out = tmp_path / "sd.img"
    r = run(out, 4, d)
    assert r.returncode == 1, r.stdout
    assert not out.exists()
    assert not list(tmp_path.glob("sd.img.tmp*"))


def test_make_sdimage(tree, tmp_path):
    d, files = tree
    build = tmp_path / "build"
    r = subprocess.run(["make", "-s", "-C", str(ROOT / "examples/hello"), f"BUILD={build}",
                        f"SDIMAGE_DIR={d}", "SDIMAGE_MIB=8", "sdimage"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    out = build / "sd.img"
    assert out.stat().st_size == 8 << 20
    assert mt("mtype", "-i", f"{out}@@{PART_OFF}", "::/config.txt") == files["config.txt"]
    r = subprocess.run(["make", "-s", "-C", str(ROOT / "examples/hello"), f"BUILD={build}",
                        "sdimage"], capture_output=True, text=True)
    assert r.returncode != 0 and "SDIMAGE_DIR" in r.stderr
