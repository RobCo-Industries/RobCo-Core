#!/usr/bin/env python3
"""Pack one nightly card: refuse Pi firmware, copy, checksum.

    pack_card.py --card build/sd.img --name robco-terminal.img \\
        --versions VERSIONS.txt --out DIR [--extra FILE ...]

It reads the FAT directory entries of every partition in the card's MBR
itself, with no tool that could hide a file: hidden, system and read-only
entries, long and short names, every folder at any depth. A file whose
long or short name equals a Pi firmware name in any case fails the job: a
kept card holds no Pi firmware. The firmware names come from the FILES
line of tools/fetch_firmware.sh, so the two cannot disagree. A partition
or a directory it cannot read fails the job too. Then it copies the card
to DIR/NAME, each --extra file under its own name, and VERSIONS.txt, and
writes DIR/SHA256SUMS over the images. Standard library only.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import sys

FETCH_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fetch_firmware.sh")
FAT_TYPES = {0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E}
MAX_DEPTH = 32
MAX_ENTRIES = 200000


class CardError(Exception):
    pass


def firmware_names(script=FETCH_SCRIPT):
    """The names in the FILES=(...) line of fetch_firmware.sh, lower case."""
    with open(script, encoding="utf-8") as f:
        found = re.findall(r"^FILES=\(([^)]*)\)", f.read(), re.M)
    if len(found) != 1 or not found[0].split():
        raise CardError(f"{script} has no single FILES=(...) line")
    return tuple(n.lower() for n in found[0].split())


class Volume:
    """One FAT12, FAT16 or FAT32 volume at a byte offset of the card."""

    def __init__(self, f, offset, size):
        self.f, self.offset = f, offset
        b = self.read(0, 512)
        if len(b) < 512:
            raise CardError(f"no boot sector at {offset}")
        bps, self.spc, rsvd, nfats, root_ents, tot16 = struct.unpack_from("<HBHBHH", b, 11)
        fat16, = struct.unpack_from("<H", b, 22)
        tot32, fat32 = struct.unpack_from("<II", b, 32)
        self.root_clus, = struct.unpack_from("<I", b, 44)
        if bps not in (512, 1024, 2048, 4096) or self.spc == 0 or self.spc & (self.spc - 1) \
                or nfats == 0 or rsvd == 0:
            raise CardError(f"no FAT boot sector at {offset}")
        fatsz = fat16 or fat32
        total = tot16 or tot32
        self.bps, self.csize = bps, bps * self.spc
        self.fat_off = rsvd * bps
        self.root_off = (rsvd + nfats * fatsz) * bps
        root_secs = (root_ents * 32 + bps - 1) // bps
        self.root_len = root_ents * 32
        self.data_off = self.root_off + root_secs * bps
        if not fatsz or not total or self.data_off >= total * bps or total * bps > size:
            raise CardError(f"FAT geometry at {offset} does not fit its partition")
        self.clusters = (total * bps - self.data_off) // self.csize
        self.bits = 12 if self.clusters < 4085 else 16 if self.clusters < 65525 else 32
        if self.bits != 32 and root_ents == 0 or self.bits == 32 and fat16:
            raise CardError(f"FAT{self.bits} volume at {offset} has a bad root directory")

    def read(self, off, n):
        self.f.seek(self.offset + off)
        return self.f.read(n)

    def next_cluster(self, c):
        if self.bits == 12:
            v, = struct.unpack("<H", self.read(self.fat_off + c * 3 // 2, 2))
            v = v >> 4 if c & 1 else v & 0xFFF
            return None if v >= 0xFF8 else v
        if self.bits == 16:
            v, = struct.unpack("<H", self.read(self.fat_off + c * 2, 2))
            return None if v >= 0xFFF8 else v
        v, = struct.unpack("<I", self.read(self.fat_off + c * 4, 4))
        v &= 0x0FFFFFFF
        return None if v >= 0x0FFFFFF8 else v

    def chain(self, first):
        seen = set()
        c = first
        while c is not None:
            if c < 2 or c >= self.clusters + 2 or c in seen:
                raise CardError(f"broken cluster chain at cluster {c}")
            seen.add(c)
            yield self.read(self.data_off + (c - 2) * self.csize, self.csize)
            c = self.next_cluster(c)

    def directory(self, first):
        if first == 0 and self.bits != 32:
            return self.read(self.root_off, self.root_len)
        return b"".join(self.chain(first or self.root_clus))


def lfn_part(e):
    raw = e[1:11] + e[14:26] + e[28:32]
    return raw.decode("utf-16-le", "replace").split("\0", 1)[0].replace("\uffff", "")


def walk(vol, label):
    """Every file and folder of the volume as (path, [names]), names holding
    the short name and, when there is one, the long name."""
    out = []
    todo = [(0, label, 0)]
    while todo:
        cluster, path, depth = todo.pop()
        if depth > MAX_DEPTH:
            raise CardError(f"{path}: folders nest deeper than {MAX_DEPTH}")
        data = vol.directory(cluster)
        lfn = []
        for i in range(0, len(data) - 31, 32):
            e = data[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            attr = e[11]
            if attr & 0x3F == 0x0F:
                lfn = [lfn_part(e)] + lfn if not e[0] & 0x40 else [lfn_part(e)]
                continue
            long_name, lfn = "".join(lfn), []
            if attr & 0x08:
                continue
            base = (bytes([0xE5]) + e[1:8] if e[0] == 0x05 else e[0:8]).decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            # The case bits Windows NT and mtools set on a lower-case short name.
            base = base.lower() if e[12] & 0x08 else base
            ext = ext.lower() if e[12] & 0x10 else ext
            short = f"{base}.{ext}" if ext else base
            if short in (".", ".."):
                continue
            names = [short] + ([long_name] if long_name and long_name != short else [])
            full = f"{path}/{long_name or short}"
            out.append((full, names))
            if len(out) > MAX_ENTRIES:
                raise CardError(f"more than {MAX_ENTRIES} entries")
            if attr & 0x10:
                first = struct.unpack_from("<H", e, 26)[0]
                if vol.bits == 32:
                    first |= struct.unpack_from("<H", e, 20)[0] << 16
                if first:
                    todo.append((first, full, depth + 1))
    return out


def card_files(card):
    """(path, names) of every entry on every FAT partition of the card."""
    size = os.path.getsize(card)
    with open(card, "rb") as f:
        mbr = f.read(512)
        if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa":
            raise CardError(f"{card} has no MBR")
        found = []
        for n in range(4):
            boot, ptype = mbr[446 + 16 * n], mbr[450 + 16 * n]
            lba, count = struct.unpack_from("<II", mbr, 454 + 16 * n)
            if ptype == 0 and lba == 0 and count == 0:
                continue
            if ptype not in FAT_TYPES:
                raise CardError(f"{card} partition {n + 1} has type 0x{ptype:02x}, not FAT")
            if boot not in (0, 0x80) or lba == 0 or (lba + count) * 512 > size:
                raise CardError(f"{card} partition {n + 1} does not fit the card")
            found += walk(Volume(f, lba * 512, count * 512), f"p{n + 1}:")
        if not found and not any(mbr[450 + 16 * n] for n in range(4)):
            raise CardError(f"{card} has no partition")
    return found


def firmware_on(files, names=None):
    names = names or firmware_names()
    return [path for path, own in files if any(n.lower() in names for n in own)]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def run(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--card", required=True)
    p.add_argument("--name", required=True)
    p.add_argument("--versions", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--extra", action="append", default=[])
    args = p.parse_args(argv)
    try:
        files = card_files(args.card)
        found = firmware_on(files)
    except (CardError, OSError, struct.error) as e:
        print(f"::error::pack_card: cannot check {args.card}: {e}")
        return 1
    print("\n".join(path for path, _ in files))
    if found:
        print(f"::error::pack_card: Pi firmware on {args.card}: {', '.join(found)}")
        return 1
    os.makedirs(args.out, exist_ok=True)
    images = [(args.card, args.name)] + [(e, os.path.basename(e)) for e in args.extra]
    for src, name in images:
        shutil.copyfile(src, os.path.join(args.out, name))
    shutil.copyfile(args.versions, os.path.join(args.out, "VERSIONS.txt"))
    with open(os.path.join(args.out, "SHA256SUMS"), "w", encoding="utf-8") as f:
        for _, name in images:
            f.write(f"{sha256(os.path.join(args.out, name))}  {name}\n")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
