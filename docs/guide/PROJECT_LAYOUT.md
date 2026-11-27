# Project layout

This page maps the repository and shows where new code and tests go. To use the library from another project, start with [GETTING_STARTED.md](./GETTING_STARTED.md).

## Top level

| Path | Holds |
|---|---|
| `include/robco/` | Public headers. One header per component. See [API.md](./API.md) and the topic files in [api/](./api/). |
| `src/` | Library sources. Every `.c` and `.S` file one level below `src/` goes into `librobco.a`. |
| `ld/image.ld.S` | Linker script, run through the C preprocessor with `LINK_ADDR` and the project's `robco_config.h` |
| `mk/robco.mk` | Build fragment that projects include. Builds the library, links and seals the image. The optional `make sdimage` target stages `SDIMAGE_DIR`, the `SDIMAGE_FILES` pairs and the project's `sdimage-stage::` rules, then writes `build/sd.img` with `tools/mksdimage.sh`. |
| `mk/host.mk` | Host unit test build. The top-level `Makefile` includes it for the library's tests, and a project includes it for its own with `HOST_TEST_DIR`. See [host tests in a project](../../tests/README.md#host-tests-in-a-project). |
| `examples/` | Small images that use the library. Each one is also a target test. |
| `tests/` | Host tests, QEMU harness, target tests, tool tests, canary suite. `tests/harness` is also the pytest plugin and the interactive runner projects use. `tests/fixtures/sample-project` is a minimal project the tool tests build. See [tests/README.md](../../tests/README.md). |
| `tools/` | Host tools: `rimg.py` seals and checks images, `check_docs.py` checks the docs, `sfx_table.py` renders sound effect models into 8-bit tables, `audio_preview.py` turns the audio trace of a serial log into a WAV file, `font2c.py` and `font_specimen.py` build and preview fonts, `fetch_firmware.sh` downloads the Pi firmware, `mkbootdir.sh` assembles an SD card boot directory, `mksdimage.sh` builds an SD card image from a directory, `check_pins.sh` checks a project's workflow pins against its submodule, `check_doc_links.sh` checks a project's doc links to a pinned RobCo repository against its submodule |
| `docs/` | This guide, the [specifications](../spec/README.md), [FONTS.md](../FONTS.md) and [HARDWARE_VALIDATION.md](../HARDWARE_VALIDATION.md) |
| `spikes/` | Three early proofs: UART hello, SD sector read, framebuffer and MMU. They have their own code in `spikes/common/` and do not use the library. |
| `ci/` | The CI container. See [ci/README.md](../../ci/README.md). |
| `.github/workflows/` | `ci.yml` for this repository, `robco-ci.yml` for projects built on it |
| `third_party/` | Upstream files with their own licences: the Fixedsys Excelsior and Spleen fonts, and FatFs R0.16 in `third_party/fatfs/` |
| `Makefile` | Test targets for this repository. Projects do not use it. |

## Inside src/

| Directory | Holds | Touches hardware |
|---|---|---|
| `src/arch/` | `start.S` entry, exception vectors, `mmio_probe()`, exception reports, `robco_handoff()`, the MMU and cache maintenance | Yes |
| `src/board/` | Board detection, `robco_start()`, the hardware side of the boot info | Yes |
| `src/drv/` | Drivers: mailbox, PL011 and mini UART, system timer, interrupt controller, tick, GPIO, power, RNG, framebuffer, SD card, input backends, audio, I2C, RTC, SPI | Yes |
| `src/gfx/` | Screen console, colour schemes, font table | Through `fb.h` |
| `src/fs/` | The FAT file system layer over FatFs, its configuration `ffconf.h`, its glue to `blk.h` and the file side of the stage loader | No |
| `src/lib/` | klibc, `k_printf`, CRC-32, heap, `rimg_validate()`, `rimg_seal()` and `rimg_pack()`, boot info, the stage loader core, RAM disk, partitions, input queue and decoders, audio sequencer and notes, date and time, the RTC register codec, INI parser | No |

`src/lib/` holds only code that runs unchanged on the host. It must not read registers, call drivers, use assembly or assume 32-bit pointers. `make test-host` builds every `src/lib/*.c` with the host compiler, ASan and UBSan and runs the tests in `tests/host/`. Code that needs hardware goes in one of the other directories.

`mk/robco.mk` picks up `src/*/*.c` and `src/*/*.S`. A file in a deeper directory, such as `src/drv/usb/hcd.c`, is not built.

## The file system and FatFs

`fs.h` wraps ChaN's FatFs R0.16. The upstream files live in `third_party/fatfs/` and stay unmodified. `third_party/fatfs/.gitattributes` sets `* -text`, so Git keeps their CRLF line ends byte for byte. Do not reformat or patch them. Change the behaviour through `src/fs/` instead.

| File | Role |
|---|---|
| `src/fs/ffconf.h` | FatFs configuration: long names, two volumes, time stamps from `fs_time_hook()`. `ROBCO_FS_READONLY` set to 1 in `robco_config.h` selects the read-only build. |
| `src/fs/fs.c` | The `fs.h` API: volumes, paths, error mapping |
| `src/fs/fs_glue.c` | The FatFs disk functions over `blk.h` and the file time stamps |
| `src/fs/loader.c` | `loader_load()` over `fs.h`. The pure part is `src/lib/loader_core.c`. |
| `src/fs/fs_priv.h` | Internal declarations shared by the two files above |
| `src/fs/fs_strchr.c` | `strchr()` for FatFs on the target |
| `src/fs/libc/string.h` | `<string.h>` for FatFs on the target, which has no C library headers |

| Build | How `src/fs/` and FatFs are built |
|---|---|
| Target, `mk/robco.mk` | `src/fs/*.c`, `ff.c` and `ffunicode.c` go into `librobco.a`. Only these files get `-I src/fs -I third_party/fatfs -I src/fs/libc`. `ROBCO_FATFS_CFLAGS` relaxes warnings for the FatFs files alone. `--gc-sections` drops the code from an image that calls no `fs_` function. |
| Host, `mk/host.mk` | The same files are built once into objects with the host compiler and linked into every host test, together with `src/lib/*.c` and the file-backed test device `tests/host/blk_file.c`. The host uses its own `<string.h>`. `HOST_FATFS_CFLAGS` relaxes warnings for FatFs. A second copy built with `ROBCO_FS_READONLY=1` links into `tests/host/test_fs_readonly.c`. |

`make test-host` also builds `build/host/fs_tool`, a command-line tool over `fs.h` that `tests/tools/test_fs_interop.py` runs against images made with mtools. When `fsck.vfat` or `fsck_msdos` is installed, the test also checks the images after each write.

## Add a driver

1. Put the code in `src/drv/<name>.c`. Find the peripheral with `board_get()->periph_base` plus the offset from [BOARD_SUPPORT.md](../spec/BOARD_SUPPORT.md#detection-table). Use the helpers in `mmio.h`.
2. Put the public declarations in `include/robco/<name>.h`. Return `0` or a negative code, and define the codes in the header.
3. Do not start anything from `robco_start()`. The project calls your `_init` function.
4. Document every function in [API.md](./API.md) or in a topic file under [api/](./api/), with its init order and context rules. `make test-docs` fails until you do. Notes in `docs/guide/pending/` do not count.
5. Add an example and a target test, as below.

## Add a library module

1. Put the code in `src/lib/<name>.c` and the declarations in `include/robco/<name>.h`.
2. Add `tests/host/test_<name>.c`. The format is in [tests/README.md](../../tests/README.md#host-tests).
3. Run `make test-host`.
4. Document the functions in [API.md](./API.md) or its topic file under [api/](./api/).

## Add a component with host-testable logic

Split a component in two when part of it is pure logic. The logic goes in `src/lib/` and runs in the host tests. The register access goes in `src/drv/` and runs only in QEMU or on a board. The real-time clock is the worked example.

| Step | Real-time clock example |
|---|---|
| 1. Put the pure logic in `src/lib/<name>.c`, with its own header. It takes and returns plain values. | `src/lib/datetime.c` and `include/robco/datetime.h`: calendar maths, BCD, FAT stamps, ISO 8601. `src/lib/rtc_regs.c`: the register codec of each chip. No register access, no timer. |
| 2. Test it on the host in `tests/host/test_<name>.c`. Cover the edges: ranges, invalid input, round trips. | `tests/host/test_datetime.c` and `tests/host/test_rtc_regs.c` |
| 3. Put the register access in `src/drv/<name>.c`, with its own header. It reads the hardware and calls the pure code for every decision. | `src/drv/rtc_ds1307.c` and `include/robco/rtc.h`: I2C transfers through `i2c.h` and chip detection, then `rtc_regs_decode()` and `rtc_regs_encode()` on the bytes. |
| 4. Add an example that prints `@RC` markers, and a target test that boots it. | `examples/rtc/` and `tests/target/test_rtc.py`, which attaches the QEMU DS1338 model to the I2C bus |
| 5. Document both headers. List what QEMU cannot prove in [HARDWARE_VALIDATION.md](../HARDWARE_VALIDATION.md). | [Date and time](./api/buses.md#date-and-time-datetimeh), [Real-time clock](./api/buses.md#real-time-clock-and-software-clock-rtch) |

The input, audio, partition and storage components follow the same split.

## Add an example

Create `examples/<name>/` with three files. Copy them from `examples/hello/`.

| File | Content |
|---|---|
| `Makefile` | `ROBCO_CORE ?= ../..`, `IMAGE`, `LINK_ADDR`, `SRCS`, `CONFIG_H`, then `include $(ROBCO_CORE)/mk/robco.mk` |
| `robco_config.h` | `ROBCO_STAGE_NAME` and the `ROBCO_IMG_*` macros |
| `main.c` | `robco_main()`. Print `@RC stage=<name> ev=boot ...` first and `ev=ready` when set up. |

`make test-examples` builds every `examples/*/Makefile` with `CI=1`, so a warning fails it. Nothing else to register.

## Add a build variant

A variant builds the same example with extra compiler flags, such as a fault injection. List it in `examples/<name>/VARIANTS`, one per line:

```
# <build-dir> <extra cflags>
build/fault -DHELLO_FAULT=1
```

`make test-examples` builds each line into its build directory. The target test builds the same variant again through `example_image(name, variant, extra_cflags)`, where `variant` is the build directory without `build/`. Keep the flags in both places the same.

## Add a host test

Create `tests/host/test_<name>.c`. It is linked with every `src/lib/*.c` and built as `build/host/test_<name>`. `rc_test.h` gives `TEST()`, `CHECK`, `CHECK_EQ`, `CHECK_STR` and `CHECK_MEM`. See [tests/README.md](../../tests/README.md#host-tests). A project runs its own host tests through the same `mk/host.mk`, see [host tests in a project](../../tests/README.md#host-tests-in-a-project).

## Tool tests

`tests/tools/` holds pytest tests for the host tools and the build. `make test-tools` runs them.

| File | Tests |
|---|---|
| `test_rimg_tool.py` | `tools/rimg.py` on images built in memory, one case per error code, `pack`, the META options and the layout against `rimg.h`. `tests/host/test_rimg_pack.c` reads the images `mk/host.mk` packs, so C and Python agree on META images. |
| `test_sfx_table.py` | `tools/sfx_table.py` on small made-up models: its arithmetic, the synthesis, every refusal, `--check`, and the generated C built with the host compiler |
| `test_audio_preview.py` | `tools/audio_preview.py` on hand-written serial logs, and its square against `src/lib/audio_square.c` built with the host compiler |
| `test_audio_engines.py` | `tools/voice_engine.py` and `tools/tone_square.py`: their constants and sine table against the C headers and sources |
| `test_check_docs.py` | `tools/check_docs.py` on small trees in a temp directory |
| `test_font_tools.py` | `tools/font2c.py` and `tools/font_specimen.py` |
| `test_scheme_ids.py` | The console scheme ids in `console.h` match `con_scheme` in BOOT_PROTOCOL.md |
| `test_slot_end.py` | The `ROBCO_SLOT_END` link check |
| `test_makefile.py` | The top-level Makefile, such as the `BOARD` variable of `make test-target` |
| `test_boot_scripts.py` | `tools/fetch_firmware.sh` and `tools/mkbootdir.sh` |
| `test_fs_interop.py` | `build/host/fs_tool` against mtools images. It builds the tool itself and skips without mtools. |
| `test_sample_project.py` | `tests/fixtures/sample-project` built from a copy outside the repository: the image and its host tests through `mk/host.mk` |
| `test_harness_plugin.py` | `harness.plugin` through pytest on a copy of the sample project outside the repository, and `harness.run` driven through a pipe |
| `test_check_pins.py` | `tools/check_pins.sh` on the workflows in `tests/fixtures/check-pins`, with hand-made submodule git files and a `git` that exits 128 |
| `test_check_doc_links.py` | `tools/check_doc_links.sh` on temp project trees, with hand-made submodule git files and a `git` that exits 128 |

## Add a target test

Create `tests/target/test_<name>.py`. Take the `board` fixture to run on each `--board`, and `example_image` to build an example once per session:

```python
from harness.boot import boot

def test_my_example(board, example_image):
    with boot(example_image("hello"), board) as vm:
        vm.wait_for("ev=ready", timeout=10)
```

`make test-target` runs every target test on all four boards. To run one file on one board:

```sh
.venv/bin/pytest tests/target/test_hello.py --board raspi0 -v
```

## Make targets

| Target | Runs |
|---|---|
| `make venv` | Creates `.venv` from `tests/requirements.txt` |
| `make test` | Every target below, in this order |
| `make test-host` | Host unit tests for `src/lib/` |
| `make test-tools` | pytest for the Python tools in `tools/` |
| `make test-docs` | `tools/check_docs.py`: links, anchors, API coverage, checked commands, banned words |
| `make test-canary` | QEMU behaviour the tests rely on |
| `make test-spikes` | Builds the spikes and boots them on every board |
| `make test-examples` | Builds every example and variant with `-Werror` |
| `make test-target` | Boots the spikes and examples through `boot()` on every board |

## What CI runs

`.github/workflows/ci.yml` runs on every push to `main` and every pull request, inside the container from `ci/`.

| Job | Command |
|---|---|
| `image` | Builds and pushes the container if `ci/IMAGE_TAG` is new |
| `host` | `make test-host HOST_SANITIZE=require` |
| `canary` | `make venv && make test-canary` |
| `test` | `make venv && make test-tools test-docs test-spikes test-examples test-target` |

Projects built on the library call `.github/workflows/robco-ci.yml`. It runs `make CI=1` and then the test targets you name once per board, with `BOARD=<board>`. [GETTING_STARTED.md](./GETTING_STARTED.md#run-it-in-ci) shows the call.

## Audio preview

QEMU has no PWM model, so a kernel's audio runs on the trace backend there. The trace prints one `ev=audio` marker per change. `tools/audio_preview.py` reads those markers from a serial log and writes what the audio pin would play as a WAV file. Use it to hear a kernel's sounds without a board.

```sh
tools/audio_preview.py serial.log out.wav --pcm DIR [--pcm DIR ...] [--scale auto|cut|nearest] [--tone-peak N]
```

A `--pcm` folder holds the sample data the log names by CRC-32 and length. A `.u8` file holds the raw 8-bit samples of one buffer, as `sfx_table.py --dump` writes them. A `.voice` file holds the packed 28-byte spans of one voice. An unknown buffer or voice plays as silence, with one warning.

The tool reads markers of every stage. It skips an `ev=audio` marker without `op=`, such as a program's own status line. It renders a play whose `op=pcm` line ends with `loop=1` until the trace ends it: a stop or a new sound. A loop the log leaves playing stops at the last marker. It parses only `ev=audio` and `ev=audio_drop` lines. After `ev=audio_drop n=`, it skips an end, a queued buffer or an overlay whose start the trace lost, warns once with the count, and goes on from the next start. A marker that fits the play active now is applied as usual. The tool cannot see a lost start that changed the play, so it trusts such a marker. It copies the driver's integer steps for the volume, the overlay, the rest ramp, the centred square and the voice, from `tools/tone_square.py` and `tools/voice_engine.py`. `tests/tools/test_audio_engines.py` ties their constants to the C.

It does not model a late sample IRQ, a volume change during a play, or a `ROBCO_AUDIO_TONE_RATE` other than 37500. The trace does not print the tone peak, so `--tone-peak` gives it, 127 by default. The coupling capacitor is a 20 Hz high-pass, not a measured circuit. `tools/audio_preview.py --help` lists every rule and limit.
