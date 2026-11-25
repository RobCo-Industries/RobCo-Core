# Hardware validation

QEMU runs every target test, but it cannot prove the items below. Run them on a real Pi Zero or Pi 1 (BCM2835) and on a real Pi 3 or Zero 2 W (BCM2837). A Pi 2 (BCM2836) adds the Cortex-A7 cases. Write each result back into the spec, as [TESTING.md](./spec/TESTING.md#hardware-checklists) describes.

Every item needs a 3.3 V USB serial adapter on GPIO 14 and 15 for the `@RC` lines. [GETTING_STARTED.md](./guide/GETTING_STARTED.md#put-it-on-an-sd-card) shows how to build the card and wire the adapter. The groups below list what you need on top of it.

## Results

### Zero W 1.1, `examples/hello`, 2026-11-24

| Item | Value |
|---|---|
| Board | Raspberry Pi Zero W rev 1.1, revision code `0x009000c1` |
| Library | RobCo-Core `4249632` |
| Image | `examples/hello`, default variant, as `HELLO.IMG` |
| Firmware | `bootcode.bin`, `start.elf`, `fixup.dat` from tag `1.20260915`, SHA-256 checked by `tools/fetch_firmware.sh` |
| Card | 16 GB, MBR, one FAT32 partition, holding only the five files `tools/mkbootdir.sh` wrote. No `.dtb`, no `overlays/`. |
| Serial | CP2102 adapter, 3.3 V, header pins 6, 8 and 10, 115200 8N1 |

The complete output:

```
@RC stage=hello ev=boot midr=0x410fb767 soc=bcm2835 base=0x20000000 board_rev=0x009000c1 mem=0x1c000000 entry_mode=svc
@RC stage=hello ev=probe pwm=ok gpio=ok
@RC stage=hello ev=ready
```

Sending the 10 bytes `RobCo 123\r` gave back `RobCo 123\r\n`.

On this board, this run confirms:

- The firmware loads a sealed image as `kernel=` with `arm_64bit=0` and no DTB.
- The start code reaches `robco_main()` in SVC mode.
- MIDR `0x410fb767` gives BCM2835 and base `0x20000000`.
- The property mailbox answers. `board_get()` reads `board_rev` with tag `0x00010002` and `mem` with tag `0x00010005`. `0x1c000000` is 448 MiB, which matches `gpu_mem=64` on a 512 MiB board.
- `0x009000c1` is the code that `tests/host/test_board_model.c` maps to `Raspberry Pi Zero W`. As a new-style code it reads revision 1, type `0x0c` Zero W, processor BCM2835, Sony UK, 512 MB.
- The PL011 on GPIO 14 and 15 sends and receives at 115200, with Bluetooth on the board and no `dtoverlay`.
- `mmio_probe()` reads the PWM block at base + `0x20c000` and GPFSEL0 at base + `0x200000` without a data abort. This shows the reads do not abort. It does not test PWM or GPIO function.

This run does not show: HDMI output, since no screen was connected; the UART clock value, which the hello line does not print; any BCM2836 or BCM2837 board; the mini UART; SD access by RobCo-Core, since only the firmware read the card. Every other row below stays open.

### How this run was done

Build the boot directory as in [GETTING_STARTED.md](./guide/GETTING_STARTED.md#put-it-on-an-sd-card). On macOS, find the card with `diskutil list`, then format it. This erases the whole card, so check the disk number twice:

```sh
diskutil eraseDisk FAT32 ROBCO MBRFormat /dev/diskN
cp sdboot/* /Volumes/ROBCO/
diskutil eject /dev/diskN
```

Wire the adapter to header pins 6, 8 and 10 as the guide shows. Open the port before you power the Pi, for example `screen /dev/tty.usbserial-XXXX 115200`. To keep a log, use `script -q hello.log screen /dev/tty.usbserial-XXXX 115200`.

## Serial adapter only

| Component | Boards | Run | Look for |
|---|---|---|---|
| UART | All | `spikes/s1-uart-hello` or `examples/hello` | The hello line and the echo on every board. On a Pi 3 or Zero 2 W, the PL011 works without `dtoverlay=disable-bt`. Done on a Zero W 1.1 only, with `examples/hello`, on 2026-11-24: see [results](#results). Open on every other board. |
| MMU | All | `examples/mmu` | `ev=bench caches=on` with `fill_us` and `copy_us` well below the `caches=off` line |
| Mailbox with caches | All | `examples/mmu` | `ev=mbox phase=on mismatches=0`, and the same rev, clock and raw values before, on and after |
| Misaligned mailbox buffer with caches | All | `examples/mmu`, then the same image in a loop of many boots or with `LOOPS` raised | `ev=guard loops=100 bad_guard=0 bad_reply=0` on every run: replies stay right and the neighbouring guards survive |
| `mmu_disable()` | All | `examples/mmu` | `ev=mmu_off` with SCTLR M, C and I clear, then `ev=mbox phase=after` |
| SMP bit | Pi 2, Pi 3, Zero 2 W | `examples/mmu` | `smp=1` on the `ev=mmu` line. `0` means the firmware locked the register: check data cache coherence again. |
| MMU faults, XP bit | Pi Zero or Pi 1 | `examples/mmu` variants `build/fault_ro` and `build/fault_unmapped` | A data abort with `dfar=0x01c00000`, status `0xd` and bit 11 set, then `dfar=0x60000000`, status `0x5` |
| User mode | All | `examples/umode` | One `ev=task` line per case and `ev=done` with `canary=ok`. QEMU enforces the MMU permissions, so the protection itself is tested. Check here what QEMU cannot prove: the real fault status values per CPU (`status=` on the `rdkern`, `wrkern`, `rdperiph`, `wrro`, `jump` and `stack` lines, expected `0xd` or `0x80d` for permission faults and `0x5` for the unmapped jump), that `mrc p15` and the undefined instruction trap in USR mode on the real cores, that `cpsid` and `msr cpsr_c` are ignored in USR mode on real silicon, and that the IFAR read gives the jump address on ARM1176 |
| User-mode jump into kernel code | Pi Zero or Pi 1 | `examples/umode` | The `jumpkern` line: `type=prefetch_abort pc=0x00008000 addr=0x00008000 status=0x0000000d`. QEMU runs the kernel code instead and stops at `type=undef pc=0x00008078`. If real silicon does that too, the task can fetch kernel code on ARM1176. |
| User-mode kernel faults | All | `examples/umode` variants `build/kfault` and `build/ksvc` | One `ev=exception` line, `type=undef` then `type=svc`, with an SVC-mode `spsr` |
| Audio probe | All | `examples/audio` | `ev=backend name=pwm probe_pwm=ok err=0` and the expected pins |
| Mini UART script channel | Pi 3 or Zero 2 W first | `examples/input`, with the adapter on GPIO 14 and 15 at ALT5 and `core_freq` fixed in `config.txt` | `ready`, then `ok` answers at 115200 baud |
| Input stuck-line guard | All, nothing wired | `examples/input` | `ev=gpio status=active`, not `disabled_stuck`, since the pull-ups read high |

## HDMI screen

| Component | Boards | Run | Look for |
|---|---|---|---|
| Framebuffer with caches | All | `examples/mmu` | 8 bars and 3 corner squares, no stale or torn rows |
| Console with caches | All | `examples/demo` variant `build/mmu` | Console text appears as it is written |
| Spike baseline | All | `spikes/s3-fb-mmu` | The same screen, `ev=fill` times and `smp_write=` on the `ev=mmu` line |
| VC bus alias | All | `examples/console` | The console draws. The alias in [BOARD_SUPPORT.md](./spec/BOARD_SUPPORT.md#detection-table) is right. |

## SD card

Use a spare card: `w` in `sdraw` overwrites raw blocks.

| Component | Boards | Run | Look for |
|---|---|---|---|
| SD pin routing | Pi 3, Zero 2 W, Zero W. Pi Zero or Pi 1 as control. | `spikes/s2-sd-read`, then `examples/sdraw` | Before the remux `pins=48-53 fsel=444444` and `pins=34-39 fsel=777777`, then `fsel=777777` on 48 to 53, `ev=card`, and `ev=read ok=1 sig=0xaa55` |
| SD init and transfers | All, with an SDSC and an SDHC card | `examples/sdraw`: `i`, `r 0 1`, `w` and `v` on a scratch block, `u`, `s` | `ev=card rc=ok`, a clock near 25000000, `mismatches=0`, the same CRC from `r` and `u` |
| SD retries | All, an old card helps | `examples/sdraw`, `r` and `w` of up to 256 blocks | No `rc=io` or `rc=timeout`, and correct data |
| No card | All | `examples/sdraw` with the slot empty | `rc=nocard` at once, then `rc=nomedia` for `r 0 1` |
| FAT on a real card | All | `examples/storage`: `ls /`, `cat /config.txt`, `put`, `atomic`, `df` | `ev=mount rc=ok`, the right sizes and CRCs, the files readable on a PC afterwards. |
| Power cut during atomic saves | All, with a card you can lose | `examples/storage`: `atomic /SAVE.SAV 70000 1`, then repeat with seeds 2, 3 and on, and pull the power at random points, at least 20 times. After each boot run `recover /SAVE.SAV`, `cat /SAVE.SAV` and `ls /`. | `ev=recover rc=ok`, a CRC that matches the last seed that returned `rc=ok` or the one after it, never `rc=noent` once a save has returned `rc=ok`, and no entry with `left=1`. The card passes `fsck.vfat -n` on a PC at the end. |
| Read-only build on a real card | All | `examples/storage` variant `build/ro` | `ls` and `cat` work, every writing command gives `rc=ro`, and the card is unchanged |
| Loading the next stage from a card | All | `examples/chain1` variant with `-DCHAIN1_SD=1`, card from `tools/mksdimage.sh` with `chain2` as `/ROBCO/MFBOOT.SYS` | `ev=mount`, `ev=load` with the size and CRC that `tools/rimg.py info` shows, then `chain2` runs. Remove the file and add `/ROBCO/MFBOOT.BAK`: `ev=error`, `ev=fallback`, then `ev=load` of the backup. |
| File time stamps from a real RTC | All, with an RTC module | `examples/storage` with the module, then `put` and `ls` | `ev=clock rc=0`, and a `date` and `time` on the new file that match the RTC, also when read on a PC |
| Demo menu with a card | All, with a keyboard over the serial adapter | `examples/demo` variant `build/menu`, card with `/DEMO.INI` | The menu starts on the item the file names and follows the arrow keys |
| Handoff with caches | All | `examples/chain1` variants `build/mmu` and `build/mmufb` | The next stage accepts the boot info and runs |

## Buttons and encoder

| Component | Boards | Run | Look for |
|---|---|---|---|
| GPIO buttons | All | `examples/input`, buttons on the pins in [BOARD_SUPPORT.md](./spec/BOARD_SUPPORT.md#pins) | `ev=gpio status=active`, one press and one release per click |
| Rotary dial | All | `examples/input` | One dial event per detent, positive for clockwise. Swap A and B if the sign is wrong. |

## Speaker or piezo

Use the audio jack on a Pi 1, 2 or 3. On a Pi Zero, add a filter or a piezo on GPIO 18, as [the audio wiring](./guide/api/audio.md#audio-audioh) shows.

| Component | Boards | Run | Look for |
|---|---|---|---|
| PWM sound | All | `examples/audio` | The beep, the arpeggio, the tune at 60% volume, the looped alarm for about 700 ms, and the 440 Hz tone cut by 880 Hz. Keys `b`, `s`, `t`, `l`, `i` and `a` replay each case. |
| PCM sound | All | `examples/audio` | After the tones, rising chirps: 100 ms at 8000 Hz, 100 ms at 22050 Hz and half volume, one cut at 200 ms, a 440 Hz tone cut by a chirp cut by 880 Hz, a 25 ms chirp looped for 300 ms, one second at 22050 Hz, then three queued chunks at 8000 Hz that play as one 300 ms chirp. No pitch wobble, and no click between chunks. Key `p` replays the first chirp, key `c` the queued chunks. Not yet validated on any board. |
| Forced PWM | A board where the probe says `ignored` | `examples/audio` variant `build/pwm` | The sound still plays |

## RTC module

Use a DS1338, a DS3231, or a DS1307 module with 3.3 V pull-ups. A logic analyser helps.

| Component | Boards | Run | Look for |
|---|---|---|---|
| I2C rate and errors | All. The Pi 3 core clock varies. | `examples/rtc` | SCL at or below 100 kHz, the scan finds `0x68`, `errors=0` |
| DS1307 behaviour | All | `examples/rtc` | `ev=rtc present=1 running=1`, the set time reads back, the time moves on by 2 s, `ev=nvram ... same=1`, `ev=h12 ... hour=23` |
| DS1307 detection | All | `examples/rtc` with a DS1307 or DS1338, RAM filled with known bytes first | `chip=0`. A logic analyser shows no write transfer during `rtc_init()`, and the RAM bytes are unchanged. Repeat at second `00` and `59`. |
| DS3231 | All | `examples/rtc` with a DS3231 module | `ev=rtc present=1 running=1 rc=0 chip=1`, the set time reads back. A logic analyser shows a 26-byte read and no write during `rtc_init()`, and byte `0x13` repeats byte `0x00`. The `ev=nvram` check fails on a DS3231, which has no RAM. |
| DS3231 detection at a tick | All | `examples/rtc` on a DS3231, reset many times | `chip=1` every time, also when the read crosses a second or a minute |
| DS3231 status bits | All | `examples/rtc` on a DS3231 with `EN32kHz` set and an alarm flag set | After `rtc_set()`, only bit 7 of `0x0F` changed |
| DS3231 stopped oscillator | Any | `examples/rtc` on a DS3231 after its battery was removed | `RTC_ERR_HALTED` until `rtc_set()` runs, then `running=1` |
| New chip | Any | `examples/rtc` on a chip never set | `RTC_ERR_HALTED` until `rtc_set()` runs |
| Drift and battery | Any | `examples/rtc` over hours, with a power cut | The software clock stays near the RTC, and the RTC keeps time through the cut |

## GPIO expander

Use a PCA9554 at `0x20` and another at `0x21`, with LEDs on outputs and switches to ground on inputs. A logic analyser helps.

| Component | Boards | Run | Look for |
|---|---|---|---|
| PCA9554 pins | All | `examples/gpiox`, press a key at each `ev=wait` | `ev=attach ... rc=0` for both chips, `ev=absent ... rc=-32`, the LED patterns of each step, and `ev=in` bits that match the switches |
| No glitch at attach | All | `examples/gpiox` reset while outputs drive LEDs | A logic analyser shows only reads during `gpiox_attach()`, and the LEDs do not flicker |

## Watchdog

| Component | Boards | Run | Look for |
|---|---|---|---|
| Watchdog | All | `examples/wdog`, then send `f`, `k`, `s`, `r` and `h` | `f` resets within about 1 s, `k` prints `ev=kicked` with `remaining=` below 1000 and stays up, `s` prints `ev=stayed`, `r` reboots at once, `h` stays halted |

## SPI device

Use a W25Qxx flash, a logic analyser, or both.

| Component | Boards | Run | Look for |
|---|---|---|---|
| SPI data path | All | `examples/spi` with a flash on CE0 | The JEDEC id of the part, such as `ef40xx` for Winbond |
| SPI signalling | All | `examples/spi` with a logic analyser | Rates at or below 1 MHz, the clock polarity and phase of each mode, chip select levels with and without `SPI_CS_HIGH`, CE1, the gap every 16 bytes, no hang |

## SPI display

Use an ILI9341 module wired as in [BOARD_SUPPORT.md](spec/BOARD_SUPPORT.md#spi-display).

| Component | Boards | Run | Look for |
|---|---|---|---|
| TFT init and picture | All | `examples/tft` | Eight upright colour bars, white to black from the left, and a white border on all four sides. Colours not swapped red for blue. |
| TFT rotation | All | `examples/tft` built with `EXTRA_CFLAGS=-DTFT_EXAMPLE_ROTATION=0`, then 180 and 270 | Each picture upright and not mirrored, the size in `ev=init` |
| TFT id | All | `examples/tft` with MISO wired | `ev=id rc=0` and the raw id bytes |
| TFT SPI rate | All | `examples/tft` at the 10 MHz default, then with `hz` raised step by step up to 64 MHz | A clean picture at 10 MHz. Note the highest rate that still shows a clean picture. |
| TFT frame time | All | A loop of `tft_present_px32()` at the default rate | Time per frame against the 128 ms the guide computes for a 250 MHz core clock |
