#!/usr/bin/env python3
"""Boot spike 3 in QEMU, take a screendump and check the test pattern colours.

Usage: check_screen.py <raspi0|raspi1ap|raspi2b|raspi3b> [...]
Run with the repo venv (make venv): .venv/bin/python spikes/s3-fb-mmu/check_screen.py raspi0
"""

import os
import subprocess
import sys
import tempfile
import time

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tests", "harness"))

from qemu import Qemu, QemuError, loader_words  # noqa: E402

W, H = 640, 480
SQ = 16

BARS = [
    ("white", (255, 255, 255)),
    ("yellow", (255, 255, 0)),
    ("cyan", (0, 255, 255)),
    ("green", (0, 255, 0)),
    ("magenta", (255, 0, 255)),
    ("red", (255, 0, 0)),
    ("blue", (0, 0, 255)),
    ("black", (0, 0, 0)),
]

CORNERS = [
    ("top-left red", (SQ // 2, SQ // 2), (255, 0, 0)),
    ("top-right green", (W - SQ // 2, SQ // 2), (0, 255, 0)),
    ("bottom-left blue", (SQ // 2, H - SQ // 2), (0, 0, 255)),
]

# raspi3b starts in AArch64: same stub as run-qemu.sh, drops to AArch32 HYP at 0x8000.
A64_STUB = [0xd2802620, 0xd51e1100, 0xd2803b40, 0xd51e4000,
            0xd2900000, 0xd51e4020, 0xd69f03e0]


def qemu_for(machine):
    elf = os.path.join(HERE, "build", "s3.elf")
    img = os.path.join(HERE, "build", "s3.img")
    if machine == "raspi3b":
        args = loader_words(0x80000, A64_STUB)
        args += ["-device", f"loader,file={img},addr=0x8000,force-raw=on"]
        args += ["-device", "loader,addr=0x80000,cpu-num=0"]
        return Qemu("qemu-system-aarch64", machine, args, serial_count=1)
    return Qemu("qemu-system-arm", machine, ["-kernel", elf], serial_count=1)


def wait_done(q, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with open(q.serial_paths[0], "rb") as f:
            text = f.read().decode("ascii", "replace")
        if "ev=exception" in text:
            raise QemuError("exception: " + text)
        if "@RC stage=spike3 ev=done" in text:
            return text
        time.sleep(0.1)
    raise QemuError("no ev=done line")


def check(machine):
    errors = []
    with qemu_for(machine) as q, tempfile.TemporaryDirectory(prefix="s3shot") as tmp:
        wait_done(q)
        out = os.path.join(tmp, "screen.ppm")
        q.execute("screendump", filename=out)  # PPM: CI QEMU has no libpng
        with Image.open(out) as shot:
            img = shot.convert("RGB")

    if img.size != (W, H):
        errors.append(f"screen size {img.size}, expected {(W, H)}")
    else:
        bar_w = W // 8
        for i, (name, want) in enumerate(BARS):
            xy = (i * bar_w + bar_w // 2, H // 2)
            got = img.getpixel(xy)
            if got != want:
                errors.append(f"bar {i} {name} at {xy}: got {got}")
        for name, xy, want in CORNERS:
            got = img.getpixel(xy)
            if got != want:
                errors.append(f"{name} at {xy}: got {got}")

    for e in errors:
        print(f"{machine}: {e}")
    print(f"{'FAIL' if errors else 'PASS'}: {machine} screen")
    return not errors


def main():
    machines = sys.argv[1:]
    if not machines:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    subprocess.run(["make", "-C", HERE], check=True, stdout=subprocess.DEVNULL)
    ok = True
    for m in machines:
        ok = check(m) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
