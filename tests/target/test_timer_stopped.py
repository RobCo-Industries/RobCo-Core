"""A system timer that never advances must not hang the serial setup or the audio probe.

examples/hello built with ROBCO_FAULT_TIMER_STOPPED reads a constant
counter from the start. QEMU cannot stop its timer from outside, so the
fault lives in a test build of the library only.
"""

import os
import re
import subprocess

from harness.boot import boot

READY = re.compile(r"@RC stage=hello ev=ready\n")
HOOK = "robco_fault_timer_stopped"
AUDIO_ERR_TIMER = -13
DEAD = ("deadtimer", "-DROBCO_FAULT_TIMER_STOPPED=1 -DHELLO_DEAD_TIMER=1")


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "hello" and m.get("ev") == name]


def one(vm, name):
    found = events(vm, name)
    assert len(found) == 1, f"{name}:\n{vm.serial}"
    return found[0]


def test_stopped_timer_does_not_hang(board, example_image):
    with boot(example_image("hello", *DEAD), board) as vm:
        vm.wait_for(READY, timeout=60)
        # uart_init() set the pins without the timer: the banner is out.
        assert one(vm, "boot")["entry_mode"], vm.serial
        assert one(vm, "fault_hook")["name"] == "timer_stopped", vm.serial
        assert one(vm, "timer")["running"] == "0", vm.serial
        audio = one(vm, "audio")
        assert audio["tick"] == "0", vm.serial
        assert audio["rc"] == str(AUDIO_ERR_TIMER), vm.serial
        assert not events(vm, "exception"), vm.serial


def nm(image):
    elf = os.path.splitext(image)[0] + ".elf"
    return subprocess.run(["arm-none-eabi-nm", elf], check=True, capture_output=True,
                          text=True).stdout


def test_shipped_library_has_no_fault_hook(example_image):
    """The hook exists only in the fault build, never in the default library."""
    shipped = example_image("hello")
    assert HOOK not in nm(shipped)
    lib = os.path.join(os.path.dirname(shipped), "librobco.a")
    out = subprocess.run(["arm-none-eabi-nm", lib], check=True, capture_output=True,
                         text=True).stdout
    assert "timer_now_us" in out
    assert HOOK not in out
    assert HOOK in nm(example_image("hello", *DEAD))
