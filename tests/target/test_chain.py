"""Boot examples/chain1, which validates and enters examples/chain2, on every board.

The harness places chain2 at 0x00200000 with a QEMU loader device. Fault
cases patch a temp copy of chain2 and reseal it where the case needs a
valid CRC.
"""

import re
import struct
import time
import zlib

import pytest

from harness.boot import boot

NEXT_ADDR = 0x00200000
CRC_OFFSET = 0x3C
MACH = {"raspi0": 0xC42, "raspi1ap": 0xC42, "raspi2b": 0xC43, "raspi3b": 0xC43}
STAGE_BIOS = 1
STAGE_BOOTAGENT = 2
BOOT_COLD = 1 << 2
CAP_FB = 1 << 0
CAP_PL011 = 1 << 1
JUMP = re.compile(r"@RC stage=chain1 ev=jump .*\n")
HALT = re.compile(r"@RC stage=chain1 ev=halt\n")
READY = re.compile(r"@RC stage=chain2 ev=ready\n")
# A tiny image for 0x8000 that jumps to chain2: ldr pc, [pc, #-4]; .word 0x00200000
TRAMPOLINE = struct.pack("<II", 0xE51FF004, NEXT_ADDR)


def chain_args(path):
    return ["-device", f"loader,file={path},addr={NEXT_ADDR:#x},force-raw=on"]


def events(vm, stage, name):
    return [m for m in vm.markers() if m.get("stage") == stage and m.get("ev") == name]


def one(vm, stage, name):
    found = events(vm, stage, name)
    assert len(found) == 1, f"{stage} {name}:\n{vm.serial}"
    return found[0]


def reseal(data):
    data = bytearray(data)
    struct.pack_into("<I", data, CRC_OFFSET, 0)
    struct.pack_into("<I", data, CRC_OFFSET, zlib.crc32(data) & 0xFFFFFFFF)
    return bytes(data)


def patched(src, tmp_path, name, offset, fmt, value, seal=True):
    with open(src, "rb") as f:
        data = bytearray(f.read())
    struct.pack_into(fmt, data, offset, value)
    if seal:
        data = reseal(data)
    path = tmp_path / name
    path.write_bytes(data)
    return path


@pytest.fixture
def chain2(example_image):
    return example_image("chain2")


def run_fault(board, example_image, next_image, timeout=10):
    args = chain_args(next_image) if next_image is not None else []
    with boot(example_image("chain1"), board, extra_args=args) as vm:
        vm.wait_for(HALT, timeout=timeout)
        assert not events(vm, "chain1", "jump"), vm.serial
        assert not events(vm, "chain2", "boot"), vm.serial
        return one(vm, "chain1", "error")["code"]


def test_chain_happy(board, example_image, chain2):
    with open(chain2, "rb") as f:
        hdr = f.read(0x60)
    file_size, crc = struct.unpack_from("<I", hdr, 0x34)[0], struct.unpack_from("<I", hdr, 0x3C)[0]
    with boot(example_image("chain1"), board, extra_args=chain_args(chain2)) as vm:
        vm.wait_for(READY, timeout=15)
        b1 = one(vm, "chain1", "boot")
        load = one(vm, "chain1", "load")
        assert load["addr"] == "0x00200000"
        assert load["size"] == str(file_size)
        assert load["name"] == "chain2"
        assert load["crc"] == f"{crc:#010x}"
        assert one(vm, "chain1", "jump")["entry"] == "0x00200000"
        b2 = one(vm, "chain2", "boot")
        assert b2["entry_mode"] == "svc"
        for key in ("midr", "soc", "base", "board_rev", "mem"):
            assert b2[key] == b1[key], key
        assert one(vm, "chain2", "handoff")["trusted"] == "1"
        info = one(vm, "chain2", "bootinfo")
        assert info["inherited"] == "1"
        assert info["prev_stage"] == str(STAGE_BIOS)
        assert info["soc"] == b1["soc"]
        assert info["mem"] == b1["mem"]
        assert info["fw_r1"] == f"{MACH[board]:#010x}"
        assert info["fdt"] == "0x00101000"
        assert int(info["fdt_size"]) > 0
        assert int(info["flags"], 16) & BOOT_COLD
        assert not events(vm, "chain2", "error"), vm.serial


def test_chain_bad_crc(board, example_image, chain2, tmp_path):
    with open(chain2, "rb") as f:
        data = bytearray(f.read())
    data[0x80] ^= 0x01
    bad = tmp_path / "crc.img"
    bad.write_bytes(data)
    assert run_fault(board, example_image, bad) == "crc"


def test_chain_wrong_magic(board, example_image, chain2, tmp_path):
    bad = patched(chain2, tmp_path, "magic.img", 0x20, "8s", b"ROM1\0\0\0\0")
    assert run_fault(board, example_image, bad) == "magic"


def test_chain_load_addr_mismatch(board, example_image, chain2, tmp_path):
    bad = patched(chain2, tmp_path, "addr.img", 0x30, "<I", 0x00210000)
    assert run_fault(board, example_image, bad) == "bounds"


def test_chain_mem_size_too_big(board, example_image, chain2, tmp_path):
    bad = patched(chain2, tmp_path, "mem.img", 0x38, "<I", 0x00200001)
    assert run_fault(board, example_image, bad) == "bounds"


def test_chain_no_image(board, example_image):
    assert run_fault(board, example_image, None) == "magic"


def test_chain2_alone(board, chain2, tmp_path):
    tramp = tmp_path / "tramp.bin"
    tramp.write_bytes(TRAMPOLINE)
    with boot(tramp, board, extra_args=chain_args(chain2)) as vm:
        vm.wait_for(READY, timeout=15)
        assert one(vm, "chain2", "handoff")["trusted"] == "0"
        info = one(vm, "chain2", "bootinfo")
        assert info["inherited"] == "0"
        assert info["prev_stage"] == str(STAGE_BOOTAGENT)
        assert info["fw_r1"] == f"{MACH[board]:#010x}"
        assert info["fdt"] == "0x00101000"


def test_chain_bad_bootinfo(board, example_image, chain2):
    img = example_image("chain1", "badinfo", "-DCHAIN1_BAD_BOOTINFO=1")
    with boot(img, board, extra_args=chain_args(chain2)) as vm:
        vm.wait_for(READY, timeout=15)
        time.sleep(0.2)
        assert events(vm, "chain1", "jump"), vm.serial
        assert one(vm, "chain2", "handoff")["trusted"] == "0"
        info = one(vm, "chain2", "bootinfo")
        assert info["inherited"] == "0"
        assert info["prev_stage"] == str(STAGE_BOOTAGENT)


def test_chain_keeps_framebuffer(board, example_image, chain2):
    img = example_image("chain1", "fb", "-DCHAIN1_FB=1")
    with boot(img, board, extra_args=chain_args(chain2)) as vm:
        vm.wait_for(READY, timeout=15)
        fb = one(vm, "chain1", "fb")["addr"]
        assert int(fb, 16) != 0
        assert one(vm, "chain2", "handoff")["trusted"] == "1"
        rb = one(vm, "chain2", "rebuild")
        assert rb["id"] == str(STAGE_BOOTAGENT)
        caps = int(rb["caps"], 16)
        assert caps & CAP_FB, vm.serial
        assert caps & CAP_PL011, vm.serial
        assert rb["fb"] == fb
        assert (rb["fb_w"], rb["fb_h"]) == ("640", "480")
        assert (rb["cols"], rb["rows"]) == ("80", "30")
        assert rb["cy"] == "1"
        assert rb["scheme"] == "1"
        assert rb["fdt"] == "0x00101000"
