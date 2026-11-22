# Spike 1: UART hello

One 32-bit image for every Pi from the Zero to the 3. It detects the board from MIDR at runtime and prints a line on the PL011 UART at 115200 8N1. Then it echoes what you type.

Shared startup code lives in `../common/`.

## Build and test in QEMU

```sh
make                      # build/s1.elf and build/s1.img
./run-qemu.sh raspi0      # also raspi1ap, raspi2b, raspi3b
```

`run-qemu.sh` exits non-zero if the `@RC` line is missing or shows the wrong SoC.

## SD card

Build the boot directory, then copy its files to the root of a FAT32 partition:

```sh
../../tools/mkbootdir.sh build/s1.img sdboot --kernel-name s1.img
```

`tools/mkbootdir.sh` fetches `bootcode.bin`, `start.elf` and `fixup.dat` with `tools/fetch_firmware.sh`, which pins one firmware release and checks each file's SHA-256. It adds the image and a `config.txt` with the settings from [BOARD_SUPPORT.md](../../docs/spec/BOARD_SUPPORT.md#configtxt). It never writes to a device.

Do not put any `.dtb` files or an `overlays/` folder on the card. Without a DTB the firmware passes ATAGS in r2 and applies no overlays. We want the plain firmware handoff.

## Wiring

Use a 3.3 V USB-serial adapter. Do not use a 5 V one.

| Pi header pin | Signal       | Adapter |
|---------------|--------------|---------|
| 6             | GND          | GND     |
| 8             | GPIO14 (TXD) | RX      |
| 10            | GPIO15 (RXD) | TX      |

Open the adapter at 115200 8N1, no flow control. For example `screen /dev/tty.usbserial-XXXX 115200`.

On the Pi 3 and Zero 2 W, the PL011 is normally used by Bluetooth. The spike muxes GPIO14/15 to the PL011 itself and takes the UART clock from the mailbox. This is pending hardware validation. If you see nothing on these boards, report it. Do not add `dtoverlay=disable-bt` alone: without a `.dtb` and the `overlays/` folder on the card the firmware ignores it. The fallbacks are in [the UART policy](../../docs/spec/BOARD_SUPPORT.md#uart-policy).

## Expected output

On a Pi Zero. Values differ per board.

```
@RC stage=spike1 ev=hello midr=0x410fb767 soc=bcm2835 base=0x20000000 board_rev=0x00900093 mem=0x1c000000 uart_clk=48000000 entry_mode=svc fw_r0=0x0 fw_r1=0xc42 fw_r2=0x100
RobCo spike 1: UART hello
arm memory: base 0x00000000 size 448 MiB
uart clock: 48000000 Hz (mailbox)
type to echo
```

On a Pi 3 or Zero 2 W, expect `soc=bcm2837 base=0x3f000000` and `entry_mode=hyp`.

If the CPU faults, the image prints a line with `ev=exception`, the exception name, the PC and DFSR/DFAR, then halts.

## What to report back

For each board:

1. The board name and the full `@RC` line.
2. Whether the first characters are garbled.
3. Whether typing echoes back.
4. Any `ev=exception` line.
5. On a Pi 3 or Zero 2 W, whether the PL011 printed anything.
