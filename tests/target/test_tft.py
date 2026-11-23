"""Boot examples/tft on every board: the ILI9341 driver on QEMU's SPI0.

What QEMU proves: its bcm2835-spi model is present on every board and a
transfer completes with nothing on the bus (see test_spi.py and canary
V16). No panel model exists, so every byte read is 0. The example runs
the driver's init sequence and draws colour bars and a border. It prints
a CRC-32 of every byte it sent with its DC level. build/host/tft_tool
runs the same calls on the fake bus of the host tests and prints the
CRC it expects, so the test compares the two and hard-codes no value.
tft_read_id() must report no panel, without hanging.

What it cannot check: the picture on a real panel, timing, DC and chip
select levels on the wire. That needs hardware.
"""

import os
import re
import subprocess

from harness.boot import boot
from harness.env import minimal_env

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "build", "host", "tft_tool")
DONE = re.compile(r"@RC stage=tft ev=done\n")
TFT_ERR_NO_PANEL = "-6"


def expected():
    subprocess.run(["make", "--no-print-directory", "build/host/tft_tool"], cwd=ROOT,
                   check=True, capture_output=True, env=minimal_env())
    out = subprocess.run([TOOL], check=True, capture_output=True, text=True,
                         env=minimal_env()).stdout
    m = re.fullmatch(r"bytes=(\d+) crc=([0-9a-f]{8})\n", out)
    assert m, out
    return m.group(1), m.group(2)


def event(vm, name):
    found = [m for m in vm.markers() if m.get("stage") == "tft" and m.get("ev") == name]
    assert len(found) == 1, f"{name}:\n{vm.serial}"
    return found[0]


def test_tft_panel_bus(board, example_image):
    want_bytes, want_crc = expected()
    with boot(example_image("tft"), board) as vm:
        vm.wait_for(DONE, timeout=60)
        assert not [m for m in vm.markers() if m.get("ev") in ("nospi", "exception")], vm.serial
        init = event(vm, "init")
        assert init["rc"] == "0", vm.serial
        assert (init["w"], init["h"]) == ("320", "240")
        # 10 MHz default; the SPI driver never rounds above it.
        assert 0 < int(init["hz"]) <= 10000000, vm.serial
        draw = event(vm, "draw")
        assert draw["rc"] == "0", vm.serial
        assert (draw["bytes"], draw["crc"]) == (want_bytes, want_crc), vm.serial
        ident = event(vm, "id")
        assert (ident["rc"], ident["id"]) == (TFT_ERR_NO_PANEL, "00000000"), vm.serial
