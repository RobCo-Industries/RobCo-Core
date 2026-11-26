#!/usr/bin/env python3
"""Pack one nightly card: refuse Pi firmware, copy, checksum.

    pack_card.py --card build/sd.img --name robco-terminal.img \\
        --versions VERSIONS.txt --out DIR [--extra FILE ...]

It lists the card's FAT partition, at 1 MiB as the suite writes it, with
mtools. A file named bootcode.bin, start.elf or fixup.dat, in any folder
and any case, fails the job: a kept card holds no Pi firmware. Then it
copies the card to DIR/NAME, each --extra file under its own name, and
VERSIONS.txt, and writes DIR/SHA256SUMS over the images. Standard library
and mtools only.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

FIRMWARE = ("bootcode.bin", "start.elf", "fixup.dat")


def card_files(card):
    proc = subprocess.run(["mdir", "-/", "-b", "-i", f"{card}@@1M", "::/"],
                          capture_output=True, text=True,
                          env=dict(os.environ, MTOOLS_SKIP_CHECK="1"))
    if proc.returncode != 0:
        raise RuntimeError(f"mdir cannot list {card}: {proc.stderr.strip()}")
    return [line.strip() for line in proc.stdout.splitlines() if line.strip()]


def firmware_on(files):
    return [f for f in files
            if f.replace("\\", "/").rsplit("/", 1)[-1].lower() in FIRMWARE]


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
    except RuntimeError as e:
        print(f"::error::pack_card: {e}")
        return 1
    print("\n".join(files))
    found = firmware_on(files)
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
