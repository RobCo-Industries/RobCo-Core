# Project layout

This page maps the repository and shows where new code and tests go. To use the library from another project, start with [GETTING_STARTED.md](./GETTING_STARTED.md).

## Top level

| Path | Holds |
|---|---|
| `include/robco/` | Public headers. One header per component. See [API.md](./API.md). |
| `src/` | Library sources. Every `.c` and `.S` file one level below `src/` goes into `librobco.a`. |
| `ld/image.ld.S` | Linker script, run through the C preprocessor with `LINK_ADDR` and the project's `robco_config.h` |
| `mk/robco.mk` | Build fragment that projects include. Builds the library, links and seals the image. |
| `mk/host.mk` | Host unit test build, included by the top-level `Makefile` |
| `examples/` | Small images that use the library. Each one is also a target test. |
| `tests/` | Host tests, QEMU harness, target tests, tool tests, canary suite. See [tests/README.md](../../tests/README.md). |
| `tools/` | Host tools in Python: `rimg.py` seals and checks images, `check_docs.py` checks the docs, `font2c.py` and `font_specimen.py` build and preview fonts |
| `docs/` | This guide, the [specifications](../spec/README.md) and [FONTS.md](../FONTS.md) |
| `spikes/` | Three early proofs: UART hello, SD sector read, framebuffer and MMU. They have their own code in `spikes/common/` and do not use the library. |
| `ci/` | The CI container. See [ci/README.md](../../ci/README.md). |
| `.github/workflows/` | `ci.yml` for this repository, `robco-ci.yml` for projects built on it |
| `third_party/` | Upstream files with their own licences, such as the Spleen font |
| `Makefile` | Test targets for this repository. Projects do not use it. |

## Inside src/

| Directory | Holds | Touches hardware |
|---|---|---|
| `src/arch/` | `start.S` entry, exception vectors, `mmio_probe()`, exception reports, `robco_handoff()` | Yes |
| `src/board/` | Board detection, `robco_start()`, the hardware side of the boot info | Yes |
| `src/drv/` | Drivers: mailbox, UART, system timer, interrupt controller, tick, GPIO, power, RNG, framebuffer | Yes |
| `src/gfx/` | Screen console, colour schemes, font table | Through `fb.h` |
| `src/lib/` | klibc, `k_printf`, CRC-32, heap, `rimg_validate()`, boot info seal and validate | No |

`src/lib/` holds only code that runs unchanged on the host. It must not read registers, call drivers, use assembly or assume 32-bit pointers. `make test-host` builds every `src/lib/*.c` with the host compiler, ASan and UBSan and runs the tests in `tests/host/`. Code that needs hardware goes in one of the other directories.

`mk/robco.mk` picks up `src/*/*.c` and `src/*/*.S`. A file in a deeper directory, such as `src/drv/usb/hcd.c`, is not built.

## Add a driver

1. Put the code in `src/drv/<name>.c`. Find the peripheral with `board_get()->periph_base` plus the offset from [BOARD_SUPPORT.md](../spec/BOARD_SUPPORT.md#detection-table). Use the helpers in `mmio.h`.
2. Put the public declarations in `include/robco/<name>.h`. Return `0` or a negative code, and define the codes in the header.
3. Do not start anything from `robco_start()`. The project calls your `_init` function.
4. Document every function in [API.md](./API.md), with its init order and context rules. `make test-docs` fails until you do.
5. Add an example and a target test, as below.

## Add a library module

1. Put the code in `src/lib/<name>.c` and the declarations in `include/robco/<name>.h`.
2. Add `tests/host/test_<name>.c`. The format is in [tests/README.md](../../tests/README.md#host-tests).
3. Run `make test-host`.
4. Document the functions in [API.md](./API.md).

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

Create `tests/host/test_<name>.c`. It is linked with every `src/lib/*.c` and built as `build/host/test_<name>`. `rc_test.h` gives `TEST()`, `CHECK`, `CHECK_EQ`, `CHECK_STR` and `CHECK_MEM`. See [tests/README.md](../../tests/README.md#host-tests).

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
