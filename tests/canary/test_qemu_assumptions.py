"""Canary tests: QEMU behaviours our bare-metal Raspberry Pi code relies on.

Each test names the fact it checks (V1, V2, ...). See tests/README.md.
"""

import os
import re
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


@pytest.mark.parametrize("binary,machine", [(ARM, m) for m in ARM_MACHINES] + [(A64, "raspi3b")])
def test_v16_spi0_transfer_completes(binary, machine):
    """V16: SPI0 is modelled. With nothing attached a byte completes at once and reads 0."""
    need(binary, machine)
    cs, fifo = BASE.get(machine, 0x3F000000) + 0x204000, BASE.get(machine, 0x3F000000) + 0x204004
    with Qemu(binary, machine, qtest=True) as q:
        q.writel(cs, 0x30)          # clear both FIFOs
        q.writel(cs, 0x80)          # TA: start a transfer
        q.writel(fifo, 0xA5)
        status = q.readl(cs)
        assert status & (1 << 16), hex(status)     # DONE
        assert status & (1 << 17), hex(status)     # RXD: a byte came back
        assert q.readl(fifo) == 0
        assert q.readl(cs) & (1 << 17) == 0        # RX FIFO empty again
        q.writel(cs, 0x30)
        assert q.readl(cs) & 0x80 == 0


# V17: QEMU's pca9554 model. BSC1 registers, driven through qtest.
BSC1 = {"raspi0": 0x20804000, "raspi1ap": 0x20804000, "raspi2b": 0x3F804000,
        "raspi3b": 0x3F804000}
PCA_MACHINES = [(ARM, m) for m in ARM_MACHINES] + [(A64, "raspi3b")]
BSC_C_GO, BSC_C_READ, BSC_C_CLEAR = 0x8080, 0x0001, 0x0010
BSC_S_CLEAR, BSC_S_ERR = 0x0302, 0x0100


class Bsc1:
    """Polled BSC1 transfers through qtest. QEMU finishes each at once."""

    def __init__(self, q, machine):
        self.q, self.base = q, BSC1[machine]

    def _go(self, addr, n, read, data=()):
        # The model sends FIFO bytes as they are written, so DLEN and A come first.
        self.q.writel(self.base + 0x04, BSC_S_CLEAR)
        self.q.writel(self.base + 0x0C, addr)
        self.q.writel(self.base + 0x08, n)
        self.q.writel(self.base, 0x8000 | BSC_C_CLEAR)
        for b in data:
            self.q.writel(self.base + 0x10, b)
        self.q.writel(self.base, BSC_C_GO | (BSC_C_READ if read else 0))
        return not self.q.readl(self.base + 0x04) & BSC_S_ERR

    def write(self, addr, data):
        return self._go(addr, len(data), False, data)

    def read(self, addr, reg, n=1):
        assert self.write(addr, [reg])
        assert self._go(addr, n, True)
        return [self.q.readl(self.base + 0x10) & 0xFF for _ in range(n)]

    def regs(self, addr):
        return [self.read(addr, r)[0] for r in range(4)]


def pca_pins(q, path):
    return sum((q.execute("qom-get", path=path, property=f"pin{p}") == "high") << p
               for p in range(8))


PCA_ARGS = ["-device", "pca9554,bus=i2c-bus.1,address=0x20,id=x0",
            "-device", "pca9554,bus=i2c-bus.1,address=0x27,id=x1",
            "-device", "pca9554,bus=i2c-bus.1,address=0x38,id=x2",
            "-device", "pca9554,bus=i2c-bus.1,address=0x3f,id=x3"]


@pytest.mark.parametrize("binary,machine", PCA_MACHINES, ids=[m for _, m in PCA_MACHINES])
def test_v17_pca9554_registers(binary, machine):
    """V17a: pca9554 sits on i2c-bus.1 at both address ranges; data sheet power-on values."""
    need(binary, machine)
    with Qemu(binary, machine, PCA_ARGS, qtest=True) as q:
        bus = Bsc1(q, machine)
        for addr in (0x20, 0x27, 0x38, 0x3F):
            # input, output, polarity, configuration: all inputs, pulled high.
            assert bus.regs(addr) == [0xFF, 0xFF, 0x00, 0xFF], hex(addr)
        assert not bus.write(0x22, [0])  # nothing there: NACK
        # Pointer bits 1:0 only; a multi-byte read repeats the register.
        assert bus.write(0x20, [1, 0x5A])
        assert bus.read(0x20, 5) == [0x5A]
        assert bus.read(0x20, 1, 3) == [0x5A, 0x5A, 0x5A]
        # An output pin reads back its output bit in the input register.
        assert bus.write(0x20, [3, 0xF0])
        assert bus.read(0x20, 0) == [0xFA]
        # The polarity register inverts the input register.
        assert bus.write(0x20, [2, 0xFF])
        assert bus.read(0x20, 0) == [0x05]


@pytest.mark.parametrize("binary,machine", PCA_MACHINES, ids=[m for _, m in PCA_MACHINES])
def test_v17_pca9554_pins_from_outside(binary, machine):
    """V17b: qom-get pinN shows config|output; set_irq_in ANDs with the pin; qom-set rewrites registers."""
    need(binary, machine)
    path = "/machine/peripheral/x0"
    with Qemu(binary, machine, PCA_ARGS, qtest=True) as q:
        bus = Bsc1(q, machine)
        assert pca_pins(q, path) == 0xFF
        assert bus.write(0x20, [1, 0x5A]) and bus.write(0x20, [3, 0xF0])
        assert pca_pins(q, path) == 0xFA  # outputs show their level, inputs show 1
        # The GPIO inputs drive input pins and leave the registers alone.
        q.set_irq_in(path, 4, 0)
        q.set_irq_in(path, 1, 1)  # an output: no effect
        assert bus.regs(0x20) == [0xEA, 0x5A, 0x00, 0xF0]
        assert pca_pins(q, path) == 0xFA  # qom-get does not show driven inputs
        q.set_irq_in(path, 4, 1)
        assert bus.read(0x20, 0) == [0xFA]
        # A low on an output driven high wins: the pin reads 0, a wired AND.
        q.set_irq_in(path, 1, 0)
        assert bus.read(0x20, 0) == [0xF8]
        q.set_irq_in(path, 1, 1)
        assert bus.read(0x20, 0) == [0xFA]
        # qom-set makes the pin an output at that level: not an outside drive.
        q.execute("qom-set", path=path, property="pin7", value="low")
        assert bus.regs(0x20) == [0x7A, 0x5A, 0x00, 0x70]


# V18: where raspi3b's cores 1 to 3 start, and where the harness leaves them.
WFI = 0xE320F003


def cpu_states(q):
    """[(pc, state)] per core from `info registers -a`. state is "EL3h" or "hyp32"."""
    out = []
    for blk in q.hmp("info registers -a").split("CPU#")[1:]:
        pc = int(re.search(r"(?:\bPC|R15)=([0-9a-f]+)", blk).group(1), 16)
        state = re.search(r"\b(EL\d[th]|usr32|svc32|hyp32|mon32|abt32|und32|irq32|fiq32)\b", blk)
        out.append((pc, state.group(1) if state else "?"))
    return out


def test_v18_raspi3b_secondaries_start_at_0_in_el3():
    """V18a: without help, raspi3b's cores 1 to 3 reset in AArch64 EL3 at 0x0 and trap at 0x200."""
    need(A64, "raspi3b")
    # A32 code at 0x0, as the firmware stub is: not valid A64 for the secondaries.
    args = loader_words(0x80000, [0x14000000], entry=0x80000) + loader_words(0x0, [LOOP]) + ["-S"]
    with Qemu(A64, "raspi3b", args) as q:
        cpus = cpu_states(q)
        assert cpus[1:] == [(0x0, "EL3h")] * 3, cpus
        run_briefly(q)
        cpus = cpu_states(q)
    assert len(cpus) == 4, cpus
    assert cpus[0] == (0x80000, "EL3h"), cpus
    # The EL3 synchronous vector with VBAR_EL3 = 0: they loop there for good.
    assert cpus[1:] == [(0x200, "EL3h")] * 3, cpus


def test_v18_raspi3b_harness_parks_secondaries(tmp_path):
    """V18b: with boot(), raspi3b's cores 1 to 3 sit in the stub's AArch32 wfi loop."""
    need(A64, "raspi3b")
    from harness.boot import boot
    img = tmp_path / "kernel.img"
    img.write_bytes(struct.pack("<I", LOOP))
    with boot(str(img), "raspi3b", keepalive=False) as vm:
        time.sleep(0.3)
        q = vm.qmp
        q.execute("stop")
        cpus = cpu_states(q)
        words = {pc: int(q.hmp(f"xp/1wx {pc:#x}").split()[1], 16) for pc, _ in cpus[1:]}
    assert cpus[0] == (0x8000, "hyp32"), cpus
    for pc, state in cpus[1:]:
        assert state == "hyp32", cpus
        assert pc < 0x8000, cpus  # in the stub, not the image
        assert words[pc] in (WFI, 0xEAFFFFFD), (hex(pc), hex(words[pc]))  # wfi; b <wfi>


# V19: Pi GPIO levels from outside, through qtest writes to GPSET/GPCLR.
GPFSEL2, GPSET0, GPSET1, GPCLR0, GPCLR1 = 0x08, 0x1C, 0x20, 0x28, 0x2C
GPLEV0, GPLEV1, GPPUD, GPPUDCLK0 = 0x34, 0x38, 0x94, 0x98
# Event detect: status, rising, falling, high, low, async rising, async falling.
GPIO_EVENT_REGS = (0x40, 0x4C, 0x58, 0x64, 0x70, 0x7C, 0x88)
IRQ_PENDING2 = 0xB208   # from the peripheral base; GPU IRQ 49 (gpio_int[0]) is bit 17
GPIO_MACHINES = [(ARM, m) for m in ARM_MACHINES] + [(A64, "raspi3b")]
PERIPH = dict(BASE, raspi3b=0x3F000000)
GPIO = {m: b + 0x200000 for m, b in PERIPH.items()}


@pytest.mark.parametrize("binary,machine", GPIO_MACHINES, ids=[m for _, m in GPIO_MACHINES])
def test_v19_gpio_level_from_gpset(binary, machine):
    """V19a: GPSET/GPCLR set GPLEV on input pins too; GPLEV writes, pulls and events do nothing."""
    need(binary, machine)
    g = GPIO[machine]
    with Qemu(binary, machine, qtest=True) as q:
        qom = [p["name"] for p in q.execute("qom-list", path="/machine/soc/peripherals/gpio")]
        assert not [p for p in qom if p.startswith("unnamed-gpio-in")], qom  # no input lines
        assert q.readl(g) == 0 and q.readl(g + GPLEV0) == 0  # every pin an input, reading 0
        q.writel(g + GPLEV0, 1 << 17)
        assert q.readl(g + GPLEV0) == 0                    # GPLEV is read-only
        q.writel(g + GPSET0, (1 << 17) | (1 << 23))
        assert q.readl(g + GPLEV0) == (1 << 17) | (1 << 23)  # sticks on input pins
        q.writel(g + GPCLR0, 1 << 17)
        assert q.readl(g + GPLEV0) == 1 << 23
        q.writel(g + GPSET1, 1 << 15)
        assert q.readl(g + GPLEV1) == 1 << 15              # bank 1, pin 47
        q.writel(g + GPCLR0, 1 << 23)
        q.writel(g + GPCLR1, 1 << 15)
        # Pull-up on pin 17: the undriven pin still reads 0.
        q.writel(g + GPPUD, 2)
        q.writel(g + GPPUDCLK0, 1 << 17)
        q.writel(g + GPPUDCLK0, 0)
        q.writel(g + GPPUD, 0)
        assert q.readl(g + GPLEV0) == 0
        # Event detect is not modelled: the registers read 0, no IRQ.
        for off in GPIO_EVENT_REGS:
            q.writel(g + off, 1 << 17)
        q.writel(g + GPSET0, 1 << 17)
        q.writel(g + GPCLR0, 1 << 17)
        assert [q.readl(g + off) for off in GPIO_EVENT_REGS] == [0] * len(GPIO_EVENT_REGS)
        assert q.readl(PERIPH[machine] + IRQ_PENDING2) & (1 << 17) == 0
        # An output pin shows what was written to it.
        q.writel(g + GPFSEL2, 1 << 15)                     # pin 25 output
        q.writel(g + GPSET0, 1 << 25)
        assert q.readl(g + GPLEV0) == 1 << 25


@pytest.mark.parametrize("binary,machine", GPIO_MACHINES, ids=[m for _, m in GPIO_MACHINES])
def test_v19_gpio_level_while_guest_runs(binary, machine, raw_image):
    """V19b: under TCG a qtest level write takes well under 5 ms; guest time follows host time."""
    need(binary, machine)
    g, clo = GPIO[machine], PERIPH[machine] + 0x3004
    with Qemu(binary, machine, ["-kernel", raw_image], qtest=True, qtest_accel=None) as q:
        q.execute("cont")
        took = []
        for i in range(200):
            t = time.perf_counter()
            q.writel(g + (GPSET0 if i & 1 else GPCLR0), 1 << 23)
            took.append(time.perf_counter() - t)
        took.sort()
        assert took[100] < 0.005 and took[190] < 0.02, took[-10:]
        t0, c0 = time.perf_counter(), q.readl(clo)
        time.sleep(0.5)
        c1, t1 = q.readl(clo), time.perf_counter()
        assert abs((c1 - c0) / ((t1 - t0) * 1e6) - 1) < 0.05

# V20 to V23: what QEMU offers a future SD card driver in SPI mode on SPI0.
SPI_MACHINES = [(ARM, m) for m in ARM_MACHINES] + [(A64, "raspi3b")]
SPI_IDS = [m for _, m in SPI_MACHINES]


def periph(machine):
    return BASE.get(machine, 0x3F000000)


@pytest.mark.parametrize("binary,machine", SPI_MACHINES, ids=SPI_IDS)
def test_v20_no_sd_card_on_spi0(binary, machine, tmp_path):
    """V20: ssi-sd cannot be created, and sd-card-spi only plugs into an SD bus, not SPI0."""
    need(binary, machine)
    rc, err = Qemu.run_to_exit(binary, machine, ["-S", "-device", "ssi-sd,bus=spi"])
    assert rc != 0, err
    assert "pluggable device type" in err, err
    img = tmp_path / "card.img"
    img.write_bytes(bytes(1 << 20))
    rc, err = Qemu.run_to_exit(binary, machine, [
        "-S", "-drive", f"if=none,id=hd,file={img},format=raw",
        "-device", "sd-card-spi,drive=hd,bus=spi"])
    assert rc != 0, err
    args = ["-S", "-drive", f"if=none,id=hd,file={img},format=raw",
            "-device", "sd-card-spi,drive=hd,id=c1"]
    with Qemu(binary, machine, args) as q:
        tree = q.hmp("info qtree")
    spi = tree[tree.index("dev: bcm2835-spi"):]
    spi = spi[:spi.index("\n  dev: ")]
    assert "bus: spi" in spi and "dev: " not in spi.split("bus: spi", 1)[1], spi
    sdhci = tree[tree.index("dev: generic-sdhci"):]
    assert 'dev: sd-card-spi, id "c1"' in sdhci, sdhci


def spi_xfer(q, base, csbits, data):
    q.writel(base, 0x30 | csbits)
    q.writel(base, 0x80 | csbits)
    out = []
    for b in data:
        q.writel(base + 4, b)
        out.append(q.readl(base + 4))
    status = q.readl(base)
    q.writel(base, 0x30 | csbits)
    return out, status


@pytest.mark.parametrize("binary,machine", SPI_MACHINES, ids=SPI_IDS)
def test_v21_spi0_ignores_chip_select(binary, machine):
    """V21: SPI0 drives no CS lines. Every device on "spi" gets every byte, replies OR."""
    need(binary, machine)
    rc, err = Qemu.run_to_exit(binary, machine, ["-S", "-device", "w25q32,bus=spi",
                                                 "-device", "m25p80,bus=spi"])
    assert rc != 0 and "CS index '0x0' in use" in err, err
    base = periph(machine) + 0x204000
    args = ["-device", "w25q32,bus=spi,cs=0", "-device", "m25p80,bus=spi,cs=1"]
    for cs in (0, 1, 2):
        with Qemu(binary, machine, args, qtest=True) as q:
            out, status = spi_xfer(q, base, cs, [0x9F, 0, 0, 0])
            assert status & 3 == cs  # the CS field is stored
            # JEDEC IDs EF 40 16 (w25q32) and 20 20 14 (m25p80), ORed.
            assert out == [0, 0xEF, 0x60, 0x16], (cs, [hex(b) for b in out])
            # No CS edge between commands: the flashes never see a new one.
            assert spi_xfer(q, base, cs, [0x9F, 0, 0, 0])[0] == [0, 0, 0, 0]


@pytest.mark.parametrize("binary,machine", SPI_MACHINES, ids=SPI_IDS)
def test_v22_sd_read_only_and_eject(binary, machine, tmp_path):
    """V22: a read-only drive is refused as an SD card. QMP eject and change-medium work."""
    need(binary, machine)
    img = tmp_path / "card.img"
    img.write_bytes(bytes(1 << 20))
    for args in (["-drive", f"if=sd,file={img},format=raw,readonly=on"],
                 ["-drive", f"if=none,id=hd,file={img},format=raw,readonly=on",
                  "-device", "sd-card-spi,drive=hd"]):
        rc, err = Qemu.run_to_exit(binary, machine, ["-S"] + args)
        assert rc != 0 and "Cannot use read-only drive as SD card" in err, err
    present = periph(machine) + 0x300024  # EMMC present state
    with Qemu(binary, machine, ["-drive", f"if=sd,file={img},format=raw"], qtest=True) as q:
        drive = [b for b in q.execute("query-block") if b["device"] == "sd0"][0]
        assert drive["removable"] and "inserted" in drive
        assert q.readl(present) & 0x50000 == 0x50000  # inserted, stable
        q.execute("eject", id=drive["qdev"], force=True)
        assert q.readl(present) & 0x50000 == 0
        q.execute("blockdev-change-medium", id=drive["qdev"], filename=str(img), format="raw")
        assert q.readl(present) & 0x50000 == 0x50000


SPI_READ_LOOP = """.global _start
_start: ldr r0, =BASE
 mov r1, #0x30
 str r1, [r0]
 mov r1, #0x80
 str r1, [r0]
 ldr r2, =COUNT
 mov r3, #0
1: str r3, [r0, #4]
2: ldr r1, [r0]
 tst r1, #0x20000
 beq 2b
 ldr r1, [r0, #4]
 subs r2, r2, #1
 bne 1b
3: b 3b
"""


@pytest.mark.parametrize("machine", ["raspi0", "raspi1ap", "raspi2b", "raspi3b"])
def test_v23_spi0_byte_rate(machine, tmp_path):
    """V23: guest code moves 2 MiB through the SPI0 FIFO, one byte at a time, in under 10 s."""
    need(A64 if machine == "raspi3b" else ARM, machine)
    gcc = shutil.which("arm-none-eabi-gcc")
    objcopy = shutil.which("arm-none-eabi-objcopy")
    if gcc is None or objcopy is None:
        pytest.skip("arm-none-eabi-gcc not installed")
    from harness.boot import boot
    src, elf, img = tmp_path / "k.S", tmp_path / "k.elf", tmp_path / "kernel.img"
    src.write_text(SPI_READ_LOOP.replace("BASE", hex(periph(machine) + 0x204000))
                   .replace("COUNT", str(2 << 20)))
    subprocess.run([gcc, "-nostdlib", "-Wl,-Ttext=0x8000", "-Wl,--build-id=none",
                    "-o", str(elf), str(src)], check=True)
    subprocess.run([objcopy, "-O", "binary", str(elf), str(img)], check=True)
    start = time.monotonic()
    with boot(str(img), machine, extra_args=["-device", "w25q32,bus=spi"],
              keepalive=False, timeout=30) as vm:
        # Done means the PC sits in the completion loop at 0x8038 with R02 at
        # zero. R02 alone is also zero before the stub reaches the image.
        r = vm.qmp.registers()
        while not (r["R15"] == 0x8038 and r["R02"] == 0):
            if time.monotonic() - start > 20:
                pytest.fail(f"{machine}: SPI loop not done after 20 s: "
                            f"PC={r['R15']:#x} R02={r['R02']:#x}")
            time.sleep(0.05)
            r = vm.qmp.registers()
    wall = time.monotonic() - start
    print(f"{machine}: 2 MiB through SPI0 in {wall:.2f} s")
    assert wall < 10, wall
