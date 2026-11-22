#!/usr/bin/env python3
"""Boot the console example in QEMU and save a screenshot of its specimen page.

Usage:
    tools/font_specimen.py -o spleen-green.png
    tools/font_specimen.py --font-c local/font8x16_fixedsys.c --scheme amber -o fixedsys-amber.png

Writes the PNG and a 2x nearest-neighbour copy next to it (name@2x.png).
Needs Pillow, the Arm toolchain and QEMU. Run it with .venv/bin/python.
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))

from harness.boot import BOARDS, boot  # noqa: E402

EXAMPLE = os.path.join(ROOT, "examples", "console")
SCHEMES = {"green": "1", "amber": "2", "blue": "3", "white": "4"}


def build(font_c):
    """Build the example and return the image path. Each font gets its own build dir."""
    if font_c:
        font_c = os.path.abspath(font_c)
        tag = hashlib.sha256(font_c.encode()).hexdigest()[:12]
        build_dir = f"build/specimen-{tag}"
    else:
        build_dir = "build/specimen-builtin"
    cmd = ["make", "-s", "-C", EXAMPLE, "CI=1", f"BUILD={build_dir}"]
    if font_c:
        cmd.append(f"FONT_C={font_c}")
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return os.path.join(EXAMPLE, build_dir, "console.img")


def command(vm, c, timeout=10):
    start = len(vm.serial)
    ok = f"@RC stage=console ev=ok cmd={c}\n"
    vm.send(c)
    deadline = time.monotonic() + timeout
    while ok not in vm.serial[start:]:
        if time.monotonic() > deadline:
            sys.exit(f"font_specimen: no reply to {c!r}:\n{vm.serial}")
        time.sleep(0.01)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--font-c", help="table from tools/font2c.py (default: built-in font)")
    ap.add_argument("--scheme", choices=list(SCHEMES), default="green")
    ap.add_argument("--board", choices=list(BOARDS), default="raspi2b")
    ap.add_argument("-o", dest="out", required=True, help="output PNG")
    args = ap.parse_args()

    from PIL import Image

    image = build(args.font_c)
    with boot(image, args.board) as vm:
        vm.wait_for(re.compile(r"@RC stage=console ev=ready\n"), timeout=10)
        name = next((m["name"] for m in vm.markers() if m.get("ev") == "font"), "?")
        command(vm, SCHEMES[args.scheme])
        command(vm, "p")
        with tempfile.TemporaryDirectory() as tmp:
            ppm = os.path.join(tmp, "screen.ppm")
            vm.screendump(ppm)
            with Image.open(ppm) as img:
                img = img.convert("RGB")

    img.save(args.out)
    base, ext = os.path.splitext(args.out)
    big = f"{base}@2x{ext}"
    img.resize((img.width * 2, img.height * 2), Image.NEAREST).save(big)
    print(f"font_specimen: font {name}, scheme {args.scheme}: {args.out}, {big}")


if __name__ == "__main__":
    main()
