"""Prove tests/harness/boot.py with the M0 spikes as payloads."""

import re
import time
import zlib

import pytest
from PIL import Image

from harness.boot import BOARDS, boot
from harness.qemu import QemuError
from harness.sdimage import make_sd_image, read_sector

SOC = {"raspi0": ("bcm2835", "0x20000000"), "raspi1ap": ("bcm2835", "0x20000000"),
       "raspi2b": ("bcm2836", "0x3f000000"), "raspi3b": ("bcm2837", "0x3f000000")}
ENTRY = {"raspi0": "svc", "raspi1ap": "svc", "raspi2b": "hyp", "raspi3b": "hyp"}
HELLO = re.compile(r"@RC stage=spike1 ev=hello .*\n")


def no_exception(vm):
    assert not [m for m in vm.markers() if m.get("ev") == "exception"], vm.serial


def test_s1_hello(board, spike_image):
    with boot(spike_image("s1"), board) as vm:
        vm.wait_for("type to echo", timeout=10)
        time.sleep(0.2)  # give parked cores a chance to misbehave
        hello = [m for m in vm.markers() if m.get("ev") == "hello"]
        atags = vm.qmp.hmp("xp/10wx 0x100").split()
        no_exception(vm)
    assert len(hello) == 1, vm.serial
    m = hello[0]
    assert (m["soc"], m["base"]) == SOC[board]
    assert m["entry_mode"] == ENTRY[board]
    assert m["fw_r0"] == "0x0"
    assert int(m["fw_r1"], 16) == BOARDS[board].mach_id
    assert m["fw_r2"] == "0x100"
    # ATAG_CORE (5 words), then ATAG_MEM with the board's RAM size.
    words = [int(w, 16) for w in atags if w.startswith("0x") and len(w) == 10]
    assert words[:2] == [5, 0x54410001]
    assert words[5:8] == [4, 0x54410002, BOARDS[board].mem]


def test_s1_svc_mode(board, spike_image):
    with boot(spike_image("s1"), board, mode="svc") as vm:
        vm.wait_for(HELLO, timeout=10)
        assert vm.markers()[0]["entry_mode"] == "svc"


def test_s1_echo(board, spike_image):
    with boot(spike_image("s1"), board) as vm:
        vm.wait_for("type to echo\n", timeout=10)
        vm.send("abc")
        vm.wait_for(re.compile(r"type to echo\r?\nabc"), timeout=5)


def test_s2_sd_read(board, spike_image, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 64, {"kernel.img": b"\0" * 4096, "robco/motd.txt": b"hello\n"})
    sector0 = read_sector(sd, 0)
    with boot(spike_image("s2"), board, sd=sd) as vm:
        vm.wait_for(re.compile(r"ev=(read|error|exception) .*\n"), timeout=15)
        read = [m for m in vm.markers() if m.get("ev") == "read"]
        no_exception(vm)
    assert read and read[0]["ok"] == "1", vm.serial
    assert int(read[0]["crc32"], 16) == zlib.crc32(sector0)
    assert read[0]["sig"] == "0xaa55"
    assert read[0]["p0_type"] == "0x0c"
    assert read[0]["p0_lba"] == "2048"


BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def test_s3_screen(board, spike_image, tmp_path):
    shot = tmp_path / "screen.ppm"
    with boot(spike_image("s3"), board) as vm:
        vm.wait_for("ev=done", timeout=20)
        no_exception(vm)
        vm.screendump(shot)
    with Image.open(shot) as im:
        img = im.convert("RGB")
    assert img.size == (640, 480)
    bar_w = 640 // 8
    got = [img.getpixel((i * bar_w + bar_w // 2, 240)) for i in range(8)]
    assert got == BARS


def test_wait_for_timeout_cleans_up(spike_image):
    start = time.monotonic()
    with pytest.raises(QemuError) as err:
        with boot(spike_image("s1"), "raspi0") as vm:
            proc = vm.qmp.proc
            vm.wait_for("this never comes", timeout=1.0)
    assert time.monotonic() - start < 5
    assert "ev=hello" in str(err.value)  # the captured log is in the error
    # The CI container has no pgrep. close() reaped QEMU, so poll() is set.
    assert proc.poll() is not None
