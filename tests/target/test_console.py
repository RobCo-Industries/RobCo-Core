"""Boot examples/console on every board and check the screen as text and pixels.

Set ROBCO_UPDATE_GOLDEN=1 to rewrite tests/golden/console_banner_green.png
from the current run instead of comparing against it.
"""

import os
import re
import subprocess
import sys
import time

import pytest
from PIL import Image

from harness.boot import BOARDS, boot
from harness.qemu import have_machine

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_C = os.path.join(ROOT, "src", "gfx", "font8x16.c")
GOLDEN = os.path.join(ROOT, "tests", "golden", "console_banner_green.png")

W, H = 640, 480
COLS, ROWS = 80, 30
FW, FH = 8, 16

READY = re.compile(r"@RC stage=console ev=ready\n")
SCREEN_END = "@RC stage=console ev=screen_end\n"

# Same values as src/gfx/schemes.c: background, dim, normal, bright.
SCHEMES = {
    "1": ("green", (0x0a, 0x1a, 0x0e), (0x1f, 0x8a, 0x3d), (0x33, 0xe0, 0x66), (0xb0, 0xff, 0xc8)),
    "2": ("amber", (0x1a, 0x10, 0x06), (0x8a, 0x5a, 0x10), (0xff, 0xb0, 0x00), (0xff, 0xe0, 0xa0)),
    "3": ("blue", (0x08, 0x10, 0x1e), (0x2a, 0x5a, 0x9a), (0x5a, 0xb4, 0xff), (0xc8, 0xe6, 0xff)),
    "4": ("white", (0x12, 0x12, 0x14), (0x80, 0x80, 0x84), (0xd8, 0xd8, 0xdc), (0xff, 0xff, 0xff)),
}

BANNER = [
    "ROBCO-CORE CONSOLE EXAMPLE",
    "8x16 cells, Spleen font, four colour schemes.",
    "Commands: d s t c x 1-4",
    ">",
]

DIGITS = "0123456789" * 10
TEXT_ROWS = [
    "A       B       C",
    "abX",
    DIGITS[:80],
    DIGITS[80:],
    "dim normal bright inverse",
]


def load_font():
    """Glyph rows from the generated table, keyed by code point."""
    glyphs = {}
    row_re = re.compile(r"\{((?:0x[0-9a-f]{2}, ){15}0x[0-9a-f]{2})\}, /\* U\+([0-9A-F]{4}) \*/")
    with open(FONT_C) as f:
        for m in row_re.finditer(f.read()):
            glyphs[int(m.group(2), 16)] = [int(v, 16) for v in m.group(1).split(", ")]
    return glyphs


def load_table(path=FONT_C):
    """Every glyph the console can draw from a byte, keyed by byte value.

    ASCII maps to itself, the extras to their CP437 byte, and every other
    byte from 0x80 to 0xff to the replacement glyph.
    """
    with open(path) as f:
        text = f.read()
    table = load_font_text(text)
    rep = re.search(r"font_replacement\[FONT_H\] = \{([^}]*)\}", text)
    replacement = [int(v, 16) for v in rep.group(1).replace(",", " ").split()]
    for b in range(0x80, 0x100):
        table[b] = replacement
    extra_re = re.compile(r"\{0x([0-9a-f]{4}), 0x([0-9a-f]{2}), \{((?:0x[0-9a-f]{2}, ){15}0x[0-9a-f]{2})\}\}")
    for m in extra_re.finditer(text):
        table[int(m.group(2), 16)] = [int(v, 16) for v in m.group(3).split(", ")]
    return table


def load_font_text(text):
    row_re = re.compile(r"\{((?:0x[0-9a-f]{2}, ){15}0x[0-9a-f]{2})\}, /\* U\+([0-9A-F]{4}) \*/")
    return {int(m.group(2), 16): [int(v, 16) for v in m.group(1).split(", ")]
            for m in row_re.finditer(text)}


class Console:
    """Drives the example over the UART."""

    def __init__(self, vm, tmp_path):
        self.vm = vm
        self.tmp = tmp_path
        self.shots = 0

    def cmd(self, c):
        start = len(self.vm.serial)
        self.vm.send(c)
        ok = f"@RC stage=console ev=ok cmd={c}\n"
        deadline = time.monotonic() + 10
        # wait_for searches the whole log, so poll for an ok line after `start`.
        while ok not in self.vm.serial[start:]:
            assert time.monotonic() < deadline, f"no {ok!r}:\n{self.vm.serial}"
            time.sleep(0.01)
        return self.vm.serial[start:]

    def dump(self):
        out = self.cmd("d")
        m = re.search(r"@RC stage=console ev=screen (.*)\n((?:\|.*\n)*)" + re.escape(SCREEN_END), out)
        assert m, out
        head = dict(tok.split("=", 1) for tok in m.group(1).split())
        rows = [line[1:] for line in m.group(2).splitlines()]
        assert len(rows) == int(head["rows"]), out
        return head, rows

    def shot(self):
        self.shots += 1
        path = self.tmp / f"screen{self.shots}.ppm"
        self.vm.screendump(path)
        with Image.open(path) as img:
            img = img.convert("RGB")
        assert img.size == (W, H)
        return img


@pytest.fixture
def console(board, example_image, tmp_path):
    with boot(example_image("console"), board) as vm:
        vm.wait_for(READY, timeout=10)
        fb = [m for m in vm.markers() if m.get("ev") == "fb"]
        assert fb and (fb[0]["w"], fb[0]["h"], fb[0]["order"]) == ("640", "480", "rgb"), vm.serial
        assert int(fb[0]["pitch"]) >= W * 4
        yield Console(vm, tmp_path)
        assert "ev=exception" not in vm.serial, vm.serial


def check_cells(img, colour, skip):
    """Samples the centre and two corners of every cell except `skip`."""
    bad = []
    for row in range(ROWS):
        for col in range(COLS):
            if (col, row) == skip:
                continue
            x, y = col * FW, row * FH
            for p in ((x + 4, y + 8), (x, y), (x + 7, y + 15)):
                if img.getpixel(p) != colour:
                    bad.append((p, img.getpixel(p)))
    assert not bad, f"{len(bad)} pixels differ, first {bad[:5]}, want {colour}"


def check_glyph(img, glyph, col, row, fg, bg):
    for y in range(FH):
        for x in range(FW):
            want = fg if glyph[y] & (0x80 >> x) else bg
            got = img.getpixel((col * FW + x, row * FH + y))
            assert got == want, f"cell ({col},{row}) pixel ({x},{y}): {got} != {want}"


def test_console_text(console):
    head, rows = console.dump()
    assert (head["cols"], head["rows"], head["scheme"]) == ("80", "30", "green")
    assert rows[:4] == BANNER and not any(rows[4:])
    assert (head["cx"], head["cy"]) == ("2", "3")

    console.cmd("s")
    head, rows = console.dump()
    assert head["cy"] == str(ROWS - 1) and head["cx"] == "0"
    assert rows[-1] == ""
    for k in range(ROWS - 2):
        assert rows[-2 - k] == f"line {40 - k:02d}", rows

    console.cmd("c")
    head, rows = console.dump()
    assert not any(rows) and (head["cx"], head["cy"]) == ("0", "0")

    console.cmd("t")
    head, rows = console.dump()
    assert rows[:5] == TEXT_ROWS and not any(rows[5:]), rows
    assert (head["cx"], head["cy"]) == ("0", "5")

    for key in "2341":
        console.cmd(key)
        head2, rows2 = console.dump()
        assert head2["scheme"] == SCHEMES[key][0]
        assert rows2 == rows and (head2["cx"], head2["cy"]) == (head["cx"], head["cy"])


def test_console_pixels(console):
    _, bg, dim, normal, bright = SCHEMES["1"]
    img = console.shot()
    if os.environ.get("ROBCO_UPDATE_GOLDEN"):
        os.makedirs(os.path.dirname(GOLDEN), exist_ok=True)
        img.save(GOLDEN)
    with Image.open(GOLDEN) as golden:
        assert golden.convert("RGB").tobytes() == img.tobytes(), "banner differs from golden"

    font = load_font()
    console.cmd("c")
    console.cmd("t")
    img = console.shot()
    check_glyph(img, font[ord("A")], 0, 0, normal, bg)
    check_glyph(img, font[ord("X")], 2, 1, normal, bg)
    check_glyph(img, font[ord("d")], 0, 4, dim, bg)
    check_glyph(img, font[ord("b")], 11, 4, bright, bg)
    check_glyph(img, font[ord("i")], 18, 4, bg, normal)
    # The cursor at (0,5) is the inverted empty cell.
    check_glyph(img, font[ord(" ")], 0, 5, bg, normal)

    for key, (name, bg, _dim, normal, _bright) in SCHEMES.items():
        console.cmd(key)
        console.cmd("x")
        check_cells(console.shot(), normal, skip=(COLS - 1, ROWS - 1))
        console.cmd("c")
        check_cells(console.shot(), bg, skip=(0, 0))


def test_console_font_marker(console):
    names = [m["name"] for m in console.vm.markers() if m.get("ev") == "font"]
    assert names == ["spleen"], console.vm.serial


def check_glyph_grid(console, table):
    """Draws every byte the table covers and compares each cell with the table."""
    _, bg, _dim, normal, _bright = SCHEMES["1"]
    console.cmd("g")
    img = console.shot()
    cells = [(b, (b - 0x20) % COLS, (b - 0x20) // COLS) for b in range(0x20, 0x7F)]
    cells += [(b, (b - 0x80) % COLS, 2 + (b - 0x80) // COLS) for b in range(0x80, 0x100)]
    for b, col, row in cells:
        check_glyph(img, table[b], col, row, normal, bg)


def test_console_every_glyph(console):
    check_glyph_grid(console, load_table())


EXAMPLE = os.path.join(ROOT, "examples", "console")


def test_console_font_override(tmp_path):
    """A project font in SRCS replaces the built-in one at link time."""
    if not have_machine(BOARDS["raspi0"].binary, "raspi0"):
        pytest.skip("qemu-system-arm -M raspi0 not available")
    font_c = tmp_path / "font8x16_override.c"
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "font2c.py"),
                    os.path.join(ROOT, "third_party", "spleen", "spleen-8x16.bdf"),
                    "--name", "override-test", "-o", str(font_c)], check=True, cwd=ROOT)
    build = "build/override-test"
    subprocess.run(["make", "-s", "-C", EXAMPLE, "CI=1", f"BUILD={build}", f"FONT_C={font_c}"],
                   check=True, stdout=subprocess.DEVNULL)
    elf = os.path.join(EXAMPLE, build, "console.elf")
    nm = subprocess.run(["arm-none-eabi-nm", elf], check=True, capture_output=True, text=True).stdout
    for sym in ("font_ascii", "font8x16_glyph", "font8x16_name"):
        assert len(re.findall(rf" {sym}$", nm, re.M)) == 1, nm

    with boot(os.path.join(EXAMPLE, build, "console.img"), "raspi0") as vm:
        vm.wait_for(READY, timeout=10)
        names = [m["name"] for m in vm.markers() if m.get("ev") == "font"]
        assert names == ["override-test"], vm.serial
        check_glyph_grid(Console(vm, tmp_path), load_table(str(font_c)))
        assert "ev=exception" not in vm.serial, vm.serial
