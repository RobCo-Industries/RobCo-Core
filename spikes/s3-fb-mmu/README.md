# Spike 3: framebuffer and MMU fill rate

One 32-bit image for every Pi from the Zero to the 3. It asks the firmware for a 640x480, 32-bit framebuffer and draws a test pattern. Then it times 20 full-screen fills with the MMU and caches off, enables them, and times 20 fills again. It ends on the test pattern and echoes what you type on the UART.

Shared code lives in `../common/` (`fb.c`, `mmu.c`, `mmu_ops.S`, `systimer.c` are new for this spike).

## Build and test in QEMU

```sh
make                      # build/s3.elf and build/s3.img
./run-qemu.sh raspi0      # also raspi1ap, raspi2b, raspi3b
make -C ../.. venv        # once, for the screen check
../../.venv/bin/python check_screen.py raspi0 raspi2b
```

`run-qemu.sh` fails if the `ev=fb`, `ev=fill` or `ev=done` lines are missing, or on an exception. `check_screen.py` takes a screendump and checks the bar and corner colours. QEMU does not model cache timing, so its fill times mean nothing.

## SD card

Same layout as spike 1 (see `../s1-uart-hello/README.md`), with `s3.img` from `build/s3.img`.

`config.txt`:

```
kernel=s3.img
arm_64bit=0
enable_uart=1
disable_splash=1
boot_delay=0
hdmi_force_hotplug=1
disable_overscan=1
```

Connect HDMI and the UART adapter as in spike 1.

## What the screen should show

Eight vertical bars of equal width, left to right:

white, yellow, cyan, green, magenta, red, blue, black.

A 1-pixel white line runs around the outer edge of the picture. There are three 16x16 squares: red in the top-left corner, green in the top-right, blue in the bottom-left. The bottom-right corner has no square.

If red and blue are swapped, the bars read white, cyan, yellow, green, magenta, blue, red, black and the top-left square is blue.

## Expected UART output

Values differ per board. On QEMU raspi0:

```
@RC stage=spike3 ev=hello midr=0x410fb767 soc=bcm2835 base=0x20000000
@RC stage=spike3 ev=fb raw_addr=0x1c100000 addr=0x1c100000 size=1228800 pitch=2560 w=640 h=480 depth=32 order=rgb mbox_alias=0x40000000
@RC stage=spike3 ev=fill caches=off frames=20 us=11135
@RC stage=spike3 ev=mmu sctlr=0x0085187d smp_reg=none smp_before=0 smp_write=none smp_after=0
@RC stage=spike3 ev=fill caches=on frames=20 us=13103 sctlr=0x0085187d actlr_smp=0
@RC stage=spike3 ev=done
type to echo
```

On a real board, `raw_addr` likely has a bus alias in its top bits (for example `0x4...` or `0xc...`). `addr` is the same address with those bits cleared.

The `ev=mmu` line shows the SMP bit on the Pi 2 and 3: `smp_before` is its value at boot, `smp_write` is `skipped` (already set), `ok`, `ignored` (write had no effect) or `trapped`.

If the CPU faults, the image prints a line with `ev=exception` and halts. If the mailbox call fails, it prints `ev=fb_error` and halts.

## What to report back

For each board (a Pi Zero or 1, and a Pi 3 or Zero 2 W):

1. The board name.
2. The `ev=fb`, `ev=mmu` and both `ev=fill` lines.
3. The bar order you see, left to right, and the colour of each corner square.
4. Whether the white border is visible on all four sides.
5. Any `ev=exception` or `ev=fb_error` line, or if the output stops before `ev=done`.
6. A photo of the screen if anything looks wrong.
