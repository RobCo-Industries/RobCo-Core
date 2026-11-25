# Spike 2: SD sector 0 through EMMC

One 32-bit image for every Pi from the Zero to the 3. It reads sector 0 of the SD card through the EMMC (Arasan SDHCI) controller and prints what it found on the UART.

The question: does one EMMC driver reach the SD card on every board, or do the Pi 3, Zero W and Zero 2 W need a separate SDHOST driver?

The spike only reads. It never writes to the SD card.

Shared code lives in `../common/` (`emmc.c`, `gpio.c`, `timer.c`, `crc32.c` are new for this spike).

## What it does

1. Prints the `ev=hello` line.
2. Prints the GPIO function select of pins 34-39 and 48-53 as the firmware left them.
3. Moves the SD card to the EMMC controller: GPIO 34-39 to input, GPIO 48-53 to ALT3. It skips this if 48-53 are already ALT3.
4. Resets the EMMC controller, identifies the card at 400 kHz, switches to 25 MHz and reads sector 0.
5. Prints the card, the read result and a hex dump of the 512 bytes. Then it echoes what you type.

Every wait has a timeout. On failure it prints `ev=error` and goes to the echo loop.

## Build and test in QEMU

```sh
make                         # build/s2.elf and build/s2.img
./run-qemu.sh raspi0         # also raspi1ap, raspi2b, raspi3b
./run-qemu.sh raspi3b --sdhc # 4 GiB sparse image, SDHC card
```

The script writes a test image with a known sector 0. It fails unless the `ev=read` line has `ok=1`, `sig=0xaa55` and the expected CRC-32.

## SD card

Same layout as spike 1 (see `../s1-uart-hello/README.md`):

```sh
make -C ../../examples/hello
../../tools/mkbootdir.sh ../../examples/hello/build/hello.img sdboot --kernel-name s2.img
cp build/s2.img sdboot/s2.img
```

`tools/mkbootdir.sh` rejects the unsealed spike image, so it packs the sealed hello image under the spike's name, and `cp` replaces it.

No `.dtb` files and no `overlays/` folder. Wiring is the same as spike 1.

## Expected output

From QEMU. On hardware the `fsel`, `ocr`, `cid`, `crc32` and partition values differ.

```
@RC stage=spike2 ev=hello midr=0x410fb767 soc=bcm2835 base=0x20000000 board_rev=0x00920092
RobCo spike 2: SD sector 0 through EMMC (read only)
@RC stage=spike2 ev=gpio pins=34-39 fsel=000000
@RC stage=spike2 ev=gpio pins=48-53 fsel=000000
@RC stage=spike2 ev=remux pins34_39=kept pins48_53=changed
@RC stage=spike2 ev=gpio pins=34-39 fsel=000000
@RC stage=spike2 ev=gpio pins=48-53 fsel=777777
@RC stage=spike2 ev=emmc base_clk=50000000
@RC stage=spike2 ev=card type=sdsc ocr=0x80ffff00 rca=0xfd27 cid=00aa585951454d552101deadbeef0062 host_ver=2
@RC stage=spike2 ev=read lba=0 ok=1 sig=0xaa55 crc32=0x9ddf14d4 p0_type=0x0c p0_lba=2048
000: 03 0a 11 18 ...
```

`fsel` digits are the raw 3-bit values: 0 input, 1 output, 4 ALT0, 5 ALT1, 6 ALT2, 7 ALT3, 3 ALT4, 2 ALT5.

On a real card with the firmware partition, expect `sig=0xaa55` and `p0_type=0x0c` (FAT32 LBA) or `0x0b`.

On a Pi with WiFi, expect the first `pins=48-53` line to show `444444` (SD on SDHOST) and `pins=34-39` to show `777777` (EMMC on the WiFi chip).

On failure you get a line like this instead of `ev=card` or `ev=read`:

```
@RC stage=spike2 ev=error step=acmd41 status=0x... interrupt=0x...
```

## What to report back

For each board:

1. The board name.
2. All `@RC` lines, especially `ev=gpio`, `ev=remux`, `ev=card`, `ev=read` or `ev=error`.
3. The hex dump, or at least its last line.
4. Any `ev=exception` line.
