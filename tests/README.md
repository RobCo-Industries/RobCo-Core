# Tests

## Run

    make venv
    make test-host            # C unit tests for src/lib and src/fs on the host
    make test-tools           # pytest for tools/, the Makefile and fs_tool
    make test-docs            # links, API coverage and words in the Markdown files
    make test-canary
    make test-examples        # every example and variant, warning-free
    make test-target          # spikes and examples on all four boards through boot()
    make test-target BOARD=raspi0   # the same on one board

`BOARD` defaults to `all`. It is passed to pytest as `--board`, so it
takes a board name or `all`. The reusable workflow sets it per board.

Needs `qemu-system-arm` and `qemu-system-aarch64` on `PATH`. A machine
missing from the installed QEMU is skipped. V2 is skipped without
`arm-none-eabi-gcc`.

## Host tests

    make test-host                  # build and run every tests/host/test_*.c
    make test-host HOST_SANITIZE=0  # without ASan and UBSan
    make test-host HOST_SANITIZE=require  # fail if they are missing (CI)
    build/host/test_alloc realloc   # run only tests whose name contains "realloc"

`make test` runs them first. Each `tests/host/test_<name>.c` becomes
`build/host/test_<name>`, linked with every real `src/lib/*.c`, the file
system layer (`src/fs/*.c` and FatFs) and the file-backed block device
in `host/blk_file.c`, and built with the host `cc`. `make test-host` also
builds `build/host/fs_tool`, which `tools/test_fs_interop.py` runs. Code in `src/lib/` must not touch registers or
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
- `host/`: host unit tests, `rc_test.h`, the file-backed block device
  `blk_file.c` and `fs_tool.c`.
- `target/`: tests that boot real images. `--board NAME` picks boards
  (repeatable, default raspi0 and raspi2b, `all` for all four). They
  boot the spikes and the examples `hello`, `tick`, `gpio`, `console`,
  `heap`, `demo`, `sdraw`, `mmu`, `input`, `audio`, `rtc` and `spi`.
  `test_sdraw.py` attaches an SD image. `test_input.py` drives the
  serial and script backends, the second over the mini UART.
  `test_rtc.py` attaches QEMU's `ds1338` model at I2C address `0x68`.
  `test_spi.py` attaches a `w25q32` SPI flash. `demo` and `chain1` also
  run with the MMU and caches on. `test_chain.py` boots `chain1`, which validates
  `chain2` (placed at 0x00200000 by a loader device) and hands off to it.
  It also covers a bad CRC, a wrong magic, a wrong `load_addr`, a
  `mem_size` past the slot, no image, `chain2` alone, a corrupt boot
  info block, and a tick in `chain1` that must leave no stale timer
  interrupt for `chain2`.
- `tools/`: pytest unit tests for the host tools: `tools/rimg.py` on
  images built in memory, one case per error code, the font and doc
  tools, the `SLOT_END` link check, the `BOARD` variable of
  `test-target`, and the argument handling, refusal cases and
  `config.txt` of `fetch_firmware.sh` and `mkbootdir.sh`. The firmware
  download test runs only with `ROBCO_NET_TESTS=1`.
  `test_fs_interop.py` checks `fs_tool` against images made with mtools
  and, when installed, `fsck.vfat` or `fsck_msdos`.
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

## What QEMU cannot prove

Caches, PWM sound, GPIO input, real SPI signalling and SD pin routing
on boards with WiFi need a real board. The checklist is in
[docs/HARDWARE_VALIDATION.md](../docs/HARDWARE_VALIDATION.md).
