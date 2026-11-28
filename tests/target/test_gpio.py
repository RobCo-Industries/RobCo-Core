"""Boot examples/gpio on every board: GPIO, RNG and power drivers."""

import re
import time

from harness.boot import boot

READY = re.compile(r"@RC stage=gpio ev=ready\n")
OUTPUT, ALT0 = "1", "4"


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "gpio" and m.get("ev") == name]


def wait_boots(vm, count, timeout=30):
    deadline = time.monotonic() + timeout
    while len(events(vm, "ready")) < count:
        assert time.monotonic() < deadline, vm.serial
        time.sleep(0.05)


def test_gpio(board, example_image):
    with boot(example_image("gpio"), board) as vm:
        vm.wait_for(READY, timeout=30)
        time.sleep(0.3)
        assert len(events(vm, "boot")) == 1, vm.serial

        out = events(vm, "out")
        assert len(out) == 2, vm.serial
        assert (out[0]["pin5"], out[0]["pin6"]) == ("1", "0")
        assert (out[1]["pin5"], out[1]["pin6"]) == ("0", "1")
        for o in out:
            assert (o["fsel5"], o["fsel6"]) == (OUTPUT, OUTPUT)

        inp = events(vm, "in")
        assert len(inp) == 1 and inp[0]["fsel17"] == "0", vm.serial
        # QEMU reads 0 for inputs and does not model pulls.
        assert inp[0]["pin17"] == "0"

        pins = events(vm, "uartpins")
        assert len(pins) == 1, vm.serial
        assert (pins[0]["fsel14"], pins[0]["fsel15"]) == (ALT0, ALT0)

        assert events(vm, "badpin")[0]["rc"] == "1"

        rng = events(vm, "rng")
        assert len(rng) == 1, vm.serial
        assert rng[0]["rc"] == "0"
        assert rng[0]["a"] != rng[0]["b"]
        # The time from rng_init() to the first word. QEMU has a word at
        # once, so there is no bound to check here. A real board waits for
        # the warm-up, see docs/HARDWARE_VALIDATION.md.
        assert rng[0]["us"].isdigit(), rng[0]
        assert not events(vm, "exception"), vm.serial


def test_gpio_reboot(board, example_image):
    with boot(example_image("gpio"), board) as vm:
        vm.wait_for(READY, timeout=30)
        vm.send("r")
        vm.wait_for("ev=reboot", timeout=15)
        assert vm.qmp.wait_event("RESET", timeout=15)["data"]["guest"] is True
        wait_boots(vm, 2)
        assert len(events(vm, "boot")) == 2, vm.serial
        assert not events(vm, "exception"), vm.serial


def test_gpio_halt(board, example_image):
    """QEMU reads the halt partition (0x555) as a power-off request.

    It sends SHUTDOWN, not RESET.
    """
    with boot(example_image("gpio"), board) as vm:
        vm.wait_for(READY, timeout=30)
        vm.send("h")
        vm.wait_for("ev=halt", timeout=15)
        ev = vm.qmp.wait_event("SHUTDOWN", timeout=15)
        assert ev["data"] == {"guest": True, "reason": "guest-shutdown"}
        assert not [e for e in vm.qmp.events if e["event"] == "RESET"]
        assert len(events(vm, "boot")) == 1, vm.serial
        assert not events(vm, "exception"), vm.serial
