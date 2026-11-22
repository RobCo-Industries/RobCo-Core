"""Boot examples/demo on every board: every component in one image.

The tick IRQ fires while the main loop prints to the screen console and
the UART, and the heap, RNG and GPIO run beside it.
"""

import re
import time

import pytest

from harness.boot import boot
from harness.sdimage import make_sd_image

DONE = re.compile(r"^@RC stage=demo ev=done ticks=(\d+) elapsed_ms=(\d+)\n", re.M)
SCREEN = re.compile(r"@RC stage=demo ev=screen .*\n((?:\|.*\n)*)@RC stage=demo ev=screen_end\n")
STATUS = re.compile(r"^\|uptime=(\d+)ms ticks=(\d+) used=(\d+) free=\d+ allocs=(\d+) "
                    r"rng=0x[0-9a-f]{8} gpio5=([01])$")


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "demo" and m.get("ev") == name]


# "mmu" turns on the MMU and caches before any other component.
VARIANTS = {"plain": None, "mmu": ("mmu", "-DDEMO_MMU=1")}


@pytest.mark.parametrize("variant", list(VARIANTS))
def test_demo(board, example_image, variant):
    img = example_image("demo", *VARIANTS[variant]) if VARIANTS[variant] else example_image("demo")
    with boot(img, board) as vm:
        done = vm.wait_for(DONE, timeout=60)
        dump = vm.wait_for(SCREEN, timeout=30)
        assert len(events(vm, "boot")) == 1, vm.serial
        mmu = events(vm, "mmu")
        assert [m["on"] for m in mmu] == (["1"] if variant == "mmu" else []), vm.serial
        assert not events(vm, "error"), vm.serial
        heap = events(vm, "heap")
        assert heap and heap[0]["base"] == "0x01000000", vm.serial
        # The guest waits 3 s on its own timer. The tick counts every period
        # by that timer, even late ones, so it cannot run ahead of it. The
        # lower bound leaves room for a late handler on a stalled host.
        ticks, elapsed_ms = int(done.group(1)), int(done.group(2))
        assert elapsed_ms >= 3000, vm.serial
        assert 100 <= ticks <= elapsed_ms // 10 + 1, vm.serial

        text = dump.group(1)
        rows = [STATUS.match(line) for line in text.splitlines()]
        status = [m.groups() for m in rows if m]
        assert len(status) == 3, text
        uptime = [int(s[0]) for s in status]
        ticks = [int(s[1]) for s in status]
        assert uptime[0] < uptime[1] < uptime[2], text
        assert ticks[0] < ticks[1] < ticks[2], text
        assert [int(s[3]) for s in status] == [1, 2, 3], text
        assert [s[4] for s in status] == ["1", "0", "1"], text
        assert "@RC stage=demo ev=done" in text, text
        assert not events(vm, "exception"), vm.serial


# The menu variant: MMU and caches, SD card, INI, serial input, audio trace and the clock.
MENU = ("menu", "-DDEMO_MENU=1 -DROBCO_AUDIO_BACKEND=2")
ITEMS = ["Diagnostics", "Event log", "Set clock", "Reboot"]
KEYS = {"up": "\x1b[A", "down": "\x1b[B", "enter": "\r"}
MENU_TIMEOUT = 60


def highlighted(dump):
    """Index of the item drawn with "> " in a console dump."""
    rows = [line[1:] for line in dump.splitlines() if line.startswith("|")]
    sel = [i for i, name in enumerate(ITEMS) if f"> {name}" in rows]
    others = [i for i, name in enumerate(ITEMS) if f"  {name}" in rows]
    assert len(sel) == 1 and len(others) == len(ITEMS) - 1, dump
    return sel[0]


def last_dump(vm, count):
    """Waits for console dump number `count` and returns its rows."""
    vm.wait_for(re.compile(r"(?:@RC stage=demo ev=screen_end\n(?:.*\n)*?){%d}" % count),
                timeout=MENU_TIMEOUT)
    return SCREEN.findall(vm.serial)[count - 1]


@pytest.mark.parametrize("card", ["sd", "nocard"])
def test_demo_menu(board, example_image, need_mtools, tmp_path, card):
    if board not in ("raspi0", "raspi2b"):
        pytest.skip("the menu runs on raspi0 and raspi2b")
    sd = None
    start = 0
    if card == "sd":
        start = 2
        sd = tmp_path / "sd.img"
        make_sd_image(sd, 16, {"DEMO.INI": b"[menu]\r\nstart = 2\r\ntitle = VAULT-TEC MENU\r\n"})
    with boot(example_image("demo", *MENU), board, sd=sd) as vm:
        vm.wait_for(re.compile(r"ev=ready sel=\d+\n"), timeout=MENU_TIMEOUT)
        assert events(vm, "mmu")[0]["on"] == "1", vm.serial
        cfg = events(vm, "config")[0]
        if card == "sd":
            assert (cfg["card"], cfg["mount"], cfg["ini"]) == ("ok", "ok", "ok"), vm.serial
        else:
            assert (cfg["card"], cfg["mount"], cfg["ini"]) == ("none", "none", "none"), vm.serial
        assert int(cfg["start"]) == start
        assert events(vm, "clock")[0]["state"] == "unset", vm.serial
        a = events(vm, "audio_init")[0]
        assert (a["rc"], a["backend"]) == ("0", "trace"), vm.serial
        assert int(events(vm, "ready")[0]["sel"]) == start
        dump = last_dump(vm, 1)
        assert highlighted(dump) == start
        assert "Clock: not set" in dump
        assert ("VAULT-TEC MENU" if card == "sd" else "ROBCO INDUSTRIES TERMLINK") in dump

        def tones():
            return [m for m in events(vm, "audio") if m.get("op") == "tone" and "t_ms" in m]

        def wait_tones(n):
            deadline = time.monotonic() + MENU_TIMEOUT
            while len(tones()) < n and time.monotonic() < deadline:
                time.sleep(0.02)

        # One click per key press, through the trace backend. A request
        # replaces one the tick has not started yet, so the test waits for
        # each click before the next key.
        sel = start
        presses = 0
        for n, key in enumerate(["down", "down", "up", "down", "down", "enter"], start=2):
            vm.send(KEYS[key])
            presses += 1
            dump = last_dump(vm, n)
            if key != "enter":
                sel = (sel + (1 if key == "down" else -1)) % len(ITEMS)
                assert int(events(vm, "menu")[-1]["sel"]) == sel, vm.serial
            assert highlighted(dump) == sel, dump
            wait_tones(presses)
        assert int(events(vm, "select")[0]["sel"]) == sel
        assert len(tones()) == presses, vm.serial
        assert all(m["hz"] == "2000" for m in tones()), vm.serial
        assert not events(vm, "error"), vm.serial
        assert not events(vm, "exception"), vm.serial
