"""Boot examples/chain1, which validates and enters examples/chain2, on every board.

The harness places chain2 at 0x00200000 with a QEMU loader device. Fault
cases patch a temp copy of chain2 and reseal it where the case needs a
valid CRC. The sd variant of chain1 loads chain2 from an SD card image
instead, with the MFBootAgent file names and fallback from BOOT_PROTOCOL.md.
"""

import re
import struct
import time
import zlib

import pytest

from harness.boot import boot
from harness.sdimage import PART_LBA, make_sd_image

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


def run_fault(board, example_image, next_image, timeout=60):
    args = chain_args(next_image) if next_image is not None else []
    with boot(example_image("chain1"), board, extra_args=args) as vm:
        vm.wait_for(HALT, timeout=timeout)
        assert not events(vm, "chain1", "jump"), vm.serial
        assert not events(vm, "chain2", "boot"), vm.serial
        return one(vm, "chain1", "error")["code"]


# SCTLR.M, C and I: the next stage must start with all three clear.
SCTLR_MCI = (1 << 0) | (1 << 2) | (1 << 12)


def chain1_image(example_image, mmu, fb=False):
    if mmu and fb:
        return example_image("chain1", "mmufb", "-DCHAIN1_MMU=1 -DCHAIN1_FB=1")
    if mmu:
        return example_image("chain1", "mmu", "-DCHAIN1_MMU=1")
    if fb:
        return example_image("chain1", "fb", "-DCHAIN1_FB=1")
    return example_image("chain1")


def check_mmu_handoff(vm, mmu):
    assert [m["on"] for m in events(vm, "chain1", "mmu")] == (["1"] if mmu else []), vm.serial
    assert int(one(vm, "chain2", "handoff")["sctlr"], 16) & SCTLR_MCI == 0, vm.serial


@pytest.mark.parametrize("mmu", [False, True], ids=["plain", "mmu"])
def test_chain_happy(board, example_image, chain2, mmu):
    with open(chain2, "rb") as f:
        hdr = f.read(0x60)
    file_size, crc = struct.unpack_from("<I", hdr, 0x34)[0], struct.unpack_from("<I", hdr, 0x3C)[0]
    with boot(chain1_image(example_image, mmu), board, extra_args=chain_args(chain2)) as vm:
        vm.wait_for(READY, timeout=60)
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
        check_mmu_handoff(vm, mmu)


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
        vm.wait_for(READY, timeout=60)
        assert one(vm, "chain2", "handoff")["trusted"] == "0"
        info = one(vm, "chain2", "bootinfo")
        assert info["inherited"] == "0"
        assert info["prev_stage"] == str(STAGE_BOOTAGENT)
        assert info["fw_r1"] == f"{MACH[board]:#010x}"
        assert info["fdt"] == "0x00101000"


def test_chain_bad_bootinfo(board, example_image, chain2):
    img = example_image("chain1", "badinfo", "-DCHAIN1_BAD_BOOTINFO=1")
    with boot(img, board, extra_args=chain_args(chain2)) as vm:
        vm.wait_for(READY, timeout=60)
        time.sleep(0.2)
        assert events(vm, "chain1", "jump"), vm.serial
        assert one(vm, "chain2", "handoff")["trusted"] == "0"
        info = one(vm, "chain2", "bootinfo")
        assert info["inherited"] == "0"
        assert info["prev_stage"] == str(STAGE_BOOTAGENT)


@pytest.mark.parametrize("mmu", [False, True], ids=["plain", "mmu"])
def test_chain_keeps_framebuffer(board, example_image, chain2, mmu):
    img = chain1_image(example_image, mmu, fb=True)
    with boot(img, board, extra_args=chain_args(chain2)) as vm:
        vm.wait_for(READY, timeout=60)
        fb = one(vm, "chain1", "fb")["addr"]
        assert int(fb, 16) != 0
        assert one(vm, "chain2", "handoff")["trusted"] == "1"
        rb = one(vm, "chain2", "rebuild")
        assert rb["id"] == str(STAGE_BOOTAGENT)
        caps = int(rb["caps"], 16)
        assert caps & CAP_FB, vm.serial
        assert caps & CAP_PL011, vm.serial
        # Neither stage touches the SD card or the mini UART here.
        assert not caps & ((1 << 3) | (1 << 2)), vm.serial
        assert rb["fb"] == fb
        assert (rb["fb_w"], rb["fb_h"]) == ("640", "480")
        assert (rb["cols"], rb["rows"]) == ("80", "30")
        assert rb["cy"] == "1"
        assert rb["scheme"] == "1"
        assert rb["fdt"] == "0x00101000"
        v = one(vm, "chain2", "video")
        assert (v["path"], v["fb"], v["bi_fb"]) == ("adopt", fb, fb), vm.serial
        check_mmu_handoff(vm, mmu)


def test_chain_handoff_quiets_timer(board, example_image):
    """A tick in stage one leaves no stale timer interrupt for stage two."""
    img = example_image("chain1", "tick", "-DCHAIN1_TICK=1")
    nxt = example_image("chain2", "tick", "-DCHAIN2_TICK=1")
    with boot(img, board, extra_args=chain_args(nxt)) as vm:
        vm.wait_for(READY, timeout=60)
        t1 = one(vm, "chain1", "tick")
        assert int(t1["ticks"]) >= 20, vm.serial
        # tick_hz(): 0 before tick_init(), then the rate it started.
        assert (t1["hz0"], t1["hz"]) == ("0", "1000"), vm.serial
        # Both channels matched before the handoff.
        assert (t1["cs1"], t1["cs3"]) == ("1", "1"), vm.serial
        t2 = one(vm, "chain2", "tick")
        assert (t2["cs1"], t2["cs3"]) == ("0", "0"), vm.serial
        assert t2["spurious"] == "0", vm.serial
        # The guest waits 200 ms on its own timer. The tick must have fired
        # and cannot count more periods than that timer saw.
        elapsed = int(t2["elapsed_us"])
        assert elapsed >= 200_000, vm.serial
        assert 1 <= int(t2["ticks"]) <= elapsed // 10_000 + 1, vm.serial
        assert not events(vm, "chain2", "exception"), vm.serial


def test_chain2_checks_own_crc(board, example_image):
    """chain2 recomputes its CRC over the whole file after start, with no skipped range."""
    nxt = example_image("chain2", "selfcheck", "-DCHAIN2_SELF_CHECK=1")
    with open(nxt, "rb") as f:
        data = f.read()
    crc = struct.unpack_from("<I", data, CRC_OFFSET)[0]
    with boot(example_image("chain1"), board, extra_args=chain_args(nxt)) as vm:
        vm.wait_for(READY, timeout=60)
        check = one(vm, "chain2", "selfcheck")
        assert check["rc"] == "ok", vm.serial
        assert check["size"] == str(len(data)), vm.serial
        assert check["crc"] == f"{crc:#010x}", vm.serial
        # The entry registers are still there: they live in BSS, not in the file.
        assert one(vm, "chain2", "boot")["entry_mode"] == "svc", vm.serial



def test_chain2_self_check_bad_size(board, example_image, tmp_path):
    """A file_size made larger after sealing gives rc=size, with no read past the image.

    The trampoline enters chain2 with no validation, as the firmware would.
    """
    nxt = example_image("chain2", "selfcheck", "-DCHAIN2_SELF_CHECK=1")
    with open(nxt, "rb") as f:
        data = bytearray(f.read())
    # Far past RAM: a scan this long would fault.
    struct.pack_into("<I", data, 0x34, 0xF0000000)
    bad = tmp_path / "selfsize.img"
    bad.write_bytes(bytes(data))
    tramp = tmp_path / "tramp.img"
    tramp.write_bytes(TRAMPOLINE)
    with boot(tramp, board, extra_args=chain_args(bad)) as vm:
        vm.wait_for(READY, timeout=60)
        check = one(vm, "chain2", "selfcheck")
        assert check["rc"] == "size", vm.serial
        assert check["size"] == str(0xF0000000), vm.serial
        assert not events(vm, "chain2", "exception"), vm.serial

# Loading from the SD card: chain1 built with CHAIN1_SD=1.
PRIMARY = "/ROBCO/MFBOOT.SYS"
BACKUP = "/ROBCO/MFBOOT.BAK"
BOOT_FALLBACK = 1 << 0
CAP_SD = 1 << 3
CAP_MINIUART = 1 << 2
BOOT_DEV_EMMC = 1
SHELL = re.compile(r"@RC stage=chain1 ev=shell .*\n")


def sd_chain1(example_image):
    return example_image("chain1", "sd", "-DCHAIN1_SD=1")


def card(tmp_path, files):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 16, {k.lstrip("/"): v for k, v in files.items()})
    return sd


def header(path):
    data = open(path, "rb").read()
    return len(data), struct.unpack_from("<I", data, 0x3C)[0]


def sd_boot_ok(board, example_image, sd, loaded, src):
    """Boots to chain2 from `sd` and checks markers for `loaded` (from file `src`)."""
    size, crc = header(src)
    with boot(sd_chain1(example_image), board, sd=sd) as vm:
        vm.wait_for(READY, timeout=60)
        load = one(vm, "chain1", "load")
        assert load["path"] == loaded, vm.serial
        assert load["size"] == str(size)
        assert load["crc"] == f"{crc:#010x}"
        assert load["addr"] == "0x00200000"
        assert one(vm, "chain1", "jump")["entry"] == "0x00200000"
        assert one(vm, "chain2", "handoff")["trusted"] == "1"
        info = one(vm, "chain2", "bootinfo")
        assert info["inherited"] == "1"
        assert info["prev_stage"] == str(STAGE_BIOS)
        assert info["path"] == loaded
        assert info["dev"] == str(BOOT_DEV_EMMC)
        assert info["part_lba"] == str(PART_LBA)
        # Set by bootinfo_build() in chain1 from the live drivers.
        assert int(info["caps"], 16) & (CAP_SD | CAP_MINIUART) == CAP_SD | CAP_MINIUART, vm.serial
        # chain2 sets up neither, so its own block carries both over.
        rb = one(vm, "chain2", "rebuild")
        assert int(rb["caps"], 16) & (CAP_SD | CAP_MINIUART) == CAP_SD | CAP_MINIUART, vm.serial
        assert not events(vm, "chain2", "error"), vm.serial
        return vm, int(info["flags"], 16)


def sd_fail(board, example_image, sd):
    """Boots chain1 from `sd`, expects the shell and a halt with no jump. Returns the markers."""
    with boot(sd_chain1(example_image), board, sd=sd) as vm:
        vm.wait_for(HALT, timeout=60)
        assert not events(vm, "chain1", "jump"), vm.serial
        assert not events(vm, "chain2", "boot"), vm.serial
        assert len(events(vm, "chain1", "shell")) == 1, vm.serial
        return vm.markers()


def chain1_events(markers, name):
    return [m for m in markers if m.get("stage") == "chain1" and m.get("ev") == name]


def test_chain_sd_primary(board, example_image, chain2, need_mtools, tmp_path):
    sd = card(tmp_path, {PRIMARY: chain2})
    vm, flags = sd_boot_ok(board, example_image, sd, PRIMARY, chain2)
    assert not flags & BOOT_FALLBACK
    assert flags & BOOT_COLD


def test_chain_sd_backup_when_primary_missing(board, example_image, chain2, need_mtools,
                                              tmp_path):
    sd = card(tmp_path, {BACKUP: chain2})
    with boot(sd_chain1(example_image), board, sd=sd) as vm:
        vm.wait_for(READY, timeout=60)
        fb = one(vm, "chain1", "fallback")
        assert (fb["from"], fb["to"], fb["reason"]) == (PRIMARY, BACKUP, "nofile")
        err = one(vm, "chain1", "error")
        assert (err["code"], err["path"]) == ("nofile", PRIMARY)
        assert one(vm, "chain1", "load")["path"] == BACKUP
        info = one(vm, "chain2", "bootinfo")
        assert info["path"] == BACKUP
        assert int(info["flags"], 16) & BOOT_FALLBACK
    # Order: error, fallback, load, jump.
    names = [m["ev"] for m in vm.markers() if m.get("stage") == "chain1"]
    assert names.index("error") < names.index("fallback") < names.index("load") < names.index("jump")


def test_chain_sd_backup_when_primary_corrupt(board, example_image, chain2, need_mtools,
                                              tmp_path):
    data = bytearray(open(chain2, "rb").read())
    data[0x80] ^= 0x01
    bad = tmp_path / "bad.img"
    bad.write_bytes(data)
    sd = card(tmp_path, {PRIMARY: bad, BACKUP: chain2})
    with boot(sd_chain1(example_image), board, sd=sd) as vm:
        vm.wait_for(READY, timeout=60)
        fb = one(vm, "chain1", "fallback")
        assert fb["reason"] == "crc", vm.serial
        assert one(vm, "chain1", "load")["path"] == BACKUP
        assert one(vm, "chain2", "bootinfo")["path"] == BACKUP


def test_chain_sd_both_missing(board, example_image, need_mtools, tmp_path):
    sd = card(tmp_path, {"ROBCO/": b""})
    markers = sd_fail(board, example_image, sd)
    errors = chain1_events(markers, "error")
    assert [(e["code"], e["path"]) for e in errors] == [("nofile", PRIMARY), ("nofile", BACKUP)]
    assert chain1_events(markers, "fallback")[0]["reason"] == "nofile"
    assert chain1_events(markers, "shell")[0]["reason"] == "nofile"
    assert not chain1_events(markers, "load")


def test_chain_sd_wrong_slot(board, example_image, chain2, need_mtools, tmp_path):
    """chain2 resealed for the OS slot: rejected at the bounds step, before any jump."""
    with open(chain2, "rb") as f:
        data = bytearray(f.read())
    struct.pack_into("<I", data, 0x30, 0x00400000)
    struct.pack_into("<I", data, 0x48, 0)  # RIMG_BOOT_UOS
    wrong = tmp_path / "wrong.img"
    wrong.write_bytes(reseal(data))
    sd = card(tmp_path, {PRIMARY: wrong, BACKUP: wrong})
    markers = sd_fail(board, example_image, sd)
    assert [e["code"] for e in chain1_events(markers, "error")] == ["bounds", "bounds"]
    assert chain1_events(markers, "fallback")[0]["reason"] == "bounds"


def test_chain_sd_no_card(board, example_image):
    with boot(sd_chain1(example_image), board) as vm:
        vm.wait_for(HALT, timeout=60)
        err = one(vm, "chain1", "error")
        assert (err["code"], err["what"]) == ("io", "sd"), vm.serial
        assert one(vm, "chain1", "shell")["reason"] == "io"
        assert not events(vm, "chain1", "jump"), vm.serial


# A later stage adopts the screen the first stage left on. chain1 built
# with CHAIN1_FB_TEXT leaves four amber lines; chain2 adopts them with
# fb_adopt() and console_adopt(), or falls back to fb_init().
from PIL import Image  # noqa: E402

from test_console import COLS, FH, FW, H, ROWS, SCHEMES, W, load_font  # noqa: E402

CHAIN1_TEXT = ["chain1", "first stage text 1", "first stage text 2", "first stage text 3"]


def fbtext_chain1(example_image):
    return example_image("chain1", "fbtext", "-DCHAIN1_FB=1 -DCHAIN1_FB_TEXT=1")


def chain2_lines(n):
    return [f"chain2 line {i:02d}" for i in range(1, n + 1)]


def on_screen(lines):
    """The rows after printing each line and a newline: the bottom row stays blank."""
    return lines[max(0, len(lines) - (ROWS - 1)):]


def render(rows):
    """The expected screen: each row's text in amber normal on the amber background."""
    _, bg, _dim, normal, _bright = SCHEMES["2"]
    font = load_font()
    img = Image.new("RGB", (W, H), bg)
    px = img.load()
    for r, text in enumerate(rows):
        for c, ch in enumerate(text):
            glyph = font[ord(ch)]
            for y in range(FH):
                for x in range(FW):
                    if glyph[y] & (0x80 >> x):
                        px[c * FW + x, r * FH + y] = normal
    return img


def shot(vm, tmp_path, name):
    path = tmp_path / f"{name}.ppm"
    vm.screendump(path)
    with Image.open(path) as img:
        return img.convert("RGB")


def check_screen(vm, tmp_path, name, rows):
    img = shot(vm, tmp_path, name)
    want = render(rows)
    if img.tobytes() != want.tobytes():
        img.save(tmp_path / f"{name}.png")
        want.save(tmp_path / f"{name}.want.png")
        raise AssertionError(f"{name}: screen differs, see {tmp_path}:\n{vm.serial}")


def wait_n(vm, n):
    vm.wait_for(f"@RC stage=chain2 ev=wait n={n}\n", timeout=60)


def dump_rows(vm):
    m = re.search(r"@RC stage=chain2 ev=screen (.*)\n((?:\|.*\n)*)@RC stage=chain2 ev=screen_end\n",
                  vm.serial)
    assert m, vm.serial
    return [line[1:] for line in m.group(2).splitlines()]


def test_chain_adopts_screen(board, example_image, tmp_path):
    """No clear, no gap: chain2 writes below chain1's lines, then scrolls them up as pixels."""
    nxt = example_image("chain2", "vwait", "-DCHAIN2_VIDEO_WAIT=1")
    with boot(fbtext_chain1(example_image), board, extra_args=chain_args(nxt)) as vm:
        wait_n(vm, 1)
        fb = one(vm, "chain1", "fb")["addr"]
        v = one(vm, "chain2", "video")
        assert (v["path"], v["fb_rc"], v["con_rc"]) == ("adopt", "0", "0"), vm.serial
        assert v["fb"] == v["bi_fb"] == fb, vm.serial
        rb = one(vm, "chain2", "rebuild")
        assert (rb["fb"], rb["cols"], rb["rows"], rb["cy"], rb["scheme"]) == \
            (fb, "80", "30", "4", "1"), vm.serial
        # Adopted and nothing written yet: exactly chain1's screen.
        check_screen(vm, tmp_path, "adopted", CHAIN1_TEXT)
        vm.send("a")
        wait_n(vm, 2)
        check_screen(vm, tmp_path, "below", CHAIN1_TEXT + chain2_lines(3))
        vm.send("b")
        vm.wait_for(READY, timeout=60)
        lines = CHAIN1_TEXT + chain2_lines(27)
        check_screen(vm, tmp_path, "scrolled", on_screen(lines))
        # Cells chain2 never wrote stay unknown and dump as '#'. Line 27 went
        # on the blank row the first scroll made, so its row is all known.
        pad = [t + "#" * (COLS - len(t)) for t in chain2_lines(26)]
        assert dump_rows(vm) == ["#" * COLS] * 2 + pad + chain2_lines(27)[26:] + [""], vm.serial
        assert not events(vm, "chain2", "error"), vm.serial


def test_chain_adopt_small_grid_scrolls_inside(board, example_image, tmp_path):
    """A 12 x 6 grid scrolls its own pixels only: the rest keeps chain1's screen."""
    gw, gh = 12 * FW, 6 * FH
    nxt = example_image("chain2", "smallgrid", "-DCHAIN2_VIDEO_WAIT=1 -DCHAIN2_SMALL_GRID=1")
    with boot(fbtext_chain1(example_image), board, extra_args=chain_args(nxt)) as vm:
        wait_n(vm, 1)
        v = one(vm, "chain2", "video")
        assert (v["path"], v["fb_rc"], v["con_rc"]) == ("adopt", "0", "0"), vm.serial
        before = shot(vm, tmp_path, "adopted")
        assert before.tobytes() == render(CHAIN1_TEXT).tobytes(), vm.serial
        vm.send("a")
        wait_n(vm, 2)
        vm.send("b")
        vm.wait_for(READY, timeout=60)
        after = shot(vm, tmp_path, "scrolled")
    # Outside the grid: chain1's pixels, the text right of column 12 included.
    mask = Image.new("1", (W, H), 1)
    mask.paste(0, (0, 0, gw, gh))
    blank = Image.new("RGB", (W, H))
    outside = Image.composite(after, blank, mask).tobytes()
    assert outside == Image.composite(before, blank, mask).tobytes(), tmp_path
    assert outside != Image.composite(render([]), blank, mask).tobytes()
    # Inside: "chain2 line " fills a row, so each line takes two rows. The
    # last newline leaves the bottom row blank.
    rows = []
    for t in chain2_lines(27):
        rows += [t[:12], t[12:]]
    rows = (rows + [""])[-6:]
    want = render(rows).crop((0, 0, gw, gh))
    assert after.crop((0, 0, gw, gh)).tobytes() == want.tobytes(), tmp_path
    assert dump_rows(vm) == [r.rstrip() for r in rows], vm.serial


@pytest.mark.parametrize("variant,flag,fb_rc,con_rc", [
    ("badpitch", "-DCHAIN2_BAD_PITCH=1", "-4", "0"),
    ("badcursor", "-DCHAIN2_BAD_CURSOR=1", "0", "-4"),
])
def test_chain_adopt_falls_back(board, example_image, tmp_path, variant, flag, fb_rc, con_rc):
    """A description that fails validation: fb_init() and console_init(), a clear screen."""
    nxt = example_image("chain2", variant, flag)
    with boot(fbtext_chain1(example_image), board, extra_args=chain_args(nxt)) as vm:
        vm.wait_for(READY, timeout=60)
        v = one(vm, "chain2", "video")
        assert (v["path"], v["fb_rc"], v["con_rc"]) == ("init", fb_rc, con_rc), vm.serial
        assert int(v["bi_fb"], 16) != 0, vm.serial
        check_screen(vm, tmp_path, "fallback", chain2_lines(3))
        assert not events(vm, "chain2", "error"), vm.serial
