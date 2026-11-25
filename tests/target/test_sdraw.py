"""Boot examples/sdraw on every board: SD card init, block reads and writes through EMMC."""

import os
import struct
import time
import zlib

from harness.boot import boot
from harness.qemu import QemuError
from harness.sdimage import SECTOR, corrupt, make_sd_image

SDSC_BLOCKS = 64 << 20 >> 9
SDHC_BYTES = 4 << 30
SDHC_BLOCKS = SDHC_BYTES // SECTOR
CMD_TIMEOUT = 60


def pattern(lba, count, seed):
    """Same formula as fill_pattern() in examples/sdraw/main.c."""
    out = bytearray()
    for b in range(lba, lba + count):
        for j in range(SECTOR // 4):
            v = (seed * 2654435761 + b * 40503 + j * 2246822519) & 0xFFFFFFFF
            out += struct.pack("<I", v)
    return bytes(out)


def read_blocks(path, lba, count):
    with open(path, "rb") as f:
        f.seek(lba * SECTOR)
        return f.read(count * SECTOR)


def crc(data):
    return f"0x{zlib.crc32(data) & 0xFFFFFFFF:08x}"


def events(vm, ev):
    done = vm.markers()
    return [m for m in done if m.get("stage") == "sdraw" and m.get("ev") == ev]


def cmd(vm, line, ev):
    """Send one command and return the marker it answers with."""
    before = len(events(vm, ev))
    vm.send(line + "\r")
    deadline = time.monotonic() + CMD_TIMEOUT
    while True:
        got = events(vm, ev)
        if len(got) > before:
            return got[before]
        if time.monotonic() > deadline:
            raise QemuError(f"no ev={ev} answer to {line!r}. serial:\n{vm.serial}")
        if vm.qemu.proc.poll() is not None:
            raise QemuError(f"QEMU exited during {line!r}. serial:\n{vm.serial}")
        time.sleep(0.02)


def no_exception(vm):
    assert not [m for m in vm.markers() if m.get("ev") == "exception"], vm.serial


def card(vm):
    vm.wait_for("ev=ready", timeout=CMD_TIMEOUT)
    got = events(vm, "card")
    assert len(got) == 1, vm.serial
    return got[0]


def check_read(vm, path, lba, count, op="r"):
    m = cmd(vm, f"{op} {lba} {count}", "uread" if op == "u" else "read")
    assert m["rc"] == "ok", vm.serial
    assert m["crc"] == crc(read_blocks(path, lba, count)), (lba, count, vm.serial)


def test_sdraw_sdsc(board, example_image, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 64, {"robco/motd.txt": b"hello\n"})
    # Random data around the write targets, so untouched neighbours are visible.
    corrupt(sd, 4990 * SECTOR, os.urandom(400 * SECTOR))
    corrupt(sd, (SDSC_BLOCKS - 300) * SECTOR, os.urandom(300 * SECTOR))
    before = sd.read_bytes()
    block0 = before[:SECTOR]
    writes = [(5000, 64, 11), (5100, 1, 12), (5120, 255, 13), (SDSC_BLOCKS - 2, 2, 14)]

    with boot(example_image("sdraw"), board, sd=sd) as vm:
        c = card(vm)
        assert c["rc"] == "ok" and c["type"] == "sdsc", vm.serial
        assert int(c["blocks"]) == SDSC_BLOCKS, vm.serial
        assert int(c["clk"]) > 0, vm.serial
        # QEMU sets the card-inserted bit with a card in: the driver trusts it.
        det = events(vm, "detect")[0]
        assert (det["inserted"], det["card_detect"]) == ("1", "reported"), vm.serial
        assert int(det["present"], 16) & (1 << 16), vm.serial

        mbr = events(vm, "mbr")[0]
        assert mbr["sig"] == "0x55aa", vm.serial
        assert mbr["p0_type"] == f"0x{block0[446 + 4]:02x}", vm.serial
        assert int(mbr["p0_lba"]) == struct.unpack_from("<I", block0, 446 + 8)[0], vm.serial
        assert mbr["crc"] == crc(block0), vm.serial

        for lba, count in [(0, 1), (4990, 2), (4991, 8), (4993, 64), (4995, 255),
                           (SDSC_BLOCKS - 1, 1), (SDSC_BLOCKS - 255, 255)]:
            check_read(vm, sd, lba, count)
        check_read(vm, sd, 4997, 3, op="u")
        check_read(vm, sd, 4990, 255, op="u")

        for lba, count in [(SDSC_BLOCKS, 1), (SDSC_BLOCKS - 1, 2), (SDSC_BLOCKS - 100, 255),
                           (1 << 40, 1)]:
            m = cmd(vm, f"r {lba} {count}", "read")
            assert m["rc"] == "range", vm.serial

        for lba, count, seed in writes:
            assert cmd(vm, f"w {lba} {count} {seed}", "write")["rc"] == "ok", vm.serial
        assert cmd(vm, "s", "sync")["rc"] == "ok", vm.serial
        for lba, count, seed in writes:
            m = cmd(vm, f"v {lba} {count} {seed}", "verify")
            assert m["rc"] == "ok" and m["mismatches"] == "0", vm.serial
        assert cmd(vm, f"w {SDSC_BLOCKS - 1} 2 1", "write")["rc"] == "range", vm.serial

        assert cmd(vm, "i", "card") == c, vm.serial
        no_exception(vm)

    after = sd.read_bytes()
    expect = bytearray(before)
    for lba, count, seed in writes:
        expect[lba * SECTOR:(lba + count) * SECTOR] = pattern(lba, count, seed)
    for lba, count, _ in writes:
        lo, hi = (lba - 1) * SECTOR, (lba + count + 1) * SECTOR
        assert after[lo:hi] == bytes(expect[lo:hi]), (lba, count)
    assert after == bytes(expect)


def test_sdraw_sdhc(board, example_image, tmp_path):
    """A 4 GiB card is SDHC: block addressing reaches past 2 GiB."""
    sd = tmp_path / "sdhc.img"
    with open(sd, "wb") as f:
        f.truncate(SDHC_BYTES)
    high = (SDHC_BYTES // 2) // SECTOR + 1000
    corrupt(sd, 0, os.urandom(SECTOR))
    corrupt(sd, (high - 10) * SECTOR, os.urandom(80 * SECTOR))
    corrupt(sd, (SDHC_BLOCKS - 1) * SECTOR, os.urandom(SECTOR))
    before = read_blocks(sd, high - 10, 80)

    with boot(example_image("sdraw"), board, sd=sd) as vm:
        c = card(vm)
        assert c["rc"] == "ok" and c["type"] == "sdhc", vm.serial
        assert int(c["blocks"]) == SDHC_BLOCKS, vm.serial
        assert events(vm, "mbr")[0]["crc"] == crc(read_blocks(sd, 0, 1)), vm.serial

        check_read(vm, sd, high - 10, 40)
        check_read(vm, sd, SDHC_BLOCKS - 1, 1)
        assert cmd(vm, f"r {SDHC_BLOCKS} 1", "read")["rc"] == "range", vm.serial
        assert cmd(vm, f"w {high} 8 99", "write")["rc"] == "ok", vm.serial
        assert cmd(vm, "s", "sync")["rc"] == "ok", vm.serial
        m = cmd(vm, f"v {high} 8 99", "verify")
        assert m["rc"] == "ok" and m["mismatches"] == "0", vm.serial
        no_exception(vm)

    after = read_blocks(sd, high - 10, 80)
    assert after[10 * SECTOR:18 * SECTOR] == pattern(high, 8, 99)
    assert after[:10 * SECTOR] == before[:10 * SECTOR]
    assert after[18 * SECTOR:] == before[18 * SECTOR:]


def test_sdraw_no_card(board, example_image):
    with boot(example_image("sdraw"), board) as vm:
        c = card(vm)
        assert c["rc"] not in ("ok", "unknown"), vm.serial
        assert c["type"] == "none", vm.serial
        assert not events(vm, "detect"), vm.serial
        assert not events(vm, "mbr"), vm.serial
        assert cmd(vm, "r 0 1", "read")["rc"] == "nomedia", vm.serial
        no_exception(vm)
