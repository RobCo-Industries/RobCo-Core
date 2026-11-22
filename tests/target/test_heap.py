"""Boot examples/heap on every board: heap self-test and CRC-32."""

import re
import time

from harness.boot import boot

DONE = re.compile(r"@RC stage=heap ev=done\n")


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "heap" and m.get("ev") == name]


def test_heap(board, example_image):
    with boot(example_image("heap"), board) as vm:
        vm.wait_for(DONE, timeout=20)
        time.sleep(0.5)  # give a late exception a chance to show up
        assert not events(vm, "exception"), vm.serial
        assert not events(vm, "pattern"), vm.serial

        crc = events(vm, "crc")
        assert len(crc) == 1, vm.serial
        assert crc[0]["value"] == "0xcbf43926"

        heap = events(vm, "heap")
        assert len(heap) == 2, vm.serial
        before, after = heap
        assert before["check"] == "0" and after["check"] == "0", vm.serial
        region = events(vm, "region")
        assert len(region) == 1, vm.serial
        base, size = int(region[0]["base"], 16), int(region[0]["size"])
        # The heap ends where the slot ends, below the boot info handoff area.
        assert base + size == 0x00100000, vm.serial
        assert int(before["total"]) == size
        assert after["used"] == "0"
        assert after["largest"] == before["largest"]
        assert events(vm, "done"), vm.serial
