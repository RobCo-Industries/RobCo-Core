"""Boot examples/tick through boot() on every board: system timer IRQs and the tick."""

import re

from harness.boot import boot

DONE = re.compile(r"@RC stage=tick ev=done .*\n")
ONESHOT = re.compile(r"@RC stage=tick ev=oneshot .*\n")
TWO_ALIVE = re.compile(r"ev=alive .*\n(?:.*\n)*?.*ev=alive .*\n")


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "tick" and m.get("ev") == name]


def test_tick(board, example_image):
    with boot(example_image("tick"), board) as vm:
        vm.wait_for(DONE, timeout=15)
        assert len(events(vm, "boot")) == 1, vm.serial

        done = events(vm, "done")
        assert len(done) == 1, vm.serial
        assert int(done[0]["ticks"]) == 200
        # QEMU timing is loose; 200 ticks at 100 Hz is 2 s on hardware.
        assert 1_600_000 <= int(done[0]["elapsed_us"]) <= 4_000_000, vm.serial
        assert int(done[0]["spurious"]) == 0, vm.serial

        vm.wait_for(ONESHOT, timeout=5)
        assert len(events(vm, "oneshot")) == 1, vm.serial

        ticks = [int(m["n"]) for m in events(vm, "tick")]
        assert ticks[:5] == [1, 2, 3, 4, 5], vm.serial
        assert {50, 100, 150, 200} <= set(ticks), vm.serial

        # The main loop still echoes UART input while ticks run.
        vm.send("echo-tick-42\r")
        vm.wait_for("echo-tick-42\n", timeout=5)

        vm.wait_for(TWO_ALIVE, timeout=6)
        alive = [int(m["ticks"]) for m in events(vm, "alive")]
        assert len(alive) >= 2, vm.serial
        assert all(a < b for a, b in zip(alive, alive[1:])), vm.serial

        assert not events(vm, "exception"), vm.serial
