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

### Zero W 1.1, `examples/sdraw`, 2026-11-24

Same board, card, adapter, firmware tag `1.20260915` and library commit `4249632` as the hello run. No DTB on the card. Only two files changed: the image, `examples/sdraw` default variant as `SDRAW.IMG`, and `config.txt` from `tools/mkbootdir.sh` with `kernel=SDRAW.IMG`.

The output at boot. The card serial and the CID are left out on purpose:

```
@RC stage=sdraw ev=boot
@RC stage=sdraw ev=card rc=ok type=sdhc blocks=31116288 clk=25000000
@RC stage=sdraw ev=info ocr=0xc0ff8000 rca=0xaaaa mid=0x03 oid=SD pnm=SC16G prv=0x80 psn=<hidden> mdt=2019-06 cid=<hidden>
@RC stage=sdraw ev=mbr rc=ok sig=0x55aa p0_type=0x0b p0_lba=8192 crc=0x04045c4a
@RC stage=sdraw ev=ready
```

Read-only commands, in this order:

| Command | Answer |
|---|---|
| `i` | The same `ev=card` and `ev=info` lines again |
| `r 0 1` | `ev=read lba=0 count=1 rc=ok crc=0x04045c4a` |
| `u 0 1` | `ev=uread lba=0 count=1 rc=ok crc=0x04045c4a` |
| `r 0 8` | `ev=read lba=0 count=8 rc=ok crc=0x1afd369a` |
| `r 2048 16` | `ev=read lba=2048 count=16 rc=ok crc=0xd8f49994` |
| `r 1000000 256` | `ev=read lba=1000000 count=256 rc=ok crc=0x7ee8cdcd` |
| `u 1000000 256` | `ev=uread lba=1000000 count=256 rc=ok crc=0x7ee8cdcd` |
| `r 31116287 1` | `ev=read lba=31116287 count=1 rc=ok crc=0xb2aa7578` |
| `r 31116288 1` | `ev=read lba=31116288 count=1 rc=range crc=0x00000000` |
| `r 0 1` | `ev=read lba=0 count=1 rc=ok crc=0x04045c4a` |

macOS reported the same card as exactly 31116288 blocks of 512 bytes.

On this board, with this SDHC card, this run confirms:

- `sd_init()` brings the card up through the EMMC controller at base + `0x300000`, on a board where the firmware gives the card to SDHOST and EMMC to the Wi-Fi chip. The driver has no SDHOST code.
- The CSD capacity matches the size macOS reports.
- `clk=25000000` is the rate the driver computed from the mailbox EMMC base clock and its divider. It was not measured on the pin.
- Single and multi-block reads up to 256 blocks return `rc=ok`, at the start, the middle and the last block of the card.
- A read into a buffer offset by one byte, `u`, gives the same CRC as an aligned read.
- `rc=range` for block 31116288 comes from the driver's own check against the CSD capacity, before any command goes to the card.
- The MBR signature and partition type `0x0b` match the macOS FAT32 format, and sector 0 reads the same CRC at the start and at the end of the session.

This run does not show: any write, since `w`, `v` and `s` were not sent; an SDSC card; the no-card case; retries on an old card; FAT through RobCo-Core; whether the block CRCs match a read on a computer; the GPIO function of pins 34 to 39 and 48 to 53 before and after `sd_init()`; any other board.

### Zero W 1.1, `examples/umode`, 2026-11-25

Same board, card, adapter and firmware tag `1.20260915` as the hello run. No DTB on the card. The image is `examples/umode`, default variant, built from RobCo-Core `4249632`, as `UMODE.IMG`, with `config.txt` from `tools/mkbootdir.sh`. The variants `build/kfault`, `build/ksvc` and `build/kmmu` were not run.

The serial output has 164 lines. It starts with `@RC stage=umode ev=boot canary_addr=0x0000fc50`. It runs two passes of `ev=task` lines, then prints `@RC stage=umode ev=done`.

- With `mmu=0`, 18 tasks: `return`, `exit`, `regs`, `badptr`, `priv`, `undef`, `rdkern`, `rdperiph`, `spin`, `svcspin`, `thumb`, `init`, `psr`, `cpsid`, `stubsvc`, `killexit`, `afterkill` and `killfirst`.
- Then `@RC stage=umode ev=mmu on=1`.
- With `mmu=1`, 24 tasks: the same 18 and the six protection cases `wrkern`, `wrro`, `jump`, `jumpkern`, `stack` and `peek`.

`run_pass(0)` skips the cases marked `need_mmu`, because without the MMU nothing is protected. The `mmu=0` lines show it: `rdkern` returned `1526644958` and `rdperiph` returned `1757775`, where the `mmu=1` pass faulted. The case `thumb2` ran in neither pass. It needs Thumb-2, which ARM1176 does not have, so both passes skip it. The nested tasks `wrshared`, `rdunmap`, `pabt` and `badmap` also ran only with `mmu=1`. `tasks=42` counts the `ev=task` lines only, 18 plus 24. The run ends with:

```
@RC stage=umode ev=done tasks=42 ticks=448 canary=ok
```

Every one of the 116 lines with a `canary` field says `canary=ok`. No `ev=exception` and no `ev=error` line came.

The fault lines with the MMU on:

```
@RC stage=umode ev=task name=rdkern mmu=1 reason=fault code=0 type=data_abort pc=0x00400180 thumb=0 addr=0x0000fc50 status=0x0000000d ticks=244 canary=ok
@RC stage=umode ev=task name=wrkern mmu=1 reason=fault code=0 type=data_abort pc=0x004001c0 thumb=0 addr=0x0000fc50 status=0x0000080d ticks=245 canary=ok
@RC stage=umode ev=task name=rdperiph mmu=1 reason=fault code=0 type=data_abort pc=0x00400200 thumb=0 addr=0x20003004 status=0x0000000d ticks=247 canary=ok
@RC stage=umode ev=task name=wrro mmu=1 reason=fault code=0 type=data_abort pc=0x00400240 thumb=0 addr=0x00400800 status=0x0000080d ticks=248 canary=ok
@RC stage=umode ev=task name=jump mmu=1 reason=fault code=0 type=prefetch_abort pc=0xe0000000 thumb=0 addr=0xe0000000 status=0x00000005 ticks=249 canary=ok
@RC stage=umode ev=task name=jumpkern mmu=1 reason=fault code=0 type=prefetch_abort pc=0x00008000 thumb=0 addr=0x00008000 status=0x0000000d ticks=251 canary=ok
@RC stage=umode ev=task name=stack mmu=1 reason=fault code=0 type=data_abort pc=0x004002c0 thumb=0 addr=0x00fffffc status=0x0000080d ticks=252 canary=ok
@RC stage=umode ev=task name=peek mmu=1 reason=fault code=0 type=data_abort pc=0x00400180 thumb=0 addr=0x01000000 status=0x0000000d ticks=357 canary=ok
```

The lines for privileged and ignored instructions with `mmu=1`. The `mmu=0` lines differ only in `mmu` and `ticks`:

```
@RC stage=umode ev=task name=priv mmu=1 reason=fault code=0 type=undef pc=0x00400100 thumb=0 addr=0x00000000 status=0x00000000 ticks=241 canary=ok
@RC stage=umode ev=task name=psr mmu=1 reason=exit code=-133234096 type=none pc=0x00400474 thumb=0 addr=0x00000000 status=0x00000000 ticks=318 canary=ok
@RC stage=umode ev=task name=cpsid mmu=1 reason=killed code=1 type=none pc=0x00400488 thumb=0 addr=0x00000000 status=0x00000000 ticks=350 canary=ok
```

#### How to read the status

`status` is the DFSR for a data abort and the IFSR for a prefetch abort. The library sets SCTLR.XP on ARM1176, so the CPU uses the ARMv6 short-descriptor format. Bits [10] and [3:0] give the fault, as `src/arch/exceptions.c` and the [memory guide](./guide/api/memory.md#mmu-mmuh) list it: `0x5` translation fault on a section, `0xd` permission fault on a section, `0x9` or `0xb` a domain fault. The library maps only 1 MiB sections in domain 0, and DACR sets domain 0 to client. So the AP bits are checked and no domain fault is expected. Bit 11 of the DFSR is set by the CPU when the abort came from a write. The IFSR has no such bit.

- `0x0000000d`: permission fault, section, read. On `jumpkern` it is a permission fault on an instruction fetch.
- `0x0000080d`: permission fault, section, write. Bit 11 is set.
- `0x00000005`: translation fault, section. Nothing maps that address.

#### Each task

Each task runs one short program from `examples/umode/user/prog.S`, at `0x00400000` plus 0x40 times its slot. `wrkern`, `wrro`, `jump`, `jumpkern`, `stack` and `peek` ran only with `mmu=1`. The other rows ran in both passes. With `mmu=0`, `rdkern` and `rdperiph` returned a value and did not fault.

| Task | What it does | What the line shows |
|---|---|---|
| `rdkern` | `ldr r0, [r0]` with r0 the kernel's canary address | The kernel's privileged-only RAM refuses a USR read |
| `wrkern` | `str r1, [r0]` to the same address | It refuses a USR write. The canary kept its value. |
| `rdperiph` | `ldr r0, [r0]` from the system timer, `0x20003004` | The privileged-only device section refuses a USR read |
| `wrro` | `str r1, [r0]` to `0x00400800`, in its own read-only code region | AP `110`, read-only for both, refuses the write. This needs the APX bit, so XP is on. |
| `jump` | `bx r0` to `0xe0000000` | A prefetch abort at the target, and the IFAR gives the jump address |
| `jumpkern` | The same `bx r0` to `0x8000`, the kernel image | A prefetch abort at the kernel's first instruction. See below. |
| `stack` | Pushes until `sp` leaves its megabyte at `0x01000000` | The first push below it writes `0x00fffffc`, in privileged-only RAM, and faults |
| `peek` | `ldr r0, [r0]` from `0x01000000`, the data region of the earlier tasks, which it does not list | The region is privileged-only again after the earlier task ended |
| `priv` | `mrc p15` read of SCTLR | Undefined instruction in USR mode |
| `psr` | `cpsid i`, `cpsie f`, `msr cpsr_c, #0xdf`, then a call whose handler writes `0xffffffff` to the saved status | Bits [8:0] of the exit code are `0x50`: USR, ARM, IRQs on, FIQ masked. The three instructions changed nothing, and the handler's write was sanitised. |
| `cpsid` | `cpsid if`, `msr cpsr_c, #0xd0`, then spins | The watchdog still kills it: IRQs stayed on |

#### Against the checklist and QEMU

The "User mode" row asked for `0xd` or `0x80d` on permission faults and `0x5` on the unmapped jump. Each line gives that, with bit 11 set on the three writes and clear on the reads. `mrc p15` traps, `cpsid` and `msr cpsr_c` are ignored, and the IFAR gives `0xe0000000`.

`tests/target/test_umode.py` expects, for `raspi0`, the same task order, results, `pc` and `addr` values, fault status bits [10] and [3:0], bit 11, `ev=errors`, `ev=refused_after`, `ev=stale`, `ev=premapped`, `ev=usermap`, the nested tasks and `ev=done tasks=42`. All 164 lines meet those checks except one.

The one difference: `jumpkern`. On real ARM1176 the USR fetch from the kernel image is refused with a prefetch abort at `0x8000`, status `0xd`. QEMU 10.1 on `raspi0` lets the task fetch, runs the boot code in USR mode, and stops at the first privileged instruction with `type=undef`.

Some fields differ from run to run or from build to build. They are not differences in behaviour:

- `ticks` on every line, and `ticks` on `ev=done`. The test checks only that they rise and pass the watchdog limits.
- `code` of `rdperiph` with `mmu=0`, `1757775`. It is the system timer value at that moment.
- `pc` of a killed task: `spin`, `svcspin`, `cpsid`, `killfirst` and the inner `spin` and `kill`. It is where the tick found the task. The test checks only the range of its slot.
- `canary_addr` and the `addr` of `rdkern`, `wrkern` and `ev=refused_after`. They depend on the build.

The test checks only bits [8:0] of the `psr` exit code. This run did not compare its upper bits, `0xf80f0200`, with QEMU. The raw status values were not compared with QEMU either, only the bits the test checks.

On this board, this run confirms:

- Real ARM1176 enforces the section permissions the library sets with XP on: privileged-only RAM, privileged-only device memory, and read-only for both.
- The fault status values are the ones the checklist expected, with bit 11 on writes.
- A USR jump into the privileged-only kernel image is a prefetch abort at the target, status `0xd`. A task cannot fetch kernel code on this CPU.
- `mrc p15` traps as undefined in USR mode, and `cpsid`, `cpsie` and `msr cpsr_c` are ignored.
- The tasks, faults, kills and nested tasks leave kernel memory unchanged, and the tick keeps running across all 42 tasks: 18 with the MMU off and 24 with it on.

This run does not show: the variants `build/kfault`, `build/ksvc` and `build/kmmu`; the `examples/mmu` variants `build/fault_ro` and `build/fault_unmapped`; the UART echo after the run, since no input was sent; the `thumb2` case, skipped because ARM1176 has no Thumb-2; any protection with the MMU off, since that pass protects nothing; any Cortex-A7 or A53 board; any other commit; a second run.

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
| User mode | All | `examples/umode` | One `ev=task` line per case and `ev=done` with `canary=ok`. QEMU enforces the MMU permissions, so the protection itself is tested. Check here what QEMU cannot prove: the real fault status values per CPU (`status=` on the `rdkern`, `wrkern`, `rdperiph`, `wrro`, `jump` and `stack` lines, expected `0xd` or `0x80d` for permission faults and `0x5` for the unmapped jump), that `mrc p15` and the undefined instruction trap in USR mode on the real cores, that `cpsid` and `msr cpsr_c` are ignored in USR mode on real silicon, and that the IFAR read gives the jump address on ARM1176. Done on a Zero W 1.1 only, default variant, on 2026-11-25: see [results](#zero-w-11-examplesumode-2026-11-25). Open on every other board. |
| User-mode jump into kernel code | Pi Zero or Pi 1 | `examples/umode` | The `jumpkern` line: `type=prefetch_abort pc=0x00008000 addr=0x00008000 status=0x0000000d`. QEMU runs the kernel code instead and stops at `type=undef pc=0x00008078`. Done on a Zero W 1.1 on 2026-11-25: the board gave the prefetch abort line, so real ARM1176 refuses the fetch. See [results](#zero-w-11-examplesumode-2026-11-25). Open on a Pi 1. |
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
| SD pin routing | Pi 3, Zero 2 W, Zero W. Pi Zero or Pi 1 as control. | `spikes/s2-sd-read`, then `examples/sdraw` | Before the remux `pins=48-53 fsel=444444` and `pins=34-39 fsel=777777`, then `fsel=777777` on 48 to 53, `ev=card`, and `ev=read ok=1 sig=0xaa55`. Zero W 1.1, 2026-11-24: `examples/sdraw` gave `ev=card rc=ok` and read sector 0 through EMMC, see [results](#zero-w-11-examplessdraw-2026-11-24). The spike's `fsel` markers were not captured, so the pin states stay open on the Zero W. Open on every other board. |
| SD init and transfers | All, with an SDSC and an SDHC card | `examples/sdraw`: `i`, `r 0 1`, `w` and `v` on a scratch block, `u`, `s` | `ev=card rc=ok`, a clock near 25000000, `mismatches=0`, the same CRC from `r` and `u`. Zero W 1.1, 2026-11-24, one SDHC card: init, `i`, `r` and `u` done, see [results](#zero-w-11-examplessdraw-2026-11-24). `w`, `v`, `s` and an SDSC card stay open. Open on every other board. |
| SD retries | All, an old card helps | `examples/sdraw`, `r` and `w` of up to 256 blocks | No `rc=io` or `rc=timeout`, and correct data |
| No card | All | `examples/sdraw` with the slot empty | `rc=nocard` at once, then `rc=nomedia` for `r 0 1` |
| FAT on a real card | All | `examples/storage`: `ls /`, `cat /config.txt`, `put`, `atomic`, `df` | `ev=mount rc=ok`, the right sizes and CRCs, the files readable on a PC afterwards. |
| Power cut during atomic saves | All, with a card you can lose | `examples/storage`: `atomic /SAVE.SAV 70000 1`, then repeat with seeds 2, 3 and on, and pull the power at random points, at least 20 times. After each boot run `recover /SAVE.SAV`, `cat /SAVE.SAV` and `ls /`. | `ev=recover rc=ok`, a CRC that matches the last seed that returned `rc=ok` or the one after it, never `rc=noent` once a save has returned `rc=ok`, and no entry with `left=1`. The card passes `fsck.vfat -n` on a PC at the end. |
| Read-only build on a real card | All | `examples/storage` variant `build/ro` | `ls` and `cat` work, every writing command gives `rc=ro`, and the card is unchanged |
| Loading the next stage from a card | All | `examples/chain1` variant with `-DCHAIN1_SD=1`, card from `tools/mksdimage.sh` with `chain2` as `/ROBCO/MFBOOT.SYS` | `ev=mount`, `ev=load` with the size and CRC that `tools/rimg.py info` shows, then `chain2` runs. Remove the file and add `/ROBCO/MFBOOT.BAK`: `ev=error`, `ev=fallback`, then `ev=load` of the backup. |
| File time stamps from a real RTC | All, with an RTC module | `examples/storage` with the module, then `put` and `ls` | `ev=clock rc=0`, and a `date` and `time` on the new file that match the RTC, also when read on a PC |
| Demo menu with a card | All, with a keyboard over the serial adapter | `examples/demo` variant `build/menu`, card with `/DEMO.INI` | The menu starts on the item the file names and follows the arrow keys |
| Handoff with caches | All | `examples/chain1` variants `build/mmu` and `build/mmufb` | The next stage accepts the boot info and runs |

## Card removal

QEMU pulls the card with QMP and sets the card inserted bit. A real slot may not, and the driver then never reads it: see [Card detect](guide/api/storage.md#card-detect). Every row is open on every board.

| Component | Boards | Run | Look for |
|---|---|---|---|
| Card-inserted bit with a card in, on each board | All | `examples/sdraw` with a working card | The `ev=detect` marker. Write down `present`, `inserted` and `card_detect` for the board. `card_detect=not_reported` means the driver never uses the bit there, and a pull shows as timeouts. |
| Card inserted bit | All | `examples/storage`: `probe` with the card in, then pull the card and `hread 8192` after `hopen` on a large file | `ev=probe rc=ok` with the card in. After the pull, `rc=gone`. If the answer is `rc=timeout` or `rc=io`, the slot does not report the pull: write that down. |
| Pulling a card during a read | All | `examples/storage`: `ramfs`, `put ram:/R.BIN 3000 5`, `hopen` on a file of several MiB, then `hread 65536` in a loop while you pull the card | Every answer after the pull is `rc=gone`, never stale data and never a hang. `cat ram:/R.BIN` still works. `hclose` and `umount` give `rc=gone` and `rc=ok`. |
| Putting another card in | All | After the row above: insert a card with other files, `sdinit`, `mount ro`, `cat sd:/<a file only on the new card>` | `ev=sdinit rc=ok`, `ev=mount rc=ok` and the new file's CRC. A file only on the old card gives `rc=noent`. |
| Pulling a card during a write | All, with a card you can lose | `examples/storage`: `atomic /SAVE.SAV 70000 1` and pull the card during it, at least 10 times. Put it back, `sdinit`, `mount`, `recover /SAVE.SAV`, `cat /SAVE.SAV`. | `rc=gone` for the write, then `ev=recover rc=ok` and the old or the new CRC. The card passes `fsck.vfat -n` on a PC. |

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

## Holotape slot

Use the Pip-Boy HAT with its slot fitted, or the slot's parts wired as in its spec, section 4.12. `examples/holo` is the bring-up tool. Nothing here has run on hardware: the driver has run only against the host card model and, with no card, in QEMU. Keep a spare card.

| Component | Boards | Run | Look for |
|---|---|---|---|
| Slot bring-up | All | `examples/holo`: `p` with the slot empty, insert a card, `p`, `a`, `i`, `r 0 1`, `r 0 64`, `d`, `p` | `cd=1 pwr=0 bus=0` empty, then `ev=inserted`, `ev=attach rc=ok`, the card's type and size, the same CRC as the card's first blocks read on a PC, `pwr=0 bus=0` after `d`. `i` prints no serial number. Open. |
| Card in at boot with the display running | All, with the ILI9341 on CE0 | An image that runs `examples/tft` frames and `holo` reads in turn, with a card in at boot | A clean picture and correct CRCs, no tearing, no `rc=io`. A logic analyser shows `HOLO_BUS` high only around card transfers and never during a display byte. Open. |
| Hot insertion | All | `examples/holo` running, insert a card, then `a` | One `ev=inserted` after the bounce, power only after `a`, the `+5V` dip under 0.05 V, `SD_3V3` rising in 0.5 ms to 3 ms. Open. |
| Hot removal mid-read | All | `examples/holo`, `r 0 64` repeated, pull the card during a read | `ev=read rc=nomedia` or `rc=io`, never a hang, then `ev=removed`, `pwr=0 bus=0`. Insert it again: `a` works after the debounce. Open. |
| SDSC card | All, with a card of 2 GB or less | `examples/holo`: `a`, `i`, `r 0 1` | `type=sdsc` and the right size and data. Open. |
| Lock tab | All | `examples/holo`: `p` with the tab on, then off | `locked=1` with the tab on, `locked=0` off, and the device stays read-only either way. Open. |
| Stepped attach | All | `examples/holo`: insert a card, `g`, `i`, `r 0 1`, `d`, then `g` again after 100 ms | `ev=steps rc=ok`, and the same card and data as with `a`. Record `steps` and `max_us`. Expect `max_us` near 1.3 ms or less at 400 kHz, and `steps` about 10 plus the passes that wait. Open. |
| Timing margin | All | `examples/holo` with a scope on SCLK and MISO at U4 and J1 | Setup time at 9.62 MHz. Under 10 ns, lower `hz` to 4.8 MHz. Open. |

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
