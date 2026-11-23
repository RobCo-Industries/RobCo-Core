"""Build SD card images with mtools. No root, no loop devices.

    make_sd_image("sd.img", 64, {"kernel.img": Path("build/kernel.img"),
                                 "robco/motd.txt": b"hello\\n"})

The image has an MBR with one FAT partition at LBA 2048. Sizes are
rounded up to a power of 2 because QEMU's SD card model needs that.
"""

import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from .env import minimal_env

SECTOR = 512
PART_LBA = 2048
FAT32_MIN_MIB = 64  # below this, FAT16
TOOLS = ("mformat", "mcopy", "mmd")


class MtoolsMissing(RuntimeError):
    pass


def have_mtools():
    return all(shutil.which(t) for t in TOOLS)


def _pow2_mib(size_mib):
    n = 1
    while n < size_mib:
        n *= 2
    return max(n, 4)


def _mbr(part_type, lba, sectors):
    # One entry, CHS fields set to the LBA-only marker values.
    entry = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", part_type, b"\xfe\xff\xff",
                        lba, sectors)
    return bytes(446) + entry + bytes(48) + b"\x55\xaa"


def make_sd_image(path, size_mib, files=None, label="ROBCO"):
    """Create `path`: MBR, one FAT partition at LBA 2048, the given files.

    `files` maps names inside the card ("dir/name") to bytes or a Path.
    A name ending in "/" creates an empty directory. Returns the size in
    bytes, which is `size_mib` rounded up to a power of 2.
    """
    if not have_mtools():
        raise MtoolsMissing("mtools (mformat, mcopy, mmd) not installed")
    size = _pow2_mib(size_mib) << 20
    sectors = size // SECTOR - PART_LBA
    fat32 = size >= FAT32_MIN_MIB << 20
    with open(path, "wb") as f:
        f.write(_mbr(0x0C if fat32 else 0x0E, PART_LBA, sectors))
        f.truncate(size)

    env = minimal_env(MTOOLS_SKIP_CHECK="1")
    img = f"{os.fspath(path)}@@{PART_LBA * SECTOR}"

    def mt(*args):
        subprocess.run(list(args), check=True, env=env, capture_output=True)

    # One-sector clusters keep small cards above the FAT12 cluster limit.
    fmt = ["mformat", "-i", img] + (["-F"] if fat32 else ["-c", "1"])
    mt(*fmt, "-v", label[:11], "-T", str(sectors), "-h", "64", "-s", "32",
       "-H", str(PART_LBA), "::")

    made = set()

    def mkdirs(parts):
        for i in range(1, len(parts) + 1):
            d = "/".join(parts[:i])
            if d not in made:
                mt("mmd", "-i", img, "::" + d)
                made.add(d)

    with tempfile.TemporaryDirectory(prefix="sdimg") as tmp:
        for i, (name, data) in enumerate(sorted((files or {}).items())):
            parts = [p for p in name.split("/") if p]
            if name.endswith("/"):
                mkdirs(parts)
                continue
            mkdirs(parts[:-1])
            src = data
            if isinstance(data, (bytes, bytearray)):
                src = Path(tmp) / f"f{i}"
                src.write_bytes(data)
            mt("mcopy", "-i", img, os.fspath(src), "::" + "/".join(parts))
    return size


def mbr_card(path, slots, size_mib=16):
    """A card with up to four MBR entries, for the boot volume rule.

    `slots` has one item per MBR slot: None for an empty slot, or
    (type, start_lba, sectors, files). With files a dict as in
    make_sd_image(), the blocks hold a FAT volume made with mformat at
    their offset; a name ending in "/" makes a folder. With files None,
    the blocks are filled with 0xA5, which is no file system. A slots
    value of "whole" with a files dict makes a card with no partition
    table and one FAT volume over all of it.
    """
    size = _pow2_mib(size_mib) << 20
    whole = isinstance(slots, tuple) and slots[0] == "whole"
    entries = b""
    if not whole:
        for slot in list(slots) + [None] * (4 - len(slots)):
            if slot is None:
                entries += bytes(16)
            else:
                ptype, lba, sectors, _ = slot
                entries += struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", ptype,
                                       b"\xfe\xff\xff", lba, sectors)
    with open(path, "wb") as f:
        if not whole:
            f.write(bytes(446) + entries + b"\x55\xaa")
        f.truncate(size)
    env = minimal_env(MTOOLS_SKIP_CHECK="1")

    def mt(*args):
        subprocess.run(list(args), check=True, env=env, capture_output=True)

    vols = [(0, size // SECTOR, slots[1], 0)] if whole else [
        (lba, sectors, files, i) for i, (_, lba, sectors, files) in
        ((i, s) for i, s in enumerate(slots) if s is not None)]
    for lba, sectors, files, i in vols:
        if files is None:
            with open(path, "r+b") as f:
                f.seek(lba * SECTOR)
                f.write(b"\xa5" * (sectors * SECTOR))
            continue
        img = f"{os.fspath(path)}@@{lba * SECTOR}"
        mt("mformat", "-i", img, "-c", "1", "-v", f"VOL{i}", "-T", str(sectors), "-h", "64",
           "-s", "32", "-H", str(lba), "::")
        made = set()
        with tempfile.TemporaryDirectory(prefix="mbrcard") as tmp:
            for j, (name, data) in enumerate(sorted(files.items())):
                parts = [p for p in name.split("/") if p]
                dirs = parts if name.endswith("/") else parts[:-1]
                for k in range(1, len(dirs) + 1):
                    d = "/".join(dirs[:k])
                    if d not in made:
                        mt("mmd", "-i", img, "::" + d)
                        made.add(d)
                if name.endswith("/"):
                    continue
                src = Path(tmp) / f"f{j}"
                src.write_bytes(data)
                mt("mcopy", "-i", img, os.fspath(src), "::" + "/".join(parts))
    return size


def corrupt(path, offset, data):
    """Overwrite bytes at `offset` in the image, for fault tests."""
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(bytes(data))


def read_sector(path, lba):
    with open(path, "rb") as f:
        f.seek(lba * SECTOR)
        return f.read(SECTOR)
