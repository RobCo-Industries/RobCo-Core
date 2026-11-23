"""Boot examples/wdog on every board: the power management watchdog.

What QEMU models: QEMU 10.1's bcm2835_powermgt resets the machine at the
PM_RSTC write that selects a full reset. It does not count PM_WDOG down,
so the timeout, wdog_kick() and wdog_stop() have no effect there, and a
reset sets PM_RSTS back to 0x1000. This holds on raspi0, raspi1ap,
raspi2b and raspi3b alike.

So on QEMU these tests check the argument errors, that an armed watchdog
resets the board and the board boots again, and the register sequence of
power_reboot() and power_halt(). The parts that need a counting watchdog
detect the early reset and skip by name: if a later QEMU models the
counter, they run. Hardware validation is pending.
"""

import re
import time

import pytest

from harness.boot import boot

READY = re.compile(r"@RC stage=wdog ev=ready\n")
TIMEOUT_S = 1.0
ALLOWANCE_S = 3.0       # serial, QMP and host scheduling
NO_COUNTER = ("QEMU 10.1 bcm2835_powermgt resets at the PM_RSTC write "
              "and does not count PM_WDOG")


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "wdog" and m.get("ev") == name]


def wait_boots(vm, count, timeout=30):
    deadline = time.monotonic() + timeout
    while len(events(vm, "ready")) < count:
        assert time.monotonic() < deadline, vm.serial
        time.sleep(0.05)


def ready(vm):
    vm.wait_for(READY, timeout=30)
    boot_ev = events(vm, "boot")[0]
    assert boot_ev["running"] == "0", vm.serial
    args = events(vm, "args")[0]
    assert (args["zero"], args["over"]) == ("-1", "-1"), vm.serial
    assert args["kick"] == "-3", vm.serial
    assert (args["timeout"], args["remaining"]) == ("0", "0"), vm.serial


def reset_before_armed(vm, ev):
    """Sends `ev`'s key, returns True when the board reset at the arming write."""
    vm.send(ev[0])
    vm.wait_for(f"ev={ev} ", timeout=15)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if events(vm, "armed"):
            return False
        if len(events(vm, "boot")) > 1:
            return not events(vm, "armed")
        time.sleep(0.02)
    raise AssertionError(vm.serial)


def test_wdog_args(board, example_image):
    with boot(example_image("wdog"), board) as vm:
        ready(vm)
        assert not events(vm, "exception"), vm.serial


def test_wdog_fires(board, example_image):
    """No kicks: the board resets within the timeout plus the allowance and boots again."""
    with boot(example_image("wdog"), board) as vm:
        ready(vm)
        vm.send("f")
        vm.wait_for("ev=fire ", timeout=15)
        t0 = time.monotonic()
        assert vm.qmp.wait_event("RESET", timeout=TIMEOUT_S + ALLOWANCE_S)["data"]["guest"] is True
        assert time.monotonic() - t0 < TIMEOUT_S + ALLOWANCE_S
        wait_boots(vm, 2)
        assert not events(vm, "late"), vm.serial
        assert events(vm, "boot")[1]["running"] == "0", vm.serial
        assert not events(vm, "exception"), vm.serial


def test_wdog_kick_keeps_up(board, example_image):
    """Kicks for three timeouts, then stops: no reset."""
    with boot(example_image("wdog"), board) as vm:
        ready(vm)
        if reset_before_armed(vm, "kick"):
            pytest.skip(f"{board}: {NO_COUNTER}")
        vm.wait_for("ev=stopped", timeout=15)
        kicked = events(vm, "kicked")[0]
        assert (kicked["rc"], kicked["bad"], kicked["running"]) == ("0", "0", "1"), vm.serial
        assert int(kicked["n"]) >= 20, vm.serial
        assert 0 < int(kicked["remaining"]) <= 1000, vm.serial
        assert events(vm, "stopped")[0]["running"] == "0"
        time.sleep(TIMEOUT_S + ALLOWANCE_S)
        assert not [e for e in vm.qmp.events if e["event"] == "RESET"]
        assert len(events(vm, "boot")) == 1, vm.serial


def test_wdog_stop_stays_up(board, example_image):
    """Arms, stops at once, then stays up past three timeouts."""
    with boot(example_image("wdog"), board) as vm:
        ready(vm)
        if reset_before_armed(vm, "stop"):
            pytest.skip(f"{board}: {NO_COUNTER}")
        vm.wait_for("ev=stayed", timeout=15)
        stayed = events(vm, "stayed")[0]
        assert (stayed["rc"], stayed["running"]) == ("0", "0"), vm.serial
        assert not [e for e in vm.qmp.events if e["event"] == "RESET"]
        assert len(events(vm, "boot")) == 1, vm.serial


def test_wdog_reboot_while_running(board, example_image):
    """power_reboot() with a 16 s watchdog running resets at once, not after 16 s."""
    with boot(example_image("wdog"), board) as vm:
        ready(vm)
        early = reset_before_armed(vm, "reboot")
        assert vm.qmp.wait_event("RESET", timeout=5)["data"]["guest"] is True
        wait_boots(vm, 2)
        assert not events(vm, "exception"), vm.serial
        if early:
            pytest.skip(f"{board}: reset came from wdog_start(): {NO_COUNTER}")


def test_wdog_halt_while_running(board, example_image):
    """power_halt() with a watchdog running still halts: QEMU sends SHUTDOWN."""
    with boot(example_image("wdog"), board) as vm:
        ready(vm)
        if reset_before_armed(vm, "halt"):
            pytest.skip(f"{board}: {NO_COUNTER}")
        ev = vm.qmp.wait_event("SHUTDOWN", timeout=5)
        assert ev["data"] == {"guest": True, "reason": "guest-shutdown"}
        assert not [e for e in vm.qmp.events if e["event"] == "RESET"]
        assert len(events(vm, "boot")) == 1, vm.serial
