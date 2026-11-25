# Testing

Every repo in the suite tests with the same harness from RobCo-Core. QEMU 10.1.2 runs the target tests. Real boards run a short checklist for what QEMU cannot model. [HARDWARE_VALIDATION.md](../HARDWARE_VALIDATION.md) holds that checklist for every component.

## Layers

| Layer | What runs | Where |
|---|---|---|
| 1. Host tests | C unit tests built for the host from the real sources: klibc, CRC, `rimg`, boot info, tick maths, allocator, partitions, FatFs over a file, input queue and decoders, audio notes, sequencer and PCM engine, date and time, INI. 18 files, about 180 tests. Later: game logic, UI layout. | `tests/host/` |
| 2. Tool tests | pytest for the host tools, the Makefile, the docs checker, and the file system against mtools | `tests/tools/` |
| 3. Serial tests | Boot an image in QEMU and assert on `@RC` markers and text. 17 files, each run on every board. | `tests/target/` |
| 4. Screen tests | Console cell grid dumped over the UART, plus QMP `screendump` compared with golden images | `tests/target/`, `tests/golden/` |
| 5. Input tests | Key input over the console backend on the PL011 and the script backend on the mini UART. GPIO buttons and the dial with levels driven through qtest. | `tests/target/test_input.py`, `tests/target/test_input_gpio.py` |
| 6. Fault tests | Bad magic, bad CRC, out-of-slot images, a missing next stage, an undefined instruction, MMU faults. Later: truncated file, oversized file, crashing holotape. | `tests/target/` |

The harness is Python with pytest and Pillow in a venv. `tests/harness/` holds the shared code:

| File | Role | State |
|---|---|---|
| `qemu.py` | Starts QEMU headless, talks QMP, HMP and qtest, captures serial, kills QEMU on exit | Exists |
| `boot.py` | `boot()`: boots a raw image the way the firmware does. Serial read and write, `@RC` marker parsing, screendumps. | Exists |
| `sdimage.py` | Builds SD images and fault images from Python | Exists |
| `pigpio.py` | Drives the Pi's own GPIO input levels through qtest: buttons, bounce and dial turns on a timed schedule | Exists |
| `expect.py` | Waits for `@RC` markers and text on a serial log | To be added. `boot.py` covers it today. |
| `screen.py` | PPM to PNG, golden compare | To be added |
| `inputs.py` | Sends scripted keys over the serial backend | To be added. `test_input.py` sends the lines itself. |

`tests/conftest.py` adds the `--board` option and the shared fixtures. `tests/fwstub/` holds the firmware stub.

## Booting images in QEMU

### Why not plain -kernel

A raw image passed with `-kernel` loads at `0x10000`, not `0x8000` (fact V1). Our images link at `0x8000` and do not relocate. A raw `-kernel` run would crash.

An ELF passed with `-kernel` loads at its link address (V2). On `raspi2b` it starts all four cores at the entry point. Real firmware parks the secondaries and enters in HYP. So an ELF run does not match hardware either.

### Firmware stub plus loader

`boot()` in `tests/harness/boot.py` runs the same raw image the firmware would load. It starts QEMU stopped, connects to the serial ports, then resumes it. The command line looks like this:

```
qemu-system-arm -M raspi2b -display none -monitor none -no-user-config \
  -qmp unix:<tmp>/qmp.sock,server=on,wait=off \
  -chardev socket,id=ser0,path=<tmp>/serial0.sock,server=on,wait=off \
  -serial chardev:ser0 -S \
  -device loader,file=tests/fwstub/build/fwstub.bin,addr=0x0,force-raw=on \
  -device loader,file=IMAGE,addr=0x8000,force-raw=on \
  -device loader,addr=0x7f00,data=<word>,data-len=4 ...
```

`fwstub.bin` mimics the firmware handoff. It is loaded raw at `0x0`, where AArch32 cores start when QEMU has no kernel. All cores run it.

- It parks secondary cores in a `wfi` loop. It reads MPIDR only when the MIDR part is not `0xB76`.
- It reads a parameter block at `0x7F00`: the magic `RCFW`, the machine id, the RAM size and flags. The harness writes these four words with `-device loader`. Without the magic it uses `0xC42` and 256 MiB.
- It writes an `ATAG_CORE`, `ATAG_MEM` and `ATAG_NONE` list at `0x100`.
- If the HYP flag is set and the CPU is not ARM1176, it switches from Secure SVC to Non-secure HYP with an `smc` into its own monitor vectors.
- It jumps to `0x8000` with r0 = 0, r1 = machine id, r2 = `0x100`.

| Board | Machine id | RAM | Entry mode |
|---|---|---|---|
| `raspi0` | `0xC42` | 512 MiB | SVC |
| `raspi1ap` | `0xC42` | 512 MiB | SVC |
| `raspi2b` | `0xC43` | 1 GiB | HYP |
| `raspi3b` | `0xC43` | 1 GiB | HYP |

`boot(image, board, mode="svc")` enters in SVC on `raspi2b` and `raspi3b` too.

The `-device loader` option with `force-raw=on` puts the image at `0x8000` byte for byte (V3).

Some tests attach more devices:

| Test | Device | Arguments |
|---|---|---|
| `test_sdraw.py` | SD card image on the EMMC controller | `-drive if=sd,format=raw,file=sd.img` |
| `test_input.py` | Mini UART as a second serial port | `boot(..., serial2=True)` |
| `test_rtc.py` | DS1338 RTC model, DS1307 registers, on I2C bus 1 | `-device ds1338,bus=i2c-bus.1,address=0x68` |
| `test_spi.py` | W25Q32 SPI flash on the SPI0 bus | `-device w25q32,bus=spi` |
| `test_gpiox.py` | Two PCA9554 GPIO expanders on I2C bus 1, inputs driven through qtest | `-device pca9554,bus=i2c-bus.1,address=0x20` and `0x21`, `boot(..., qtest=True)` |
| `test_input_gpio.py` | Pi GPIO input levels driven through qtest, every active-low line high at start | `boot(..., serial2=True, qtest=True, gpio_high=[...])` |

`test_rtc.py` and `test_spi.py` also boot once without the device, to check the absent case.

### raspi3b

`raspi3b` exists only in `qemu-system-aarch64`, and its CPU always starts in AArch64 (V5). `tests/fwstub/a64_shim.py` holds seven AArch64 words. The harness loads them at `0x80000` and starts CPU 0 there. They drop CPU 0 to AArch32 HYP at `0x0`, the firmware stub:

| Word | Instruction |
|---|---|
| `0xD2802620` | `mov x0, #0x131` |
| `0xD51E1100` | `msr scr_el3, x0` |
| `0xD2803B40` | `mov x0, #0x1DA`, AArch32 HYP with A, I and F masked |
| `0xD51E4000` | `msr spsr_el3, x0` |
| `0xD2800000` | `mov x0, #0x0`, the firmware stub |
| `0xD51E4020` | `msr elr_el3, x0` |
| `0xD69F03E0` | `eret` |

For `mode="svc"` the third word is `0xD2803A60`, `mov x0, #0x1D3`. The stub then runs as on the other boards, but never issues `smc`, because the CPU is already in the right mode. The other cores stay parked by QEMU. No AArch64 toolchain is needed.

### Guest state on timeout

A wait in `boot.py` that times out appends a guest state block to its `QemuError`, after the serial log. The dump asks QEMU for the run state, stops the VM, reads the interrupt controller and system timer registers and one CPU0 sample, then resumes it. It then takes five samples of CPU0's pc, lr, sp and CPSR while the VM runs, and names functions from the image's ELF. The VM is resumed even when a read fails. A VM that was already stopped stays stopped. If QEMU does not report the run state, the dump sends no stop and no cont and labels the state unknown. The block says whether the keep-alive was on. `vm.state_dump()` returns the same block. [tests/README.md](../../tests/README.md#when-a-wait-times-out) shows how to read it.

| Board | System timer | Interrupt controller | Local controller |
|---|---|---|---|
| `raspi0`, `raspi1ap` | `0x20003000` | `0x2000B200` | none |
| `raspi2b`, `raspi3b` | `0x3F003000` | `0x3F00B200` | `0x40000000` |

### QEMU keep-alive

While a VM from `boot()` runs, the harness sends QMP `query-status` every 20 ms from a background thread, under the same lock as every other QMP command. A thread that holds the lock and waits on the socket sends the due pings itself, so a long wait or slow command does not delay them. Pings go on while the VM is stopped. It covers all waits and sleeps in a test.

On the GitHub Linux runner, QEMU 10.1 sometimes failed to deliver a due system timer interrupt to the halted ARM1176 CPU on `raspi0` and `raspi1ap`. Pip-OS boots stalled in `wfi` for the whole timeout: 17 of 900 on `raspi1ap`, 1 of 900 on `raspi0`, none on `raspi2b`, `raspi3b` or 12,000 boots on macOS. In all 21 captured cases the compare had matched about 30 seconds earlier, and the guest resumed within one second of the first QMP monitor command. The cause inside QEMU is not identified.

The keep-alive cannot hide a guest bug: the guest cannot see monitor traffic. A guest that masks IRQs, never arms its timer or waits on its own logic still hangs. `ROBCO_QEMU_KEEPALIVE=0` or `boot(..., keepalive=False)` turns it off, to reproduce the stall. [tests/README.md](../../tests/README.md#qemu-keep-alive) has the details.

## Boards and suites

| QEMU machine | SoC | Suites |
|---|---|---|
| `raspi0` | BCM2835 | Canary, spikes, target tests |
| `raspi1ap` | BCM2835 | Canary, spikes, target tests |
| `raspi2b` | BCM2836 | Canary, spikes, target tests |
| `raspi3b` | BCM2837 | Canary, spikes, target tests |

`make test-target` runs on all four boards. `BOARD=<name>` limits it to one. The Spleen font tests run on `raspi0` and `raspi2b` only. Without `--board`, pytest picks `raspi0` and `raspi2b`. The reusable workflow runs `raspi0` and `raspi2b` unless the caller passes other boards.

Pi 2 runs only in QEMU. Real hardware is the authority for BCM2837, GPIO input, audio and the TFT.

## What QEMU cannot test

| Gap | Fact | Coverage |
|---|---|---|
| GPIO input | An undriven pin reads 0 and pulls change nothing (V12, V18). There are no input lines, edge and level events are not modelled and no GPIO interrupt comes. A qtest write to GPSET0/1 or GPCLR0/1 sets the level of any pin (V18). | `test_input_gpio.py` drives buttons and the dial through `pigpio.py` on every board, and checks the stuck-line guard with every line low. Real contacts, pulls and edges run on the hardware checklist. |
| PWM audio | Not modelled. Reading PWM data-aborts on `raspi0` and `raspi1ap` (V7). On `raspi2b` and `raspi3b` writes are dropped. | Host tests for the notes, the sequencer and the PCM engine. The audio driver probes PWM and falls back to the trace backend, which the target test checks. PCM on the trace backend runs the real timer channel 3 IRQ. Sound runs on the hardware checklist. |
| Real SPI signalling | QEMU ignores the rate, the mode and chip select. | `test_spi.py` checks the data path with the flash model. Rates, modes and chip select levels run on the hardware checklist. |
| Watchdog countdown | `bcm2835_powermgt` resets at the PM_RSTC write and does not count PM_WDOG. A reset clears PM_RSTS to `0x1000`. | `tests/host/test_wdog.c` checks every register write. `test_wdog.py` checks that an armed watchdog resets the board and skips the kick, stop, reboot and halt cases by name. They run on the hardware checklist. |
| SPI TFT | No model | The display interface is the same for HDMI and TFT, so HDMI goldens cover drawing. The TFT runs on the hardware checklist. |
| I2C timing | Every transfer finishes at once. No clock stretching. | The bus rate, timeouts and clock stretching run on the hardware checklist. |
| Cache and timing | QEMU does not model caches, memory types or the SMP bit | `test_mmu.py` checks the map, the faults and that every component still works. Speed and coherence run on the hardware checklist. |
| VC bus alias, SD routing on WiFi boards | QEMU does not match real firmware here | Pending hardware validation |

### GPIO buttons and dials in project tests

A project test can drive the Pi's own button and dial lines. Boot with `qtest=True` and list every active-low line in `gpio_high=`, so it reads released before the guest starts. Then use `PiGpio` from `tests/harness/pigpio.py`: `press()`, `release()` and `tap()` for buttons, `bounce()` for chatter, `turn()` for a dial with 4 transitions per detent. Timing follows the host clock. Hold a press for several ticks of the guest, and use `slip()` on `io.log` to check how late the host was. Lines on an I2C expander use `set_irq_in` instead, as `test_gpiox.py` does.

| Function | What it does |
|---|---|
| `boot(..., qtest=True, gpio_high=pins)` | Drives `pins` high before the guest starts |
| `PiGpio(vm, board, names=None, active_low=True)` | Wraps the qtest socket. `names` maps button names to pins. |
| `set(pin, level)`, `get(pin)` | Writes a level through GPSET or GPCLR, reads the GPLEV bit |
| `press(pin)`, `release(pin)`, `tap(pin, hold_s)` | Button levels, active low unless `active_low=False` |
| `play(schedule)` | Applies `(t, pin, level)` changes on time, keeps `(planned, real)` times in `io.log`, returns the latest write |
| `bounce(pin, count, period_s, settle=False)` | A button that chatters, then rests released or pressed |
| `turn(pin_a, pin_b, detents, state_s, lengths=(1, 1, 1, 1), bounce=None)` | A dial turn from rest, both lines high. `lengths` scales each Gray state. |
| `slip(log)` | The latest write and the most a state was cut short, in seconds |
| `pressed_span(schedule, log, press)` | The longest time from the first press to the real end of the last press, in seconds |
| `one_sample_at_most(span_s, missed, jitter_s, tick_s)` | True when no two tick samples can fall inside the span |
| `hosttiming.skip(numbers)` | Skips a test whose runs had poor host timing, with the reason `host timing: <numbers>` |
| `quad_schedule()`, `bounce_schedule()`, `quad_states()`, `level_writes()` | The pure parts behind the above, tested in `tests/tools/test_harness_pigpio.py` |

#### Host timing skips

QEMU's guest tick is irregular when the host is busy. Each timed check in `test_input_gpio.py` judges a run only when the host timing of that run was good enough. A run is good enough when it missed no tick and stayed within the check's own bound on the tick gap or jitter. One helper, `judged()`, applies both rules to every check. It tries a bad run 3 times, then skips the test through `hosttiming.skip()`. That is a host timing skip. Its reason starts with `host timing:` and gives the numbers of the last run.

A bad run can still change the guest's state. A dial turn tried again may have lost or added a step, and that step stays in the dial total. So a check of the whole total leaves out the change of each run tried again, and still fails on a step outside every run. The next run starts clean: each turn ends at the rest state, and the decoder clears its partial count there. `quad_rest_clears_any_history` in `tests/host/test_input_quad.c` proves that rule.

`tests/target/conftest.py` counts host timing skips in each session. A session allows 1 per selected board: 1 for `--board raspi0`, 4 for `--board all`. A session over the limit fails, and its summary lists each counted skip with its numbers. Other skips, such as a board QEMU lacks, do not count. On a loaded machine, `--timing-skips-max=N` sets another limit for the whole session. `make test-target` passes `-rs`, so the log shows every skip reason. Project tests get this rule only by copying the target conftest, since `harness.plugin` does not include it.

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
| V12 | GPLEV0/1 read 0 | The stuck-line guard test: undriven active-low lines look pressed |
| V13 | QMP `screendump` works headless and writes a PPM | Screen tests in CI |
| V14 | PM_WDOG then PM_RSTC full reset emits a QMP `RESET` event | Reboot and watchdog tests |
| V15 | First `-serial` is the PL011, second is the mini UART | Serial capture order |
| V16 | SPI0 is modelled on every board. With nothing attached a byte completes at once and reads 0. | The SPI and TFT panel target tests |
| V17 | QEMU's `pca9554` model answers on I2C bus 1 with the data sheet registers. qtest `set_irq_in` drives its input pins. | The GPIO expander target test |
| V18 | The GPIO device has no input lines. A qtest write to GPSET0/1 or GPCLR0/1 sets or clears that pin's GPLEV0/1 bit, input pins included, in both banks. A write to GPLEV changes nothing. A pull-up leaves an undriven pin at 0. The event detect registers read 0 and GPU IRQ 49 never goes pending. An output pin reads back its level. With the guest running, one write takes under 5 ms at the median and guest timer time follows host time within 5 %. | `pigpio.py` and the GPIO button and dial target tests |

V4 and V8 are not checked.

## SD images

`make_sd_image()` in `tests/harness/sdimage.py` builds SD images with mtools. It needs no root and no loop devices.

| Property | Value |
|---|---|
| Size | Rounded up to a power of 2 (V11). Tests use 64 MiB. |
| Partition table | MBR, one partition |
| Partition | Starts at LBA 2048. Type `0x0C`, FAT32 LBA, from 64 MiB. Type `0x0E`, FAT16 LBA, below that. |
| Format | `mformat` on `sd.img@@1M` |
| Copy | `mmd` and `mcopy` on `sd.img@@1M` |

Fault images start from a good image. `corrupt()` then patches raw bytes in place, such as one byte of `MFBOOT.SYS`.

## CI

CI runs in the `robco-ci` container, built from `ci/Dockerfile` in RobCo-Core. It holds Ubuntu 24.04, QEMU 10.1.2 built from source with `qemu-system-arm` and `qemu-system-aarch64`, `gcc-arm-none-eabi`, a host `gcc`, mtools and Python.

The image tag is a content tag. It is the first 12 hex characters of the SHA-256 of `ci/Dockerfile`. `ci/IMAGE_TAG` stores it. CI fails if `ci/image-tag.sh` gives a different value. The image is `ghcr.io/robco-industries/robco-ci:<tag>`. A new Dockerfile gives a new tag, so a repo never runs on a container it did not pin.

RobCo-Core's own `.github/workflows/ci.yml` runs on pushes to `main` and on pull requests. It has four jobs:

| Job | What it runs |
|---|---|
| `image` | Checks that `ci/IMAGE_TAG` equals `ci/image-tag.sh`, then builds and pushes the container if that tag does not exist yet |
| `host` | `make test-host HOST_SANITIZE=require` |
| `canary` | `make venv && make test-canary` |
| `test` | `make venv && make test-tools test-docs test-spikes test-examples test-target` |

`host`, `canary` and `test` run inside the container and need `image`. Together they run every target `make test` runs. The container runs as root, and socket paths stay under the system temporary directory.

Each sibling repo calls the reusable workflow `.github/workflows/robco-ci.yml` from RobCo-Core with the tag from `ci/IMAGE_TAG` in its RobCo-Core submodule. It checks out with `submodules: recursive` and runs two jobs in the container:

| Job | What it runs |
|---|---|
| `build` | `make CI=1 <make-targets>`, default `all`. Uploads `artifact-path` as the `build` artifact when it is set. |
| `test` | `make CI=1 <test-targets> BOARD=<board>` once per board in `boards`, default `raspi0` and `raspi2b`. Skipped when `test-targets` is empty. |

### Catch CI failures before a push

CI builds the host tests on Linux with GCC and `-Werror`. A Mac builds them with clang, which lacks some GCC warnings. Two checks close the gap.

`make test-host-gccwarn` runs first in `make test-host` when the host `cc` is clang. It compiles every host C file with `arm-none-eabi-gcc` and the macOS SDK headers, without linking, in two passes:

| Pass | Flags | Catches |
|---|---|---|
| `ci` | CI's host flags: `-O1` and `-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer` | Warnings GCC gives only with the sanitizers, such as a `-Wformat-truncation` on `snprintf` in a loop |
| `o2` | The same flags without the sanitizers, at `-O2` | Warnings GCC gives only with more optimisation |

It takes about 35 seconds from clean. After that it compiles a file again only when the file, a header it includes or the compile command changes. The compile command covers the compiler path, its version and every flag, so a new define in a project's `Makefile` or on the command line also counts. It skips with one line when the host `cc` is GCC, when the cross GCC is missing or when no SDK headers are found. The cross GCC targets 32-bit ARM, so it cannot see warnings that depend on a 64-bit `long` or pointer.

`make ci-local` runs the CI build itself, in the `robco-ci` container with Docker. It catches what the cross GCC cannot: 64-bit type sizes, the Linux libc and the exact GCC version of CI. It copies the tree to `build/ci-local/src` without `build`, `.venv` and `.git`, writes the `.git/HEAD` files the doc-link check reads, then runs `make CI=1 <targets>` as the calling user. No host environment variable reaches the container. The container gets at most `CI_LOCAL_CPUS` CPUs, 4 by default. The script refuses a bare `-j`, which would start unlimited jobs.

The container sees only the copy. When `ROBCO_CORE` points outside the tree, the script copies RobCo-Core to `.ci-local/robco-core` in the copy and passes `ROBCO_CORE` for that place to make. A symbolic link to RobCo-Core, such as `external/robco-core`, points there too. The script stops on any other link that leaves the tree, and on an absolute link. A path in a project's `Makefile` that leaves the tree, such as a source outside it, fails in the container.

```sh
make ci-local                                  # the default targets
make ci-local CI_LOCAL_TARGETS="-k test-host"  # other targets, once
make ci-local-image                            # build robco-ci:<tag> locally
```

The default targets are CI's build job. In RobCo-Core they are `test-host HOST_SANITIZE=require`, the `host` job. A project that includes `mk/host.mk` gets both targets with `all test-host` as the default. It sets `CI_LOCAL_TARGETS` to the `make-targets` of its workflow when they differ. `tools/ci_local.sh` takes the same targets as arguments.

The script runs `robco-ci:<tag>` when that local image exists, else `ghcr.io/robco-industries/robco-ci:<tag>`, which needs a registry login. `<tag>` comes from `ci/IMAGE_TAG`. `CI_IMAGE` overrides both. With no image, it names the command that builds one. `make ci-local-image` builds the image from `ci/Dockerfile`. It compiles QEMU, so a first build takes several minutes and loads every CPU.

Project maintainers: after you bump the RobCo-Core pin, remove local copies of these checks. UOS has `test-host-gccwarn-ci` in its `Makefile` and its own `tools/ci_local.sh`.

## Hardware checklists

QEMU is not the authority for real boards. A change that touches what QEMU cannot model ships with a short checklist in the repo's `docs/`.

- One checklist per hardware question. Each step says what to do and what to expect, in `@RC` lines where possible.
- Checklists run on a Pi Zero or Pi 1 for BCM2835, and on a Pi 3 or Zero 2 W for BCM2837.
- The result is written back into the spec. A spot marked "pending hardware validation" stays marked until a checklist has run on that board.
- Work that does not depend on the result continues while a checklist waits.
