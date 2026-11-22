"""Boot examples/tick through boot() on every board: system timer IRQs and the tick."""

import re

from harness.boot import boot

DONE = re.compile(r"@RC stage=tick ev=done .*\n")
ONESHOT = re.compile(r"@RC stage=tick ev=oneshot .*\n")
SLOW = re.compile(r"@RC stage=tick ev=slow .*\n")
TWO_ALIVE = re.compile(r"ev=alive .*\n(?:.*\n)*?.*ev=alive .*\n")


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "tick" and m.get("ev") == name]


def test_tick(board, example_image):
    with boot(example_image("tick"), board) as vm:
        vm.wait_for(DONE, timeout=60)
        assert len(events(vm, "boot")) == 1, vm.serial
        delay = events(vm, "delay")
        assert len(delay) == 1 and delay[0]["ok"] == "1", vm.serial

        done = events(vm, "done")
        assert len(done) == 1, vm.serial
        assert int(done[0]["ticks"]) == 200
        # Tick 200 is due 2 s after tick_init() by the guest timer and never
        # runs early. A stalled host can only make it later.
        assert int(done[0]["elapsed_us"]) >= 2_000_000, vm.serial
        assert int(done[0]["spurious"]) == 0, vm.serial

        vm.wait_for(ONESHOT, timeout=30)
        assert len(events(vm, "oneshot")) == 1, vm.serial

        ticks = [int(m["n"]) for m in events(vm, "tick")]
        assert ticks[:5] == [1, 2, 3, 4, 5], vm.serial
        assert {50, 100, 150, 200} <= set(ticks), vm.serial

        # The main loop still echoes UART input while ticks run.
        vm.send("echo-tick-42\r")
        vm.wait_for("echo-tick-42\n", timeout=30)

        vm.wait_for(TWO_ALIVE, timeout=30)
        alive = [int(m["ticks"]) for m in events(vm, "alive")]
        assert len(alive) >= 2, vm.serial
        assert all(a < b for a, b in zip(alive, alive[1:])), vm.serial

        assert not events(vm, "exception"), vm.serial


def test_tick_slow_callback(board, example_image):
    """A callback slower than the period must not starve the main loop."""
    with boot(example_image("tick", "slow", "-DTICK_SLOW=1"), board) as vm:
        vm.wait_for(SLOW, timeout=60)
        slow = events(vm, "slow")[0]
        ticks, calls, missed = (int(slow[k]) for k in ("ticks", "calls", "missed"))
        assert ticks >= 100, vm.serial
        assert missed > 0, vm.serial
        # Callbacks run once per interrupt; the rest is counted as missed.
        # The example reads these with IRQs masked, so they add up exactly.
        assert calls + missed == ticks, vm.serial
        assert int(slow["loops"]) > 0, vm.serial
        # tick_ms() follows the guest timer, ticks at 100 Hz are 10 ms each.
        # Counted periods cannot run ahead of the time the guest measured.
        assert int(slow["ms"]) == ticks * 10, vm.serial
        assert int(slow["ms"]) * 1000 <= int(slow["elapsed_us"]), vm.serial

        vm.send("echo-slow-7\r")
        vm.wait_for("echo-slow-7\n", timeout=30)
        assert not events(vm, "exception"), vm.serial


def test_tick_init_order(board, example_image):
    """tick_init() before irq_init() fails cleanly, and a second irq_init() keeps handlers."""
    with boot(example_image("tick", "order", "-DTICK_ORDER=1"), board) as vm:
        vm.wait_for(DONE, timeout=60)
        order = events(vm, "order")
        assert len(order) == 1, vm.serial
        # TICK_ERR_NO_IRQ, then 0 for the same rate, TICK_ERR_RUNNING for another.
        assert order[0] == {**order[0], "early": "-3", "same": "0", "other": "-2"}, vm.serial
        done = events(vm, "done")[0]
        assert int(done["spurious"]) == 0, vm.serial
        vm.wait_for(ONESHOT, timeout=30)
        assert not events(vm, "exception"), vm.serial
