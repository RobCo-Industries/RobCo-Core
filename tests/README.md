# Tests

## Run

    make venv
    make test-canary
    make test-target          # spikes and examples on all four boards through boot()

Needs `qemu-system-arm` and `qemu-system-aarch64` on `PATH`. A machine
missing from the installed QEMU is skipped. V2 is skipped without
`arm-none-eabi-gcc`.

## Host tests

    make test-host                  # build and run every tests/host/test_*.c
    make test-host HOST_SANITIZE=0  # without ASan and UBSan
    make test-host HOST_SANITIZE=require  # fail if they are missing (CI)
    build/host/test_alloc realloc   # run only tests whose name contains "realloc"

`make test` runs them first. Each `tests/host/test_<name>.c` becomes
`build/host/test_<name>`, linked with every real `src/lib/*.c` and built
with the host `cc`. Code in `src/lib/` must not touch registers or
drivers; that code belongs in `src/drv/`, `src/arch/`, `src/board/` or
`src/gfx/`. ASan and UBSan are on when the compiler supports
them. The build defines `ROBCO_HOST`, which drops the library's own
`memcpy`, `memset`, `memmove` and `memcmp` so the host libc ones are used.

To add a test, create `tests/host/test_<name>.c`:

```c
#include <robco/crc32.h>

#include "rc_test.h"

TEST(check_value)
{
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u);
}
```

`rc_test.h` gives `CHECK`, `CHECK_EQ`, `CHECK_STR` and `CHECK_MEM`. A
failed check prints file, line and values, and the test goes on.

What fits: code in `src/lib/` that touches no hardware and makes no
assumption about pointer size (the host is 64-bit, the target 32-bit).
What does not: drivers, MMIO, assembly, exceptions, timing and anything
in `src/arch`, `src/board` or `src/drv`. Test those under `target/`.

## Layout

- `harness/qemu.py`: starts QEMU headless and talks QMP, HMP (through
  QMP) and qtest. It can poke instruction words with `-device loader`
  and capture serial output to files. It kills QEMU on exit, even after
  a timeout or a failed test.
- `harness/boot.py`: boots a raw image at 0x8000 like the GPU firmware.
  Gives serial read and write, `@RC` marker parsing and screendumps.
- `harness/sdimage.py`: builds MBR + FAT SD images with mtools.
- `fwstub/`: the firmware stub `boot()` loads at 0x0. It parks secondary
  cores, writes ATAGS at 0x100, switches to HYP on raspi2b and enters
  the image with r0 = 0, r1 = machine id, r2 = 0x100. `a64_shim.py`
  holds the AArch64 words that drop raspi3b to AArch32 first.
- `host/`: host unit tests and `rc_test.h`.
- `target/`: tests that boot real images. `--board NAME` picks boards
  (repeatable, default raspi0 and raspi2b, `all` for all four). They
  boot the spikes and the examples `hello`, `tick`, `gpio`, `console`
  and `heap`.
- `canary/`: checks the QEMU behaviours our code depends on. Run it
  after any QEMU upgrade. A failure means QEMU changed, not our code.

## Canary facts

| ID  | Machines | Fact | Why we care |
|-----|----------|------|-------------|
| V1  | raspi0, 1ap, 2b | Raw `-kernel` loads at 0x10000. Entry: SVC, r0=0, r1=0xC42, r2=0x100, ATAGS at 0x100. | Link address and boot register handling for `-kernel` runs. |
| V2  | raspi0, 1ap, 2b | ELF `-kernel` loads at its link address. | ELF builds can boot at 0x8000 like real hardware. |
| V3  | raspi0, 1ap, 2b | `-device loader,...,addr=0x8000,force-raw=on,cpu-num=0` loads and starts at 0x8000. | Run the same raw image as the real board. |
| V5  | raspi3b | AArch64 cannot be turned off. Seven AArch64 words drop to AArch32 HYP (or SVC) at 0x8000. | Pi 3 needs an EL3 shim to run our 32-bit kernel. |
| V6  | raspi0, 2b | `mrc p15,0,r1,c0,c0,5` gives MIDR on raspi0, MPIDR (bit 31 set) on raspi2b. | Core ID code must handle both. |
| V7  | raspi0, 2b | PWM read data-aborts on raspi0, works on raspi2b. | Do not touch PWM on raspi0 under QEMU. |
| V9  | raspi0, 1ap, 2b | Mailbox channel 8 answers board revision, ARM memory, UART clock (3 MHz). | Property mailbox driver and UART baud setup. |
| V10 | raspi0, 1ap, 2b | Timer C1 match sets CS bit 1 and IRQ 1 pending. Writing CS bit 1 clears it. | Timer interrupt driver. |
| V11 | raspi0, 1ap, 2b | `-drive if=sd` attaches to the SDHCI (EMMC) controller. Size must be a power of 2. | SD driver targets EMMC. Disk images need padding. |
| V12 | raspi0, 1ap, 2b | GPLEV0/1 read 0. No way to inject GPIO input. | GPIO input tests cannot use QEMU. |
| V13 | raspi0, 1ap, 2b | QMP `screendump` works headless and writes a PPM. | Framebuffer tests in CI. |
| V14 | raspi0, 1ap, 2b | PM_WDOG then PM_RSTC full reset emits a QMP `RESET` event. | Reboot and watchdog tests. |
| V15 | raspi0, 1ap, 2b | First `-serial` is the PL011, second is the mini UART. | Serial capture order in tests. |

V4 and V8 are not checked here.

## Writing a target test

Put it under `tests/target/`. Take the `board` fixture to run it on each
`--board`:

```python
import re
from harness.boot import boot

def test_hello(board, spike_image):
    with boot(spike_image("s1"), board) as vm:
        vm.wait_for("type to echo", timeout=10)
        hello = [m for m in vm.markers() if m["ev"] == "hello"]
        assert hello[0]["fw_r2"] == "0x100"
        vm.send("abc")
        vm.wait_for(re.compile(r"\nabc"), timeout=5)
```

Examples build through the session fixture `example_image`.
`example_image(name, variant=None, extra_cflags=None)` builds
`examples/<name>` once per (name, variant) with `CI=1` and returns the
path of its `.img`. A variant builds into `build/<variant>`:

```python
from harness.boot import boot

def test_my_example(board, example_image):
    with boot(example_image("hello"), board) as vm:
        vm.wait_for("ev=ready", timeout=10)

def test_my_fault(board, example_image):
    img = example_image("hello", "fault", "-DHELLO_FAULT=1")
    with boot(img, board) as vm:
        vm.wait_for("ev=exception", timeout=10)
```

`boot()` options: `mode="svc"` forces SVC entry, `sd=path` attaches an
SD image, `serial2=True` adds the mini UART, `extra_args=[...]` goes
straight to QEMU. `vm.qmp` is the `Qemu` object from `harness/qemu.py`.
QEMU is killed when the `with` block ends, even on failure.
