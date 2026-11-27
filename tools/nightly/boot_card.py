#!/usr/bin/env python3
"""Boot a kept nightly card and check that the chain reaches its last stage.

    boot_card.py --chain terminal --board raspi2b --first retros.img \\
        --card card/robco-terminal.img --sums card/SHA256SUMS --timeout 120

It checks the card, and the first-stage image when SHA256SUMS lists it,
against SHA256SUMS. It copies the card into a temp folder, checks the copy,
and boots the first-stage image on the board with the copy as the SD card,
opened with snapshot=on, so a write during the boot never reaches a file.
It reads the serial markers until the chain's last step or the time limit.
The run passes when every step of the chain's table shows in order and no
stage printed error, panic, exception, shell, halt or rom_kill. Then it
checks the card against SHA256SUMS again. It boots through the harness of
tests/harness, the one every QEMU test uses. Standard library only.
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from harness.boot import boot, parse_markers  # noqa: E402
from harness.qemu import QemuError  # noqa: E402

# Each chain is a table of steps that must show in this order. A step is
# (what it means, stage, event, keys it must carry) for a marker, or
# (what it means, None, a regex for a whole line, None) for a plain line.
# The names follow the serial markers of docs/spec/BOOT_PROTOCOL.md.
CHAINS = {
    "terminal": [
        ("RETROS-BIOS starts", "bios", "boot", {}),
        ("RETROS-BIOS loads the agent", "bios", "load", {"path": "/ROBCO/MFBOOT.SYS"}),
        ("RETROS-BIOS jumps", "bios", "jump", {}),
        ("MFBootAgent starts", "bootagent", "boot", {}),
        ("MFBootAgent trusts the boot info", "bootagent", "handoff", {"trusted": "1"}),
        ("MFBootAgent loads UOS", "bootagent", "load", {}),
        ("MFBootAgent jumps", "bootagent", "jump", {}),
        ("UOS starts", "uos", "boot", {}),
        ("UOS trusts the boot info", "uos", "handoff", {"trusted": "1"}),
        ("UOS shows the login screen", "uos", "ready", {"screen": "login"}),
    ],
    # BOOT_PROTOCOL.md lists `ready` for the stage `deitrix`, but Deitrix
    # prints "deitrix: ready" through the log call. Either line counts.
    "pipboy": [
        ("Pip-OS starts", "pipos", "boot", {}),
        ("Pip-OS loads the ROM", "pipos", "load", {}),
        ("Pip-OS jumps to the ROM", "pipos", "jump", {}),
        ("the ROM is ready", None, r"deitrix: ready|@RC stage=deitrix ev=ready\b.*", None),
    ],
}

# Events a healthy boot of a kept card never prints.
BAD_EVENTS = frozenset({"error", "panic", "exception", "shell", "halt", "rom_kill"})

def marker(line):
    found = parse_markers(line + "\n")
    return found[0] if found else None


def matches(step, line, mk):
    _, stage, ev, keys = step
    if stage is None:
        return re.fullmatch(ev, line.strip()) is not None
    return mk is not None and mk.get("stage") == stage and mk.get("ev") == ev \
        and all(mk.get(k) == v for k, v in keys.items())


def check(lines, chain):
    """(steps seen, problem) for complete serial lines. problem is None
    while nothing went wrong; the boot passes when every step is seen."""
    steps = CHAINS[chain]
    seen = 0
    for line in lines:
        mk = marker(line)
        if mk and mk.get("ev") in BAD_EVENTS:
            return seen, f"stage {mk.get('stage')} printed: {line.strip()}"
        if seen < len(steps) and matches(steps[seen], line, mk):
            seen += 1
    return seen, None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def read_sums(path):
    sums = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            digest, _, name = line.rstrip("\n").partition("  ")
            sums[name] = digest
    return sums


def verify(sums, path, required):
    """Fails when the file is listed, or required, and does not match."""
    name = os.path.basename(path)
    if name not in sums:
        if required:
            raise SystemExit(f"::error::boot_card: SHA256SUMS does not list {name}")
        return
    if sha256(path) != sums[name]:
        raise SystemExit(f"::error::boot_card: {name} does not match SHA256SUMS")


def boot_and_read(first, board, sd, timeout, chain):
    """Boots `first` with `sd` as the card, snapshot on, and returns the
    serial lines once the chain is done, failed or out of time."""
    drive = ["-drive", f"if=sd,format=raw,snapshot=on,file={sd}"]
    deadline = time.monotonic() + timeout
    try:
        with boot(first, board, serial2=True, extra_args=drive, timeout=30.0) as vm:
            while True:
                lines = vm.lines()
                seen, problem = check(lines, chain)
                if problem or seen == len(CHAINS[chain]) or time.monotonic() > deadline \
                        or vm.qemu.proc.poll() is not None:
                    return lines
                time.sleep(0.2)
    except QemuError as e:
        raise SystemExit(f"::error::boot_card: QEMU failed: {e}")


def run(argv):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--chain", required=True, choices=sorted(CHAINS))
    p.add_argument("--board", required=True)
    p.add_argument("--first", required=True, help="the first-stage image")
    p.add_argument("--card", required=True)
    p.add_argument("--sums", required=True, help="the SHA256SUMS written with the card")
    p.add_argument("--timeout", type=float, default=120.0, help="seconds")
    p.add_argument("--log", help="write the serial output here")
    args = p.parse_args(argv)
    sums = read_sums(args.sums)
    verify(sums, args.card, True)
    verify(sums, args.first, False)
    want = sums[os.path.basename(args.card)]
    with tempfile.TemporaryDirectory(prefix="bootcard") as tmp:
        copy = os.path.join(tmp, "sd.img")
        shutil.copyfile(args.card, copy)
        if sha256(copy) != want:
            print("::error::boot_card: the copy of the card does not match SHA256SUMS")
            return 1
        start = time.monotonic()
        lines = boot_and_read(os.path.abspath(args.first), args.board, copy, args.timeout,
                              args.chain)
        took = time.monotonic() - start
        copy_changed = sha256(copy) != want
    if args.log:
        with open(args.log, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    seen, problem = check(lines, args.chain)
    steps = CHAINS[args.chain]
    for n, step in enumerate(steps):
        print(f"{'ok  ' if n < seen else 'MISS'} {step[0]}")
    if sha256(args.card) != want:
        print(f"::error::boot_card: {args.card} changed during the boot")
        return 1
    if copy_changed:
        print("::error::boot_card: the boot wrote to the card: the snapshot did not hold")
        return 1
    if problem or seen < len(steps):
        why = problem or f"no {steps[seen][0]!r} within {args.timeout:g} s"
        print("\n".join(lines[-40:]))
        print(f"::error::boot_card: the {args.chain} card on {args.board}: {why}")
        return 1
    print(f"boot_card: the {args.chain} card reached its last stage on {args.board} "
          f"in {took:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
