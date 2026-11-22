# Testing

Every repo in the suite tests with the same harness from RobCo-Core. QEMU 10.1.2 runs the target tests. Real boards run a short checklist for what QEMU cannot model.

## Layers

| Layer | What runs | Where |
|---|---|---|
| 1. Host tests | C unit tests built for the host from the real sources: klibc, CRC, `rimg`, boot info, INI, allocator, console grid, FatFs over a file, game logic, UI layout, audio sequencer | `tests/host/` |
| 2. Serial tests | Boot an image in QEMU and assert on `@RC` markers and text | `tests/target/` |
| 3. Screen tests | Console cell grid dumped over the maintenance port, plus QMP `screendump` compared with golden images | `tests/target/`, `tests/golden/` |
| 4. Input tests | Scripted key input over the serial input backend | `tests/target/` |
| 5. Fault tests | Bad magic, bad CRC, truncated file, missing file, oversized file, crashing holotape | `tests/target/` |

The harness is Python with pytest and Pillow in a venv. `tests/harness/` holds the shared code:

| File | Role | State |
|---|---|---|
| `qemu.py` | Starts QEMU headless, talks QMP, HMP and qtest, captures serial, kills QEMU on exit | Exists |
| `expect.py` | Waits for `@RC` markers and text on a serial log | To be added |
| `screen.py` | Screendump, PPM to PNG, golden compare | To be added |
| `sdimage.py` | Builds SD images and fault images from Python | To be added |
| `inputs.py` | Sends scripted keys over the serial backend | To be added |
| `conftest.py` | `--board` option and fixtures | To be added |

## Booting images in QEMU

### Why not plain -kernel

A raw image passed with `-kernel` loads at `0x10000`, not `0x8000` (fact V1). Our images link at `0x8000` and do not relocate. A raw `-kernel` run would crash.

An ELF passed with `-kernel` loads at its link address (V2). On `raspi2b` it starts all four cores at the entry point. Real firmware parks the secondaries and enters in HYP. So an ELF run does not match hardware either. The spikes use ELF `-kernel` today, which is fine for them because their startup code parks secondaries itself.

### Firmware stub plus loader

The default method runs the same raw `.IMG` file the firmware would load:

```
qemu-system-arm -M raspi2b -display none \
  -kernel tests/fwstub/fwstub.elf \
  -device loader,file=RETROS.IMG,addr=0x8000,force-raw=on \
  -serial file:pl011.log -serial file:mini.log
```

`fwstub.elf` mimics the firmware handoff:

- It links at `0x4000`, below every image.
- It parks secondary cores in a `wfi` loop.
- On Cortex-A7 it enters the image in HYP, as Pi 2 firmware does.
- It writes an `ATAG_CORE`, `ATAG_MEM` and `ATAG_NONE` list at `0x100`.
- It jumps to `0x8000` with r0 = 0, r1 = `0xC42`, r2 = `0x100`.

The `-device loader` option with `force-raw=on` puts the image at `0x8000` byte for byte (V3).

Chain tests add an SD card and assert on markers from every stage:

```
-drive if=sd,format=raw,file=sd.img
```

### raspi3b

`raspi3b` exists only in `qemu-system-aarch64`, and its CPU always starts in AArch64 (V5). Seven AArch64 words at `0x80000` drop CPU 0 to AArch32 HYP at `0x8000`:

| Word | Instruction |
|---|---|
| `0xD2802620` | `mov x0, #0x131` |
| `0xD51E1100` | `msr scr_el3, x0` |
| `0xD2803B40` | `mov x0, #0x1DA`, AArch32 HYP with A, I and F masked |
| `0xD51E4000` | `msr spsr_el3, x0` |
| `0xD2900000` | `mov x0, #0x8000` |
| `0xD51E4020` | `msr elr_el3, x0` |
| `0xD69F03E0` | `eret` |

The harness writes each word with `-device loader,addr=...,data=...,data-len=4`, loads the image at `0x8000` with `force-raw=on`, and starts CPU 0 at `0x80000`. No AArch64 toolchain is needed.

## Boards and suites

| QEMU machine | SoC | Suites |
|---|---|---|
| `raspi0` | BCM2835 | Canary, full suite |
| `raspi1ap` | BCM2835 | Canary, spikes |
| `raspi2b` | BCM2836 | Canary, full suite |
| `raspi3b` | BCM2837 | Canary, smoke: boot to `ev=ready` |

Pi 2 runs only in QEMU. Real hardware is the authority for BCM2837, GPIO input, audio and the TFT.

## What QEMU cannot test

| Gap | Fact | Coverage |
|---|---|---|
| GPIO input | GPLEV0/1 read 0, input cannot be injected (V12) | Input logic runs through the serial input backend in layer 4. Pin reading runs on the hardware checklist. |
| PWM audio | Not modelled. Reading PWM data-aborts on `raspi0` (V7). | Host tests for the sequencer. Drivers use `mmio_probe` and skip audio when it fails. Sound runs on the hardware checklist. |
| SPI TFT | No model | The display interface is the same for HDMI and TFT, so HDMI goldens cover drawing. The TFT runs on the hardware checklist. |
| Cache and timing | QEMU does not model caches | Hardware checklist |
| VC bus alias, SD routing on WiFi boards | QEMU does not match real firmware here | Pending hardware validation |

## Canary suite

`tests/canary/` checks the QEMU behaviour the harness relies on. Run it after any QEMU upgrade. A canary failure means QEMU changed, not our code.

| ID | Fact | What it protects |
|---|---|---|
| V1 | Raw `-kernel` loads at `0x10000`. Entry in SVC, r0 = 0, r1 = `0xC42`, r2 = `0x100`, ATAGS at `0x100`. | The reason for the stub plus loader method |
| V2 | ELF `-kernel` loads at its link address | The fwstub boot |
| V3 | `-device loader,...,addr=0x8000,force-raw=on,cpu-num=0` loads and starts at `0x8000` | Running the exact raw image |
| V5 | `raspi3b` cannot turn AArch64 off. Seven words drop to AArch32 HYP or SVC at `0x8000`. | The raspi3b shim |
| V6 | `mrc p15, 0, r1, c0, c0, 5` gives MIDR on `raspi0` and MPIDR on `raspi2b` | The ARM1176 MPIDR trap in `start.S` |
| V7 | PWM read data-aborts on `raspi0`, works on `raspi2b` | `mmio_probe` use in the audio driver |
| V9 | Mailbox channel 8 answers board revision, ARM memory and UART clock, 3 MHz | Mailbox driver and UART divisor |
| V10 | Timer C1 match sets CS bit 1 and IRQ 1 pending. Writing CS bit 1 clears it. | Timer interrupt driver |
| V11 | `-drive if=sd` attaches to the EMMC controller. The image size must be a power of 2. | EMMC driver and SD image padding |
| V12 | GPLEV0/1 read 0 | The decision to test GPIO input on hardware |
| V13 | QMP `screendump` works headless and writes a PPM | Screen tests in CI |
| V14 | PM_WDOG then PM_RSTC full reset emits a QMP `RESET` event | Reboot and watchdog tests |
| V15 | First `-serial` is the PL011, second is the mini UART | Serial capture order |

V4 and V8 are not checked.

## SD images

`tools/mksdimage.sh` builds SD images with mtools. It needs no root and no loop devices.

| Property | Value |
|---|---|
| Size | 64 MiB. Always a power of 2 (V11). |
| Partition table | MBR, one partition |
| Partition | Type `0x0C`, FAT32 LBA, starting at LBA 2048 |
| Format | `mformat -F -i sd.img@@1M` |
| Copy | `mcopy -i sd.img@@1M -s <files> ::/` |

Fault images start from a good image. The harness then edits a file in place, such as flipping one byte in `MFBOOT.SYS`, truncating it or deleting it. It can also patch raw image bytes.

## CI

CI runs in the `robco-ci` container, built from `ci/Dockerfile` in RobCo-Core. It holds Ubuntu 24.04, QEMU 10.1.2 built from source with `qemu-system-arm` and `qemu-system-aarch64`, `gcc-arm-none-eabi`, mtools and Python.

The image tag is a content tag. It is the first 12 hex characters of the SHA-256 of `ci/Dockerfile`. `ci/IMAGE_TAG` stores it. CI fails if `ci/image-tag.sh` gives a different value. The image is `ghcr.io/robco-industries/robco-ci:<tag>`. A new Dockerfile gives a new tag, so a repo never runs on a container it did not pin.

Each sibling repo calls the reusable workflow `.github/workflows/robco-ci.yml` from RobCo-Core with `submodules: recursive`. It reads the tag from `ci/IMAGE_TAG` in its RobCo-Core submodule. Jobs per repo, each able to fail:

- host tests
- `-Werror` build
- QEMU matrix: `raspi0`, `raspi2b`, `raspi3b` smoke
- canary
- image size budget
- Markdown link check
- SD image artifact

A nightly workflow in RobCo-Core checks out every sibling at its default branch and runs both chains end to end.

## Hardware checklists

QEMU is not the authority for real boards. A change that touches what QEMU cannot model ships with a short checklist in the repo's `docs/`.

- One checklist per hardware question. Each step says what to do and what to expect, in `@RC` lines where possible.
- Checklists run on a Pi Zero or Pi 1 for BCM2835, and on a Pi 3 or Zero 2 W for BCM2837.
- The result is written back into the spec. A spot marked "pending hardware validation" stays marked until a checklist has run on that board.
- Work that does not depend on the result continues while a checklist waits.
