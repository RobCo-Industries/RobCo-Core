"""Canary tests: QEMU behaviours our bare-metal Raspberry Pi code relies on.

Each test names the fact it checks (V1, V2, ...). See tests/README.md.
"""

import os
import shutil
import struct
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from harness.qemu import Qemu, have_machine, loader_words  # noqa: E402

ARM = "qemu-system-arm"
A64 = "qemu-system-aarch64"
BASE = {"raspi0": 0x20000000, "raspi1ap": 0x20000000, "raspi2b": 0x3F000000}
ARM_MACHINES = list(BASE)

LOOP = 0xEAFFFFFE  # b .
MODE_SVC, MODE_ABT, MODE_HYP = 0x13, 0x17, 0x1A


def need(binary, machine):
    if not have_machine(binary, machine):
        pytest.skip(f"{binary} -M {machine} not available")


def run_briefly(q, seconds=0.3):
    """Let the guest run, stop it, return CPU0 registers."""
    q.execute("cont")
    time.sleep(seconds)
    q.execute("stop")
    return q.registers()


@pytest.fixture
def raw_image(tmp_path):
    path = tmp_path / "kernel.img"
    path.write_bytes(struct.pack("<2I", LOOP, 0x12345678))
    return str(path)


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v1_raw_kernel_at_0x10000(machine, raw_image):
    """V1: raw -kernel loads at 0x10000; entry SVC, r0=0, r1=0xC42, r2=0x100, ATAGS at 0x100."""
    need(ARM, machine)
    with Qemu(ARM, machine, ["-kernel", raw_image, "-S"]) as q:
        roms = q.hmp("info roms")
        assert "addr=0000000000010000 size=0x000008" in roms, roms
        assert "0x12345678" in q.hmp("xp/2wx 0x10000")
        r = run_briefly(q)
        assert r["R15"] == 0x10000
        assert r["PSR"] & 0x1F == MODE_SVC
        assert (r["R00"], r["R01"], r["R02"]) == (0, 0xC42, 0x100)
        atag = q.hmp("xp/2wx 0x100").split()
        assert int(atag[2], 16) == 0x54410001, atag  # ATAG_CORE


ELF_SRC = ".global _start\n_start:\n b _start\n .word 0xcafef00d\n"


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v2_elf_kernel_at_link_address(machine, tmp_path):
    """V2: an ELF passed with -kernel is loaded at its link address."""
    need(ARM, machine)
    gcc = shutil.which("arm-none-eabi-gcc")
    if gcc is None:
        pytest.skip("arm-none-eabi-gcc not installed")
    src, elf = tmp_path / "k.S", tmp_path / "k.elf"
    src.write_text(ELF_SRC)
    subprocess.run([gcc, "-nostdlib", "-Wl,-Ttext=0x20000", "-Wl,--build-id=none",
                    "-o", str(elf), str(src)], check=True)
    with Qemu(ARM, machine, ["-kernel", str(elf), "-S"]) as q:
        assert "0xcafef00d" in q.hmp("xp/2wx 0x20000")
        assert run_briefly(q)["R15"] == 0x20000


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v3_loader_raw_file_at_0x8000(machine, raw_image):
    """V3: -device loader,file=...,addr=0x8000,force-raw=on,cpu-num=0 loads and starts at 0x8000."""
    need(ARM, machine)
    dev = f"loader,file={raw_image},addr=0x8000,force-raw=on,cpu-num=0"
    with Qemu(ARM, machine, ["-device", dev, "-S"]) as q:
        assert "0x12345678" in q.hmp("xp/2wx 0x8000")
        assert run_briefly(q)["R15"] == 0x8000


def test_v5_raspi3b_aarch64_cannot_be_disabled():
    """V5a: raspi3b rejects -global cortex-a53-arm-cpu.aarch64=off."""
    need(A64, "raspi3b")
    rc, err = Qemu.run_to_exit(A64, "raspi3b", ["-global", "cortex-a53-arm-cpu.aarch64=off", "-S"])
    assert rc != 0, err
    assert "aarch64" in err, err


EL3_TO_A32 = [0xD2802620, 0xD51E1100, 0xD2803B40, 0xD51E4000,
              0xD2900000, 0xD51E4020, 0xD69F03E0]


@pytest.mark.parametrize("spsr_word,mode", [(0xD2803B40, MODE_HYP), (0xD2803A60, MODE_SVC)],
                         ids=["hyp", "svc"])
def test_v5_raspi3b_eret_to_aarch32(spsr_word, mode):
    """V5b: seven AArch64 words at 0x80000 drop to AArch32 (HYP or SVC) at 0x8000."""
    need(A64, "raspi3b")
    words = list(EL3_TO_A32)
    words[2] = spsr_word
    args = loader_words(0x80000, words, entry=0x80000) + loader_words(0x8000, [LOOP]) + ["-S"]
    with Qemu(A64, "raspi3b", args) as q:
        r = run_briefly(q)
        assert r["R15"] == 0x8000
        assert r["PSR"] & 0x1F == mode


@pytest.mark.parametrize("machine,expected", [("raspi0", 0x410FB767), ("raspi2b", 0x80000F00)])
def test_v6_mrc_c0_c0_5(machine, expected):
    """V6: mrc p15,0,r1,c0,c0,5 gives MIDR on raspi0 and MPIDR (bit 31 set) on raspi2b."""
    need(ARM, machine)
    args = loader_words(0x8000, [0xEE101FB0, LOOP], entry=0x8000) + ["-S"]
    with Qemu(ARM, machine, args) as q:
        r = run_briefly(q)
        assert r["R15"] == 0x8004
        assert r["R01"] == expected, hex(r["R01"])


@pytest.mark.parametrize("machine,faults", [("raspi0", True), ("raspi2b", False)])
def test_v7_pwm_read(machine, faults):
    """V7: reading PWM (base+0x20C000) data-aborts on raspi0, not on raspi2b."""
    need(ARM, machine)
    a = BASE[machine] + 0x20C000
    # ldr r0,[pc,#4]; ldr r1,[r0]; b .; .word a  (ARMv6-safe, no movw/movt)
    code = [0xE59F0004, 0xE5901000, LOOP, a]
    vectors = loader_words(0x4, [LOOP] * 7)  # park every exception on its vector
    args = loader_words(0x8000, code, entry=0x8000) + vectors + ["-S"]
    with Qemu(ARM, machine, args) as q:
        r = run_briefly(q)
        if faults:
            assert r["R15"] == 0x10
            assert r["PSR"] & 0x1F == MODE_ABT
        else:
            assert r["R15"] == 0x8008
            assert r["PSR"] & 0x1F == MODE_SVC


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v9_mailbox_property_channel(machine):
    """V9: mailbox channel 8 answers board revision, ARM memory and UART clock (3 MHz)."""
    need(ARM, machine)
    buf, mbox = 0x10000, BASE[machine] + 0xB880
    words = [0, 0,
             0x00010002, 4, 0, 0,
             0x00010005, 8, 0, 0, 0,
             0x00030002, 8, 0, 2, 0,
             0]
    words[0] = 4 * len(words)
    with Qemu(ARM, machine, qtest=True) as q:
        for i, w in enumerate(words):
            q.writel(buf + 4 * i, w)
        q.writel(mbox + 0x20, buf | 8)
        assert q.readl(mbox + 0x18) & 0x40000000 == 0  # not empty
        assert q.readl(mbox) == buf | 8
        out = [q.readl(buf + 4 * i) for i in range(len(words))]
    assert out[1] == 0x80000000
    print(f"{machine}: revision={out[5]:#x} arm_mem base={out[9]:#x} size={out[10]:#x}")
    assert out[10] > 0
    assert out[14:16] == [2, 3000000]


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v10_system_timer_c1(machine):
    """V10: timer C1 match sets CS bit 1 and IRQ 1 pending; writing CS bit 1 clears it."""
    need(ARM, machine)
    st, ic = BASE[machine] + 0x3000, BASE[machine] + 0xB200
    with Qemu(ARM, machine, qtest=True) as q:
        q.writel(st + 0x10, q.readl(st + 0x04) + 1000)
        q.writel(ic + 0x10, 1 << 1)  # enable IRQ 1
        assert q.readl(st) & 2 == 0
        q.clock_step(2_000_000)
        assert q.readl(st) & 2
        assert q.readl(ic + 0x04) & 2  # IRQ pending 1
        assert q.readl(ic) & (1 << 8)  # basic pending: "pending 1 has bits"
        q.writel(st, 2)
        assert q.readl(st) & 2 == 0
        assert q.readl(ic + 0x04) & 2 == 0


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v11_sd_card_on_sdhci(machine):
    """V11: -drive if=sd attaches the card to generic-sdhci at reset."""
    need(ARM, machine)
    with Qemu(ARM, machine, ["-S", "-drive", "if=sd,driver=null-co,size=64M"]) as q:
        tree = q.hmp("info qtree")
    sdhci = tree[tree.index("dev: generic-sdhci"):]
    sdhci = sdhci[:sdhci.index("\n  dev: ")]
    assert "dev: sd-card" in sdhci, sdhci


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v11_sd_size_power_of_two(machine):
    """V11: a 48M SD card is rejected (size must be a power of 2)."""
    need(ARM, machine)
    rc, err = Qemu.run_to_exit(ARM, machine, ["-S", "-drive", "if=sd,driver=null-co,size=48M"])
    assert rc != 0, err
    assert "power of 2" in err, err


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v12_gpio_inputs_read_zero(machine):
    """V12: GPLEV0/1 read 0 and the GPIO device has no input-injection properties."""
    need(ARM, machine)
    gpio = BASE[machine] + 0x200000
    with Qemu(ARM, machine, qtest=True) as q:
        assert q.readl(gpio + 0x34) == 0
        assert q.readl(gpio + 0x38) == 0
        props = [p["name"] for p in q.execute("qom-list", path="/machine/soc/peripherals/gpio")]
    pins = [p for p in props if "gpio" in p and not p.startswith("bcm2835_gpio")]
    assert pins and all(p.startswith("unnamed-gpio-out[") for p in pins), pins
    assert not [p for p in props if "in" in p.split("[")[0].split("-")], props


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v13_screendump_headless(machine, tmp_path):
    """V13: QMP screendump works with -display none and writes a PPM."""
    need(ARM, machine)
    out = tmp_path / "screen.ppm"
    with Qemu(ARM, machine, ["-S"]) as q:
        assert q.execute("screendump", filename=str(out)) == {}
    data = out.read_bytes()
    assert data.startswith(b"P6") and len(data) > 100


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v14_watchdog_reset(machine):
    """V14: PM_WDOG then PM_RSTC full reset produces a QMP RESET event."""
    need(ARM, machine)
    pm = BASE[machine] + 0x100000
    with Qemu(ARM, machine, qtest=True) as q:
        q.writel(pm + 0x24, 0x5A00000A)
        q.writel(pm + 0x1C, 0x5A000020)
        q.clock_step(1_000_000_000)
        assert q.wait_event("RESET")["data"]["guest"] is True


@pytest.mark.parametrize("machine", ARM_MACHINES)
def test_v15_serial_order(machine):
    """V15: first -serial is the PL011, second is the mini UART (bcm2835-aux)."""
    need(ARM, machine)
    with Qemu(ARM, machine, ["-S"], serial_count=2) as q:
        tree = q.hmp("info qtree")

    def chardev_of(dev):
        block = tree[tree.index(f"dev: {dev},"):]
        return block[block.index("chardev = "):].split('"')[1]

    assert chardev_of("pl011") == "serial0"
    assert chardev_of("bcm2835-aux") == "serial1"
