"""Interop between our fs layer (build/host/fs_tool) and mtools.

Images made by mtools must read back through fs_tool, files written by
fs_tool must read back through mtools, and images formatted by fs_mkfs
must be accepted by mtools. When fsck.vfat or fsck_msdos exists, images
must check clean after our writes. Skips without mtools.
"""

import os
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from harness.sdimage import PART_LBA, SECTOR, _mbr, have_mtools, make_sd_image  # noqa: E402
from harness.env import minimal_env  # noqa: E402

TOOL = os.path.join(ROOT, "build", "host", "fs_tool")
ENV = minimal_env(MTOOLS_SKIP_CHECK="1")

pytestmark = pytest.mark.skipif(not have_mtools() or not shutil.which("mdir"),
                                reason="mtools not installed")

LONG = "A long file name.text"
FILES = {
    "ROBCO/MFBOOT.SYS": b"mfboot" * 100,
    "PIPBOY/ROM1.ROM": bytes(range(256)) * 300,
    "README.TXT": b"hello from mtools\n",
    "DEEP/ER/" + LONG: b"long name content",
}


@pytest.fixture(scope="module")
def fs_tool():
    subprocess.run(["make", "--no-print-directory", "build/host/fs_tool"], cwd=ROOT, check=True,
                   capture_output=True)
    return TOOL


def tool(*args, check=True):
    r = subprocess.run([TOOL, *map(str, args)], capture_output=True)
    if check and r.returncode != 0:
        raise AssertionError(f"fs_tool {args} failed: {r.stderr.decode()}")
    return r


def mt(*args):
    return subprocess.run(list(map(str, args)), env=ENV, check=True, capture_output=True).stdout


def fsck(path):
    """Checks the image read-only with the system FAT checker, if any."""
    for cmd in (["fsck.vfat", "-n"], ["fsck_msdos", "-n"]):
        if shutil.which(cmd[0]):
            r = subprocess.run(cmd + [str(path)], capture_output=True)
            assert r.returncode == 0, r.stdout.decode() + r.stderr.decode()
            return True
    return False


def superfloppy(path, size_mib, fat32=False, cluster1=False):
    """mtools image with no partition table."""
    with open(path, "wb") as f:
        f.truncate(size_mib << 20)
    args = ["mformat", "-i", path, "-T", (size_mib << 20) // SECTOR, "-h", 64, "-s", 32]
    if fat32:
        args.append("-F")
    if cluster1:
        args += ["-c", 1]
    mt(*args, "::")
    return str(path)


def copy_in(img, files):
    made = set()
    for name, data in files.items():
        parts = name.split("/")
        for i in range(1, len(parts)):
            d = "/".join(parts[:i])
            if d not in made:
                mt("mmd", "-i", img, "::" + d)
                made.add(d)
        src = os.path.join(os.path.dirname(img.split("@@")[0]), "src.bin")
        with open(src, "wb") as f:
            f.write(data)
        mt("mcopy", "-i", img, src, "::" + name)


def extract_partition(img, out):
    with open(img, "rb") as f:
        f.seek(PART_LBA * SECTOR)
        data = f.read()
    with open(out, "wb") as f:
        f.write(data)
    return out


# (id, builder) -> (fs_tool args before IMAGE, mtools image spec, expected FAT type, fsck image)
def build(kind, tmp_path):
    img = str(tmp_path / f"{kind}.img")
    if kind == "fat12":
        superfloppy(img, 1)
        return [], img, 12
    if kind == "fat16":
        superfloppy(img, 16, cluster1=True)
        return [], img, 16
    if kind == "fat32":
        superfloppy(img, 64, fat32=True)
        return [], img, 32
    if kind == "mbr16":
        make_sd_image(img, 16)
        return ["-p", 0], f"{img}@@{PART_LBA * SECTOR}", 16
    if kind == "mbr32":
        make_sd_image(img, 64)
        return ["-p", 0], f"{img}@@{PART_LBA * SECTOR}", 32
    raise ValueError(kind)


KINDS = ["fat12", "fat16", "fat32", "mbr16", "mbr32"]


def check_clean(kind, img, tmp_path):
    path = img.split("@@")[0]
    if kind.startswith("mbr"):
        path = extract_partition(path, str(tmp_path / "part.img"))
    fsck(path)


@pytest.mark.parametrize("kind", KINDS)
def test_reads_mtools_images(fs_tool, tmp_path, kind):
    pre, img, fat = build(kind, tmp_path)
    copy_in(img, FILES)
    path = img.split("@@")[0]
    assert tool(*pre, path, "df").stdout.decode().startswith(f"FAT{fat} ")
    for name, data in FILES.items():
        assert tool(*pre, path, "cat", "/" + name).stdout == data
    root = tool(*pre, path, "ls").stdout.decode().splitlines()
    assert {"d ROBCO", "d PIPBOY", "d DEEP", f"f {len(FILES['README.TXT'])} README.TXT"} <= set(root)
    assert tool(*pre, path, "ls", "/DEEP/ER").stdout.decode() == f"f 17 {LONG}\n"
    r = tool(*pre, path, "cat", "/NOPE.TXT", check=False)
    assert r.returncode == 1 and b"no such file" in r.stderr


@pytest.mark.parametrize("kind", KINDS)
def test_mtools_reads_our_writes(fs_tool, tmp_path, kind):
    pre, img, _ = build(kind, tmp_path)
    copy_in(img, {"OLD/GONE.TXT": b"gone", "OLD/KEEP.TXT": b"keep"})
    path = img.split("@@")[0]
    data = os.urandom(70000)
    src = tmp_path / "data.bin"
    src.write_bytes(data)
    tool(*pre, path, "mkdir", "/SAVES")
    tool(*pre, path, "mkdir", "/SAVES/Long Directory Name")
    tool(*pre, path, "put", src, "/SAVES/GAME1.SAV")
    tool(*pre, path, "put", src, "/SAVES/Long Directory Name/" + LONG)
    tool(*pre, path, "mv", "/OLD/KEEP.TXT", "/SAVES/KEEP.TXT")
    tool(*pre, path, "rm", "/OLD/GONE.TXT")
    tool(*pre, path, "rm", "/OLD")

    assert mt("mtype", "-i", img, "::/SAVES/GAME1.SAV") == data
    assert mt("mtype", "-i", img, f"::/SAVES/Long Directory Name/{LONG}") == data
    assert mt("mtype", "-i", img, "::/SAVES/KEEP.TXT") == b"keep"
    listing = mt("mdir", "-i", img, "-b", "::/SAVES").decode()
    assert "GAME1.SAV" in listing and "KEEP.TXT" in listing and "Long Directory Name" in listing
    assert "OLD" not in mt("mdir", "-i", img, "-b", "::/").decode()
    check_clean(kind, img, tmp_path)


@pytest.mark.parametrize("fmt,size_mib,fat", [("any", 1, 12), ("fat", 16, 16), ("fat32", 64, 32)])
def test_mtools_accepts_our_mkfs(fs_tool, tmp_path, fmt, size_mib, fat):
    img = tmp_path / "ours.img"
    with open(img, "wb") as f:
        f.truncate(size_mib << 20)
    tool(img, "mkfs", fmt)
    assert tool(img, "df").stdout.decode().startswith(f"FAT{fat} ")
    info = mt("minfo", "-i", img, "::").decode()
    assert "sector size: 512" in info
    mt("mmd", "-i", img, "::/FROMMT")
    src = tmp_path / "x.bin"
    src.write_bytes(b"from mtools")
    mt("mcopy", "-i", img, src, "::/FROMMT/X.BIN")
    assert tool(img, "cat", "/FROMMT/X.BIN").stdout == b"from mtools"
    tool(img, "put", src, "/OURS.TXT")
    assert mt("mtype", "-i", img, "::/OURS.TXT") == b"from mtools"
    fsck(img)


def test_mkfs_in_partition(fs_tool, tmp_path):
    img = tmp_path / "mbr.img"
    sectors = (8 << 20) // SECTOR - PART_LBA
    with open(img, "wb") as f:
        f.write(_mbr(0x0E, PART_LBA, sectors))
        f.truncate(8 << 20)
    out = tool(img, "parts").stdout.decode()
    assert out == f"0 slot 0 type 0x0e start {PART_LBA} count {sectors}\n"
    tool("-p", 0, img, "mkfs")
    tool("-p", 0, img, "mkdir", "/D")
    src = tmp_path / "x.bin"
    src.write_bytes(b"in partition")
    tool("-p", 0, img, "put", src, "/D/F.TXT")
    spec = f"{img}@@{PART_LBA * SECTOR}"
    assert mt("mtype", "-i", spec, "::/D/F.TXT") == b"in partition"
    with open(img, "rb") as f:
        assert f.read(SECTOR)[446:] == _mbr(0x0E, PART_LBA, sectors)[446:]
    check_clean("mbr", spec, tmp_path)


def test_superfloppy_parts(fs_tool, tmp_path):
    img = superfloppy(tmp_path / "sf.img", 1)
    out = tool(img, "parts").stdout.decode()
    assert out == f"0 slot 0 type 0x00 start 0 count {(1 << 20) // SECTOR} superfloppy\n"
    assert tool("-p", 0, img, "df").stdout.decode().startswith("FAT12 ")


def test_usage_errors(fs_tool, tmp_path):
    assert tool(check=False).returncode == 2
    img = superfloppy(tmp_path / "u.img", 1)
    assert tool(img, "bogus", check=False).returncode == 2
    assert tool("-p", 3, img, "ls", check=False).returncode == 1
    blank = tmp_path / "blank.img"
    blank.write_bytes(bytes(1 << 16))
    r = tool(blank, "ls", check=False)
    assert r.returncode == 1 and b"no FAT file system" in r.stderr
