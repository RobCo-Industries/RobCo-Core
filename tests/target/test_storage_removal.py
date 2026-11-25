"""Pull the EMMC card out of examples/storage and put another one in, through QMP.

QMP `eject` and `blockdev-change-medium` move the card at run time, and the
EMMC present state bit follows them (canary V22). A RAM volume stays mounted
next to the card the whole time.
"""

from harness.boot import boot
from harness.sdimage import make_sd_image
from test_storage import cmd, crc, events, no_exception, pattern, start

BIG = pattern(20000, 1)
BIG_B = pattern(12345, 2)


def sd_drive(vm):
    return [b for b in vm.qemu.execute("query-block") if b["device"] == "sd0"][0]["qdev"]


def changes(vm):
    return int(cmd(vm, "changes")["n"])


def test_storage_card_pulled_and_replaced(board, example_image, need_mtools, tmp_path):
    card_a = tmp_path / "a.img"
    card_b = tmp_path / "b.img"
    make_sd_image(card_a, 64, {"NAME.TXT": b"card A", "GAMES/BIG.BIN": BIG})
    make_sd_image(card_b, 64, {"NAME.TXT": b"card B, a holotape", "GAMES/BIG.BIN": BIG_B})

    with boot(example_image("storage"), board, sd=card_a) as vm:
        assert start(vm)["mount"]["rc"] == "ok", vm.serial
        assert cmd(vm, "ramfs")["rc"] == "ok", vm.serial
        assert cmd(vm, "put ram:/R.BIN 3000 5", "put")["rc"] == "ok", vm.serial
        n0 = changes(vm)

        m = cmd(vm, "cat /NAME.TXT", "cat")
        assert (m["rc"], m["crc"]) == ("ok", crc(b"card A")), vm.serial
        assert cmd(vm, "hopen /GAMES/BIG.BIN")["rc"] == "ok", vm.serial
        m = cmd(vm, "hread 1000")
        assert (m["rc"], m["crc"]) == ("ok", crc(BIG[:1000])), vm.serial

        drive = sd_drive(vm)
        vm.qemu.execute("eject", id=drive, force=True)

        # The read that meets the empty slot and every later call: gone, no hang.
        assert cmd(vm, "hread 8192")["rc"] == "gone", vm.serial
        assert cmd(vm, "hread 10")["rc"] == "gone", vm.serial
        assert cmd(vm, "cat /NAME.TXT", "cat")["rc"] == "gone", vm.serial
        assert cmd(vm, "ls /", "ls")["rc"] == "gone", vm.serial
        assert cmd(vm, "probe")["rc"] == "gone", vm.serial
        assert changes(vm) == n0 + 1, vm.serial
        m = cmd(vm, "cat ram:/R.BIN", "cat")
        assert (m["rc"], m["crc"]) == ("ok", crc(pattern(3000, 5))), vm.serial
        assert cmd(vm, "hclose")["rc"] == "gone", vm.serial
        assert cmd(vm, "hread 1")["rc"] == "badf", vm.serial
        assert cmd(vm, "umount")["rc"] == "ok", vm.serial
        assert changes(vm) == n0 + 2, vm.serial
        assert cmd(vm, "sdinit")["rc"] != "ok", vm.serial
        assert cmd(vm, "mount")["rc"] != "ok", vm.serial

        vm.qemu.execute("blockdev-change-medium", id=drive, filename=str(card_b), format="raw")

        assert cmd(vm, "sdinit")["rc"] == "ok", vm.serial
        assert cmd(vm, "mount ro", "mount")["rc"] == "ok", vm.serial
        assert changes(vm) == n0 + 3, vm.serial
        m = cmd(vm, "cat sd:/NAME.TXT", "cat")
        assert (m["rc"], m["crc"]) == ("ok", crc(b"card B, a holotape")), vm.serial
        m = cmd(vm, "cat sd:/GAMES/BIG.BIN", "cat")
        assert (m["rc"], m["size"], m["crc"]) == ("ok", str(len(BIG_B)), crc(BIG_B)), vm.serial
        assert cmd(vm, "put sd:/X.BIN 10 1", "put")["rc"] == "ro", vm.serial
        assert cmd(vm, "mkdir sd:/D")["rc"] == "ro", vm.serial
        assert cmd(vm, "probe")["rc"] == "ok", vm.serial
        m = cmd(vm, "cat ram:/R.BIN", "cat")
        assert (m["rc"], m["crc"]) == ("ok", crc(pattern(3000, 5))), vm.serial
        # Unmounting the card made "ram" the default volume.
        assert cmd(vm, "eject")["rc"] == "ok", vm.serial
        assert cmd(vm, "cat sd:/GAMES/BIG.BIN", "cat")["rc"] == "novol", vm.serial
        assert not events(vm, "error"), vm.serial
        no_exception(vm)

    # The read-only mount wrote nothing to card B.
    assert b"X       BIN" not in card_b.read_bytes()
