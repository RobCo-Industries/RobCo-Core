"""State dump parsers and the timeout path, on captured QEMU 10.1 output. No QEMU needed."""

import os
import threading
import time

import pytest

from harness import dump
from harness.boot import Vm
from harness.qemu import QemuError

FIX = os.path.join(os.path.dirname(__file__), "fixtures", "dump")
BOARDS = ["raspi0", "raspi1ap", "raspi2b", "raspi3b"]
# pc, lr, CPSR in each captured `info registers` fixture.
EXPECT = {"raspi0": (0x9740, 0x85A8, 0x20000153), "raspi1ap": (0x9740, 0x85A8, 0x20000153),
          "raspi2b": (0x85A4, 0x8510, 0x60000153), "raspi3b": (0x9740, 0x85A8, 0x20000153)}


def fixture(name):
    with open(os.path.join(FIX, name)) as f:
        return f.read()


@pytest.mark.parametrize("machine", BOARDS)
def test_parse_aarch32_registers(machine):
    r = dump.parse_registers(fixture(f"info-registers-{machine}.txt"))
    assert not r["aarch64"]
    assert (r["pc"], r["lr"], r["cpsr"]) == EXPECT[machine]
    assert r["sp"] == 0x12DE8
    assert r["r1"] in (0x20201000, 0x3F201000)


def test_parse_aarch64_registers():
    # raspi3b before the shim drops CPU0 to AArch32.
    r = dump.parse_registers(fixture("info-registers-raspi3b-a64.txt"))
    assert r["aarch64"]
    assert r["pc"] == 0x80000 and r["lr"] == 0 and r["sp"] == 0
    assert r["cpsr"] == 0x400003CD
    assert dump.decode_cpsr(r["cpsr"])["mode"] == "EL3h"


def test_parse_registers_rejects_other_text():
    with pytest.raises(ValueError):
        dump.parse_registers("unknown command: 'info registerz'")


def test_core_registers_drop_fp_lines():
    lines = dump.core_registers(fixture("info-registers-raspi2b.txt"))
    assert lines[-1].startswith("PSR=60000153 -ZC- A NS svc32")
    assert not any(line.startswith(("s0", "FPSCR")) for line in lines)


@pytest.mark.parametrize("machine", BOARDS)
def test_parse_xp_timer(machine):
    words = dump.parse_xp(fixture(f"xp-timer-{machine}.txt"))
    assert len(words) == 7
    assert words[0] == 0 and words[2] == 0  # CS, CHI
    assert words[4] > words[1]  # C1 ahead of CLO: the tick is armed


def test_parse_xp_ic():
    words = dump.parse_xp(fixture("xp-ic-raspi2b.txt"))
    assert words == [0, 0, 0, 0, 2, 0, 0]  # enable1 bit 1: system timer C1


@pytest.mark.parametrize("value, mode, i, f, t", [
    (0x20000153, "svc", 0, 1, 0),
    (0x600001D3, "svc", 1, 1, 0),
    (0x000001DA, "hyp", 1, 1, 0),
    (0x00000032, "irq", 0, 0, 1),
    (0x0000001F, "sys", 0, 0, 0),
    (0x00000010, "usr", 0, 0, 0),
    (0x000003C5, "EL1h", 1, 1, 0),
])
def test_decode_cpsr(value, mode, i, f, t):
    d = dump.decode_cpsr(value)
    assert (d["mode"], d["I"], d["F"], d["T"]) == (mode, i, f, t)


@pytest.mark.parametrize("compare, counter, dist", [
    (0x1100, 0x1000, 0x100),
    (0x1000, 0x1100, -0x100),
    (0x00000010, 0xFFFFFFF0, 0x20),       # ahead across the wrap
    (0xFFFFFFF0, 0x00000010, -0x20),      # behind across the wrap
    (0x80001000, 0x00001000, -(1 << 31)),  # half the range away counts as behind
])
def test_compare_distance(compare, counter, dist):
    assert dump.compare_distance(compare, counter) == dist


def test_format_timer_flags_and_distance():
    lines = dump.format_timer([0b0010, 0xFFFFFFF0, 7, 0, 0x10, 0, 0xFFFFFF00])
    assert lines[0].startswith("CS=0x00000002 (match M3..M0=0010)")
    assert "C1=0x00000010 match=1 distance=+32 us (ahead)" in lines
    assert "C3=0xffffff00 match=0 distance=-240 us (behind)" in lines


def regs(pc, cpsr=0x153, r0=0):
    r = {"pc": pc, "lr": 0, "sp": 0, "cpsr": cpsr}
    r.update({f"r{i}": 0 for i in range(13)})
    r["r0"] = r0
    return r


def test_summary_cases():
    idle = [regs(0x9740)] * 5
    assert dump.summary(idle) == ("summary: guest executing: no (pc and registers fixed "
                                  "over 5 samples); IRQs unmasked (CPSR.I=0 in every sample)")
    spin = [regs(0x8004 + 4 * (i % 2), cpsr=0x1D3) for i in range(5)]
    assert "yes (pc changed between samples)" in dump.summary(spin)
    assert "IRQs masked (CPSR.I=1 in every sample)" in dump.summary(spin)
    counting = [regs(0x8004, cpsr=0x1D3, r0=i) for i in range(5)]
    assert "yes (pc fixed, registers changed)" in dump.summary(counting)


def test_nm_lookup():
    nm = "00008000 T _start\n00008000 t $a\n00009700 T main\n00009740 t idle\n"
    assert dump.nm_lookup(nm, [0x9740, 0x9744, 0x7000]) == {
        0x9740: "idle", 0x9744: "idle+0x4", 0x7000: "??"}


def test_resolve_without_elf_says_why():
    names, note = dump.resolve([0x8000], None)
    assert names == {} and note.startswith("symbols: none, no ELF")
    names, note = dump.resolve([0x8000], "/nonexistent/x.elf")
    assert names == {} and "ELF not found" in note


def fake_hmp(machine):
    regs_text = fixture(f"info-registers-{machine}.txt")
    timer = fixture(f"xp-timer-{machine}.txt")
    ic = fixture("xp-ic-raspi0.txt" if machine in ("raspi0", "raspi1ap") else "xp-ic-raspi2b.txt")

    def hmp(cmd):
        if cmd == "info registers":
            return regs_text
        addr = int(cmd.split()[-1], 16)
        if addr & 0xFFFF == 0x3000:
            return timer
        if addr & 0xFFFF == 0xB200:
            return ic
        return f"{addr:016x}: 0x00000000\n"
    return hmp


@pytest.mark.parametrize("machine", BOARDS)
def test_collect_from_fixtures(machine):
    lines = dump.collect(fake_hmp(machine), machine, sleep=lambda s: None)
    text = "\n".join(lines)
    assert lines[0].startswith("summary: guest executing: no")
    pc, lr, cpsr = EXPECT[machine]
    assert text.count(f" pc={pc:#010x} lr={lr:#010x} ") == 5
    assert "symbols: none, no ELF" in text
    assert ("local controller" in text) == (machine in ("raspi2b", "raspi3b"))
    assert "C1=" in text and f"PSR={cpsr:08x}" in text


class FakeProc:
    def __init__(self, rc=None):
        self.returncode = rc

    def poll(self):
        return self.returncode


class FakeQemu:
    """Monitor stand-in: hmp() raises, like a dead or confused QMP socket."""
    machine = "raspi0"
    timeout = 10.0
    _qmp = None

    def __init__(self, rc=None):
        self.proc = FakeProc(rc)

    def hmp(self, command):
        raise OSError("socket closed")

    def stderr(self):
        return ""


class FakeVm(Vm):
    def __init__(self, qemu):
        self.qemu = qemu
        self.elf = None
        self._bufs = [bytearray(b"booted\n")]
        self._cond = threading.Condition()


def test_failed_collection_keeps_the_timeout_error():
    vm = FakeVm(FakeQemu())
    with pytest.raises(QemuError) as exc:
        vm.wait_for("never", timeout=0.05)
    msg = str(exc.value)
    assert msg.startswith("timeout (0.05s) waiting for 'never'. serial:\nbooted\n")
    assert "state dump failed: OSError: socket closed" in msg


def test_failed_collection_survives_a_broken_state_dump(monkeypatch):
    vm = FakeVm(FakeQemu())

    def boom(*a, **k):
        raise RuntimeError("bug in the dump")
    monkeypatch.setattr(Vm, "state_dump", boom)
    with pytest.raises(QemuError) as exc:
        vm.wait_for("never", timeout=0.05)
    assert "state dump failed: RuntimeError: bug in the dump" in str(exc.value)


def test_no_dump_after_qemu_exit():
    vm = FakeVm(FakeQemu(rc=0))
    text = vm.state_dump()
    assert "not collected: QEMU has exited (status 0)" in text


# Budgeted collection. Small real timeouts keep these fast.

def slow_hmp(machine, delay, fail_on=None):
    inner, calls = fake_hmp(machine), []

    def hmp(cmd):
        calls.append(cmd)
        if len(calls) == fail_on:
            raise OSError("monitor gone")
        time.sleep(delay)
        return inner(cmd)
    return hmp


def fake_tool(tmp_path, monkeypatch, seconds):
    """A fake arm-none-eabi-addr2line that sleeps, and an ELF it can 'read'."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    tool = bindir / "arm-none-eabi-addr2line"
    tool.write_text(f"#!/bin/sh\nexec /bin/sleep {seconds}\n")
    tool.chmod(0o755)
    monkeypatch.setenv("PATH", str(bindir))
    elf = tmp_path / "x.elf"
    elf.write_bytes(b"\x7fELF")
    return str(elf)


def test_slow_monitor_and_slow_symbols_keep_the_hardware_state(tmp_path, monkeypatch):
    elf = fake_tool(tmp_path, monkeypatch, 30)
    budget = 1.0
    start = time.monotonic()
    lines = dump.collect(slow_hmp("raspi0", 0.08), "raspi0", elf, sleep=lambda s: None,
                         deadline=start + budget)
    took = time.monotonic() - start
    text = "\n".join(lines)
    assert text.count(" pc=0x00009740 lr=0x000085a8 ") == 5  # raw addresses, no <name>
    assert "<" not in text.split("interrupt controller")[0].split("apart):")[1]
    assert "system timer @ 0x20003000:" in text and "C1=" in text
    assert "symbols: none, lookup failed: timed out" in text
    assert took < budget + 0.5


def test_monitor_failing_on_third_command_keeps_earlier_parts():
    lines = dump.collect(slow_hmp("raspi0", 0, fail_on=3), "raspi0", sleep=lambda s: None)
    text = "\n".join(lines)
    assert "cpu0 samples (2, 100 ms apart):" in text
    assert "not collected: cpu0 sample #3 of 5: OSError: monitor gone" in text
    assert "interrupt controller @" in text and "system timer @" in text
    assert "info registers (sample #1):" in text


def test_over_budget_names_each_missing_part():
    clock = iter([0.0, 0.0, 0.0] + [99.0] * 50).__next__
    lines = dump.collect(fake_hmp("raspi2b"), "raspi2b", sleep=lambda s: None,
                         deadline=1.0, clock=clock)
    text = "\n".join(lines)
    assert "cpu0 samples (1, 100 ms apart):" in text
    assert "not collected: cpu0 sample #2 of 5: over the time budget" in text
    for part in ("interrupt controller", "local controller", "system timer"):
        assert f"not collected: {part}: over the time budget" in text
    assert "symbols: none, over the time budget" in text


def test_nothing_collected_raises():
    def dead(cmd):
        raise OSError("socket closed")
    with pytest.raises(OSError):
        dump.collect(dead, "raspi0", sleep=lambda s: None)


def test_absent_symbol_tool(tmp_path, monkeypatch):
    monkeypatch.setenv("PATH", str(tmp_path))
    elf = tmp_path / "x.elf"
    elf.write_bytes(b"\x7fELF")
    lines = dump.collect(fake_hmp("raspi0"), "raspi0", str(elf), sleep=lambda s: None,
                         deadline=time.monotonic() + 5)
    text = "\n".join(lines)
    assert "symbols: none, arm-none-eabi-addr2line and arm-none-eabi-nm not found" in text
    assert text.count(" pc=0x00009740 lr=0x000085a8 ") == 5
    assert "system timer @" in text
