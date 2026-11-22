"""Boot examples/mmu on every board: MMU and caches on, faults, coherence, off again.

QEMU does not model cache timing, so the bench numbers are only parsed.
"""

import re
import time

import pytest
from PIL import Image

from harness.boot import boot

PATTERN = re.compile(r"^@RC stage=mmu ev=pattern\n", re.M)
DONE = re.compile(r"^@RC stage=mmu ev=done\n", re.M)
EXCEPTION = re.compile(r"^@RC stage=mmu ev=exception .*\n", re.M)
ORDER = ["boot", "mbox", "bench", "mmu", "bench", "probe", "mbox", "pattern",
         "mmu_off", "mbox", "done"]
PART = {"raspi0": 0xB76, "raspi1ap": 0xB76, "raspi2b": 0xC07, "raspi3b": 0xD03}
SCTLR_M, SCTLR_C, SCTLR_Z, SCTLR_I, SCTLR_XP = 1 << 0, 1 << 2, 1 << 11, 1 << 12, 1 << 23
W, H, SQ = 640, 480, 16
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
CORNERS = [((SQ // 2, SQ // 2), (255, 0, 0)), ((W - SQ // 2, SQ // 2), (0, 255, 0)),
           ((SQ // 2, H - SQ // 2), (0, 0, 255))]
# Short-descriptor fault status, FSR bits [10] and [3:0], section forms.
FS_TRANSLATION = 0x05
FS_PERMISSION = 0x0D


def markers(vm):
    return [m for m in vm.markers() if m.get("stage") == "mmu"]


def events(vm, name):
    return [m for m in markers(vm) if m.get("ev") == name]


def fault_status(fsr):
    v = int(fsr, 16)
    return ((v >> 6) & 0x10) | (v & 0xF)


def check_pattern(path):
    with Image.open(path) as shot:
        img = shot.convert("RGB")
    assert img.size == (W, H)
    for i, want in enumerate(BARS):
        xy = (i * (W // 8) + W // 16, H // 2)
        assert img.getpixel(xy) == want, f"bar {i} at {xy}"
    for xy, want in CORNERS:
        assert img.getpixel(xy) == want, f"corner at {xy}"


def test_mmu(board, example_image, tmp_path):
    with boot(example_image("mmu"), board) as vm:
        vm.wait_for(PATTERN, timeout=120)
        check_pattern(vm.screendump(tmp_path / "screen.ppm"))
        vm.send("g")
        vm.wait_for(DONE, timeout=60)
        time.sleep(0.2)  # give a late exception a chance to show up

        evs = [m["ev"] for m in markers(vm)]
        assert evs == ORDER, vm.serial

        bench = events(vm, "bench")
        assert [b["caches"] for b in bench] == ["off", "on"], vm.serial
        for b in bench:
            int(b["fill_us"]), int(b["copy_us"])

        mmu = events(vm, "mmu")[0]
        sctlr = int(mmu["sctlr"], 16)
        part = (int(mmu["midr"], 16) >> 4) & 0xFFF
        assert part == PART[board], vm.serial
        want = SCTLR_M | SCTLR_C | SCTLR_Z | SCTLR_I
        if part == 0xB76:
            want |= SCTLR_XP
            assert mmu["smp"] == "na", vm.serial
        else:
            assert mmu["smp"] in ("0", "1"), vm.serial
        assert sctlr & want == want, vm.serial
        off = int(events(vm, "mmu_off")[0]["sctlr"], 16)
        assert off & (SCTLR_M | SCTLR_C | SCTLR_I) == 0, vm.serial

        probe = events(vm, "probe")[0]
        assert (probe["unmapped"], probe["device"], probe["ro_read"]) == ("abort", "ok", "ok")

        before, on, after = events(vm, "mbox")
        assert on["loops"] == "100" and on["mismatches"] == "0", vm.serial
        for key in ("rev", "clk", "raw"):
            assert before[key] == on[key] == after[key], key
        assert before["rev"] == before["raw"], vm.serial
        assert not events(vm, "exception") and not events(vm, "error"), vm.serial


@pytest.mark.parametrize("variant,flag,addr,status", [
    ("fault_ro", "MMU_FAULT_RO", "0x01c00000", FS_PERMISSION),
    ("fault_unmapped", "MMU_FAULT_UNMAPPED", "0x60000000", FS_TRANSLATION),
])
def test_mmu_fault(board, example_image, variant, flag, addr, status):
    img = example_image("mmu", variant, f"-D{flag}=1")
    with boot(img, board) as vm:
        vm.wait_for(EXCEPTION, timeout=120)
        assert [m["ev"] for m in markers(vm)][-3:] == ["bench", "probe", "exception"], vm.serial
        exc = events(vm, "exception")
        assert len(exc) == 1, vm.serial
        assert exc[0]["type"] == "data_abort", vm.serial
        assert exc[0]["dfar"] == addr, vm.serial
        assert fault_status(exc[0]["dfsr"]) == status, vm.serial
        assert int(exc[0]["dfsr"], 16) & (1 << 11), "WnR: the fault is a write"
