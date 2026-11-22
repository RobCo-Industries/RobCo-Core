# Getting started

This tutorial takes you from an empty directory to a RobCo-Core image that boots in QEMU and passes an automated test. You build one project, `hello-pi`, that prints its board and echoes what you type.

Every command on this page was run as written. The function reference is in [API.md](./API.md). The repository map is in [PROJECT_LAYOUT.md](./PROJECT_LAYOUT.md).

## Prerequisites

| Tool | Version | Used for |
|---|---|---|
| `arm-none-eabi-gcc` and binutils | Any recent release. CI uses Ubuntu 24.04's `gcc-arm-none-eabi`. | Building images |
| `qemu-system-arm` and `qemu-system-aarch64` | 10.1.x. The tests are verified on 10.1.2. | Running images |
| mtools (`mformat`, `mcopy`, `mmd`) | Any | SD card images in tests |
| Python 3 with `venv` | Any 3.x supported by pytest 8. CI uses 3.12. | Sealing images and the test harness |
| GNU make, git, a host C compiler | Any | Build, submodules, host unit tests |

Older QEMU releases model the `raspi` machines differently. Ubuntu's packaged QEMU is 8.x, which is too old. The CI container in [ci/README.md](../../ci/README.md) holds the exact versions.

On macOS with Homebrew:

```sh
brew install arm-none-eabi-gcc qemu mtools python
```

Check what you have:

<!-- check: runs -->
```sh
arm-none-eabi-gcc --version
qemu-system-arm --version
python3 --version
```

Also check `qemu-system-aarch64 --version` and `mtools --version`. The QEMU version line must read 10.1.

## Build and test the library

Clone the library and run its tests once. This proves your tools work before you write any code.

```sh
git clone https://github.com/RobCo-Industries/RobCo-Core.git
cd RobCo-Core
make venv && make test
```

`make venv` creates `.venv` with pytest and Pillow. `make test` runs the host unit tests, the tool tests, the documentation check and the QEMU canary suite. It then builds every example warning-free and boots every spike and example on `raspi0`, `raspi1ap`, `raspi2b` and `raspi3b`. It takes about three minutes. The last line of a good run is a pytest summary with no failures.

## Create a project

A project is a directory with a `Makefile`, a `robco_config.h`, your sources and the library as a git submodule. Make it:

```sh
mkdir hello-pi && cd hello-pi
git init
git submodule add https://github.com/RobCo-Industries/RobCo-Core.git robco-core
```

When you are done with this tutorial the project looks like this:

```
hello-pi/
  Makefile
  robco_config.h
  main.c
  robco-core/          git submodule
  tests/
    test_boot.py
  build/               created by make
  .venv/               created by make venv
```

Add `build/` and `.venv/` to `.gitignore`.

### Makefile

```make
ROBCO_CORE := robco-core
IMAGE      := hello-pi
LINK_ADDR  := 0x8000
SRCS       := main.c
CONFIG_H   := robco_config.h

include $(ROBCO_CORE)/mk/robco.mk

.PHONY: venv test
venv: .venv/.installed

.venv/.installed: $(ROBCO_CORE)/tests/requirements.txt
	$(PYTHON) -m venv .venv
	.venv/bin/pip install -q -r $<
	touch $@

test: all venv
	.venv/bin/pytest tests -v
```

Recipe lines start with a tab. The first five variables and the `include` are all a build needs. The `venv` and `test` targets are for the test you add below.

Every variable `mk/robco.mk` reads:

| Variable | Default | Meaning |
|---|---|---|
| `ROBCO_CORE` | none, required | Path to the library |
| `IMAGE` | none, required | Base name of the outputs |
| `LINK_ADDR` | none, required | Link and load address. `0x8000` for an image the firmware loads. It must equal the slot base of your subtype. |
| `CONFIG_H` | none, required | Path to `robco_config.h`. Its directory is added to the include path. |
| `SRCS` | empty | Your `.c` and `.S` files, relative to the project directory |
| `BUILD` | `build` | Output directory |
| `CROSS` | `arm-none-eabi-` | Toolchain prefix |
| `PYTHON` | `python3` | Python used to seal the image |
| `EXTRA_CFLAGS` | empty | Added to every compile, library included |
| `CI` | empty | `CI=1` adds `-Werror` |
| `SLOT_END` | empty | Sets `ROBCO_SLOT_END` for the linker script |
| `SDIMAGE_DIR` | empty | A tree `make sdimage` copies to the card root |
| `SDIMAGE_FILES` | empty | `source:destination` pairs `make sdimage` copies, such as `$(BUILD)/hello-pi.img:HELLO.IMG` |
| `SDIMAGE_DEPS` | empty | Extra prerequisites of `make sdimage` |
| `SDIMAGE_MIB` | `64` | Card size in MiB |

The fragment compiles every `.c` and `.S` file under `robco-core/src/*/` into `build/librobco.a`. The linker keeps only what your image uses.

### robco_config.h

`mk/robco.mk` forces this file into every C file, every assembly file and the linker script. So it may hold preprocessor macros only: no declarations, no types.

```c
#ifndef ROBCO_CONFIG_H
#define ROBCO_CONFIG_H

/* Preprocessor macros only: this file is also forced into asm and the linker script. */

#define ROBCO_STAGE_NAME        "hellopi"
#define ROBCO_FIRST_STAGE       1

#define ROBCO_IMG_MAGIC         RIMG_MAGIC_BOOT
#define ROBCO_IMG_SUBTYPE       RIMG_BOOT_EXAMPLE
#define ROBCO_IMG_ABI_VERSION   (2 << 16)
#define ROBCO_IMG_VERSION       1
#define ROBCO_IMG_NAME          "hello-pi"

#endif
```

Every macro the library reads, with its default when you leave it out:

| Macro | Default | Read by | Meaning |
|---|---|---|---|
| `ROBCO_STAGE_NAME` | `"robco"` | `exc.h` | `stage=` name in the markers the library prints |
| `ROBCO_FIRST_STAGE` | `0` | `bootinfo.h` | 1 for an image the firmware loads. It then ignores any boot info block in memory. |
| `ROBCO_IMG_MAGIC` | `RIMG_MAGIC_BOOT` | `rimg.h` | Header magic |
| `ROBCO_IMG_SUBTYPE` | `RIMG_BOOT_EXAMPLE` | `rimg.h` | Header subtype. Picks the slot the image must fit. |
| `ROBCO_IMG_ABI_VERSION` | `(2 << 16)` | `rimg.h` | Header ABI version, `(major << 16) \| minor` |
| `ROBCO_IMG_VERSION` | `0` | `rimg.h` | Header image version |
| `ROBCO_IMG_NAME` | `"robco"` | `rimg.h` | Header name, at most 16 bytes |
| `ROBCO_IMG_FLAGS` | `0` | `rimg.h` | Header flags. Must not set `RIMG_FLAG_META`. |
| `ROBCO_STACK_SVC_SIZE` | `0x4000` | `ld/image.ld.S` | Stack of `robco_main()` |
| `ROBCO_STACK_IRQ_SIZE` | `0x1000` | `ld/image.ld.S` | Stack of interrupt handlers |
| `ROBCO_STACK_FIQ_SIZE` | `0x1000` | `ld/image.ld.S` | FIQ stack |
| `ROBCO_STACK_ABT_SIZE` | `0x1000` | `ld/image.ld.S` | Abort stack |
| `ROBCO_STACK_UND_SIZE` | `0x1000` | `ld/image.ld.S` | Undefined instruction stack |
| `ROBCO_SLOT_END` | Not set | `ld/image.ld.S` | End of the image's slot, such as `0x00100000` for an image at `0x8000`. The link fails if the image does not fit. |
| `ROBCO_MBOX_BOUNCE_SIZE` | `256` | `mbox.h` | Bounce buffer of `mbox_call()` with the data cache on. A multiple of 64. |
| `ROBCO_CONSOLE_MAX_COLS` | `100` | `console.h` | Screen console columns |
| `ROBCO_CONSOLE_MAX_ROWS` | `40` | `console.h` | Screen console rows |
| `ROBCO_FONT` | `ROBCO_FONT_FIXEDSYS` | `font.h` | Built-in console font: Fixedsys Excelsior, or `ROBCO_FONT_SPLEEN` for Spleen. See [FONTS.md](../FONTS.md). |

`RIMG_BOOT_EXAMPLE` has the slot `0x00008000` to `0x000FFFFF`. A real stage uses its own subtype, such as `RIMG_BOOT_BIOS`. The subtypes and their slots are in [API.md](./API.md#image-header-rimgh).

### main.c

The library calls `robco_main()` after it has set up the CPU and detected the board.

```c
#include <robco/board.h>
#include <robco/boot.h>
#include <robco/klibc.h>
#include <robco/uart.h>

void robco_main(void)
{
    const robco_board_t *b = board_get();
    int c;

    uart_init(115200);
    k_printf("\n@RC stage=hellopi ev=boot soc=%s mem=0x%08x\n",
             b->soc_name, (unsigned)b->mem_size);
    k_printf("@RC stage=hellopi ev=ready\n");

    for (;;) {
        c = uart_getc();
        uart_putc(c == '\r' ? '\n' : (char)c);
    }
}
```

`@RC` lines are the markers the tests read. Their format is in [BOOT_PROTOCOL.md](../spec/BOOT_PROTOCOL.md#serial-markers).

## Build

```sh
make CI=1
```

The outputs land in `build/`:

| File | Content |
|---|---|
| `hello-pi.img` | The sealed raw image. This is what you boot. |
| `hello-pi.elf` | The linked ELF, for `gdb` and `objdump` |
| `hello-pi.map` | The linker map |
| `librobco.a` | The library, built with your `robco_config.h` |
| `image.ld` | The linker script after the preprocessor |
| `obj/`, `robco/` | Object files |

The last steps of the build turn the ELF into a raw binary and seal it:

```
arm-none-eabi-objcopy -O binary build/hello-pi.elf build/hello-pi.img.tmp
python3 robco-core/tools/rimg.py seal build/hello-pi.img.tmp
mv build/hello-pi.img.tmp build/hello-pi.img
```

`start.S` already wrote the `rimg` header at offset `0x20`. Sealing checks that word 0 is a branch and that the header is sane, then writes the CRC-32 of the whole file into the header. It then runs the six validation steps of [IMAGE_FORMAT.md](../spec/IMAGE_FORMAT.md#validation) on the result. If the image does not fit its subtype's slot, or `LINK_ADDR` is not the slot base, the build fails here.

Inspect the header:

```sh
python3 robco-core/tools/rimg.py info build/hello-pi.img
```

```
magic=BOOT
hdr_version=0x1
hdr_size=0x40
flags=0x0
load_addr=0x8000
file_size=0x1810
mem_size=0x9aa0
crc32=0x2f9ca34b
abi_version=0x20000
image_version=0x1
subtype=0x7
name=hello-pi
reserved=0x0
crc_ok=1
```

Your sizes and CRC differ with your compiler version. `mem_size` covers the file, BSS and the 32 KiB of stacks.

## Run it in QEMU by hand

The firmware loads the image at `0x8000`. Do the same in QEMU with a loader device:

```sh
qemu-system-arm -M raspi0 -nographic \
  -device loader,file=build/hello-pi.img,addr=0x8000,force-raw=on,cpu-num=0
```

You see:

```
@RC stage=hellopi ev=boot soc=bcm2835 mem=0x1c000000
@RC stage=hellopi ev=ready
```

Type and the image echoes. Press Ctrl-A, then X, to quit QEMU. Use `-M raspi1ap` or `-M raspi2b` for the other 32-bit boards.

Do not use `-kernel build/hello-pi.img`. QEMU loads a raw `-kernel` file at `0x10000`, not `0x8000`. The image does not relocate, so it crashes without any output. The canary test V1 in [tests/README.md](../../tests/README.md#canary-facts) checks this QEMU behaviour.

`raspi3b` exists only in `qemu-system-aarch64`, and its CPU starts in AArch64. Seven AArch64 words at `0x80000` drop it to AArch32 HYP at `0x8000`:

```sh
qemu-system-aarch64 -M raspi3b -nographic \
  -device loader,addr=0x80000,data=0xd2802620,data-len=4 \
  -device loader,addr=0x80004,data=0xd51e1100,data-len=4 \
  -device loader,addr=0x80008,data=0xd2803b40,data-len=4 \
  -device loader,addr=0x8000c,data=0xd51e4000,data-len=4 \
  -device loader,addr=0x80010,data=0xd2900000,data-len=4 \
  -device loader,addr=0x80014,data=0xd51e4020,data-len=4 \
  -device loader,addr=0x80018,data=0xd69f03e0,data-len=4 \
  -device loader,file=build/hello-pi.img,addr=0x8000,force-raw=on \
  -device loader,addr=0x80000,cpu-num=0
```

A run by hand starts the image with r0 to r3 at 0 and no ATAGS. Real firmware passes a machine id and ATAGS, and enters `raspi2b` and `raspi3b` in HYP mode. The test harness reproduces that. Use it for anything beyond a first look.

## Test it with the harness

`boot()` in `robco-core/tests/harness/boot.py` boots an image the way the firmware does. It loads a firmware stub at `0x0` that parks the other cores, writes ATAGS at `0x100` and enters the image at `0x8000` with the firmware's registers and mode. It connects to the serial port and kills QEMU when the test ends.

Create `tests/test_boot.py`:

```python
"""Boot build/hello-pi.img in QEMU through the RobCo-Core harness."""

import os
import re
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "robco-core", "tests"))

from harness.boot import boot  # noqa: E402

IMAGE = os.path.join(ROOT, "build", "hello-pi.img")
BOARDS = os.environ.get("BOARD", "raspi0 raspi2b raspi3b").split()


@pytest.mark.parametrize("board", BOARDS)
def test_boot(board):
    with boot(IMAGE, board) as vm:
        vm.wait_for("ev=ready", timeout=10)
        boots = [m for m in vm.markers() if m["ev"] == "boot"]
        assert boots[0]["stage"] == "hellopi"
        vm.send("abc")
        vm.wait_for(re.compile(r"ev=ready\n.*abc", re.S), timeout=5)
```

Run it:

```sh
make test
```

```
tests/test_boot.py::test_boot[raspi0] PASSED
tests/test_boot.py::test_boot[raspi2b] PASSED
tests/test_boot.py::test_boot[raspi3b] PASSED
```

`make test BOARD=raspi2b` runs one board. The first run creates `.venv` from `robco-core/tests/requirements.txt` and builds the firmware stub inside the submodule's ignored `build/` directory.

What the test uses:

| Call | Does |
|---|---|
| `boot(image, board)` | Starts QEMU on `raspi0`, `raspi1ap`, `raspi2b` or `raspi3b` and yields a `Vm` |
| `vm.wait_for(text_or_regex, timeout)` | Waits for serial output. Raises with the whole log on timeout. |
| `vm.markers()` | Every `@RC` line so far, as dicts such as `{"stage": "hellopi", "ev": "boot", ...}` |
| `vm.send(text)` | Types on the serial input |
| `vm.serial` | All serial text so far, CRLF turned into LF |
| `vm.screendump(path)` | Writes the framebuffer as a PPM file |

`boot()` also takes `mode="svc"`, `sd=path` for an SD card image, `serial2=True` for the mini UART and `extra_args=[...]` for QEMU. [tests/README.md](../../tests/README.md#writing-a-target-test) has more examples.

## Run it in CI

The library ships a reusable GitHub workflow that runs `make CI=1` and then `make CI=1 test BOARD=<board>` per board, inside the pinned CI container. Add `.github/workflows/ci.yml`:

```yaml
name: ci
on: [push, pull_request]
permissions:
  contents: read
  packages: read
jobs:
  ci:
    uses: RobCo-Industries/RobCo-Core/.github/workflows/robco-ci.yml@<submodule commit>
    with:
      image-tag: <contents of robco-core/ci/IMAGE_TAG>
      test-targets: test
```

Pin both values to the submodule. `<submodule commit>` is the full hash `git -C robco-core rev-parse HEAD` prints, never `main`, so CI runs the same library as your build. The tag comes from `robco-core/ci/IMAGE_TAG` at that commit. The `permissions` block is required: the workflow reads the CI image from the GitHub package registry.

Both values are copies, so they drift when you move the submodule. `tools/check_pins.sh` compares them. Run it from the project root:

```sh
robco-core/tools/check_pins.sh .github/workflows/ci.yml
```

It prints `check-pins: ok (<commit>, <tag>)`, or names each value that differs and exits 1. It reads the commit from the submodule's git files, so it also works in the CI container, where git refuses a checkout owned by another user. Add it to `make test` so CI runs it too.

The inputs are documented at the top of [robco-ci.yml](../../.github/workflows/robco-ci.yml).

## Build a card image for QEMU

`make sdimage` writes `build/sd.img`, a card image with an MBR and one FAT partition. It stages the card in `$(SDIMAGE_STAGE)`, which is `build/sdstage`. It copies `SDIMAGE_DIR`, then each `SDIMAGE_FILES` pair, then runs the double-colon target `sdimage-stage`, then packs the stage with `tools/mksdimage.sh`. Set the variables before the `include`. Add your own steps with `sdimage-stage::` rules anywhere in the Makefile:

```make
SDIMAGE_FILES := build/hello-pi.img:HELLO.IMG config/HELLO.CFG:HELLO/HELLO.CFG

include $(ROBCO_CORE)/mk/robco.mk

sdimage-stage::
	mkdir -p $(SDIMAGE_STAGE)/HELLO/SAVE
```

`boot(image, board, sd="build/sd.img")` inserts the card in a test.

## Put it on an SD card

Not yet validated on hardware. The steps below come from [BOARD_SUPPORT.md](../spec/BOARD_SUPPORT.md#configtxt), not from a run on a real board.

Build the boot directory with `tools/mkbootdir.sh`:

```sh
robco-core/tools/mkbootdir.sh build/hello-pi.img sdboot --kernel-name HELLO.IMG
```

It writes these files into `sdboot/`. It never formats or writes a device.

| File | Source |
|---|---|
| `bootcode.bin`, `start.elf`, `fixup.dat` | `tools/fetch_firmware.sh` downloads them from the [Raspberry Pi firmware repository](https://github.com/raspberrypi/firmware) at one pinned release and checks each SHA-256 |
| `HELLO.IMG` | Your `build/hello-pi.img` |
| `config.txt` | The settings from [BOARD_SUPPORT.md](../spec/BOARD_SUPPORT.md#configtxt), with `kernel=HELLO.IMG` |

`config.txt`:

```
kernel=HELLO.IMG
arm_64bit=0
enable_uart=1
disable_splash=1
boot_delay=0
gpu_mem=64
hdmi_force_hotplug=1
disable_overscan=1
framebuffer_width=640
framebuffer_height=480
framebuffer_depth=32
```

Format a card with one FAT32 partition and copy the files in `sdboot/` to its root. `tools/fetch_firmware.sh --check sdboot` checks the firmware files again later.

Put no `.dtb` file and no `overlays/` directory on the card. Without a DTB the firmware passes ATAGS in r2, as QEMU and the test stub do. `arm_64bit=0` makes the firmware load the image at `0x8000` in AArch32 state.

Connect a 3.3 V USB serial adapter. Never use a 5 V adapter.

| Pi header pin | Signal | Adapter |
|---|---|---|
| 6 | GND | GND |
| 8 | GPIO14, TXD | RX |
| 10 | GPIO15, RXD | TX |

Open the adapter at 115200 baud, 8N1, no flow control, then power the Pi. On a Pi 3 or Zero 2 W, Bluetooth normally owns the PL011. `uart_init()` muxes GPIO14 and GPIO15 to the PL011 itself, but whether that is enough is pending hardware validation, so start with a Pi Zero or Pi 1. The fallbacks are in [the UART policy](../spec/BOARD_SUPPORT.md#uart-policy).

## Where to go next

Each capability below has an example under `examples/` and a section in the API reference. Each example is also a target test, so you can boot it with the harness as you did with `hello-pi`.

Read a file from the SD card. Call `sd_init()`, find the FAT partition with `part_scan()` and `part_open()`, mount it with `fs_mount()`, then read with `fs_read_file()` or `fs_open()` and `fs_read()`. See [examples/storage](../../examples/storage/main.c), which lists, reads and writes files, and [the storage reference](./api/storage.md#fat-file-system-fsh). `tools/mksdimage.sh` builds a card image to test with.

Turn on the caches. Call `mmu_init()` and then `mmu_enable()` early in `robco_main()`. Software drawing runs faster with the data cache on, and every library component keeps working. See [examples/mmu](../../examples/mmu/main.c) and [the MMU reference](./api/memory.md#mmu-mmuh).

Read buttons. Start the tick, call `input_init()`, register pins with `input_gpio_add_button()`, start the backend with `input_gpio_start()`, then read events with `input_wait()`. In QEMU, drive the same events over the serial line. See [examples/input](../../examples/input/main.c) and [the input reference](./api/input.md#input-events-inputh).

Play a tone. Start the tick, call `audio_init()`, then `audio_beep()`, `audio_tone()` or `audio_play_rtttl()`. In QEMU the trace backend prints each tone as an `@RC` line. See [examples/audio](../../examples/audio/main.c) and [the audio reference](./api/audio.md#audio-audioh).

Read the clock. Call `i2c_init(100000)` and `rtc_init()`, then `clock_get()`. Without an RTC module, set the software clock with `clock_set()`. See [examples/rtc](../../examples/rtc/main.c) and [the clock reference](./api/buses.md#real-time-clock-and-software-clock-rtch).
