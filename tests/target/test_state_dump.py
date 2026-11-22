"""A wait that times out carries a guest state block, on every board."""

import re
import struct
import time

import pytest

from harness.boot import boot
from harness.qemu import QemuError

READY = re.compile(r"@RC stage=audio ev=ready .*\n")

# cpsid i; loop: add r0, r0, #1; b loop. Spins with IRQs masked, no ELF.
SPIN_WORDS = [0xF10C0080, 0xE2800001, 0xEAFFFFFD]


def timeout_error(vm):
    with pytest.raises(QemuError) as exc:
        vm.wait_for("this pattern never comes", timeout=2)
    return str(exc.value)


def state_block(msg):
    head = msg.index("--- guest state at timeout")
    assert msg.index("serial:") < head, msg
    assert "--- end of guest state ---" in msg, msg
    return msg[head:]


def test_dump_idle_in_wfi(board, example_image):
    image = example_image("audio")
    with boot(image, board) as vm:
        vm.wait_for(READY, timeout=60)
        msg = timeout_error(vm)
    block = state_block(msg)
    assert "state dump failed" not in block, block
    samples = re.findall(r"^  #(\d) pc=0x[0-9a-f]{8} <(\S+)> lr=", block, re.M)
    assert [n for n, _ in samples] == ["1", "2", "3", "4", "5"], block
    # The ELF next to the .img names the functions.
    assert samples[0][1] != "??", block
    assert "symbols: audio.elf via arm-none-eabi-" in block, block
    assert re.search(r"cpsr=0x[0-9a-f]{8} (svc|hyp|irq|sys) I=[01] F=[01] T=[01]", block), block
    assert "summary: guest executing: " in block, block
    assert re.search(r"CS=0x[0-9a-f]{8} .* CLO=0x[0-9a-f]{8} CHI=0x[0-9a-f]{8}", block), block
    for i in range(4):
        assert re.search(rf"C{i}=0x[0-9a-f]{{8}} match=[01] distance=[+-]?\d+ us", block), block
    assert "interrupt controller @ " in block, block
    assert ("local controller @ 0x40000000" in block) == (board in ("raspi2b", "raspi3b"))


def test_dump_spin_irqs_masked(board, tmp_path):
    image = tmp_path / "spin.img"
    image.write_bytes(struct.pack("<3I", *SPIN_WORDS))
    with boot(image, board) as vm:
        msg = timeout_error(vm)
    block = state_block(msg)
    assert "state dump failed" not in block, block
    assert "summary: guest executing: yes" in block, block
    assert "IRQs masked (CPSR.I=1 in every sample)" in block, block
    assert "symbols: none, no ELF for this image" in block, block
    pcs = re.findall(r"^  #\d pc=0x([0-9a-f]{8})", block, re.M)
    assert len(pcs) == 5 and all(0x8000 <= int(p, 16) <= 0x8008 for p in pcs), block


def test_state_dump_on_demand(board, tmp_path):
    image = tmp_path / "spin.img"
    image.write_bytes(struct.pack("<3I", *SPIN_WORDS))
    with boot(image, board) as vm:
        text = vm.state_dump()
        assert text.startswith(f"--- guest state ({board}) ---"), text
        assert "system timer @ " in text, text
    # After QEMU is gone the dump says so instead of trying.
    assert "not collected: QEMU has exited" in vm.state_dump()


def test_keepalive_on_by_default(board, example_image):
    with boot(example_image("audio"), board) as vm:
        vm.wait_for(READY, timeout=60)
        before = vm.qmp.keepalive_pings
        time.sleep(0.5)
        assert vm.qmp.keepalive_on
        assert vm.qmp.keepalive_pings > before
        text = vm.state_dump()
    assert "keep-alive: on" in text, text
    assert "device registers: read with the VM stopped" in text, text


def test_boot_with_keepalive_off(board, example_image):
    # Keeps the path without pings alive: ROBCO_QEMU_KEEPALIVE=0 runs it.
    with boot(example_image("audio"), board, keepalive=False) as vm:
        vm.wait_for(READY, timeout=60)
        msg = timeout_error(vm)
        assert not vm.qmp.keepalive_on and vm.qmp.keepalive_pings == 0
    block = state_block(msg)
    assert "keep-alive: off" in block, block
    assert "summary: guest executing: " in block, block
