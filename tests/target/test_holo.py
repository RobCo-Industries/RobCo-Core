"""Boot examples/holo on every board: the holotape slot with no card.

What QEMU can show: no SD card model attaches to SPI0 (canary V20), so
no card ever answers. Card detect, GPIO 12, is driven through qtest.
These tests check that the slot stays off with detect high, that attach
with detect low fails by its CMD0 timeout and leaves the power and the
bus off, that the stepped attach fails the same way in many short steps,
that insertion and removal are reported, and that the example keeps
answering. The protocol itself runs against the host card model,
tests/host/test_sdspi.c and test_holo.c. Real cards need hardware.
"""

import time

from harness.boot import boot
from harness.pigpio import PiGpio
from harness.qemu import QemuError

CD, WP, BUS, PWR, CS = 12, 13, 20, 21, 7
CMD_TIMEOUT = 30
CMD0_MS = 100           # SDSPI_CMD0_US
STEP_US_MAX_QEMU = 50000  # one step on a host-timed QEMU; real cards: HARDWARE_VALIDATION.md


def events(vm, ev):
    return [m for m in vm.markers() if m.get("stage") == "holo" and m.get("ev") == ev]


def wait_event(vm, ev, before):
    deadline = time.monotonic() + CMD_TIMEOUT
    while True:
        got = events(vm, ev)
        if len(got) > before:
            return got[before]
        if time.monotonic() > deadline:
            raise QemuError(f"no ev={ev}. serial:\n{vm.serial}")
        if vm.qemu.proc.poll() is not None:
            raise QemuError(f"QEMU exited waiting for ev={ev}. serial:\n{vm.serial}")
        time.sleep(0.02)


def cmd(vm, line, ev):
    """Send one command and return the marker it answers with."""
    before = len(events(vm, ev))
    vm.send(line + "\r")
    return wait_event(vm, ev, before)


def slot_off(gpio):
    assert (gpio.get(PWR), gpio.get(BUS), gpio.get(CS)) == (0, 0, 1)


def test_holo_no_card(board, example_image):
    with boot(example_image("holo"), board, qtest=True, gpio_high=[CD, WP]) as vm:
        vm.wait_for("ev=ready", timeout=CMD_TIMEOUT)
        gpio = PiGpio(vm, vm.qmp.machine)
        assert events(vm, "init")[0]["rc"] == "ok", vm.serial
        pins = events(vm, "pins")[0]
        assert (pins["cd"], pins["pwr"], pins["bus"], pins["cs"]) == ("1", "0", "0", "1"), vm.serial
        assert (pins["card"], pins["present"], pins["attached"]) == ("0", "0", "0"), vm.serial
        slot_off(gpio)

        # Detect high: attach refuses and never powers the slot.
        assert cmd(vm, "a", "attach")["rc"] == "nomedia", vm.serial
        slot_off(gpio)
        assert cmd(vm, "r 0 1", "read")["rc"] == "nomedia", vm.serial
        assert cmd(vm, "i", "card")["type"] == "none", vm.serial
        slot_off(gpio)

        # Detect low, but nothing on SPI0: inserted after the debounce,
        # then attach fails by the CMD0 timeout and turns the slot off.
        before = len(events(vm, "inserted"))
        gpio.set(CD, 0)
        wait_event(vm, "inserted", before)
        a = cmd(vm, "a", "attach")
        assert a["rc"] == "nocard", vm.serial
        assert int(a["ms"]) >= CMD0_MS, vm.serial
        slot_off(gpio)
        pins = cmd(vm, "p", "pins")
        assert (pins["cd"], pins["pwr"], pins["bus"], pins["attached"]) == ("0", "0", "0", "0"), \
            vm.serial
        assert pins["present"] == "1", vm.serial

        # The stepped attach: the same failure, in many short steps.
        g = cmd(vm, "g", "steps")
        assert g["rc"] == "nocard", vm.serial
        assert int(g["ms"]) >= CMD0_MS, vm.serial
        assert int(g["steps"]) > CMD0_MS // 10, vm.serial
        assert int(g["max_us"]) < STEP_US_MAX_QEMU, vm.serial
        slot_off(gpio)

        # Removal is reported, and the example still answers.
        before = len(events(vm, "removed"))
        gpio.set(CD, 1)
        wait_event(vm, "removed", before)
        slot_off(gpio)
        assert cmd(vm, "p", "pins")["cd"] == "1", vm.serial
        assert cmd(vm, "d", "detach") is not None
        assert cmd(vm, "x", "error")["what"] == "unknown_command"
        slot_off(gpio)
        assert not [m for m in vm.markers() if m.get("ev") == "exception"], vm.serial
