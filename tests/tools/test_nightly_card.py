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


def open_volume(f, card):
    return pack_card.Volume(f, 1 << 20, os.path.getsize(card) - (1 << 20))


def entry_at(card, short):
    """The byte offset of the one directory entry whose 8.3 name is `short`."""
    data = card.read_bytes()
    at = [m.start() for m in re.finditer(re.escape(short), data) if m.start() % 32 == 0]
    assert len(at) == 1, (short, at)
    return at[0]


def put(f, off, data):
    f.seek(off)
    f.write(data)


def set_fat(f, vol, cluster, value):
    """Set one FAT16 or FAT32 entry in every FAT copy."""
    width = vol.bits // 8
    for n in range(vol.nfats):
        put(f, vol.offset + vol.fat_off + n * vol.fat_len + cluster * width,
            value.to_bytes(width, "little"))


def first_cluster(card, off):
    e = card.read_bytes()[off:off + 32]
    return int.from_bytes(e[26:28], "little") | int.from_bytes(e[20:22], "little") << 16


def patch_entry(at, field, data):
    def patch(f, vol, card):
        put(f, entry_at(card, at) + field, data)
    return patch


def end_chain_early(f, vol, card):
    first = first_cluster(card, entry_at(card, UOS))
    set_fat(f, vol, first, (1 << vol.bits) - 1 if vol.bits == 16 else 0x0FFFFFFF)


def cluster_out_of_range(f, vol, card):
    first = first_cluster(card, entry_at(card, UOS))
    set_fat(f, vol, first, vol.clusters + 5)


def loop_a_folder(f, vol, card):
    first = first_cluster(card, entry_at(card, b"ROBCO      \x10"))
    set_fat(f, vol, first, first)


def lfn_without_short(f, vol, card):
    # The short entry right after the long name is marked deleted.
    put(f, entry_at(card, b"ALONGN~1TXT"), b"\xe5")


def lfn_with_wrong_checksum(f, vol, card):
    put(f, entry_at(card, b"ALONGN~1TXT"), b"B")


def fat32_root_zero(f, vol, card):
    put(f, vol.offset + 44, (0).to_bytes(4, "little"))


def fats_disagree(f, vol, card):
    off = vol.offset + vol.fat_off + vol.fat_len + 2 * vol.bits // 8 * 3
    f.seek(off)
    b = f.read(1)
    put(f, off, bytes([b[0] ^ 0xFF]))


UOS = b"UOS     SYS"
# One row per thing a healthy card never holds: (patch, FAT32 only, what the error says).
BAD_CARDS = {
    "folder-cluster-zero": (patch_entry(b"ROBCO      \x10", 26, b"\0\0"), False,
                            "folder with starting cluster 0"),
    "cluster-out-of-range": (cluster_out_of_range, False, "broken cluster chain"),
    "file-starts-out-of-range": (patch_entry(UOS, 26, b"\xf0\xff"), False,
                                 "broken cluster chain"),
    "folder-chain-loops": (loop_a_folder, False, "broken cluster chain"),
    "chain-ends-early": (end_chain_early, False, "clusters for 3000 bytes"),
    "chain-longer-than-size": (patch_entry(UOS, 28, (1).to_bytes(4, "little")), False,
                               "clusters for 1 bytes"),
    "size-passes-partition": (patch_entry(UOS, 28, b"\xf0\xff\xff\xff"), False,
                              "passes the partition"),
    "long-name-without-short-entry": (lfn_without_short, False, "long name with no short"),
    "long-name-of-another-entry": (lfn_with_wrong_checksum, False, "long name with no short"),
    "volume-label-bit-on-a-file": (patch_entry(UOS, 11, b"\x28"), False, "volume label bit"),
    "invalid-first-byte": (patch_entry(UOS, 0, b"\x01"), False, "invalid short name"),
    "invalid-name-byte": (patch_entry(UOS, 3, b"*"), False, "invalid short name"),
    "fat32-root-cluster-zero": (fat32_root_zero, True, "root cluster 0"),
    "fat-copies-disagree": (fats_disagree, False, "differs from the first"),
}


def healthy_card(tmp_path, size):
    """A card from tools/mksdimage.sh, labelled ROBCO, with a deleted file in
    the root and in a folder, a long name and a 3000-byte file."""
    stage = tmp_path / "stage"
    (stage / "ROBCO").mkdir(parents=True)
    (stage / "ROBCO" / "UOS.SYS").write_bytes(b"u" * 3000)
    (stage / "ROBCO" / "GONE.TXT").write_bytes(b"g" * 700)
    (stage / "ROBCO" / "a long name.txt").write_bytes(b"x")
    (stage / "GONE2.TXT").write_bytes(b"g")
    card = tmp_path / "sd.img"
    subprocess.run([os.path.join(ROOT, "tools", "mksdimage.sh"), str(card), size, str(stage)],
                   check=True, capture_output=True)
    for path in ("ROBCO/GONE.TXT", "GONE2.TXT"):
        subprocess.run(["mdel", "-i", f"{card}@@1M", f"::/{path}"], check=True,
                       capture_output=True, env=MTOOLS_ENV)
    return card


@needs_mtools
@pytest.mark.parametrize("size", ["16", "64"], ids=["fat16", "fat32"])
def test_a_healthy_card_with_deleted_files_and_a_label_passes(tmp_path, size):
    card = healthy_card(tmp_path, size)
    data = card.read_bytes()
    assert re.search(rb"ROBCO      \x08", data) and data.count(b"\xe5ONE") >= 2
    proc = pack(tmp_path, card)
    assert proc.returncode == 0, proc.stdout
    listed = [line for line in proc.stdout.splitlines() if line.startswith("p1:")]
    assert sorted(listed) == ["p1:/ROBCO", "p1:/ROBCO/UOS.SYS", "p1:/ROBCO/a long name.txt"]


@needs_mtools
@pytest.mark.parametrize("size", ["16", "64"], ids=["fat16", "fat32"])
@pytest.mark.parametrize("case", sorted(BAD_CARDS))
def test_what_it_cannot_read_is_refused(tmp_path, case, size):
    patch, fat32_only, says = BAD_CARDS[case]
    if fat32_only and size != "64":
        pytest.skip("a FAT32 field")
    card = healthy_card(tmp_path, size)
    with open(card, "r+b") as f:
        patch(f, open_volume(f, card), card)
    proc = pack(tmp_path, card)
    assert proc.returncode == 1, proc.stdout
    assert "cannot check" in proc.stdout and says in proc.stdout, proc.stdout
    assert not (tmp_path / "out").exists()
