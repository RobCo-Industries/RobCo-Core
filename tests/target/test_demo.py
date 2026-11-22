"""Boot examples/demo on every board: every component in one image.

The tick IRQ fires while the main loop prints to the screen console and
the UART, and the heap, RNG and GPIO run beside it.
"""

import re

import pytest

from harness.boot import boot

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
