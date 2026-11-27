#!/usr/bin/env python3
"""Build the files of a suite release from one night's two pins cards.

    assemble.py --manifest F --terminal DIR --pipboy DIR --notices F \\
        --fetch-firmware F --out dist

--terminal and --pipboy are the card folders a nightly run uploads:
each holds its image, VERSIONS.txt and SHA256SUMS. The tool checks every
line of each SHA256SUMS, needs robco-terminal.img and robco-pipboy.img
there, and needs the two VERSIONS.txt to be the same bytes. It packs
each image with gzip -n, then checks that gunzip -c gives the night's
SHA-256 again. It writes into --out, which must not exist yet:

    assets/robco-terminal.img.gz, assets/robco-pipboy.img.gz
    assets/fetch_firmware.sh, assets/NOTICES.txt, assets/VERSIONS.txt
    assets/<suite>.json, a copy of the manifest
    assets/SHA256SUMS over every other file in assets/
    NOTES.md, the release note

It does not check the manifest against the night: manifest.py
verify-night does that. It makes no tag and no release. Standard
library only.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

CARDS = (("Terminal", "robco-terminal.img"), ("Pip-Boy", "robco-pipboy.img"))
REPOS = ("RobCo-Core", "RETROS-BIOS", "MFBootAgent", "UOS", "Pip-OS", "Deitrix")
SUITE_RE = re.compile(r"suite-\d{4}\.(0[1-9]|1[0-2])(\.[1-9]\d*)?")
SUM_RE = re.compile(r"([0-9a-f]{64})  ([A-Za-z0-9._-]+)")


class AssembleError(Exception):
    pass


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_sums(folder):
    """{name: sha256} of folder/SHA256SUMS, after checking every line."""
    path = os.path.join(folder, "SHA256SUMS")
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise AssembleError(f"cannot read {path}: {e.strerror}") from None
    sums = {}
    for line in lines:
        m = SUM_RE.fullmatch(line)
        if not m or m.group(2) in sums:
            raise AssembleError(f"{path}: bad line {line!r}")
        sums[m.group(2)] = m.group(1)
    if not sums:
        raise AssembleError(f"{path} is empty")
    for name, want in sums.items():
        file = os.path.join(folder, name)
        if not os.path.isfile(file):
            raise AssembleError(f"{folder}: {name} is missing")
        got = sha256_file(file)
        if got != want:
            raise AssembleError(f"{folder}: {name} has SHA-256 {got}, SHA256SUMS says {want}")
    return sums


def load_manifest(path):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except OSError as e:
        raise AssembleError(f"cannot read {path}: {e.strerror}") from None
    except ValueError as e:
        raise AssembleError(f"{path} is not JSON: {e}") from None
    try:
        suite = data["suite"]
        repos = {name: (data["repositories"][name]["version"],
                        data["repositories"][name]["commit"]) for name in REPOS}
    except (TypeError, KeyError):
        raise AssembleError(f"{path} is not a suite manifest; run manifest.py check") from None
    if not isinstance(suite, str) or not SUITE_RE.fullmatch(suite):
        raise AssembleError(f"{path}: suite {suite!r} is not suite-YYYY.MM or suite-YYYY.MM.N")
    return suite, repos


def gzip_card(src, dst, want):
    """gzip -n src into dst, then check gunzip -c gives want again."""
    with open(dst, "wb") as out:
        proc = subprocess.run(["gzip", "-n", "-9", "-c", src], stdout=out,
                              stderr=subprocess.PIPE, text=False)
    if proc.returncode != 0:
        raise AssembleError(f"gzip -n {src} failed: {proc.stderr.decode(errors='replace').strip()}")
    h = hashlib.sha256()
    proc = subprocess.Popen(["gunzip", "-c", dst], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    for block in iter(lambda: proc.stdout.read(1 << 20), b""):
        h.update(block)
    proc.stdout.close()
    if proc.wait() != 0:
        raise AssembleError(f"gunzip -c {dst} failed")
    if h.hexdigest() != want:
        raise AssembleError(f"gunzip -c {dst} gives {h.hexdigest()}, the night's card is {want}")
    return sha256_file(dst)


def notes(suite, repos, cards):
    lines = [
        f"# {suite}",
        "",
        "Two card images of the RobCo suite, built and tested whole in QEMU by one",
        "nightly run at the commits below. This is a pre-release.",
        "",
        "| Repository | Version | Commit |",
        "|---|---|---|",
    ]
    lines += [f"| {name} | {repos[name][0]} | `{repos[name][1]}` |" for name in REPOS]
    lines += [
        "",
        "| File | Chain | SHA-256 of the image | SHA-256 of the .gz |",
        "|---|---|---|---|",
    ]
    lines += [f"| `{name}.gz` | {chain} | `{img}` | `{gz}` |" for chain, name, img, gz in cards]
    lines += [
        "",
        "The images hold no Raspberry Pi firmware. To make a card:",
        "",
        "1. Check the downloads with `sha256sum -c SHA256SUMS`.",
        "2. Write the `.gz` file to an SD card, for example with Raspberry Pi Imager.",
        "3. Mount the card and run `fetch_firmware.sh <card folder>`. It downloads",
        "   `bootcode.bin`, `start.elf` and `fixup.dat` at pinned hashes.",
        "",
        "`VERSIONS.txt` is the night's version table. "
        f"`{suite}.json` is the manifest. `NOTICES.txt` holds the licence texts.",
        "",
    ]
    return "\n".join(lines)


def assemble(args):
    suite, repos = load_manifest(args.manifest)
    if os.path.lexists(args.out):
        raise AssembleError(f"{args.out} exists already")
    folders = (args.terminal, args.pipboy)
    sums = [read_sums(folder) for folder in folders]
    for (chain, name), folder, s in zip(CARDS, folders, sums):
        if name not in s:
            raise AssembleError(f"{folder}: SHA256SUMS has no {name}")
    versions = []
    for folder in folders:
        path = os.path.join(folder, "VERSIONS.txt")
        try:
            with open(path, "rb") as f:
                versions.append(f.read())
        except OSError as e:
            raise AssembleError(f"cannot read {path}: {e.strerror}") from None
    if versions[0] != versions[1]:
        raise AssembleError("the two cards hold different VERSIONS.txt files")
    for path in (args.notices, args.fetch_firmware):
        if not os.path.isfile(path):
            raise AssembleError(f"{path} is missing")

    assets = os.path.join(args.out, "assets")
    os.makedirs(assets)
    cards = []
    for (chain, name), folder, s in zip(CARDS, folders, sums):
        gz = gzip_card(os.path.join(folder, name), os.path.join(assets, name + ".gz"), s[name])
        cards.append((chain, name, s[name], gz))
    shutil.copyfile(args.fetch_firmware, os.path.join(assets, "fetch_firmware.sh"))
    shutil.copyfile(args.notices, os.path.join(assets, "NOTICES.txt"))
    shutil.copyfile(os.path.join(args.terminal, "VERSIONS.txt"),
                    os.path.join(assets, "VERSIONS.txt"))
    shutil.copyfile(args.manifest, os.path.join(assets, f"{suite}.json"))
    names = sorted(os.listdir(assets))
    with open(os.path.join(assets, "SHA256SUMS"), "w", encoding="utf-8") as f:
        for name in names:
            f.write(f"{sha256_file(os.path.join(assets, name))}  {name}\n")
    with open(os.path.join(args.out, "NOTES.md"), "w", encoding="utf-8") as f:
        f.write(notes(suite, repos, cards))
    print(f"assemble: wrote {suite} to {args.out}")
    return 0


def run(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--manifest", required=True)
    p.add_argument("--terminal", required=True)
    p.add_argument("--pipboy", required=True)
    p.add_argument("--notices", required=True)
    p.add_argument("--fetch-firmware", required=True)
    p.add_argument("--out", required=True)
    args = p.parse_args(argv)
    try:
        return assemble(args)
    except (AssembleError, OSError) as e:
        print(f"::error::assemble: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
