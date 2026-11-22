"""Boot examples/demo on every board: every component in one image.

The tick IRQ fires while the main loop prints to the screen console and
the UART, and the heap, RNG and GPIO run beside it.
"""

import re

from harness.boot import boot

DONE = re.compile(r"^@RC stage=demo ev=done ticks=(\d+)\n", re.M)
SCREEN = re.compile(r"@RC stage=demo ev=screen .*\n((?:\|.*\n)*)@RC stage=demo ev=screen_end\n")
STATUS = re.compile(r"^\|uptime=(\d+)ms ticks=(\d+) used=(\d+) free=\d+ allocs=(\d+) "
                    r"rng=0x[0-9a-f]{8} gpio5=([01])$")


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "demo" and m.get("ev") == name]


def test_demo(board, example_image):
    with boot(example_image("demo"), board) as vm:
        done = vm.wait_for(DONE, timeout=20)
        dump = vm.wait_for(SCREEN, timeout=10)
        assert len(events(vm, "boot")) == 1, vm.serial
        assert not events(vm, "error"), vm.serial
        heap = events(vm, "heap")
        assert heap and heap[0]["base"] == "0x01000000", vm.serial
        assert 250 <= int(done.group(1)) <= 600, vm.serial

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
