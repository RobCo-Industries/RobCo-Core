"""tools/nightly/pack_card.py: the firmware check and the packed files."""
import hashlib
import os
import re
import shutil
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PACK = os.path.join(ROOT, "tools", "nightly", "pack_card.py")
sys.path.insert(0, os.path.join(ROOT, "tools", "nightly"))
import pack_card  # noqa: E402


def test_firmware_names_come_from_the_fetch_script():
    names = pack_card.firmware_names()
    assert names == ("bootcode.bin", "start.elf", "fixup.dat")
    with open(os.path.join(ROOT, "tools", "fetch_firmware.sh"), encoding="utf-8") as f:
        hashed = re.findall(r"^\s+(\S+)\)\s+echo [0-9a-f]{64} ;;$", f.read(), re.M)
    assert tuple(hashed) == names


def test_a_script_without_one_files_line_fails(tmp_path):
    script = tmp_path / "fetch.sh"
    script.write_text("FILES=(a)\nFILES=(b)\n")
    with pytest.raises(pack_card.CardError):
        pack_card.firmware_names(str(script))


def test_firmware_names_in_any_folder_and_case():
    files = [("p1:/RETROS.IMG", ["RETROS.IMG"]), ("p1:/BOOTCODE.BIN", ["BOOTCODE.BIN"]),
             ("p1:/boot/start.elf", ["START.ELF", "start.elf"]),
             ("p1:/fixup.dat.txt", ["FIXUP~1.TXT", "fixup.dat.txt"]),
             ("p1:/x", ["FIXUP.DAT", "x"])]
    assert pack_card.firmware_on(files) == ["p1:/BOOTCODE.BIN", "p1:/boot/start.elf", "p1:/x"]


needs_mtools = pytest.mark.skipif(not shutil.which("mformat"), reason="mtools is not installed")


MTOOLS_ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")


def make_card(tmp_path, names, size="64", attrib=()):
    """A card from tools/mksdimage.sh, as the card builders make it. attrib
    holds (flags, path) pairs for mattrib, such as ("+h", "start.elf")."""
    stage = tmp_path / "stage"
    for name in names:
        (stage / name).parent.mkdir(parents=True, exist_ok=True)
        (stage / name).write_bytes(name.encode())
    card = tmp_path / "sd.img"
    subprocess.run([os.path.join(ROOT, "tools", "mksdimage.sh"), str(card), size, str(stage)],
                   check=True, capture_output=True)
    for flags, path in attrib:
        subprocess.run(["mattrib", "-i", f"{card}@@1M", flags, f"::/{path}"], check=True,
                       capture_output=True, env=MTOOLS_ENV)
    return card


def pack(tmp_path, card, *extra):
    versions = tmp_path / "VERSIONS.txt"
    versions.write_text("| table |\n")
    return subprocess.run([sys.executable, "-I", PACK, "--card", str(card), "--name",
                           "robco-terminal.img", "--versions", str(versions),
                           "--out", str(tmp_path / "out"), *extra],
                          capture_output=True, text=True)


@needs_mtools
def test_a_clean_card_is_packed(tmp_path):
    card = make_card(tmp_path, ["RETROS.IMG", "ROBCO/UOS.SYS"])
    kernel = tmp_path / "pipos.img"
    kernel.write_bytes(b"kernel")
    proc = pack(tmp_path, card, "--extra", str(kernel))
    assert proc.returncode == 0, proc.stdout + proc.stderr
    out = tmp_path / "out"
    assert sorted(os.listdir(out)) == ["SHA256SUMS", "VERSIONS.txt", "pipos.img",
                                       "robco-terminal.img"]
    sums = (out / "SHA256SUMS").read_text().splitlines()
    assert sums == [f"{hashlib.sha256(card.read_bytes()).hexdigest()}  robco-terminal.img",
                    f"{hashlib.sha256(b'kernel').hexdigest()}  pipos.img"]


@needs_mtools
def test_firmware_on_the_card_fails(tmp_path):
    card = make_card(tmp_path, ["RETROS.IMG", "start.elf"])
    proc = pack(tmp_path, card)
    assert proc.returncode == 1
    assert "Pi firmware on" in proc.stdout and "start.elf" in proc.stdout
    assert not (tmp_path / "out").exists()


def test_an_unreadable_card_fails(tmp_path):
    card = tmp_path / "sd.img"
    card.write_bytes(b"\0" * 4096)
    proc = pack(tmp_path, card)
    assert proc.returncode == 1
    assert "::error::pack_card:" in proc.stdout


FIRMWARE_CARDS = {
    "hidden": (["RETROS.IMG", "start.elf"], (("+h", "start.elf"),)),
    "system": (["RETROS.IMG", "bootcode.bin"], (("+s", "bootcode.bin"),)),
    "hidden-system-read-only": (["fixup.dat"], (("+h", "fixup.dat"), ("+s", "fixup.dat"),
                                                ("+r", "fixup.dat"))),
    "in-a-hidden-folder": (["ROBCO/start.elf"], (("+h", "ROBCO"),)),
    "lower-case": (["start.elf"], ()),
    "upper-case": (["START.ELF"], ()),
    "mixed-case": (["BootCode.Bin"], ()),
    "long-folder-names": (["a folder with a long name/another long one/fixup.dat"], ()),
    "deep": (["a/b/c/d/e/f/g/h/i/j/k/l/BOOTCODE.BIN"], ()),
}


@needs_mtools
@pytest.mark.parametrize("size", ["16", "64"], ids=["fat16", "fat32"])
@pytest.mark.parametrize("case", sorted(FIRMWARE_CARDS))
def test_firmware_is_refused_however_stored(tmp_path, case, size):
    names, attrib = FIRMWARE_CARDS[case]
    card = make_card(tmp_path, names, size, attrib)
    proc = pack(tmp_path, card)
    assert proc.returncode == 1, proc.stdout
    assert "Pi firmware on" in proc.stdout
    assert not (tmp_path / "out").exists()


@needs_mtools
def test_a_clean_card_lists_hidden_and_long_names(tmp_path):
    card = make_card(tmp_path, ["RETROS.IMG", "ROBCO/a long name.txt", "start.elf.txt"],
                     attrib=(("+h", "RETROS.IMG"),))
    proc = pack(tmp_path, card)
    assert proc.returncode == 0, proc.stdout
    assert "p1:/RETROS.IMG" in proc.stdout and "p1:/ROBCO/a long name.txt" in proc.stdout


def two_partition_card(tmp_path, second):
    """An MBR with two FAT16 partitions of 8 MiB, firmware on the second."""
    card = tmp_path / "sd.img"
    sectors = 8 * 2048
    mbr = bytearray(512)
    for n, lba in enumerate((2048, 2048 + sectors)):
        mbr[446 + 16 * n:462 + 16 * n] = bytes([0, 0xFE, 0xFF, 0xFF, 0x0E, 0xFE, 0xFF, 0xFF]) \
            + lba.to_bytes(4, "little") + sectors.to_bytes(4, "little")
    mbr[510:512] = b"\x55\xaa"
    with open(card, "wb") as f:
        f.write(mbr)
        f.truncate((2048 + 2 * sectors) * 512)
    for n, lba in enumerate((2048, 2048 + sectors)):
        img = f"{card}@@{lba * 512}"
        subprocess.run(["mformat", "-i", img, "-c", "1", "-T", str(sectors), "-h", "64",
                        "-s", "32", "-H", str(lba), "::"], check=True, capture_output=True,
                       env=MTOOLS_ENV)
        src = tmp_path / ("RETROS.IMG" if n == 0 else second)
        src.write_bytes(b"x")
        subprocess.run(["mcopy", "-i", img, str(src), "::"], check=True, capture_output=True,
                       env=MTOOLS_ENV)
    return card


@needs_mtools
def test_firmware_on_a_second_partition_is_refused(tmp_path):
    proc = pack(tmp_path, two_partition_card(tmp_path, "start.elf"))
    assert proc.returncode == 1, proc.stdout
    assert "p2:/start.elf" in proc.stdout


@needs_mtools
def test_a_clean_second_partition_passes(tmp_path):
    proc = pack(tmp_path, two_partition_card(tmp_path, "UOS.SYS"))
    assert proc.returncode == 0, proc.stdout
    assert "p2:/UOS.SYS" in proc.stdout


def test_a_partition_that_is_not_fat_fails(tmp_path):
    card = tmp_path / "sd.img"
    mbr = bytearray(4096)
    mbr[446 + 4] = 0x83
    mbr[446 + 8:446 + 16] = (1).to_bytes(4, "little") + (2).to_bytes(4, "little")
    mbr[510:512] = b"\x55\xaa"
    card.write_bytes(bytes(mbr))
    proc = pack(tmp_path, card)
    assert proc.returncode == 1
    assert "not FAT" in proc.stdout


@needs_mtools
def test_a_directory_cluster_loop_fails(tmp_path):
    card = make_card(tmp_path, ["ROBCO/UOS.SYS"], size="16")
    with open(card, "r+b") as f:
        vol = pack_card.Volume(f, 1 << 20, os.path.getsize(card) - (1 << 20))
        entry = [p for p, _ in pack_card.walk(vol, "p1:")]
        assert entry == ["p1:/ROBCO", "p1:/ROBCO/UOS.SYS"]
        # Point the ROBCO folder's cluster at itself in the FAT.
        root = vol.read(vol.root_off, vol.root_len)
        robco = next(root[i:i + 32] for i in range(0, len(root), 32)
                     if root[i:i + 11] == b"ROBCO      " and root[i + 11] & 0x10)
        first = int.from_bytes(robco[26:28], "little")
        f.seek(vol.offset + vol.fat_off + first * 2)
        f.write(first.to_bytes(2, "little"))
    proc = pack(tmp_path, card)
    assert proc.returncode == 1
    assert "broken cluster chain" in proc.stdout
