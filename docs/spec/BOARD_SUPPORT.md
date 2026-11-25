# Board support

RobCo-Core builds one AArch32 image per stage. The same image runs on every supported board. It detects the board at runtime. There is no compile-time board selection and no `BCM=` variable.

## Supported boards

| Board | SoC | CPU | Status |
|---|---|---|---|
| Pi Zero, Zero W, Pi 1 A+/B+ | BCM2835 | ARM1176JZF-S | Supported. Hardware checklist. Boot, detection, mailbox and PL011 validated on a Zero W 1.1 on 2026-11-24, see [the Zero W result](../HARDWARE_VALIDATION.md#zero-w-11-exampleshello-2026-11-24). |
| Pi 2 B v1.1 | BCM2836 | Cortex-A7 | Supported in QEMU only. |
| Pi 2 B v1.2, Pi 3 B/B+, Zero 2 W | BCM2837 | Cortex-A53 | Supported. Hardware checklist. |
| Pi 4 B | BCM2711 | Cortex-A72 | Not supported. Table entry only. |

All code runs in AArch32. The build targets ARMv6 with `-mcpu=arm1176jzf-s -marm -mfloat-abi=soft -mgeneral-regs-only -mno-unaligned-access`. The same code runs on ARMv7 and ARMv8 in AArch32 state.

## Detection table

`board_get()` in `include/robco/board.h` detects the board on its first call and caches the result. It reads MIDR with `mrc p15, 0, rX, c0, c0, 0`. The part number is MIDR bits [15:4]. `robco_board_t` holds the SoC as a `robco_soc_t` value and as a name such as `bcm2835`. The `soc` column is the number the boot info stores.

| MIDR part | SoC | `soc` value | Peripheral base | Interrupt controller | VC bus alias |
|---|---|---|---|---|---|
| `0xB76` | BCM2835 | `0x2835` | `0x20000000` | bcm2835-ic at base + `0xB200` | `0x40000000`, pending hardware validation |
| `0xC07` | BCM2836 | `0x2836` | `0x3F000000` | bcm2835-ic, plus local controller at `0x40000000` | `0xC0000000`, pending hardware validation |
| `0xD03` | BCM2837 | `0x2837` | `0x3F000000` | bcm2835-ic, plus local controller at `0x40000000` | `0xC0000000`, pending hardware validation |
| `0xD08` | BCM2711 | `0x2711` | `0xFE000000` | GIC-400 | Not used |
| other | unknown | `0` | none | none | none |

`robco_start()` calls `robco_main()` only on BCM2835, BCM2836 and BCM2837. On BCM2711 it brings up the PL011 at base `0xFE000000`, prints `@RC stage=<name> ev=error code=unsupported_board midr=0x...` and halts. On an unknown part it halts in a `wfi` loop without output, because it does not know where the UART is. No project code runs on either board.

The VC bus alias is the value the VideoCore adds to ARM physical addresses. Drivers OR it into addresses they give to the mailbox. They clear it from addresses they get back, such as the framebuffer address.

Peripheral offsets used by RobCo-Core, relative to the base:

| Offset | Block |
|---|---|
| `0x00003000` | System timer |
| `0x0000B200` | Interrupt controller |
| `0x0000B880` | Mailbox |
| `0x00100000` | Power management and watchdog |
| `0x00200000` | GPIO |
| `0x00101000` | Clock manager, PWM clock |
| `0x00201000` | PL011 UART |
| `0x00204000` | SPI0 |
| `0x0020C000` | PWM |
| `0x00215000` | AUX, mini UART |
| `0x00300000` | EMMC |
| `0x00804000` | BSC1, I2C |

## Board revision

The exact board comes from the property mailbox, channel 8, tag `0x00010002` "get board revision". RobCo-Core stores the raw value in `robco_bootinfo_t.board_rev`. `board_model_name()` decodes new-style revision codes, where bit 23 is set, for the board name. Old-style codes map through a lookup table. Bit 24, the warranty bit of old-style codes, is ignored. An unknown code gives `Raspberry Pi (unknown model)`. A Zero W 1.1 reported `0x009000c1` on hardware, the code the host tests map to `Raspberry Pi Zero W`. Code never uses the revision to pick a peripheral base. MIDR decides that.

Other mailbox tags used at boot:

| Tag | Use |
|---|---|
| `0x00010002` | Board revision |
| `0x00010005` | ARM memory base and size |
| `0x00030002` | Clock rate. Clock id 2 is the UART clock. Clock id 4 is the core clock. |

## UART policy

The PL011 is the console on every board.

| Setting | Value |
|---|---|
| Port | PL011 at base + `0x201000` |
| Pins | GPIO14 TXD, GPIO15 RXD, function ALT0 |
| Format | 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control |
| FIFO | Enabled |
| Clock | From mailbox tag `0x00030002`, clock id 2 |

The driver muxes GPIO14/15 itself. It does not rely on the firmware. On BCM2835 to BCM2837 it also clears the pulls with the GPPUD and GPPUDCLK0 sequence. The waits in that sequence count loop iterations and do not read the system timer, so the console comes up even when the timer is stopped.

The driver computes the divisor from the mailbox clock. It never hardcodes a clock. QEMU reports 3 MHz. Current firmware reports 48 MHz.

```c
div64 = (clock_hz * 4 + baud / 2) / baud;  /* clock / (16 * baud), in 1/64 units, rounded */
IBRD  = div64 >> 6;
FBRD  = div64 & 0x3F;
```

| Clock | IBRD | FBRD |
|---|---|---|
| 48 000 000 Hz | 26 | 3 |
| 3 000 000 Hz | 1 | 40 |

If the mailbox clock query fails, the driver uses 48 MHz.

On Pi 3 and Zero 2 W, Bluetooth normally owns the PL011. `uart_init()` does not depend on the firmware for this. It muxes GPIO14/15 to the PL011 (ALT0) itself and takes the UART clock from the mailbox. Without a DTB the firmware applies no overlay, so nothing else changes the pins. Whether this is enough on these boards is pending hardware validation. On a Zero W 1.1, which also has Bluetooth, the PL011 sent and received at 115200 with no `dtoverlay` on 2026-11-24, see [the Zero W result](../HARDWARE_VALIDATION.md#zero-w-11-exampleshello-2026-11-24).

`dtoverlay=disable-bt` is not a fallback under this policy. The card holds no `.dtb` file and no `overlays/` folder (see [below](#no-dtb-on-the-card)), and without them the firmware ignores `dtoverlay=`. If the PL011 stays silent on a board, the options are:

- Use the mini UART as the console on that board. Its divisor depends on the core clock, which the firmware can change, so this needs a fixed core clock in `config.txt`.
- Put the board's `.dtb` and the `disable-bt` overlay on the card and add `dtoverlay=disable-bt`. The firmware then passes a DTB in r2 instead of ATAGS. The library accepts both: `bootinfo_copy_fw_blob()` copies either one. This breaks the "no .dtb" rule, so the result goes into this file first.

The mini UART is the second channel. Stages use it as the maintenance and test port. Its baud divisor comes from the core clock, clock id 4, which the firmware can change. That is why it is not the console. On hardware the mini UART pins on the 40-pin header are GPIO14/15, which the PL011 already uses. So on hardware the mini UART is not routed to any pin, and the maintenance port exists only in QEMU. The input script backend uses it there. In QEMU the second `-serial` option is the mini UART.

## Pins

The library muxes these pins itself when you call the driver. It never touches a pin you do not use.

| Pins | Function | Set by |
|---|---|---|
| GPIO 14, 15 | PL011 TXD, RXD at ALT0 | `uart_init()` |
| GPIO 2, 3 | I2C SDA, SCL at ALT0. The board has fixed 1.8 kOhm pull-ups. | `i2c_init()` |
| GPIO 7, 8, 9, 10, 11 | SPI0 CE1, CE0, MISO, MOSI, SCLK at ALT0 | `spi_init()` |
| GPIO 34 to 39, 48 to 53 | SD card routing, see below | `sd_init()` |
| Audio pins, per board below | PWM | `audio_init()` with the PWM backend |

| Board | Audio channel 1 | Audio channel 2 |
|---|---|---|
| Pi Zero, Zero W, Zero 2 W | GPIO 18 at ALT5 | None, mono |
| Pi 1, Pi 2 | GPIO 40 at ALT0 | GPIO 45 at ALT0 |
| Pi 3 | GPIO 40 at ALT0 | GPIO 41 at ALT0 |

The Zero is told apart from the board revision: a new-style code with type `0x09`, `0x0c` or `0x12`. `ROBCO_AUDIO_PIN0` and `audio_set_pins()` override the table.

`uart_mini_init()` muxes no pin. The mini UART can only use GPIO 14 and 15 at ALT5 on the header, which the PL011 already holds.

`examples/input/robco_config.h` uses this mapping. It is an example, not a library default. Each button ties its pin to ground and uses the internal pull-up.

| Input | Pin | Input | Pin |
|---|---|---|---|
| UP | GPIO 5 | BACK | GPIO 27 |
| DOWN | GPIO 6 | Dial A | GPIO 23 |
| LEFT | GPIO 17 | Dial B | GPIO 24 |
| RIGHT | GPIO 22 | Dial push | GPIO 16 |
| SELECT | GPIO 26 | | |

It avoids the pins above, GPIO 0 and 1 (ID EEPROM), GPIO 12 and 13 and GPIO 18 to 21.

### SPI display

`TFT_CONFIG_DEFAULT` in `tft.h` wires an ILI9341 panel this way. It is a default that any `tft_config_t` can change. It is not validated on hardware. The default SPI clock is 10 MHz, the fastest write clock in the ILI9341 data sheet. The divider rounds it down to 9.62 MHz with a 250 MHz core clock. Faster rates up to 64 MHz are accepted but are outside the data sheet and not validated.

| Panel pin | Pin |
|---|---|
| SCK, SDI, SDO | GPIO 11, 10, 9: SPI0 at ALT0 |
| CS | GPIO 8, CE0 |
| D/C | GPIO 25 |
| RESET | GPIO 4 |
| LED | 3.3 V. No backlight GPIO by default. |

GPIO 24, the usual reset pin in panel guides, is Dial B in the input mapping above. GPIO 4 and 25 are the only header pins that the tables above leave free, so they take DC and reset. CE1, GPIO 7, stays free for a second SPI device.

### I2C GPIO expanders

`gpiox.h` drives PCA9554 chips at `0x20` to `0x27` and PCA9554A chips at `0x38` to `0x3F`, up to four at once. They share the I2C bus on GPIO 2 and 3 with the clock chip. Each adds eight pins. The driver is tested only against QEMU's model, not on hardware.

### SD card pin routing

On boards with WiFi (Pi 3, Zero W, Zero 2 W) the firmware routes the card to the SDHOST controller on GPIO 48 to 53 and the EMMC controller to the WiFi chip on GPIO 34 to 39. `sd_init()` drives the card through EMMC. It sets GPIO 34 to 39 to input when any of them is at ALT3, and GPIO 48 to 53 to ALT3 when any of them is not. WiFi stops working after this. On a Zero W 1.1, `sd_init()` brought up an SDHC card through EMMC and read it on 2026-11-24, see [the sdraw result](../HARDWARE_VALIDATION.md#zero-w-11-examplessdraw-2026-11-24). So that board needs no SDHOST driver to read the card. The pin states before and after were not captured. The routing is pending hardware validation on the Pi 3 and the Zero 2 W.

## config.txt

Each chain ships one `config.txt` at the root of the FAT boot partition. Only the `kernel=` line differs.

Terminal chain:

```
kernel=RETROS.IMG
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

Pip-Boy chain:

```
kernel=PIPOS.IMG
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

`arm_64bit=0` makes the firmware load the image at `0x8000` in AArch32 state. The framebuffer lines set the default mode. Stages still ask for 640x480x32 through the mailbox.

`tools/mkbootdir.sh <image.img> <target-dir> [--kernel-name NAME]` writes a boot directory with these settings, the image and the firmware files from `tools/fetch_firmware.sh`. It first verifies the image with `tools/rimg.py` and fails if the image is invalid, if it is not linked at 0x8000, or if `python3` (or `PYTHON`) is missing. Copy its files to the card's boot partition.

## No .dtb on the card

The card holds no `.dtb` file and no `overlays/` folder.

Without a DTB the firmware passes an ATAGS list in r2, at `0x100`. It applies no overlays and patches nothing. The handoff is the same on every board and matches what QEMU gives. We do not parse device trees, so a DTB would only add firmware behaviour we cannot test.

## Pi 4

BCM2711 has a row in the detection table so that the code does not design it out. Its peripheral base and GIC differ, and it needs its own firmware files. A stage that detects part `0xD08` prints the `unsupported_board` error marker and halts. Nothing else supports BCM2711.

## Startup sequence

Every image starts in `src/arch/start.S`. The order is fixed.

| Step | Action | Reason |
|---|---|---|
| 1 | Save r0 to r3 in r4 to r7 and the entry mode, CPSR bits [4:0], in r8. | They carry the firmware arguments or the handoff. Later steps clobber r0 to r3. |
| 2 | Read MIDR. If the part is not `0xB76`, read MPIDR and park every core whose bits [1:0] are not 0. | Multi-core parts may start secondaries at the entry point. See the ARM1176 trap below. |
| 3 | If the entry mode is HYP, clear HCR, HCPTR and HSTR, then drop to SVC with `msr SPSR_hyp`, `msr ELR_hyp`, `eret`. Then force SVC with IRQ and FIQ masked, with `cpsid if`. | Pi 2 and Pi 3 firmware enters in HYP. HYP has its own banked registers and exception model. `cps` cannot leave HYP, so `eret` is the only way out. The HYP trap registers reset to unknown values, and a set HCPTR.TCP10 would make step 7 fault. |
| 4 | If the part is not `0xB76`, set SCTLR.CP15BEN, bit 5. | ARMv7 and later need it for the CP15 barrier forms that `mmio.h` uses. Its reset value is not guaranteed on ARMv8 AArch32. ARM1176 has no such bit. |
| 5 | Set sp for IRQ, FIQ, ABT, UND and SVC from the image's own `.stack` section. End in SVC. | Exceptions need a valid stack before VBAR points at handlers. Each image owns its stacks, so no stage depends on another stage's memory. |
| 6 | Clear SCTLR.V, then write VBAR. | Low vectors plus VBAR put the vector table inside the image. |
| 7 | Enable cp10 and cp11 in CPACR, flush the prefetch buffer, then set FPEXC.EN. | The current flags emit no VFP code. Enabling access costs nothing and avoids an undefined-instruction trap if a later component uses VFP. |
| 8 | Zero BSS from `__bss_start` to `__bss_end`. Then store the saved r0 to r3 and entry mode in `robco_boot_args_store`. | C code expects zeroed statics. A loader does not zero BSS for the image. The store sits in `.data`, so the BSS clear cannot erase it. |
| 9 | Call `robco_start()`. It detects the board with `board_get()`, then calls `robco_main()` on a supported SoC. On any other SoC it halts, as the detection table describes. | One C entry point per image. See [BOOT_PROTOCOL.md](./BOOT_PROTOCOL.md). |
| 10 | If `robco_main` returns, mask IRQ and FIQ and park in a `wfi` loop. | There is nowhere to return to. |

### ARM1176 MPIDR trap

ARM1176 has no MPIDR. Reading `c0, c0, 5` on it returns MIDR, `0x410FB767` on QEMU `raspi0`. Its bits [1:0] are `3`. Code that parks "every core whose MPIDR is not 0" parks the only core on a Pi Zero or Pi 1. So step 2 checks the MIDR part first and skips the MPIDR read on ARM1176. On QEMU `raspi2b` the same read returns `0x80000F00`, a real MPIDR with bit 31 set.

### Probing registers

`mmio_probe(addr)` reads a register and returns an error instead of crashing if the read data-aborts. Code uses it for blocks that QEMU may not model. Reading PWM on QEMU `raspi0` data-aborts, for example.
