"""Boot examples/spi on every board: SPI0 master driver.

What QEMU proves: QEMU's bcm2835-spi model moves bytes through its FIFOs
at once and sets DONE when the TX FIFO is empty. It ignores the clock
divider, the mode bits and chip select. With no device on its bus "spi",
every received byte is 0. A SPI flash model (w25q32) attached with
`-device w25q32,bus=spi` answers the JEDEC ID command, which checks that
bytes go out and come back in order. The flash sees no chip select
change, so only the first command on the bus reaches it.

So these tests check return codes, lengths, argument checks, the
transaction rules of spi_begin()/spi_end(), the CS register bits the
driver writes (chip select, CPOL, CPHA, CSPOL, TA), and that nothing
faults. They cannot check clock rate, signal timing, mode behaviour on
the wire, chip select levels or the timeout path: that needs hardware.
"""

import re

from harness.boot import boot

DONE = re.compile(r"@RC stage=spi ev=done\n")
FLASH = ["-device", "w25q32,bus=spi"]
SPI_ERR_ARG, SPI_ERR_INIT, SPI_ERR_BUSY = "-1", "-2", "-4"


def events(vm, name):
    return [m for m in vm.markers() if m.get("stage") == "spi" and m.get("ev") == name]


def event(vm, name):
    found = events(vm, name)
    assert len(found) == 1, vm.serial
    return found[0]


def check_driver(vm):
    assert event(vm, "preinit")["xfer"] == SPI_ERR_INIT
    init = event(vm, "init")
    assert init["rc"] == "0", vm.serial
    assert init["fsel"] == "44444"      # GPIO 7 to 11 at ALT0
    assert 0 < int(init["hz0"]) <= 1000000
    assert 0 < int(init["hz1"]) <= 1000000

    args = event(vm, "args")
    for k in ("cs", "mode", "hz", "begin", "end", "xfer", "write"):
        assert args[k] == SPI_ERR_ARG, (k, vm.serial)
    assert args["empty"] == "0"

    modes = events(vm, "mode")
    assert len(modes) == 8, vm.serial
    for m in modes:
        mode = int(m["mode"])
        assert (m["cfg"], m["begin"], m["xfer"], m["end"]) == ("0", "0", "0", "0"), m
        assert m["cs_bits"] == m["cs"]
        assert m["cpha"] == str(mode & 1)
        assert m["cpol"] == str(mode >> 1)
        assert m["ta"] == "1"
        assert m["zeros"] == "8"        # no device on the bus shifts in zeros
    assert event(vm, "idle")["ta"] == "0"

    cspol = event(vm, "cspol")
    assert (cspol["cfg"], cspol["cspol"], cspol["cspol1"], cspol["cspol0"]) == ("0", "1", "1", "0")

    long = event(vm, "long")
    assert (long["len"], long["rc"], long["zeros"]) == ("300", "0", "300"), vm.serial
    halves = event(vm, "halves")
    assert (halves["write"], halves["read"]) == ("0", "0")
    assert halves["zeros"] == "100"
    assert halves["untouched"] == "200"   # the read wrote exactly 100 bytes

    txn = event(vm, "txn")
    assert (txn["begin"], txn["cmd"], txn["data"], txn["ta"]) == ("0", "0", "0", "1"), vm.serial
    assert (txn["other"], txn["other_begin"], txn["other_cfg"]) == (SPI_ERR_BUSY,) * 3
    assert txn["end"] == "0"
    assert not [m for m in vm.markers() if m.get("ev") == "exception"], vm.serial


def test_spi_controller(board, example_image):
    with boot(example_image("spi"), board) as vm:
        vm.wait_for(DONE, timeout=60)
        check_driver(vm)
        jedec = event(vm, "jedec")
        assert (jedec["rc"], jedec["id"]) == ("0", "000000")


def test_spi_flash_id(board, example_image):
    with boot(example_image("spi"), board, extra_args=FLASH) as vm:
        vm.wait_for(DONE, timeout=60)
        check_driver(vm)
        jedec = event(vm, "jedec")
        # Winbond, SPI NOR, 32 Mbit.
        assert (jedec["rc"], jedec["id"]) == ("0", "ef4016"), vm.serial
