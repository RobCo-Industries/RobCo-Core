"""Boot examples/hello through boot() on every board."""

import re
import time

import pytest

from harness.boot import boot

SOC = {"raspi0": ("bcm2835", "0x20000000"), "raspi1ap": ("bcm2835", "0x20000000"),
       "raspi2b": ("bcm2836", "0x3f000000"), "raspi3b": ("bcm2837", "0x3f000000")}
ENTRY = {"raspi0": "svc", "raspi1ap": "svc", "raspi2b": "hyp", "raspi3b": "hyp"}
PWM = {"raspi0": "abort", "raspi1ap": "abort", "raspi2b": "ok", "raspi3b": "ok"}
MAGIC = b"BOOT\0\0\0\0"
READY = re.compile(r"@RC stage=hello ev=ready\n")
EXCEPTION = re.compile(r"@RC stage=hello ev=exception .*\n")


def hello_image(example_image, fault=False):
    if fault:
        return example_image("hello", "fault", "-DHELLO_FAULT=1")
    return example_image("hello")


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "hello" and m.get("ev") == name]


def check_boot(vm, board):
    boots = events(vm, "boot")
    assert len(boots) == 1, vm.serial
    assert (boots[0]["soc"], boots[0]["base"]) == SOC[board]
    assert boots[0]["entry_mode"] == ENTRY[board]
    probes = events(vm, "probe")
    assert len(probes) == 1, vm.serial
    assert probes[0]["pwm"] == PWM[board]
    assert probes[0]["gpio"] == "ok"
    assert events(vm, "ready"), vm.serial


@pytest.mark.parametrize("fault", [False, True], ids=["normal", "fault"])
def test_hello_magic(example_image, fault):
    with open(hello_image(example_image, fault), "rb") as f:
        head = f.read(0x28)
    assert head[0x20:0x28] == MAGIC


def test_hello(board, example_image):
    with boot(hello_image(example_image), board) as vm:
        vm.wait_for(READY, timeout=10)
        time.sleep(0.5)  # give a late exception a chance to show up
        check_boot(vm, board)
        assert not events(vm, "exception"), vm.serial


def test_hello_fault(board, example_image):
    with boot(hello_image(example_image, fault=True), board) as vm:
        vm.wait_for(EXCEPTION, timeout=10)
        check_boot(vm, board)
        exc = events(vm, "exception")
        assert len(exc) == 1, vm.serial
        assert exc[0]["type"] == "undef"
