"""The boot volume rule, fs_mount_boot(), on cards with several MBR entries.

examples/chain1 built with CHAIN1_SD=1 finds the volume, loads chain2 from
/ROBCO/MFBOOT.SYS and hands over its start in boot_part_lba.
examples/storage mounts with the same call and no markers.
"""

import re

import pytest

from harness.boot import boot
from harness.sdimage import mbr_card

READY = re.compile(r"@RC stage=chain2 ev=ready\n")
HALT = re.compile(r"@RC stage=chain1 ev=halt\n")
STORAGE_READY = re.compile(r"@RC stage=storage ev=ready\n")
PRIMARY = "ROBCO/MFBOOT.SYS"
LINUX = 0x83
FAT16 = 0x0E
EXTENDED = 0x05
GPT = 0xEE
LEN = 8192  # 4 MiB per entry


def lba(slot):
    return 2048 + slot * LEN


def events(vm, stage, name):
    return [m for m in vm.markers() if m.get("stage") == stage and m.get("ev") == name]


def one(vm, stage, name):
    found = events(vm, stage, name)
    assert len(found) == 1, f"{stage} {name}:\n{vm.serial}"
    return found[0]


@pytest.fixture
def chain1(example_image):
    return example_image("chain1", "sd", "-DCHAIN1_SD=1")


@pytest.fixture
def chain2_bytes(example_image):
    with open(example_image("chain2"), "rb") as f:
        return f.read()


def booted_from(board, chain1, sd):
    """Boots to chain2 and returns the start LBA chain1 mounted and chain2 received."""
    with boot(chain1, board, sd=sd) as vm:
        vm.wait_for(READY, timeout=60)
        mount = one(vm, "chain1", "mount")
        assert one(vm, "chain1", "load")["path"] == "/" + PRIMARY, vm.serial
        info = one(vm, "chain2", "bootinfo")
        assert info["part_lba"] == mount["lba"], vm.serial
        return int(mount["lba"]), int(mount["blocks"])


def halted(board, chain1, sd):
    with boot(chain1, board, sd=sd) as vm:
        vm.wait_for(HALT, timeout=60)
        assert not events(vm, "chain1", "jump"), vm.serial
        return one(vm, "chain1", "error")


def test_non_fat_first_then_fat(board, chain1, chain2_bytes, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    mbr_card(sd, [(LINUX, lba(0), LEN, None), (FAT16, lba(1), LEN, {PRIMARY: chain2_bytes})])
    assert booted_from(board, chain1, sd) == (lba(1), LEN)


def test_empty_slot_then_fat(board, chain1, chain2_bytes, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    mbr_card(sd, [None, (FAT16, lba(1), LEN, {PRIMARY: chain2_bytes})])
    assert booted_from(board, chain1, sd) == (lba(1), LEN)


def test_marker_only_on_second(board, chain1, chain2_bytes, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    mbr_card(sd, [(FAT16, lba(0), LEN, {"OTHER.TXT": b"x"}),
                  (FAT16, lba(1), LEN, {PRIMARY: chain2_bytes})])
    assert booted_from(board, chain1, sd) == (lba(1), LEN)


def test_marker_on_both(board, chain1, chain2_bytes, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    mbr_card(sd, [(FAT16, lba(0), LEN, {PRIMARY: chain2_bytes}),
                  (FAT16, lba(1), LEN, {PRIMARY: chain2_bytes})])
    assert booted_from(board, chain1, sd) == (lba(0), LEN)


def test_folder_named_like_marker(board, chain1, chain2_bytes, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    mbr_card(sd, [(FAT16, lba(0), LEN, {PRIMARY + "/": None}),
                  (FAT16, lba(1), LEN, {PRIMARY: chain2_bytes})])
    assert booted_from(board, chain1, sd) == (lba(1), LEN)


def test_whole_card_no_table(board, chain1, chain2_bytes, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    size = mbr_card(sd, ("whole", {PRIMARY: chain2_bytes}))
    assert booted_from(board, chain1, sd) == (0, size // 512)


def test_non_fat_only(board, chain1, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    mbr_card(sd, [(LINUX, lba(0), LEN, None), (EXTENDED, lba(1), LEN, None)])
    err = halted(board, chain1, sd)
    # The 0x83 entry was tried in the second pass and did not mount.
    assert (err["code"], err["what"]) == ("io", "mount"), err


def test_gpt_card(board, chain1, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    mbr_card(sd, [(GPT, 1, 3 * LEN, None)])
    err = halted(board, chain1, sd)
    assert (err["code"], err["what"]) == ("io", "mount"), err


def test_blank_card(board, chain1, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    with open(sd, "wb") as f:
        f.truncate(16 << 20)
    err = halted(board, chain1, sd)
    assert (err["code"], err["what"]) == ("io", "part"), err


def test_storage_mounts_fat_after_non_fat(board, example_image, need_mtools, tmp_path):
    """examples/storage takes the same rule: the first FAT volume, not the first entry."""
    sd = tmp_path / "sd.img"
    mbr_card(sd, [(LINUX, lba(0), LEN, None), (FAT16, lba(1), LEN, {"A.TXT": b"abc"})])
    with boot(example_image("storage"), board, sd=sd) as vm:
        vm.wait_for(STORAGE_READY, timeout=60)
        part = one(vm, "storage", "part")
        assert (part["p0_type"], part["p0_lba"]) == (f"{LINUX:#04x}", str(lba(0))), vm.serial
        assert one(vm, "storage", "mount")["rc"] == "ok", vm.serial
        assert int(one(vm, "storage", "mount")["total"]) <= LEN * 512, vm.serial
