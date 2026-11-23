# API reference

This page and the topic files under [api/](./api/) list every public function, type and constant in `include/robco/`. This page holds the conventions, boot, board, the core drivers, the screen, images, boot info and faults. The topic files hold memory, storage, input, audio, the buses and the SPI display.

Each function signature is copied from its header. `tools/check_docs.py` fails the build when a header declares a function that neither this page nor a topic file lists, or when one of them lists a function that no header declares.

| Topic file | Headers |
|---|---|
| [Memory, MMU and caches](./api/memory.md) | `alloc.h`, `mmu.h`, `cache.h` |
| [Storage and files](./api/storage.md) | `blk.h`, `blk_ram.h`, `part.h`, `sd.h`, `fs.h`, `loader.h` |
| [Input](./api/input.md) | `input.h`, `uart_mini.h` |
| [Audio](./api/audio.md) | `audio.h` |
| [Buses, clock and configuration](./api/buses.md) | `i2c.h`, `rtc.h`, `datetime.h`, `spi.h`, `ini.h` |
| [SPI display](./api/display.md) | `tft.h` |
| [User mode](./api/umode.md) | `umode.h` |

## Contents

| Header | Section |
|---|---|
| `boot.h` | [Boot and entry](#boot-and-entry-booth) |
| `board.h` | [Board detection](#board-detection-boardh) |
| `mbox.h` | [Property mailbox](#property-mailbox-mboxh) |
| `uart.h` | [PL011 UART](#pl011-uart-uarth) |
| `klibc.h` | [Strings and printf](#strings-and-printf-klibch) |
| `timer.h` | [System timer](#system-timer-timerh) |
| `irq.h` | [Interrupt controller](#interrupt-controller-irqh) |
| `tick.h` | [Periodic tick](#periodic-tick-tickh) |
| `gpio.h` | [GPIO](#gpio-gpioh) |
| `power.h` | [Reboot and halt](#reboot-and-halt-powerh) |
| `rng.h` | [Hardware RNG](#hardware-rng-rngh) |
| `fb.h` | [Framebuffer](#framebuffer-fbh) |
| `console.h` | [Screen console](#screen-console-consoleh) |
| `font.h` | [Console font](#console-font-fonth) |
| `alloc.h` | [Heap](./api/memory.md#heap-alloch) |
| `mmu.h` | [MMU](./api/memory.md#mmu-mmuh) |
| `cache.h` | [Caches](./api/memory.md#caches-cacheh) |
| `blk.h` | [Block devices](./api/storage.md#block-devices-blkh) |
| `blk_ram.h` | [RAM disk](./api/storage.md#ram-disk-blk_ramh) |
| `part.h` | [Partitions](./api/storage.md#partitions-parth) |
| `sd.h` | [SD card](./api/storage.md#sd-card-sdh) |
| `fs.h` | [FAT file system](./api/storage.md#fat-file-system-fsh) |
| `loader.h` | [Stage loader](./api/storage.md#stage-loader-loaderh) |
| `input.h` | [Input events](./api/input.md#input-events-inputh) |
| `uart_mini.h` | [Mini UART](./api/input.md#mini-uart-uart_minih) |
| `audio.h` | [Audio](./api/audio.md#audio-audioh) |
| `i2c.h` | [I2C master](./api/buses.md#i2c-master-i2ch) |
| `rtc.h` | [Real-time clock](./api/buses.md#real-time-clock-and-software-clock-rtch) |
| `datetime.h` | [Date and time](./api/buses.md#date-and-time-datetimeh) |
| `spi.h` | [SPI master](./api/buses.md#spi-master-spih) |
| `ini.h` | [Configuration files](./api/buses.md#configuration-files-inih) |
| `tft.h` | [TFT panel](./api/display.md#tft-panel-tfth) |
| `umode.h` | [User-mode tasks](./api/umode.md#user-mode-tasks-umodeh) |
| `crc32.h` | [CRC-32](#crc-32-crc32h) |
| `rimg.h` | [Image header](#image-header-rimgh) |
| `bootinfo.h` | [Boot info block](#boot-info-block-bootinfoh) |
| `exc.h` | [Exceptions](#exceptions-exch) |
| `mmio.h` | [Registers and probing](#registers-and-probing-mmioh) |

## Conventions

| Topic | Rule |
|---|---|
| Errors | Functions that can fail return `0` on success and a negative value on failure, unless the table says otherwise. Each header defines its own error codes. |
| Init | A driver with an `_init` function does nothing useful before that call. Most calls before init return silently or return an error. |
| Context | "Main context" means SVC mode with IRQs either masked or enabled. "IRQ context" means inside an interrupt handler or a tick callback. IRQs are masked there. |
| Cores | Only core 0 runs. `start.S` parks the other cores. |
| Caches | The library turns on the MMU and caches only when the image calls `mmu_enable()`. Every component works with them on or off. The default map is privileged-only: USR mode gets access only through [permission flags](./api/memory.md#permissions). |
| Locking | The library has no locks. Functions that touch shared state from both contexts say so. |
| Addresses | All addresses are ARM physical addresses. |

## Initialisation order

`start.S` and `robco_start()` run first. Your `robco_main()` then brings up what it needs, in this order.

| Step | Call | Needs |
|---|---|---|
| 1 | Automatic: `board_get()` | Nothing. Uses the mailbox. |
| 2 | Automatic: `bootinfo_receive()` | Skipped when `ROBCO_FIRST_STAGE` is 1. |
| 3 | `uart_init(115200)` | Step 1. Also makes `k_printf()` print to the UART. |
| 4 | `mmu_init()`, then `mmu_enable()` | Step 1. Optional. `fb_init()` may come before or after. |
| 5 | `fb_init(640, 480, 32)` | Step 1. |
| 6 | `console_init(scheme)`, then `console_attach_printf()` | Step 5. |
| 7 | `robco_heap_slot()` or `robco_heap_high()`, then `k_heap_init()` | Nothing. `robco_heap_high()` uses step 1. |
| 8 | `rng_init()` | Step 1. |
| 9 | `sd_init()`, `i2c_init(100000)`, `spi_init()`, `uart_mini_init(115200)` | Step 1. Each only if you use it. |
| 10 | `rtc_init()` | `i2c_init()`. Seeds the software clock. |
| 11 | `irq_init()` | Step 1. |
| 12 | `irq_register()`, `tick_register()`, `tick_init(hz)` | Step 11. `tick_init()` returns `TICK_ERR_NO_IRQ` before it. |
| 13 | `irq_cpu_enable()` | Step 12. |
| 14 | `input_init()`, the input backends, `audio_init()` | Step 13. They register tick callbacks, and `audio_init()` checks that the tick runs. |

The timer, GPIO, power and mailbox functions need no init call, nor do `datetime.h`, `ini.h`, `part.h` and `blk_ram.h`. `examples/demo/main.c` follows this order.

## Boot and entry (`boot.h`)

`boot.h` holds the image entry point, the saved entry registers, halting, and the jump to the next stage. Every image includes it.

`start.S` saves r0 to r3 and the CPU mode, sets up stacks and vectors, clears BSS and calls `robco_start()`. `robco_start()` detects the board, applies the boot info trust rule and calls your `robco_main()`. See [the startup sequence](../spec/BOARD_SUPPORT.md#startup-sequence) for each step.

```c
typedef struct {
    uint32_t r0, r1, r2, r3, entry_mode;
} robco_boot_args_t;
```

`entry_mode` is CPSR bits 4:0 at entry: `0x13` for SVC, `0x1a` for HYP.

```c
void robco_main(void);
```

You write this function. It runs in SVC mode with IRQ and FIQ masked. If it returns, the image halts.

```c
const robco_boot_args_t *robco_boot_args(void);
```

Returns the registers and mode saved at entry. Do not write through the pointer. `start.S` stores them in BSS right after it zeroes BSS, so they are not part of the file bytes the image CRC covers.

```c
void robco_halt(void) __attribute__((noreturn));
```

Masks IRQ and FIQ, then waits in a `wfi` loop forever. Callable from any context.

```c
void robco_start(void);
```

Called by `start.S`. Do not call it. On BCM2835, BCM2836 and BCM2837 it calls `bootinfo_receive()` (unless `ROBCO_FIRST_STAGE` is 1), then `robco_main()`, then `robco_halt()`. On BCM2711 it starts the UART, prints `@RC stage=<name> ev=error code=unsupported_board midr=0x...` and halts. On an unknown CPU it halts without output.

```c
void robco_handoff(uint32_t entry, const struct robco_bootinfo *bi, uint32_t fdt_or_atags)
    __attribute__((noreturn));
```

Jumps to the next stage at `entry`. It masks IRQ and FIQ on the CPU, masks every source and FIQ at the interrupt controller, parks system timer channels 1 and 3 a counter wrap ahead and clears their match flags, calls `mmu_disable()` if the MMU or data cache is on and the image links it, which cleans and invalidates the whole data cache on every CPU and turns the MMU and both caches off, invalidates the I-cache and branch predictor, and branches in SVC mode with:

| Register | Value |
|---|---|
| r0 | `0` |
| r1 | `robco_boot_args()->r1`, the machine id from the firmware |
| r2 | `fdt_or_atags` |
| r3 | `bi` |

What the caller must do before the call:

| Requirement | Why |
|---|---|
| Load and validate the next image at `entry`. | The function jumps without any check. |
| Build the block with `bootinfo_build()` at `ROBCO_BOOTINFO_ADDR`. | The next stage trusts the block only if r3 equals `0x00100000`. |
| Pass `ROBCO_HANDOFF_FDT_ADDR` or `0` as `fdt_or_atags`. | Use the return value of `bootinfo_copy_fw_blob()` to choose. |
| Call `uart_flush()`. | Output still in the FIFO is lost otherwise. |
| If your own code turned the data cache on without `mmu.h` or `cache.h`, clean it first. | Without `mmu_disable()` linked, the function cleans the data cache only on ARM1176. On ARMv7 and later dirty lines are lost. |

It does not stop the framebuffer. The steps are listed in [the boot protocol](../spec/BOOT_PROTOCOL.md#finding-loading-and-entering-the-next-stage). Example from `examples/chain1/main.c`:

```c
fdt_size = bootinfo_copy_fw_blob(ROBCO_HANDOFF_FDT_ADDR, ROBCO_HANDOFF_FDT_MAX);
bootinfo_build(bi, ROBCO_STAGE_BIOS);
k_printf("@RC stage=" STAGE " ev=jump entry=0x%08x\n", (unsigned)h->load_addr);
uart_flush();
robco_handoff(h->load_addr, bi, fdt_size != 0 ? ROBCO_HANDOFF_FDT_ADDR : 0);
```

## Board detection (`board.h`)

`board.h` tells you which SoC you run on and where its peripherals are. One image runs on every supported Pi, so drivers read the base address from here at runtime.

```c
typedef enum {
    ROBCO_SOC_UNKNOWN = 0,
    ROBCO_SOC_BCM2835 = 0x2835,
    ROBCO_SOC_BCM2836 = 0x2836,
    ROBCO_SOC_BCM2837 = 0x2837,
    ROBCO_SOC_BCM2711 = 0x2711,
} robco_soc_t;

typedef struct {
    uint32_t midr;
    robco_soc_t soc;
    uint32_t periph_base;
    uint32_t bus_alias;     /* ORed into ARM addresses handed to the VC */
    const char *soc_name;
    uint32_t board_rev;     /* 0 if the mailbox did not answer */
    uint32_t mem_base, mem_size;
} robco_board_t;
```

| MIDR part | `soc` | `soc_name` | `periph_base` | `bus_alias` | Boards |
|---|---|---|---|---|---|
| `0xb76` | `ROBCO_SOC_BCM2835` | `bcm2835` | `0x20000000` | `0x40000000` | Zero, Zero W, 1 |
| `0xc07` | `ROBCO_SOC_BCM2836` | `bcm2836` | `0x3f000000` | `0xc0000000` | 2 v1.1 |
| `0xd03` | `ROBCO_SOC_BCM2837` | `bcm2837` | `0x3f000000` | `0xc0000000` | 2 v1.2, 3, 3+, Zero 2 W |
| `0xd08` | `ROBCO_SOC_BCM2711` | `bcm2711` | `0xfe000000` | `0xc0000000` | 4, not supported |
| other | `ROBCO_SOC_UNKNOWN` | `unknown` | `0` | `0` | |

```c
const robco_board_t *board_get(void);
```

Detects the board on the first call, then returns the cached result. The first call reads MIDR and asks the mailbox for the board revision and the ARM memory range. `robco_start()` makes that first call, so later calls only return a pointer and are safe from IRQ context. Do not write through the pointer.

`mem_base` and `mem_size` are the ARM memory range from the mailbox, or `0` if it did not answer.

```c
const char *board_model_name(uint32_t revision);
```

Returns the model name for a [board revision code](../spec/BOARD_SUPPORT.md#board-revision), such as `"Raspberry Pi Zero W"`. It reads new-style codes and the old-style codes of the Pi 1. It returns `BOARD_MODEL_UNKNOWN`, `"Raspberry Pi (unknown model)"`, for `0` and for any code it does not know. It reads no hardware, so the host tests run it.

```c
const robco_board_t *b = board_get();

k_printf("@RC stage=hello ev=boot soc=%s base=0x%08x\n", b->soc_name, (unsigned)b->periph_base);
```

## Property mailbox (`mbox.h`)

`mbox.h` talks to the VideoCore firmware through property channel 8. The library uses it for the board revision, the ARM memory range, the UART clock and the framebuffer.

| Constant | Value | Meaning |
|---|---|---|
| `MBOX_TAG_BOARD_REV` | `0x00010002` | Get board revision |
| `MBOX_TAG_ARM_MEMORY` | `0x00010005` | Get ARM memory base and size |
| `MBOX_TAG_CLOCK_RATE` | `0x00030002` | Get clock rate |
| `MBOX_CLOCK_UART` | `2` | Clock id of the UART reference clock |
| `MBOX_ERR` | `-1` | Unknown board, misaligned buffer, timeout or error answer |
| `MBOX_ERR_SIZE` | `-2` | D-cache on and the message neither fits the bounce buffer nor fills whole 64-byte lines |
| `ROBCO_MBOX_BOUNCE_SIZE` | `256` | Bytes in the bounce buffer. Set it in `robco_config.h` to a multiple of 64, or the build stops. |

```c
int mbox_call(uint32_t *buf);
```

Sends a property buffer and waits for the answer. `buf` must be 16-byte aligned, and `buf[0]` holds its size in bytes. Returns `0` when the firmware sets the response bit in `buf[1]`, else `MBOX_ERR` or `MBOX_ERR_SIZE`. It busy-waits up to about a million status reads in each direction.

With the data cache on, cache maintenance never touches data next to the message, so the caller's alignment does not matter for correctness. A message that starts on a 64-byte boundary and fills whole 64-byte lines is used in place. Any other message is copied through a driver-owned bounce buffer of `ROBCO_MBOX_BOUNCE_SIZE` bytes, and a larger one fails with `MBOX_ERR_SIZE`. The largest library message, from `fb_init()`, is 136 bytes. With the data cache off, `buf` is used in place with no copy. See [mailbox and framebuffer coherence](./api/memory.md#mailbox-and-framebuffer-coherence).

It is not reentrant: do not call it, or the helpers below, from IRQ context.

```c
int mbox_get_board_rev(uint32_t *rev);
int mbox_get_arm_memory(uint32_t *base, uint32_t *size);
int mbox_get_clock_rate(uint32_t clock_id, uint32_t *hz);
```

Each sends one tag and returns `0` on success, `-1` on failure. The output stays untouched on failure. `mbox_get_clock_rate()` also fails when the firmware reports a rate of 0.

```c
int mbox_plan(uintptr_t buf, uint32_t len, uint32_t line,
              uintptr_t bounce, uint32_t bounce_size, mbox_plan_t *plan);
```

The pure rule `mbox_call()` applies with the data cache on. It returns `MBOX_PLAN_DIRECT` when `buf` and `len` are multiples of `line`, `MBOX_PLAN_BOUNCE` when `len` fits the line-aligned bounce buffer, and `MBOX_PLAN_REJECT` otherwise or for a `len` of `0`. `plan->start` and `plan->len` give the range to clean before the call and to invalidate after it: the caller's buffer, or the bounce buffer rounded up to whole lines. `tests/host/test_mbox_plan.c` covers it.

## PL011 UART (`uart.h`)

`uart.h` drives the PL011 on GPIO14 (TX) and GPIO15 (RX) at 8N1. It is the console of every image and the channel the tests read. See [the UART policy](../spec/BOARD_SUPPORT.md#uart-policy).

| Constant | Value | Meaning |
|---|---|---|
| `UART_FALLBACK_CLOCK` | `48000000u` | Reference clock used when the mailbox gives none |

```c
void uart_init(uint32_t baud);
```

Asks the mailbox for the UART clock, sets GPIO14 and GPIO15 to ALT0 with no pulls, programs the divisor and enables the FIFO. It then calls `k_console_set_default(uart_putc)`, so `k_printf()` prints here unless a console hook was already set. It does nothing when `baud` is 0 or the board is unknown. Call it once, first, from main context. It does not use the system timer, so a stage can print even when the timer is stopped.

```c
void uart_putc(char c);         /* '\n' goes out as "\r\n" */
void uart_puts(const char *s);
```

Write one character or a NUL-terminated string. They busy-wait while the transmit FIFO is full. Before `uart_init()` they do nothing.

```c
int uart_getc(void);            /* blocking */
int uart_poll(void);            /* -1 when nothing is pending */
```

Read one byte. `uart_getc()` waits for a byte, or returns `-1` at once before `uart_init()`. `uart_poll()` returns at once with the byte, or `-1` when the receive FIFO is empty or the UART is not initialised. There is no receive interrupt: poll from your main loop.

```c
void uart_flush(void);          /* waits until the transmitter is idle */
```

Waits until the last byte has left the shift register. Call it before `robco_halt()`, `power_reboot()` or `robco_handoff()`.

```c
int uart_get_config(uint32_t *clock, uint32_t *baud);
```

Stores the reference clock and baud rate in use. Returns `0`, or `-1` before `uart_init()`. `bootinfo_build()` uses it.

The UART functions have no lock. Output from IRQ context and main context can interleave at character level.

```c
uart_init(115200);
k_printf("\n@RC stage=hello ev=ready\n");
for (;;) {
    c = uart_getc();
    uart_putc(c == '\r' ? '\n' : (char)c);
}
```

## Strings and printf (`klibc.h`)

`klibc.h` is the small C library: memory and string functions and a `printf` family. The code lives in `src/lib/` and runs on the host in the unit tests.

```c
void *k_memcpy(void *dst, const void *src, size_t n);
void *k_memset(void *dst, int value, size_t n);
void *k_memmove(void *dst, const void *src, size_t n);
int k_memcmp(const void *a, const void *b, size_t n);
size_t k_strlen(const char *s);
int k_strcmp(const char *a, const char *b);
int k_strncmp(const char *a, const char *b, size_t n);
char *k_strncpy(char *dst, const char *src, size_t n);
```

Same contracts as the standard C functions of the same name without the `k_` prefix. `k_strncpy()` pads with NUL up to `n` and does not terminate a source of `n` bytes or more.

```c
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
```

GCC emits calls to these even with `-ffreestanding`, so the library provides them. You may call them directly. Host test builds define `ROBCO_HOST` and use the host libc versions instead.

```c
void k_console_set(void (*putc)(char));
void k_console_set_default(void (*putc)(char));
```

Set the function `k_printf()` writes each character to. `k_console_set()` always replaces it. `k_console_set_default()` sets it only if none is set yet. `uart_init()` uses the second, `console_attach_printf()` the first.

```c
void k_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int k_snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int k_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
```

`k_printf()` formats to the console hook and drops the output while no hook is set. `k_snprintf()` and `k_vsnprintf()` write at most `size - 1` characters plus a NUL and return the length the full output would have.

| Supported | Not supported |
|---|---|
| `%s %c %d %u %x %X %p %%` | Length modifiers (`%l`, `%ll`, `%z`), `+`, space and `#` flags, floating point |
| `-` and `0` flags, a field width and a precision, such as `%08x`, `%-6s`, `%.5s`, `%.3d` | |
| `*` for the width or the precision, such as `%*d`, `%-*s`, `%.*s` | |

The flags, width and precision follow C. `-` pads on the right and beats `0`. A negative `*` width means `-`. A negative `*` precision means no precision. `%.5s` prints at most 5 bytes and reads no further. On `%d %u %x %X` the precision is the minimum digit count, it turns `0` off, and `%.0d` prints nothing for 0. `%c` takes a width. `%s` ignores `0`. `%p` always prints `0x` and the full pointer in zero-padded hex, and ignores flags, width and precision.

Pass 64-bit values as two 32-bit halves or cast them to `unsigned`. `%s` prints `(null)` for a NULL pointer, cut by the precision. An unknown conversion prints its whole spec as written, such as `%-5q`. A `*` in it still takes an argument. A `%` spec cut off by the end of the format prints nothing.

`k_printf()` holds no lock and the output hook busy-waits. Calling it from IRQ context works, but the text can land in the middle of a main-context line.

## System timer (`timer.h`)

`timer.h` reads the BCM283x system timer, a free-running 1 MHz counter with four compare channels. Use it for timestamps, delays and deadlines. It needs no init.

| Channel | Owner |
|---|---|
| 0 | GPU firmware. Do not use. |
| 1 | `tick.c` |
| 2 | GPU firmware. Do not use. |
| 3 | Free for your code |

```c
uint64_t timer_now_us(void);
uint32_t timer_now_ms(void);
uint32_t timer_now_lo(void);
```

Microseconds since the counter started, the same in milliseconds (wraps after about 49 days), and the low 32 bits (CLO, wraps after about 71 minutes). `timer_now_us()` reads CHI and CLO until CHI is stable.

```c
void timer_delay_us(uint32_t us);
void timer_delay_ms(uint32_t ms);
```

Busy-wait at least the given time. Both are correct up to `UINT32_MAX`. They block, so keep them out of IRQ context unless the wait is short.

Every wait and deadline on this counter needs it to run. When it does not advance, `timer_delay_us()`, `timer_delay_ms()` and any loop on `timer_expired()` wait forever. To check it, read `timer_now_lo()` a bounded number of times and see whether it changes. `uart_init()` and `gpio_set_pull()` do not use the timer. `tick_init()` returns 0 with a stopped timer, but no tick comes. `audio_init()` returns `AUDIO_ERR_TIMER`. A finite `input_wait()` timeout never expires.

```c
int timer_expired(uint64_t deadline_us);
```

Non-zero once `timer_now_us()` has reached `deadline_us`. `timer_expired(0)` is always true and `timer_expired(UINT64_MAX)` never is.

```c
void timer_compare_set(unsigned ch, uint32_t clo);
void timer_compare_clear(unsigned ch);
int timer_compare_pending(unsigned ch);
```

Set compare channel `ch` to match when CLO equals `clo`, clear its match flag, or test the flag. Channels above 3 are ignored. The comparator fires only on equality, so a value already in the past matches only after CLO wraps. The functions do not check which channel you use.

```c
int timer_compare_arm(unsigned ch, uint32_t clo);
uint32_t timer_oneshot_arm(unsigned ch, uint32_t clo);
```

Use these to arm a match. A write can land after CLO has passed `clo`, for example after a long masked section or a host stall under QEMU. The match is then missed. `timer_compare_arm()` writes `clo`, clears the flag and re-reads CLO. It returns `0` when the interrupt will come and `1` when the match was missed. `timer_oneshot_arm()` handles the miss itself: it arms a short distance ahead, `TIMER_ARM_MIN_US` at first and doubling up to `TIMER_ARM_MAX_US`, until the match holds. It returns the value armed, so the interrupt always comes, maybe late. The tick uses `timer_compare_arm()` and counts a missed period in `tick_count()` and `tick_missed()`.

All timer reads are safe from any context. A one-shot on channel 3, from `examples/tick/main.c`:

```c
static void on_oneshot(unsigned irq, void *arg)
{
    (void)arg;
    oneshot_late = timer_now_lo() - oneshot_deadline;
    timer_compare_clear(ONESHOT_CH);
    irq_disable(irq);
    oneshot_fired = 1;
}

irq_register(IRQ_TIMER3, on_oneshot, 0);
oneshot_deadline = timer_now_lo() + ONESHOT_US;
timer_oneshot_arm(ONESHOT_CH, oneshot_deadline);
irq_enable(IRQ_TIMER3);
irq_cpu_enable();
```

## Interrupt controller (`irq.h`)

`irq.h` drives the BCM283x legacy interrupt controller and dispatches IRQs to handlers. On BCM2836 and BCM2837 it also routes the controller to core 0 through the local controller.

| Constant | Value | Meaning |
|---|---|---|
| `IRQ_GPU_COUNT` | `64` | IRQs 0 to 63 are GPU sources (pending 1 and 2) |
| `IRQ_BASIC_FIRST` | `64` | IRQs 64 to 71 are ARM basic sources, bits 0 to 7 |
| `IRQ_COUNT` | `72` | Number of IRQ numbers |
| `IRQ_TIMER1` | `1` | System timer channel 1, used by the tick |
| `IRQ_TIMER3` | `3` | System timer channel 3 |

```c
typedef void (*irq_handler_t)(unsigned irq, void *arg);
```

```c
void irq_init(void);
```

Masks IRQs on the CPU, disables every source and FIQ at the controller, clears every handler and the spurious count, and routes the controller to core 0. Call it before `tick_init()`. Only the first call does this. Later calls change nothing, so handlers registered in between, the tick included, stay.

```c
int irq_ready(void);
```

Non-zero once `irq_init()` has run. `tick_init()` uses it.

```c
int irq_register(unsigned irq, irq_handler_t fn, void *arg);
void irq_unregister(unsigned irq);
```

`irq_register()` installs `fn` with `arg` for `irq`. Returns `0`, or `-1` if `irq` is out of range, `fn` is NULL or the slot is taken. It does not enable the source. `irq_unregister()` disables the source and removes the handler.

```c
void irq_enable(unsigned irq);
void irq_disable(unsigned irq);
```

Unmask or mask one source at the controller. Both do nothing before `irq_init()`.

```c
uint32_t irq_save(void);
void irq_restore(uint32_t state);
void irq_cpu_enable(void);
void irq_cpu_disable(void);
```

`irq_save()` masks IRQs on the CPU and returns the previous mask state. `irq_restore()` unmasks only if they were unmasked before. Use the pair around code that shares data with a handler. `irq_cpu_enable()` and `irq_cpu_disable()` clear and set CPSR.I directly. IRQs start masked: nothing fires until you call `irq_cpu_enable()`.

```c
uint32_t irq_spurious_count(void);
```

Number of sources that fired with no handler. The dispatcher disables each such source.

```c
void robco_irq_dispatch(uint32_t pc, uint32_t spsr);
```

Called by the IRQ vector. Do not call it. It reads pending 1, pending 2 and basic pending, masked by the sources you enabled, and runs each handler. It repeats up to 8 rounds while sources stay pending, then returns. Without `src/drv/irq_bcm.c` in the image, a weak fallback reports any IRQ as an exception.

Context rules:

| Rule | Detail |
|---|---|
| Handlers run in IRQ mode with IRQs masked. | No nested interrupts. A slow handler delays every other source. |
| No FIQ. | `irq_init()` disables FIQ routing. The FIQ vector reports an exception. |
| Clear the source in the handler. | Otherwise the dispatcher calls it again, up to 8 times, then the CPU re-enters the vector. |
| The IRQ stack is 4 KiB by default. | Set `ROBCO_STACK_IRQ_SIZE` for deep handlers. |

## Periodic tick (`tick.h`)

`tick.h` gives a periodic interrupt on system timer channel 1 and a tick counter. Use it for time slices, blinking and polling at a fixed rate.

| Constant | Value | Meaning |
|---|---|---|
| `TICK_MAX_CALLBACKS` | `4` | Callback slots |
| `TICK_HZ_MIN` | `1u` | Lowest accepted rate |
| `TICK_HZ_MAX` | `10000u` | Highest accepted rate, a 100 microsecond period |
| `TICK_ERR_RANGE` | `-1` | `hz` outside `TICK_HZ_MIN` to `TICK_HZ_MAX` |
| `TICK_ERR_RUNNING` | `-2` | The tick already runs at another rate |
| `TICK_ERR_NO_IRQ` | `-3` | `irq_init()` has not run |
| `TICK_ERR_IRQ_TAKEN` | `-4` | Another handler owns `IRQ_TIMER1` |

```c
int tick_init(uint32_t hz);
```

Registers the channel 1 handler, arms the first match and enables `IRQ_TIMER1`. Returns `0` or a `TICK_ERR_*` code. It leaves the CPU IRQ mask as it was: call `irq_cpu_enable()` afterwards. Call `irq_init()` first. Before it, `tick_init()` returns `TICK_ERR_NO_IRQ` and changes nothing, so you can call it again after `irq_init()`. While the tick runs, a call with the same rate returns `0` and a call with another rate returns `TICK_ERR_RUNNING`: the rate never changes once set. There is no way to stop the tick.

```c
uint32_t tick_count(void);
uint32_t tick_missed(void);
uint32_t tick_ms(void);
```

`tick_count()` is the number of periods since `tick_init()`, counted even when the handler ran late. `tick_missed()` counts periods that passed without a callback run. `tick_ms()` converts the count to milliseconds with the programmed period, or returns `0` before `tick_init()`. It counts past a `tick_count()` wrap and wraps itself at 2^32 ms. `tick_count()` and `tick_missed()` are safe from any context. `tick_ms()` uses a 64-bit division: keep it out of IRQ context.

```c
uint32_t tick_hz(void);
```

The rate `tick_init()` started, in Hz, or `0` when the tick does not run. The real rate can be a little higher when the rate does not divide 1000000. Safe from any context.

```c
int tick_register(void (*fn)(void));
```

Adds a callback. Returns `0`, or `-1` when all four slots are full. There is no unregister. You may register before or after `tick_init()`.

Sharp edges:

| Edge | Detail |
|---|---|
| Callbacks run in IRQ context. | IRQs are masked. Do not block, do not call the heap, keep them short. |
| Four slots for the whole image. | The input GPIO backend, input auto-repeat, the input console backend in tick mode and audio each take one. Plan the slots before you add your own callbacks. |
| Printing from a callback. | Do not. The UART and `k_printf()` have no lock. The audio trace backend queues its records in the tick and prints them from `audio_poll()` in main context. |
| At most one run per interrupt. | If callbacks or a masked section make the handler late, it counts the skipped periods in `tick_count()` and `tick_missed()` and runs the callbacks once. |
| Period resolution. | The period is `1000000 / hz` microseconds, rounded down. 7000 Hz gives 142 microseconds, about 7042 Hz. `tick_ms()` uses the real period, so it stays exact. |

```c
uint32_t tick_period_us_for(uint32_t hz);
uint64_t tick_to_ms(uint64_t ticks, uint32_t period_us);
```

Pure functions in `src/lib/tick_conv.c`, tested on the host. `tick_period_us_for()` returns the period in microseconds for `hz`, or `0` if `hz` is out of range. `tick_to_ms()` converts `ticks` periods of `period_us` to milliseconds, rounded down. It is exact for `ticks` below 2^44.

From `examples/tick/main.c`:

```c
static void on_tick(void)
{
    uint32_t n = tick_count();
    uint32_t now = (uint32_t)timer_now_us();

    tick_us[n % RING] = now;
}

irq_init();
tick_register(on_tick);
tick_init(TICK_HZ);
irq_cpu_enable();
```

## GPIO (`gpio.h`)

`gpio.h` sets pin functions, pulls and levels on BCM283x pins 0 to 53. It needs no init. `uart_init()` already sets GPIO14 and GPIO15.

| Constant | Value | Meaning |
|---|---|---|
| `GPIO_PIN_COUNT` | `54` | Pins 0 to 53 |
| `GPIO_ERR_PIN` | `-1` | Pin out of range |
| `GPIO_ERR_ARG` | `-2` | Bad function, pull or level |
| `GPIO_ERR_BOARD` | `-3` | No GPIO block, or pulls on BCM2711 |

| Function code | Value | | Function code | Value |
|---|---|---|---|---|
| `GPIO_FN_INPUT` | `0` | | `GPIO_FN_ALT2` | `6` |
| `GPIO_FN_OUTPUT` | `1` | | `GPIO_FN_ALT3` | `7` |
| `GPIO_FN_ALT0` | `4` | | `GPIO_FN_ALT4` | `3` |
| `GPIO_FN_ALT1` | `5` | | `GPIO_FN_ALT5` | `2` |

| Pull code | Value |
|---|---|
| `GPIO_PULL_NONE` | `0` |
| `GPIO_PULL_DOWN` | `1` |
| `GPIO_PULL_UP` | `2` |

```c
int gpio_set_function(unsigned pin, unsigned fn);
int gpio_get_function(unsigned pin);
```

Set or read the pin's 3-bit GPFSEL code. `gpio_get_function()` returns the code or a negative error.

```c
int gpio_set_pull(unsigned pin, unsigned pull);
```

Runs the GPPUD and GPPUDCLK sequence with two waits of at least 5 microseconds. The waits count 7500 loop iterations instead of reading the system timer, so `uart_init()` works with a stopped timer. One iteration takes at least one CPU cycle, so each wait lasts 5 microseconds or more up to 1.5 GHz. Returns `GPIO_ERR_BOARD` on BCM2711.

```c
int gpio_write(unsigned pin, int level);
int gpio_read(unsigned pin);
int gpio_toggle(unsigned pin);
```

`gpio_write()` drives the output latch: `0` low, anything else high. `gpio_read()` returns `0` or `1` from GPLEV, or a negative error. `gpio_toggle()` reads the level and writes its inverse.

```c
int gpio_config_output(unsigned pin, int initial_level);
int gpio_config_input(unsigned pin, unsigned pull);
```

`gpio_config_output()` sets the level first, then switches to output, so the pin does not glitch. `gpio_config_input()` switches to input and sets the pull.

Every function returns `0` or a negative `GPIO_ERR_*`. `gpio_set_function()` and `gpio_toggle()` read, modify and write without a lock: do not change pins in the same GPFSEL register from both IRQ and main context. In QEMU every input reads 0.

```c
gpio_config_output(5, 1);
gpio_toggle(5);
gpio_config_input(17, GPIO_PULL_UP);
k_printf("@RC stage=gpio ev=in pin17=%d\n", gpio_read(17));
```

## Reboot and halt (`power.h`)

`power.h` resets the SoC through the power management watchdog.

```c
void power_reboot(void) __attribute__((noreturn));
```

Arms the watchdog with a short timeout and requests a full reset. Call `uart_flush()` first.

```c
void power_halt(void) __attribute__((noreturn));
```

Writes the halt partition `0x555` into PM_RSTS, the firmware convention for "stay halted", then resets. If the reset does not happen it waits in `wfi`.

```c
uint32_t power_reset_status(void);
```

Returns the raw PM_RSTS register, or `0` on an unknown board. Read it early in `robco_main()` to see why the board reset.

## Hardware RNG (`rng.h`)

`rng.h` reads the SoC hardware random number generator.

| Constant | Value | Meaning |
|---|---|---|
| `RNG_ERR_BOARD` | `-1` | No peripheral base |
| `RNG_ERR_TIMEOUT` | `-2` | No data within the poll limit |

```c
int rng_init(void);
```

Starts the generator if it is off and discards its first words. Returns `0` or `RNG_ERR_BOARD`.

```c
int rng_read(void *buf, size_t len);
```

Fills `buf` with `len` random bytes. It busy-waits for each word, up to a million status reads. Returns `0` or a negative error. Before `rng_init()` it may time out.

```c
int rng_u32(uint32_t *out);
```

Stores one random word in `*out`. Returns `0`, or a negative error as `rng_read()` does and leaves `*out` untouched.

## Framebuffer (`fb.h`)

`fb.h` allocates a 32-bit framebuffer through the mailbox and draws rectangles, pixels and 1-bit bitmaps into it. The screen console builds on it.

| Constant | Value | Meaning |
|---|---|---|
| `FB_ORDER_BGR` | `0` | Pixel word is `0xAARRGGBB` |
| `FB_ORDER_RGB` | `1` | Pixel word is `0xAABBGGRR` |
| `FB_ERR_MBOX` | `-1` | Mailbox call or a tag failed |
| `FB_ERR_DEPTH` | `-2` | Depth other than 32 asked for |
| `FB_ERR_MISMATCH` | `-3` | Firmware gave another size or depth, or a size of 0 was asked for |
| `FB_ERR_BUFFER` | `-4` | No buffer, or pitch or size too small |

```c
typedef struct {
    uint8_t *addr;
    uint32_t size, width, height, pitch, depth, order;
} fb_info_t;
```

```c
int fb_init(uint32_t width, uint32_t height, uint32_t depth);
```

Sends one property message that sets the size and depth, asks for RGB order, allocates the buffer and reads the pitch and the actual order. Only `depth == 32` works. Returns `0`, or a negative `FB_ERR_*` and keeps the previous state. A second successful call replaces the buffer. The console then draws nothing and `console_cols()` returns `0` until you call `console_init()` again, which sizes the grid for the new mode.

```c
const fb_info_t *fb_get(void);
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);
```

`fb_get()` returns the active state, or NULL until `fb_init()` succeeds. `fb_rgb()` builds a pixel word for the active pixel order. The firmware may ignore the RGB request, so always build colours with `fb_rgb()`.

```c
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t colour);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t colour);
void fb_blit_1bpp(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  const uint8_t *bits, uint32_t fg, uint32_t bg);
void fb_scroll_up(uint32_t pixels, uint32_t bg);
void fb_clear(uint32_t colour);
```

Drawing clips to the screen and does nothing before `fb_init()`. `fb_blit_1bpp()` takes rows MSB-left, each padded to a whole byte. `fb_scroll_up()` moves the screen up by `pixels` rows and fills the freed rows with `bg`. There is no double buffering and no vsync. None of these lock: draw from one context only.

```c
rc = fb_init(640, 480, 32);
if (rc != 0) {
    k_printf("@RC stage=console ev=error what=fb_init rc=%d\n", rc);
    robco_halt();
}
fb_clear(fb_rgb(0, 0, 0));
```

## Screen console (`console.h`)

`console.h` is a text console on the framebuffer: a grid of 8x16 cells, four phosphor colour schemes, attributes and a block cursor. It keeps a cell buffer, so it can redraw, scroll and dump the screen as text for tests.

| Config macro | Default | Meaning |
|---|---|---|
| `ROBCO_CONSOLE_MAX_COLS` | `100` | Columns in the cell buffer |
| `ROBCO_CONSOLE_MAX_ROWS` | `40` | Rows in the cell buffer |

The grid is `width / 8` by `height / 16` cells. 640x480 gives 80x30. A mode larger than the buffer, such as 1024x768, fails with `CONSOLE_ERR_TOO_BIG` unless you raise the limits in `robco_config.h`.

| Scheme | Value | Background | Dim | Normal | Bright |
|---|---|---|---|---|---|
| `CONSOLE_SCHEME_GREEN` | `0` | `#0a1a0e` | `#1f8a3d` | `#33e066` | `#b0ffc8` |
| `CONSOLE_SCHEME_AMBER` | `1` | `#1a1006` | `#8a5a10` | `#ffb000` | `#ffe0a0` |
| `CONSOLE_SCHEME_BLUE` | `2` | `#08101e` | `#2a5a9a` | `#5ab4ff` | `#c8e6ff` |
| `CONSOLE_SCHEME_WHITE` | `3` | `#121214` | `#808084` | `#d8d8dc` | `#ffffff` |

`CONSOLE_SCHEME_COUNT` is `4`. The boot info `con_scheme` field uses the same numbers.

| Attribute | Value | Meaning |
|---|---|---|
| `CONSOLE_ATTR_NORMAL` | `0` | Normal brightness |
| `CONSOLE_ATTR_DIM` | `1` | Dim |
| `CONSOLE_ATTR_BRIGHT` | `2` | Bright |
| `CONSOLE_ATTR_INVERSE` | `4` | Swap foreground and background. OR it with a brightness. |

| Error | Value | Meaning |
|---|---|---|
| `CONSOLE_ERR_NO_FB` | `-1` | `fb_init()` has not succeeded |
| `CONSOLE_ERR_TOO_BIG` | `-2` | The grid is empty or larger than the buffer |
| `CONSOLE_ERR_SCHEME` | `-3` | Unknown scheme id |

```c
typedef struct {
    uint8_t r, g, b;
} console_rgb_t;

typedef struct {
    const char *name;
    console_rgb_t bg, dim, normal, bright;
} console_scheme_t;
```

```c
const console_scheme_t *console_scheme_info(unsigned scheme);
```

Returns the colours and name (`green`, `amber`, `blue`, `white`) of a scheme, or NULL for an unknown id.

```c
int console_init(unsigned scheme);
```

Sizes the grid from the framebuffer, loads the scheme, clears the screen and puts the cursor at 0,0 with attribute normal and the cursor hidden. Returns `0` or a `CONSOLE_ERR_*`. Call it again after every successful `fb_init()`: until then the console is stale and every drawing call does nothing.

```c
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_puts(const char *s);
```

Write text at the cursor with the current attribute. Before `console_init()`, or while the console is stale after `fb_init()`, they do nothing.

| Byte | Effect |
|---|---|
| `'\n'` | Column 0 of the next row. Scrolls at the bottom. |
| `'\r'` | Column 0 of the same row |
| `'\b'` | One column left, no erase. Cancels a pending wrap instead. |
| `'\t'` | Next multiple of 8. Past the last column, a new line. |
| `0x20` to `0x7e` | The ASCII glyph |
| `0x80` to `0xff` | The CP437 glyph, if the font has it, else a replacement box |
| Other control bytes, `0x7f` | Ignored |

Last-column wrap: a character written in the last column leaves the cursor on that column and sets a pending wrap. The next printable character moves to the next row first. So a line of exactly 80 characters followed by `'\n'` gives one line, not a blank one. `'\r'`, `'\n'`, `'\t'`, `'\b'` and `console_set_cursor()` cancel the pending wrap.

```c
void console_clear(void);
void console_set_cursor(unsigned col, unsigned row);
void console_get_cursor(unsigned *col, unsigned *row);
unsigned console_cols(void);
unsigned console_rows(void);
void console_cursor_show(int on);
```

`console_clear()` blanks the grid and homes the cursor. `console_set_cursor()` clamps to the grid. `console_get_cursor()` accepts NULL for either output. `console_cols()` and `console_rows()` return `0` before `console_init()` and while the console is stale. `console_cursor_show()` draws a block cursor by inverting the cell under it. It is off after init.

```c
void console_set_attr(unsigned attr);
void console_set_scheme(unsigned scheme);
unsigned console_scheme(void);
```

`console_set_attr()` applies to characters written after the call. It accepts a brightness, optionally ORed with `CONSOLE_ATTR_INVERSE`. It ignores any other value, such as a brightness of `3` or an unknown bit, and keeps the current attribute. `console_set_scheme()` redraws the whole screen in the new colours and ignores unknown ids. `console_scheme()` returns the active id, `CONSOLE_SCHEME_GREEN` before `console_init()`.

```c
void console_dump(void (*out)(char));
```

Test hook. Writes `@RC stage=<name> ev=screen cols=.. rows=.. cx=.. cy=.. scheme=..`, then one line per row starting with `|` with trailing spaces trimmed, then `@RC stage=<name> ev=screen_end`. Cells outside printable ASCII print as `#`. Pass `uart_putc`.

```c
void console_attach_printf(int also_uart);
```

Makes `k_printf()` write to the console, and also to the UART when `also_uart` is non-zero. It replaces any hook set before.

The console has no lock and draws synchronously. Write to it from one context only. Do not attach it to `k_printf()` if IRQ handlers print.

Based on `examples/demo/main.c`:

```c
rc = fb_init(640, 480, 32);
if (rc != 0) {
    fail("fb_init", rc);
}
rc = console_init(CONSOLE_SCHEME_GREEN);
if (rc != 0) {
    fail("console_init", rc);
}
console_attach_printf(1);
console_set_attr(CONSOLE_ATTR_BRIGHT);
console_puts("RobCo-Core demo\n");
console_set_attr(CONSOLE_ATTR_NORMAL);
```

## Console font (`font.h`)

`font.h` gives the 8x16 glyph table the console draws with. You rarely call it directly. The library has two built-in fonts. Set `ROBCO_FONT` in `robco_config.h` to pick one. The sources, licences and how to build another table are in [FONTS.md](../FONTS.md).

| Config macro | Default | Meaning |
|---|---|---|
| `ROBCO_FONT` | `ROBCO_FONT_FIXEDSYS` | `ROBCO_FONT_FIXEDSYS` (1) for Fixedsys Excelsior, `ROBCO_FONT_SPLEEN` (2) for Spleen. Any other value stops the build. |

| Constant | Value | Meaning |
|---|---|---|
| `FONT_W` | `8` | Glyph width in pixels |
| `FONT_H` | `16` | Glyph height in pixels |
| `FONT_CH_BLOCK` | `0xdb` | Full block |
| `FONT_CH_SHADE1`, `FONT_CH_SHADE2`, `FONT_CH_SHADE3` | `0xb0`, `0xb1`, `0xb2` | Light, medium and dark shade |
| `FONT_CH_HLINE`, `FONT_CH_VLINE` | `0xc4`, `0xb3` | Single box lines |
| `FONT_CH_TL`, `FONT_CH_TR`, `FONT_CH_BL`, `FONT_CH_BR` | `0xda`, `0xbf`, `0xc0`, `0xd9` | Single box corners |

```c
const uint8_t *font8x16_glyph(uint32_t ch);
```

Returns 16 bytes, one per row, bit 7 on the left. Takes printable ASCII, a CP437 byte the table holds, or the matching Unicode code point. Anything else gives a replacement box.

```c
const char *font8x16_name(void);
```

Short name of the font the table was built from: `fixedsys-excelsior` or `spleen` for the built-in tables.

## CRC-32 (`crc32.h`)

`crc32.h` computes CRC-32/ISO-HDLC, the zlib `crc32()`: reflected, polynomial `0xEDB88320`, init and final XOR `0xFFFFFFFF`. The image header and the boot info use it.

```c
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t crc32(const void *data, size_t len);
```

`crc32()` checksums one buffer. To stream, start with `crc = 0` and pass each result back into `crc32_update()`. Both are pure and safe from any context. `crc32("123456789", 9)` is `0xCBF43926`.

## Image header (`rimg.h`)

`rimg.h` defines the `rimg` image format and validates an image in memory. Every image the library builds carries this header, and a loader checks it before a jump. The format is in [IMAGE_FORMAT.md](../spec/IMAGE_FORMAT.md).

| Offset | Content |
|---|---|
| `0x00` | Branch to the entry point, padded to `0x20` |
| `0x20` | `rimg_header_t`, 64 bytes |
| `0x60` | `rimg_meta_t`, 192 bytes, only if flags bit 0 is set |

| Constant | Value |
|---|---|
| `RIMG_HDR_OFFSET` | `0x20` |
| `RIMG_META_OFFSET` | `0x60` |
| `RIMG_HDR_VERSION` | `1` |
| `RIMG_HDR_SIZE` | `64` |
| `RIMG_META_SIZE` | `192` |
| `RIMG_FLAG_META` | `1 << 0` |
| `RIMG_MAGIC_BOOT` | `"BOOT\0\0\0\0"` |
| `RIMG_MAGIC_ROM1` | `"ROM1\0\0\0\0"` |
| `RIMG_MAGIC_ROBCO78` | `"ROBCO78\0"` |

| Subtype | Value | Default slot |
|---|---|---|
| `RIMG_BOOT_UOS` | `0` | `0x00400000`, `0x00c00000` bytes |
| `RIMG_BOOT_MAINT` | `2` | `0x00400000`, `0x00c00000` bytes |
| `RIMG_BOOT_DIAG` | `3` | `0x00400000`, `0x00c00000` bytes |
| `RIMG_BOOT_AGENT` | `4` | `0x00200000`, `0x00200000` bytes |
| `RIMG_BOOT_BIOS` | `5` | `0x00008000`, `0x000f8000` bytes |
| `RIMG_BOOT_PIPOS` | `6` | `0x00008000`, `0x003f8000` bytes |
| `RIMG_BOOT_EXAMPLE` | `7` | `0x00008000`, `0x000f8000` bytes |
| `RIMG_ROM1_SYSTEM` | `0` | `0x00400000`, `0x00400000` bytes |
| `RIMG_HOLO_GAME` | `0` | `0x00800000`, `0x00400000` bytes |
| `RIMG_HOLO_UTILITY` | `1` | `0x00800000`, `0x00400000` bytes |
| `RIMG_HOLO_DATA` | `2` | None. Not executable. |

`start.S` builds the header from these `robco_config.h` macros:

| Config macro | Default | Header field |
|---|---|---|
| `ROBCO_IMG_MAGIC` | `RIMG_MAGIC_BOOT` | `magic` |
| `ROBCO_IMG_SUBTYPE` | `RIMG_BOOT_EXAMPLE` | `subtype` |
| `ROBCO_IMG_ABI_VERSION` | `(2 << 16)` | `abi_version`, `(major << 16) \| minor` |
| `ROBCO_IMG_VERSION` | `0` | `image_version` |
| `ROBCO_IMG_NAME` | `"robco"` | `name`, at most 16 bytes |
| `ROBCO_IMG_FLAGS` | `0` | `flags`. `start.S` refuses `RIMG_FLAG_META`. |

```c
typedef struct __attribute__((packed)) rimg_header {
    char     magic[8];
    uint16_t hdr_version;
    uint16_t hdr_size;
    uint32_t flags;
    uint32_t load_addr;
    uint32_t file_size;     /* size of the raw image */
    uint32_t mem_size;      /* file size + bss + stacks */
    uint32_t crc32;         /* 0 until a host tool seals the image */
    uint32_t abi_version;
    uint32_t image_version;
    uint32_t subtype;
    char     name[16];
    uint32_t reserved;
} rimg_header_t;

typedef struct __attribute__((packed)) rimg_meta {
    char    title[64];
    uint8_t icon[128];
} rimg_meta_t;
```

The META block is [specified](../spec/IMAGE_FORMAT.md#meta-block). `tools/rimg.py seal --title --icon` fills it, and `tools/rimg.py pack` wraps a raw payload into a complete image with one. `rimg_validate()` requires it on `ROM1` and `ROBCO78` images and refuses it on `BOOT` images.

```c
typedef enum rimg_err {
    RIMG_OK        = 0,
    RIMG_E_NOFILE  = 1,
    RIMG_E_IO      = 2,
    RIMG_E_MAGIC   = 3,
    RIMG_E_VERSION = 4,
    RIMG_E_SIZE    = 5,
    RIMG_E_BOUNDS  = 6,
    RIMG_E_ABI     = 7,
    RIMG_E_CRC     = 8,
} rimg_err_t;

typedef struct rimg_expect {
    const char *magic;
    uint32_t slot_base;
    uint32_t slot_size;
    uint32_t abi_major;
    uint32_t file_size;
} rimg_expect_t;
```

| `rimg_expect_t` field | Meaning | `0` or NULL means |
|---|---|---|
| `magic` | 8-byte magic to accept | Any of the three magics |
| `slot_base`, `slot_size` | Slot the image must fit | The default slot of the subtype (when `slot_size` is 0) |
| `abi_major` | ABI major to accept | 2 for BOOT, 1 otherwise |
| `file_size` | Size from the FAT directory entry | Skip that comparison |

```c
int rimg_validate(const void *image, size_t avail, const rimg_expect_t *expect,
                  const rimg_header_t **out);
```

Runs [validation steps 1 to 6](../spec/IMAGE_FORMAT.md#validation) on an image in memory. `avail` is the number of readable bytes at `image`, and the image's `file_size` must not exceed it. Returns `RIMG_OK` and sets `*out` (if not NULL) to the header, or the code of the first failed step. Fewer than `0x60` bytes gives `RIMG_E_IO`. `expect` may be NULL. It touches no hardware and is safe from any context.

```c
uint32_t rimg_crc(const void *image, uint32_t file_size);
```

The image CRC from the [spec](../spec/IMAGE_FORMAT.md#crc): CRC-32 over `[0, file_size)` with the `crc32` field at `0x3C` read as zero. Returns `0` for a NULL `image` or a `file_size` below `0x40`. It touches no hardware.

```c
int rimg_self_check(void);
```

Checks the running image against its own sealed CRC, over the whole file. It first checks the header with `rimg_self_scan()` against the values the image was linked with. A header that does not match returns that check's code, and the CRC scan does not run. So a corrupt `file_size` gives `RIMG_E_SIZE` and no read past the image. Otherwise it returns `RIMG_OK` or `RIMG_E_CRC`. Call it early in `robco_main()`. The library keeps its startup state in BSS, but a `.data` variable that the image writes before the call changes the result. `examples/chain2` built with `CHAIN2_SELF_CHECK=1` calls it after its boot line.

```c
int rimg_self_scan(const rimg_header_t *h, const char magic[8], uint32_t load_addr,
                   uint32_t file_size, uint32_t *scan);
```

The header checks of `rimg_self_check()`, in the order of the validation steps. `magic` must match (`RIMG_E_MAGIC`). `hdr_version` and `hdr_size` must be 1 and 64 (`RIMG_E_VERSION`). `file_size` must equal the linked size `__image_file_size` (`RIMG_E_SIZE`). `load_addr` must equal the linked start (`RIMG_E_BOUNDS`). On `RIMG_OK` it sets `*scan` to the bytes the CRC may cover. It touches no hardware.

```c
const char *rimg_strerror(int code);
```

The `@RC` name of a code, such as `crc`, or `unknown`.

From `examples/chain1/main.c`:

```c
const rimg_expect_t expect = {
    .magic = RIMG_MAGIC_BOOT,
    .slot_base = NEXT_ADDR,
    .slot_size = NEXT_SIZE,
    .abi_major = 2,
};
const rimg_header_t *h;

rc = rimg_validate((const void *)NEXT_ADDR, NEXT_SIZE, &expect, &h);
if (rc != RIMG_OK) {
    k_printf("@RC stage=" STAGE " ev=error code=%s\n", rimg_strerror(rc));
    halt();
}
```

```c
int rimg_check_header(const void *head, size_t avail, const rimg_expect_t *expect);
```

Runs validation steps 1 to 5 on the first `0x60` bytes of an image before the rest is read. `head` holds at least `0x60` bytes. `avail` is the size of the whole file. It reads only `head`, and returns `RIMG_E_IO` for a `NULL` `head` or `avail` below `0x60`. `rimg_validate()` runs the same checks, then the CRC.

To load an image from the SD card with these checks and the fallback rule, use the [stage loader](./api/storage.md#stage-loader-loaderh).

## Boot info block (`bootinfo.h`)

`bootinfo.h` defines the 320-byte block one stage passes to the next at `0x00100000`. It carries the board, memory, UART, framebuffer and console state, the firmware registers and a copy of the firmware's ATAGS or DTB. See [the boot info spec](../spec/BOOT_PROTOCOL.md#boot-info).

| Constant | Value | Meaning |
|---|---|---|
| `ROBCO_BOOTINFO_ADDR` | `0x00100000u` | Where the block lives during a handoff |
| `ROBCO_BOOTINFO_MAGIC` | `0x524F4243u` | `magic` field |
| `ROBCO_BOOTINFO_VERSION` | `2u` | `version` field |
| `ROBCO_BOOTINFO_SIZE` | `320u` | `size` field and `sizeof(robco_bootinfo_t)` |
| `ROBCO_HANDOFF_FDT_ADDR` | `0x00101000u` | Where the ATAGS or DTB copy goes |
| `ROBCO_HANDOFF_FDT_MAX` | `0x0001F000u` | Largest copy |

| Group | Constants |
|---|---|
| `stage` ids | `ROBCO_STAGE_NONE` 0, `ROBCO_STAGE_BIOS` 1, `ROBCO_STAGE_BOOTAGENT` 2, `ROBCO_STAGE_UOS` 3, `ROBCO_STAGE_MAINT` 4, `ROBCO_STAGE_DIAG` 5, `ROBCO_STAGE_PIPOS` 6, `ROBCO_STAGE_DEITRIX` 7 |
| `caps` bits | `ROBCO_CAP_FB` bit 0, `ROBCO_CAP_PL011` bit 1, `ROBCO_CAP_MINIUART` bit 2, `ROBCO_CAP_SD` bit 3, `ROBCO_CAP_LOCAL_INTC` bit 4 |
| `boot_flags` bits | `ROBCO_BOOT_FALLBACK` bit 0, `ROBCO_BOOT_MAINT` bit 1, `ROBCO_BOOT_COLD` bit 2 |
| `uart_type` | `ROBCO_UART_NONE` 0, `ROBCO_UART_PL011` 1, `ROBCO_UART_MINI` 2 |
| `boot_dev` | `ROBCO_BOOT_DEV_UNKNOWN` 0, `ROBCO_BOOT_DEV_EMMC` 1, `ROBCO_BOOT_DEV_SDHOST` 2 |
| Firmware blob tags | `ROBCO_FDT_MAGIC` `0xD00DFEEDu`, `ROBCO_ATAG_CORE` `0x54410001u`, `ROBCO_ATAG_NONE` `0x00000000u` |
| `bootinfo_merge()` groups | `BOOTINFO_OWN_FW_REGS` bit 0, `BOOTINFO_OWN_FW_BLOB` bit 1, `BOOTINFO_OWN_UART` bit 2, `BOOTINFO_OWN_FB` bit 3, `BOOTINFO_OWN_CON` bit 4, `BOOTINFO_OWN_BOOT_DEV` bit 5, `BOOTINFO_OWN_MINIUART` bit 6, `BOOTINFO_OWN_SD` bit 7 |
| Validation results | `BOOTINFO_OK` 0, `BOOTINFO_E_MAGIC` -1, `BOOTINFO_E_VERSION` -2, `BOOTINFO_E_SIZE` -3, `BOOTINFO_E_CRC` -4 |

| Config macro | Default | Meaning |
|---|---|---|
| `ROBCO_FIRST_STAGE` | `0` | Set to 1 in a first stage. It then always cold-boots and ignores r3. |

`bootinfo_build()` sets every `caps` bit from the drivers this stage has running: `ROBCO_CAP_FB` after `fb_init()`, `ROBCO_CAP_PL011` after `uart_init()`, `ROBCO_CAP_MINIUART` after a successful `uart_mini_init()`, `ROBCO_CAP_SD` while `sd_blkdev()` returns a device, and `ROBCO_CAP_LOCAL_INTC` from the SoC. It holds only weak references to these drivers, so an image without them does not link them. It does not set `boot_dev` or `boot_path`: a loader that picks a boot device fills them.

`robco_bootinfo_t` is a packed struct of 320 bytes. The field list with offsets is in [BOOT_PROTOCOL.md](../spec/BOOT_PROTOCOL.md#boot-info) and in the header.

Trust rule: unless `ROBCO_FIRST_STAGE` is 1, `robco_start()` calls `bootinfo_receive()` before `robco_main()`. The block is trusted when r3 at entry equals `ROBCO_BOOTINFO_ADDR`, the block passes `bootinfo_validate()`, and its `midr` equals this CPU's MIDR. Otherwise the image cold-boots.

```c
void bootinfo_seal(robco_bootinfo_t *bi);
int bootinfo_validate(const robco_bootinfo_t *bi);
uint32_t bootinfo_fw_blob_size(const void *blob, uint32_t max);
```

Pure functions in `src/lib/bootinfo.c`. `bootinfo_seal()` sets magic, version and size and computes the CRC. Call it again after you change a field. `bootinfo_validate()` checks magic, version, size and CRC in that order and returns `BOOTINFO_OK` or a `BOOTINFO_E_*`. `bootinfo_fw_blob_size()` returns the size of the FDT or ATAGS list at `blob`, or `0` if there is none or it is larger than `max`.

```c
const robco_bootinfo_t *bootinfo_get(void);
int bootinfo_inherited(void);
```

`bootinfo_get()` returns the trusted block, the block the image built with `bootinfo_build()`, or NULL. `bootinfo_inherited()` returns 1 if the block from the previous stage passed the trust rule.

```c
void bootinfo_receive(void);
```

Applies the trust rule. `robco_start()` calls it. You do not need to.

```c
uint32_t bootinfo_copy_fw_blob(uint32_t dest, uint32_t max);
```

Copies the FDT or ATAGS found at the saved firmware r2 to `dest`. Returns the size, or `0` if there is none or it is larger than `max`. Call it in a first stage, before `bootinfo_build()`, with `ROBCO_HANDOFF_FDT_ADDR` and `ROBCO_HANDOFF_FDT_MAX`.

```c
void bootinfo_build(robco_bootinfo_t *bi, uint32_t stage_id);
```

Fills `bi` for the next stage and seals it, following the carry-over rule below. Cold, every field comes from this stage: the entry registers, the last `bootinfo_copy_fw_blob()`, `ROBCO_BOOT_COLD`, and the UART, framebuffer and console if this stage set them up, else `0`. After the call `bootinfo_get()` returns `bi`.

Carry-over rule: a field that describes state this stage set up itself comes from this stage. Any other field is carried over from the inherited block, because that state still holds after the handoff.

| Field group | `own_groups` bit | Comes from this stage when |
|---|---|---|
| `stage`, board fields, `ROBCO_CAP_LOCAL_INTC` | None | Always |
| `fw_r0` to `fw_r2` | `BOOTINFO_OWN_FW_REGS` | The stage is cold |
| `fdt_addr`, `fdt_size` | `BOOTINFO_OWN_FW_BLOB` | It called `bootinfo_copy_fw_blob()` |
| `uart_*`, `ROBCO_CAP_PL011` | `BOOTINFO_OWN_UART` | It configured the PL011 |
| `ROBCO_CAP_MINIUART` | `BOOTINFO_OWN_MINIUART` | It initialised the mini UART |
| `ROBCO_CAP_SD` | `BOOTINFO_OWN_SD` | It initialised the SD card |
| `fb_*`, `ROBCO_CAP_FB` | `BOOTINFO_OWN_FB` | It called `fb_init()` |
| `con_*` | `BOOTINFO_OWN_CON` | Its console is active |
| `boot_dev`, `boot_part_lba`, `boot_flags`, `boot_path`, `cmdline` | `BOOTINFO_OWN_BOOT_DEV` | The stage is cold |
| Other `caps` bits | None | Never. They are reserved and 0. |

A new framebuffer without a new console clears `con_cols`, `con_rows`, `con_cx` and `con_cy`, since the old console drew on the old framebuffer. `con_scheme` stays. A loader that picks a boot device writes the `boot_*` fields after `bootinfo_build()` and calls `bootinfo_seal()`.

```c
void bootinfo_merge(robco_bootinfo_t *out, const robco_bootinfo_t *prev,
                    const robco_bootinfo_t *own, uint32_t own_groups);
```

Pure function in `src/lib/bootinfo.c`, used by `bootinfo_build()` and tested on the host. Writes to `out` the merge of `prev`, the inherited block, and `own`, the fields this stage filled. `own_groups` holds the `BOOTINFO_OWN_*` bits of the groups this stage set up. With `prev` NULL, `out` is a copy of `own`. `out` may equal `prev` or `own`. It does not seal.

From `examples/chain2/main.c`, a stage that also runs alone:

```c
static robco_bootinfo_t local;

if (!bootinfo_inherited()) {
    bootinfo_copy_fw_blob(ROBCO_HANDOFF_FDT_ADDR, ROBCO_HANDOFF_FDT_MAX);
    bootinfo_build(&local, ROBCO_STAGE_BOOTAGENT);
}
bi = bootinfo_get();
```

## Exceptions (`exc.h`)

`exc.h` reports CPU exceptions. The default vector stubs print one `@RC ... ev=exception` line and halt, so a crash shows up in the test log instead of as silence.

| Config macro | Default | Meaning |
|---|---|---|
| `ROBCO_STAGE_NAME` | `"robco"` | `stage=` name in every marker the library prints |

| Constant | Value | `type=` in the marker |
|---|---|---|
| `ROBCO_EXC_RESET` | `0` | `reset` |
| `ROBCO_EXC_UNDEF` | `1` | `undef` |
| `ROBCO_EXC_SVC` | `2` | `svc` |
| `ROBCO_EXC_PABT` | `3` | `prefetch_abort` |
| `ROBCO_EXC_DABT` | `4` | `data_abort` |
| `ROBCO_EXC_UNUSED` | `5` | `unused` |
| `ROBCO_EXC_IRQ` | `6` | `irq` |
| `ROBCO_EXC_FIQ` | `7` | `fiq` |

```c
void robco_exc_report(uint32_t type, uint32_t pc, uint32_t lr, uint32_t spsr)
    __attribute__((noreturn));
```

Prints `@RC stage=<name> ev=exception type=<type> pc=0x... lr=0x... spsr=0x... dfsr=0x... dfar=0x... ifsr=0x...` through `k_printf()` and halts. `ifsr` is the prefetch abort status. `lr` is the banked LR of the interrupted mode. If the UART is not initialised yet, it calls `uart_init(115200)` first, so a fault before `uart_init()` still prints. On an unknown board that does nothing and the report is silent.

To handle SVC or FIQ yourself, define a strong `robco_vec_svc` or `robco_vec_fiq` in assembly. The library versions are weak. [`umode.h`](./api/umode.md) provides a strong `robco_vec_svc` and `robco_vec_irq` when the image links it. To handle undefined instructions and aborts, use the hook below.

### Exception frames and the fault hook

| Constant | Value | Meaning |
|---|---|---|
| `EXC_NOT_HANDLED` | `0` | Hook result: run the fatal path |
| `EXC_RESUME` | `1` | Hook result: restore every register from the frame |
| `EXC_FRAME_SIZE` | `80` | `sizeof(exc_frame_t)` |
| `EXC_FRAME_SP`, `EXC_FRAME_LR`, `EXC_FRAME_PC`, `EXC_FRAME_CPSR`, `EXC_FRAME_FSR`, `EXC_FRAME_FAR` | `52` to `72` | Field offsets for assembly |

```c
typedef struct exc_frame {
    uint32_t r[13];     /* r0-r12 */
    uint32_t sp, lr;    /* banked sp and lr of the mode in cpsr */
    uint32_t pc;        /* faulting instruction; after the svc for a system call */
    uint32_t cpsr;      /* cpsr of the interrupted context */
    uint32_t fsr, far;  /* DFSR/DFAR (data abort), IFSR/IFAR (prefetch abort), else 0 */
    uint32_t pad;
} exc_frame_t;
typedef int (*exc_hook_t)(unsigned type, exc_frame_t *frame);
```

```c
exc_hook_t exc_set_hook(exc_hook_t hook);
```

Installs the hook (`0` removes it) and returns the previous one. Linking this function replaces the default undefined instruction, prefetch abort and data abort stubs with entries that save a full `exc_frame_t` on the ABT or UND stack and call the hook with IRQs masked. The frame `pc` is the faulting instruction in ARM and Thumb state, for 16-bit and 32-bit Thumb instructions alike: in Thumb state the CPU sets `lr_und` to the instruction address plus 2 whatever its length, and aborts use the same offsets in both states. The hook returns `EXC_RESUME` to restore every register, the banked `sp` and `lr` of the target mode and `cpsr` from the possibly changed frame, then return to `frame->pc`. It can skip an instruction, change the return address or switch the saved mode. Only USR, SYS, SVC and IRQ can be resumed. Any other mode, or `EXC_NOT_HANDLED`, or no hook, runs `robco_exc_report()` with the same one-line format as before. `mmio_probe()` keeps working and takes precedence: the hook never sees a probe fault. Faults in FIQ mode report wrong `r8` to `r12`, and a fault in ABT or UND mode itself reports that mode's `sp` and `lr` as the entry's own. Both stay fatal.

```c
void exc_print_frame(const exc_frame_t *frame);
```

Prints `@RC stage=<name> ev=frame r0=0x... ... r12=0x... sp=0x... lr=0x... pc=0x... cpsr=0x... fsr=0x... far=0x...`. Call it from a hook before returning `EXC_NOT_HANDLED` to log the full frame.

```c
void robco_exc_dispatch(unsigned type, exc_frame_t *frame);
```

Called by the frame-saving entries. Do not call it. It returns only when the hook resumes a valid mode.

The `hello` fault variant (`examples/hello/VARIANTS`) runs an undefined instruction. On `raspi0` it prints:

```
@RC stage=hello ev=exception type=undef pc=0x00008394 lr=0x00008394 spsr=0x600001d3 dfsr=0x00000008 dfar=0x2020c000 ifsr=0x00000000
```

`dfsr` and `dfar` keep the values of the last data abort, here the PWM probe. They describe the current exception only for `data_abort`. `ifsr` describes it only for `prefetch_abort`.

## Registers and probing (`mmio.h`)

`mmio.h` holds the register access helpers every driver uses: 32-bit reads and writes, barriers, a crude delay, and a read that survives a data abort.

```c
static inline void mmio_write(uint32_t addr, uint32_t value);
static inline uint32_t mmio_read(uint32_t addr);
```

Volatile 32-bit write and read at a physical address.

```c
static inline void mmio_dmb(void);
static inline void mmio_dsb(void);
static inline void mmio_isb(void);
```

Data memory barrier, data synchronisation barrier and instruction barrier, in the ARMv6 CP15 form. They work on ARMv7 and ARMv8 too because `start.S` sets SCTLR.CP15BEN. Put a `mmio_dmb()` between accesses to two different peripherals.

```c
static inline void mmio_delay(uint32_t count);
```

Busy-waits about `count` loop iterations. The time depends on the CPU clock. Use `timer_delay_us()` for a real delay.

```c
int mmio_probe(uint32_t addr, uint32_t *value);
```

Reads a 32-bit register that may not exist. Returns `0` and stores the value, or non-zero if the read took a data abort. Use it for blocks that QEMU does not model, such as PWM on `raspi0`. It masks IRQs on the CPU for the whole probe and restores the mask after. With the MMU on, a probe of an unmapped address reports an abort.

Sharp edges of `mmio_probe()`:

| Edge | Detail |
|---|---|
| Synchronous aborts only. | An asynchronous external abort arrives later and is reported as a normal exception. |
| Only that one load. | The data abort stub compares the faulting PC with the probe instruction. Any other abort still halts. |
| `0` does not prove the block exists. | It proves only that the read did not abort. Unmapped addresses on real hardware may return a value without an abort. |
| Not reentrant. | One global flag records the fault. Because IRQs stay masked during a probe, an IRQ handler can probe safely: it never runs inside a probe of the main flow. Do not probe from an abort handler. |

From `examples/hello/main.c`:

```c
/* PWM aborts on QEMU raspi0/1ap; GPFSEL0 exists everywhere. */
pwm = mmio_probe(b->periph_base + 0x20c000, &value);
gpio = mmio_probe(b->periph_base + 0x200000, &value);
k_printf("@RC stage=hello ev=probe pwm=%s gpio=%s\n",
         pwm ? "abort" : "ok", gpio ? "abort" : "ok");
```

## Linker symbols

`ld/image.ld.S` exports these symbols. Declare them as `extern char name[];`.

| Symbol | Meaning |
|---|---|
| `__image_start` | Link address, `LINK_ADDR` |
| `__image_end` | End of the loaded bytes |
| `__bss_start`, `__bss_end` | BSS range, cleared by `start.S` |
| `__heap_start` | First free byte after the stacks, 16-byte aligned |
| `__slot_end` | `ROBCO_SLOT_END`, or `0` when it is not set |

| Config macro | Default | Meaning |
|---|---|---|
| `ROBCO_SLOT_END` | Not set | First byte past the image's slot. When set, the link fails with `image and stacks do not fit below ROBCO_SLOT_END` if `__image_mem_end` is above it. `SLOT_END=<addr>` on the make command line sets it too and wins over a value in `robco_config.h` guarded by `#ifndef`. |

| Config macro | Default | Stack |
|---|---|---|
| `ROBCO_STACK_SVC_SIZE` | `0x4000` | SVC, where `robco_main()` runs |
| `ROBCO_STACK_IRQ_SIZE` | `0x1000` | IRQ handlers and tick callbacks |
| `ROBCO_STACK_FIQ_SIZE` | `0x1000` | FIQ |
| `ROBCO_STACK_ABT_SIZE` | `0x1000` | Abort handlers |
| `ROBCO_STACK_UND_SIZE` | `0x1000` | Undefined instruction handler |

Nothing detects a stack overflow.

## Configuration macros

Each image sets these in its `robco_config.h`. A macro you do not set takes its default.

| Macro | Default | Section |
|---|---|---|
| `ROBCO_STAGE_NAME` | `"robco"` | [Exceptions](#exceptions-exch) |
| `ROBCO_FIRST_STAGE` | `0` | [Boot info block](#boot-info-block-bootinfoh) |
| `ROBCO_IMG_MAGIC`, `ROBCO_IMG_SUBTYPE`, `ROBCO_IMG_ABI_VERSION`, `ROBCO_IMG_VERSION`, `ROBCO_IMG_NAME`, `ROBCO_IMG_FLAGS` | See the table | [Image header](#image-header-rimgh) |
| `ROBCO_SLOT_END` | Not set | [Linker symbols](#linker-symbols) |
| `ROBCO_STACK_SVC_SIZE` | `0x4000` | [Linker symbols](#linker-symbols) |
| `ROBCO_STACK_IRQ_SIZE`, `ROBCO_STACK_FIQ_SIZE`, `ROBCO_STACK_ABT_SIZE`, `ROBCO_STACK_UND_SIZE` | `0x1000` | [Linker symbols](#linker-symbols) |
| `ROBCO_CONSOLE_MAX_COLS` | `100` | [Screen console](#screen-console-consoleh) |
| `ROBCO_CONSOLE_MAX_ROWS` | `40` | [Screen console](#screen-console-consoleh) |
| `ROBCO_FONT` | `ROBCO_FONT_FIXEDSYS` | [Console font](#console-font-fonth) |
| `ROBCO_FS_READONLY` | `0` | [FAT file system](./api/storage.md#fat-file-system-fsh) |
| `ROBCO_AUDIO_BACKEND` | `AUDIO_BACKEND_AUTO` | [Audio](./api/audio.md#audio-audioh) |
| `ROBCO_AUDIO_PIN0`, `ROBCO_AUDIO_FN0` | Per board | [Audio](./api/audio.md#audio-audioh) |
| `ROBCO_AUDIO_PIN1`, `ROBCO_AUDIO_FN1` | Not set, mono | [Audio](./api/audio.md#audio-audioh) |
| `ROBCO_AUDIO_CLOCK_SRC` | `AUDIO_CLK_SRC_OSC` | [Audio](./api/audio.md#audio-audioh) |
| `ROBCO_AUDIO_VOLUME` | `100` | [Audio](./api/audio.md#audio-audioh) |
| `ROBCO_AUDIO_BEEP_HZ`, `ROBCO_AUDIO_BEEP_MS` | `1000`, `80` | [Audio](./api/audio.md#audio-audioh) |
| `ROBCO_AUDIO_TUNE_MAX` | `64` | [Audio](./api/audio.md#audio-audioh) |
| `ROBCO_AUDIO_TRACE_QUEUE` | `64` | [Audio](./api/audio.md#audio-audioh) |

The MMU, cache, input, I2C, RTC, SPI, date and INI code read no macro.
