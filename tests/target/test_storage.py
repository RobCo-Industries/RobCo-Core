"""Boot examples/storage on every board: SD card, MBR partition and FAT file system together.

Card images come from harness.sdimage (mtools). Files the guest writes are
read back on the host with mtools after QEMU exits, and the volume is
checked with fsck where a FAT checker is installed.
"""

import datetime
import os
import re
import shutil
import struct
import subprocess
import time
import zlib

import pytest

from harness.boot import boot, parse_markers
from harness.qemu import QemuError
from harness.sdimage import PART_LBA, SECTOR, corrupt, make_sd_image

STAGE = "storage"
CMD_TIMEOUT = 60
PART_OFF = PART_LBA * SECTOR
RTC_DEVICE = ["-device", "ds1338,bus=i2c-bus.1,address=0x68"]
MT_ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1")


def pattern(size, seed):
    """Same formula as fill_pattern() in examples/storage/main.c."""
    return bytes((((seed + i) * 2654435761) & 0xFFFFFFFF) >> 24 for i in range(size))


def crc(data):
    return f"0x{zlib.crc32(data) & 0xFFFFFFFF:08x}"


def events(vm, ev):
    # Complete lines only: a marker may still be arriving.
    text = vm.serial
    done = parse_markers(text[:text.rfind("\n") + 1])
    return [m for m in done if m.get("stage") == STAGE and m.get("ev") == ev]


def no_exception(vm):
    assert not [m for m in vm.markers() if m.get("ev") in ("exception", "panic")], vm.serial


def cmd(vm, line, ev=None, timeout=CMD_TIMEOUT):
    """Send one command and return the marker it answers with."""
    ev = ev or line.split()[0]
    before = len(events(vm, ev))
    vm.send(line + "\r")
    deadline = time.monotonic() + timeout
    while True:
        got = events(vm, ev)
        if len(got) > before:
            return got[before]
        if time.monotonic() > deadline:
            raise QemuError(f"no ev={ev} answer to {line[:80]!r}. serial:\n{vm.serial}")
        if vm.qemu.proc.poll() is not None:
            raise QemuError(f"QEMU exited during {line[:80]!r}. serial:\n{vm.serial}")
        time.sleep(0.02)


def ls(vm, path):
    before = len(events(vm, "entry"))
    done = cmd(vm, f"ls {path}", "ls")
    entries = events(vm, "entry")[before:]
    assert int(done["count"]) == len(entries) or done["rc"] != "ok", vm.serial
    return done, {e["name"]: e for e in entries}


def start(vm):
    vm.wait_for("ev=ready", timeout=CMD_TIMEOUT)
    return {ev: events(vm, ev)[0] for ev in ("card", "part", "mount")}


def mt(*args):
    return subprocess.run(list(args), check=True, env=MT_ENV, capture_output=True, text=True).stdout


def host_read(sd, path):
    """A file from the card image, read with mtools."""
    out = subprocess.run(["mtype", "-i", f"{sd}@@{PART_OFF}", "::" + path], check=True,
                         env=MT_ENV, capture_output=True).stdout
    return out


def host_names(sd, path):
    """Long names (or 8.3 names) in a directory, from `mdir -b`."""
    out = mt("mdir", "-b", "-i", f"{sd}@@{PART_OFF}", "::" + path)
    return {line.rstrip("/").rsplit("/", 1)[-1] for line in out.splitlines() if line.strip()}


def fsck(sd, tmp_path):
    """Checks the FAT volume read-only. Skips the check when no checker is installed."""
    tool = shutil.which("fsck.vfat") or shutil.which("fsck.fat") or shutil.which("fsck_msdos")
    if tool is None:
        return
    vol = tmp_path / "vol.img"
    with open(sd, "rb") as src, open(vol, "wb") as dst:
        src.seek(PART_OFF)
        shutil.copyfileobj(src, dst)
    r = subprocess.run([tool, "-n", str(vol)], capture_output=True, text=True)
    vol.unlink()
    text = r.stdout + r.stderr
    assert r.returncode == 0, f"{tool}: {text}"
    # fsck_msdos -n exits 0 even after finding errors. Its only clean warning is the summary.
    problems = [line for line in text.splitlines()
                if line.startswith("Warning:") and not re.match(r"Warning: \d+ files, ", line)
                or line.rstrip().endswith("?")]
    assert not problems, f"{tool}: {text}"


@pytest.fixture
def image(example_image):
    return example_image("storage")


# One small file, a nested tree, a long name and a file of several MiB.
BIG = os.urandom(3 * 1024 * 1024 + 4321)
TREE = {
    "README.TXT": b"RobCo Industries\r\n",
    "DIR1/SUB/DEEP.BIN": os.urandom(5000),
    "DIR1/EMPTY/": b"",
    "Long_File_Name_Test.data": os.urandom(777),
    "BIG.BIN": BIG,
    "ROBCO/BOOT.CFG": b"[boot]\ndefault = UOS\ntimeout = 3\n\n[entry UOS]\npath = /ROBCO/UOS.SYS\n",
    "PIPBOY/PIPOS.CFG": b"# Pip-OS\r\n[display]\r\nscheme = amber ; comment\r\n[rom]\r\n"
                        b"path = \"/PIPBOY/ROM1.ROM\"\r\n",
}


@pytest.mark.parametrize("mib,fat", [(16, "16"), (64, "32")], ids=["fat16", "fat32"])
def test_storage_read(board, image, need_mtools, tmp_path, mib, fat):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, mib, TREE)
    with boot(image, board, sd=sd) as vm:
        s = start(vm)
        assert s["card"]["rc"] == "ok", vm.serial
        p = s["part"]
        assert (p["rc"], p["n"]) == ("ok", "1"), vm.serial
        assert p["p0_type"] == ("0x0c" if fat == "32" else "0x0e")
        assert int(p["p0_lba"]) == PART_LBA
        assert int(p["p0_blocks"]) == (os.path.getsize(sd) // SECTOR) - PART_LBA
        assert (s["mount"]["rc"], s["mount"]["fat"]) == ("ok", fat), vm.serial

        done, root = ls(vm, "/")
        assert done["rc"] == "ok", vm.serial
        assert set(root) == {"README.TXT", "DIR1", "Long_File_Name_Test.data", "BIG.BIN", "ROBCO",
                         "PIPBOY"}
        assert root["DIR1"]["dir"] == "1" and root["BIG.BIN"]["dir"] == "0"
        assert int(root["BIG.BIN"]["size"]) == len(BIG)
        _, sub = ls(vm, "/DIR1")
        assert set(sub) == {"SUB", "EMPTY"}
        done, empty = ls(vm, "/DIR1/EMPTY")
        assert (done["rc"], done["count"], empty) == ("ok", "0", {})

        for name, data in TREE.items():
            if name.endswith("/"):
                continue
            m = cmd(vm, f"cat /{name}")
            assert (m["rc"], int(m["size"]), m["crc"]) == ("ok", len(data), crc(data)), name
        # Names are case-insensitive.
        assert cmd(vm, "cat /dir1/sub/deep.bin")["crc"] == crc(TREE["DIR1/SUB/DEEP.BIN"])
        no_exception(vm)


def test_storage_write(board, image, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 16, {"OLD.SAV": b"old content", "GONE.TXT": b"bye", "KEEP/A.TXT": b"a"})
    writes = {"/W1.BIN": (1, 1), "/W2.BIN": (70000, 2), "/KEEP/W3.BIN": (1024 * 1024 + 3, 3),
              "/Written_With_Long_Name.bin": (513, 4)}
    with boot(image, board, sd=sd) as vm:
        s = start(vm)
        df0 = s["mount"]
        assert df0["rc"] == "ok", vm.serial
        for path, (size, seed) in writes.items():
            m = cmd(vm, f"put {path} {size} {seed}")
            assert (m["rc"], int(m["size"])) == ("ok", size), vm.serial
            assert cmd(vm, f"cat {path}")["crc"] == crc(pattern(size, seed))

        df1 = cmd(vm, "df")
        used = sum(-(-size // int(df1["cluster"])) * int(df1["cluster"])
                   for size, _ in writes.values())
        assert int(df1["total"]) == int(df0["total"])
        # Files only: the root already exists, KEEP has room for more entries.
        assert int(df0["free"]) - int(df1["free"]) == used, vm.serial

        # Atomic replace: new content, no temp file left.
        assert cmd(vm, "atomic /OLD.SAV 3000 9")["rc"] == "ok", vm.serial
        assert cmd(vm, "atomic /NEW.SAV 100 10")["rc"] == "ok", vm.serial
        _, root = ls(vm, "/")
        assert not [n for n in root if n.upper().endswith(".TMP")], root

        assert cmd(vm, "mkdir /NEWDIR")["rc"] == "ok"
        assert cmd(vm, "mkdir /NEWDIR")["rc"] == "exist"
        assert cmd(vm, "mv /W1.BIN /NEWDIR/MOVED.BIN")["rc"] == "ok"
        assert cmd(vm, "rm /GONE.TXT")["rc"] == "ok"
        assert cmd(vm, "rm /KEEP")["rc"] == "denied"
        df2 = cmd(vm, "df")
        cl = int(df1["cluster"])

        def clusters(n):
            return -(-n // cl)
        # OLD.SAV grew, NEW.SAV and NEWDIR were added, GONE.TXT went away.
        delta = clusters(3000) - clusters(11) + clusters(100) + 1 - clusters(3)
        assert int(df1["free"]) - int(df2["free"]) == delta * cl, vm.serial
        assert cmd(vm, "umount")["rc"] == "ok"
        assert cmd(vm, "ls /", "ls")["rc"] == "novol"
        assert cmd(vm, "mount")["rc"] == "ok"
        assert cmd(vm, "sync")["rc"] == "ok"
        assert cmd(vm, "umount")["rc"] == "ok"
        no_exception(vm)

    assert host_read(sd, "/W2.BIN") == pattern(70000, 2)
    assert host_read(sd, "/KEEP/W3.BIN") == pattern(1024 * 1024 + 3, 3)
    assert host_read(sd, "/Written_With_Long_Name.bin") == pattern(513, 4)
    assert host_read(sd, "/NEWDIR/MOVED.BIN") == pattern(1, 1)
    assert host_read(sd, "/OLD.SAV") == pattern(3000, 9)
    assert host_read(sd, "/NEW.SAV") == pattern(100, 10)
    assert host_read(sd, "/KEEP/A.TXT") == b"a"
    names = host_names(sd, "/")
    assert "Written_With_Long_Name.bin" in names
    assert "GONE.TXT" not in names and "W1.BIN" not in names
    assert not [n for n in names if n.upper().endswith(".TMP")], names
    assert "NEWDIR" in names
    fsck(sd, tmp_path)


def test_storage_ini(board, image, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 16, TREE)
    with boot(image, board, sd=sd) as vm:
        start(vm)
        assert cmd(vm, "ini /PIPBOY/PIPOS.CFG display scheme")["value"] == "amber"
        assert cmd(vm, "ini /PIPBOY/PIPOS.CFG rom path")["value"] == "/PIPBOY/ROM1.ROM"
        assert cmd(vm, "ini /PIPBOY/PIPOS.CFG rom nokey")["rc"] == "ini_notfound"
        assert cmd(vm, "ini /PIPBOY/NONE.CFG rom path")["rc"] == "noent"
        # The draft BOOT.CFG in BOOT_PROTOCOL.md names sections "[entry UOS]".
        # ini.h allows no space in a section name, so the strict parser rejects it.
        assert cmd(vm, "ini /ROBCO/BOOT.CFG boot default")["rc"] == "ini_syntax"
        no_exception(vm)


def test_storage_clock_stamps(board, image, need_mtools, tmp_path):
    """With the RTC attached, files get the guest clock's time, close to the host's."""
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 16, {})
    with boot(image, board, sd=sd, extra_args=RTC_DEVICE) as vm:
        start(vm)
        clock = events(vm, "clock")[0]
        assert clock["now"] != "unset", vm.serial
        assert cmd(vm, "put /STAMP.TXT 10 1")["rc"] == "ok"
        assert cmd(vm, "umount")["rc"] == "ok"
    out = mt("mdir", "-i", f"{sd}@@{PART_OFF}", "::/STAMP.TXT")
    m = re.search(r"(\d{4})-(\d{2})-(\d{2})", out)
    assert m, out
    stamped = datetime.date(*map(int, m.groups()))
    today = datetime.datetime.now(datetime.timezone.utc).date()
    assert abs((stamped - today).days) <= 1, (stamped, today, out)


def test_storage_fixed_stamp_without_clock(board, image, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 16, {})
    with boot(image, board, sd=sd) as vm:
        start(vm)
        assert events(vm, "clock")[0]["now"] == "unset", vm.serial
        assert cmd(vm, "put /STAMP.TXT 10 1")["rc"] == "ok"
        _, root = ls(vm, "/")
        # 2026-01-01 00:00 in FAT format.
        assert (root["STAMP.TXT"]["date"], root["STAMP.TXT"]["time"]) == ("0x5c21", "0x0000")


def test_storage_errors(board, image, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 16, {"A/B.TXT": b"b"})
    with boot(image, board, sd=sd) as vm:
        start(vm)
        assert cmd(vm, "cat /MISSING.TXT")["rc"] == "noent"
        assert cmd(vm, "ls /NODIR", "ls")["rc"] == "noent"
        assert cmd(vm, "cat /NODIR/X.TXT")["rc"] == "noent"
        assert cmd(vm, "put /NODIR/X.TXT 10 1")["rc"] == "noent"
        assert cmd(vm, "rm /MISSING.TXT")["rc"] == "noent"
        assert cmd(vm, "mv /MISSING.TXT /X.TXT")["rc"] == "noent"
        assert cmd(vm, "cat /A")["rc"] == "isdir"
        assert cmd(vm, "mkdir /NODIR/SUB")["rc"] == "noent"
        long = "/" + "A" * 300
        assert cmd(vm, f"cat {long}")["rc"] == "nametoolong"
        assert cmd(vm, f"put {long} 1 1")["rc"] == "nametoolong"
        assert cmd(vm, "frobnicate", "error")["what"] == "unknown_command"
        # Still works after the errors.
        assert cmd(vm, "cat /A/B.TXT")["crc"] == crc(b"b")
        no_exception(vm)


def test_storage_full(board, image, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 4, {})
    with boot(image, board, sd=sd) as vm:
        s = start(vm)
        free = int(s["mount"]["free"])
        m = cmd(vm, f"put /FILL.BIN {free + 4096} 5")
        assert m["rc"] == "nospc", vm.serial
        assert int(m["size"]) <= free
        assert int(cmd(vm, "df")["free"]) < int(s["mount"]["cluster"])
        assert cmd(vm, "put /MORE.BIN 4096 1")["rc"] == "nospc"
        assert cmd(vm, "rm /FILL.BIN")["rc"] == "ok"
        assert int(cmd(vm, "df")["free"]) == free
        assert cmd(vm, "put /MORE.BIN 4096 1")["rc"] == "ok"
        assert cmd(vm, "umount")["rc"] == "ok"
        no_exception(vm)
    assert host_read(sd, "/MORE.BIN") == pattern(4096, 1)
    fsck(sd, tmp_path)


def test_storage_no_card(board, image):
    with boot(image, board) as vm:
        s = start(vm)
        assert s["card"]["rc"] == "nocard", vm.serial
        assert s["part"]["rc"] == "nomedia"
        assert s["mount"]["rc"] == "notready"
        assert cmd(vm, "ls /", "ls")["rc"] == "novol"
        assert cmd(vm, "cat /X")["rc"] == "novol"
        assert cmd(vm, "mount")["rc"] == "notready"
        no_exception(vm)


def test_storage_blank_card(board, image, tmp_path):
    sd = tmp_path / "zero.img"
    with open(sd, "wb") as f:
        f.truncate(16 << 20)
    with boot(image, board, sd=sd) as vm:
        s = start(vm)
        assert s["card"]["rc"] == "ok", vm.serial
        assert (s["part"]["rc"], s["part"]["n"]) == ("ok", "0"), vm.serial
        assert s["mount"]["rc"] == "nofs", vm.serial
        assert cmd(vm, "ls /", "ls")["rc"] == "novol"
        no_exception(vm)


def test_storage_bad_boot_sector(board, image, need_mtools, tmp_path):
    sd = tmp_path / "sd.img"
    make_sd_image(sd, 16, {"A.TXT": b"a"})
    # No boot signature and a nonsense bytes-per-sector field.
    corrupt(sd, PART_OFF + 510, b"\0\0")
    corrupt(sd, PART_OFF + 11, struct.pack("<H", 0x123))
    with boot(image, board, sd=sd) as vm:
        s = start(vm)
        assert s["part"]["n"] == "1", vm.serial
        assert s["mount"]["rc"] == "nofs", vm.serial
        assert cmd(vm, "cat /A.TXT")["rc"] == "novol"
        no_exception(vm)


def test_storage_sdhc(board, image, need_mtools, tmp_path):
    """A 4 GiB sparse card: SDHC block addressing under FAT32."""
    if board != "raspi2b":
        pytest.skip("SDHC runs on one board")
    sd = tmp_path / "sdhc.img"
    make_sd_image(sd, 4096, {"HELLO.TXT": b"hello\n"})
    with boot(image, board, sd=sd) as vm:
        s = start(vm)
        assert (s["card"]["rc"], s["card"]["type"]) == ("ok", "sdhc"), vm.serial
        assert s["mount"]["fat"] == "32"
        assert int(s["mount"]["total"]) > 3 << 30
        assert cmd(vm, "cat /HELLO.TXT")["crc"] == crc(b"hello\n")
        assert cmd(vm, "put /HIGH.BIN 200000 7")["rc"] == "ok"
        assert cmd(vm, "umount")["rc"] == "ok"
        no_exception(vm)
    assert host_read(sd, "/HIGH.BIN") == pattern(200000, 7)
