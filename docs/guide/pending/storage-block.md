## Block devices (`blk.h`)

`blk.h` is the interface between storage drivers and file systems. A driver fills in a `blk_dev_t`. A file system calls the checked wrappers. Blocks are 512 bytes.

| Constant | Value | Meaning |
|---|---|---|
| `BLK_SECTOR_SIZE` | `512` | Bytes per block |
| `BLK_ERR_IO` | `-1` | The device reported an error |
| `BLK_ERR_TIMEOUT` | `-2` | The device did not answer in time |
| `BLK_ERR_RANGE` | `-3` | Block number or count out of range |
| `BLK_ERR_NOMEDIA` | `-4` | No card or medium present |
| `BLK_ERR_RO` | `-5` | The device is read-only |
| `BLK_ERR_PARAM` | `-6` | Bad argument |
| `BLK_FLAG_READONLY` | `0x1` | Writes return `BLK_ERR_RO` |
| `BLK_FLAG_REMOVABLE` | `0x2` | The medium can be removed |

`struct blk_dev` holds `name`, `block_size`, `block_count` (`0` if unknown), `flags`, the `read`, `write` and `sync` callbacks, and a `priv` pointer for the driver.

```c
static inline int blk_check(const blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf);
```

Returns `BLK_ERR_PARAM` for a NULL device or buffer or a zero count, `BLK_ERR_RANGE` when the blocks do not fit in `block_count`, `0` otherwise.

```c
static inline int blk_read(blk_dev_t *dev, uint64_t lba, uint32_t count, void *buf);
static inline int blk_write(blk_dev_t *dev, uint64_t lba, uint32_t count, const void *buf);
```

Run `blk_check()`, then call the driver. `buf` holds `count * block_size` bytes. `blk_write()` returns `BLK_ERR_RO` on a read-only device.

```c
static inline int blk_sync(blk_dev_t *dev);
```

Calls the driver's `sync`, or returns `0` when it has none.

```c
static inline const char *blk_strerror(int err);
```

Returns a short name: `"ok"`, `"io"`, `"timeout"`, `"range"`, `"nomedia"`, `"readonly"`, `"param"` or `"unknown"`.

## SD card (`sd.h`)

`sd.h` drives the SD card through the BCM283x EMMC controller (Arasan SDHCI) at peripheral base + `0x300000`. It uses polled PIO on a 1-bit bus at 25 MHz: no DMA, no interrupts. It supports SDSC (byte addressing) and SDHC or SDXC (block addressing) cards. It does not support BCM2711.

Init order: `sd_init()` needs step 1 (`board_get()`) and nothing else. It uses the mailbox for the base clock (tag `0x00030002`, clock id 1), the system timer for every wait and `gpio.h` for pin routing. Call `uart_init()` first if you want to print the result.

| Constant | Value | Name | Meaning |
|---|---|---|---|
| `SD_ERR_BOARD` | `-10` | `board` | Unknown board, or BCM2711 |
| `SD_ERR_CLOCK` | `-11` | `clock` | No base clock, or the SD clock did not settle |
| `SD_ERR_RESET` | `-12` | `reset` | The controller did not leave reset |
| `SD_ERR_NOCARD` | `-13` | `nocard` | No answer to CMD8 nor to ACMD41 |
| `SD_ERR_VOLTAGE` | `-14` | `voltage` | CMD8 echo wrong, or the card rejects 3.3 V |
| `SD_ERR_BUSY` | `-15` | `busy` | The card stayed busy in ACMD41 |
| `SD_ERR_CMD` | `-16` | `cmd` | An identification command failed |
| `SD_ERR_CSD` | `-17` | `csd` | Unknown CSD layout |

```c
int sd_init(void);
```

Routes the card to EMMC, resets the controller, identifies the card at 400 kHz (CMD0, CMD8, ACMD41, CMD2, CMD3, CMD9, CMD7), switches to 25 MHz and sets the block length on SDSC cards. Returns `0` or `SD_ERR_*`. Each wait is bounded by the system timer, so a missing card fails within about a second. Calling it again restarts from scratch.

```c
const char *sd_strerror(int err);
```

Returns the name of an `sd_init()` result from the table above, `"ok"` for `0`, or `"unknown"`.

```c
blk_dev_t *sd_blkdev(void);
```

Returns the `"sd0"` block device after a successful `sd_init()`, `NULL` otherwise. `block_count` comes from the CSD. `flags` is `BLK_FLAG_REMOVABLE`.

```c
const sd_info_t *sd_info(void);
```

Returns card details after a successful `sd_init()`, `NULL` otherwise: `type` (`SD_TYPE_SDSC` or `SD_TYPE_SDHC`), `ocr`, `rca`, the raw `cid` and its fields (`mid`, `oid`, `pnm`, `prv`, `psn`, `mdt_year`, `mdt_month`), `capacity_blocks` and `clock_hz`.

```c
void sd_deinit(void);
```

Stops the SD clock and turns bus power off. `sd_blkdev()` and `sd_info()` return `NULL` after it.

Transfers through `sd_blkdev()`:

- `read` and `write` accept any count. They use CMD18 and CMD25 with auto-CMD12, up to 65535 blocks per command. A single block uses CMD17 or CMD24.
- A failed transfer resets the controller lines, sends CMD12 and retries up to 3 times. If a multi-block transfer still fails, the driver moves the blocks one at a time.
- `write` returns after the card has left the programming state. `sync` polls CMD13 until the card is ready again.
- Buffers may have any alignment. Aligned buffers take a faster word path.
- Errors: `BLK_ERR_NOMEDIA` before a successful init, `BLK_ERR_RANGE` past the end of the card, `BLK_ERR_TIMEOUT` when the card or controller does not answer, `BLK_ERR_IO` for CRC and other errors.

Context: main context only. The driver has no locks and must not run from an IRQ handler or a tick callback. A large transfer busy-waits for its whole length.

Pin routing: on boards with WiFi (Pi 3, Zero W, Zero 2 W) the firmware routes the card to the SDHOST controller on GPIO 48 to 53 (ALT0) and EMMC to the WiFi chip on GPIO 34 to 39 (ALT3). `sd_init()` sets GPIO 34 to 39 to input when any of them is ALT3, and GPIO 48 to 53 to ALT3 when any of them is not. It never selects ALT0. WiFi stops working after this. This routing is verified in QEMU only and still needs checking on a real Pi 3 and Zero 2 W.

```c
int rc = sd_init();
uint8_t sector[BLK_SECTOR_SIZE];

k_printf("sd: %s\n", sd_strerror(rc));
if (rc == 0) {
    blk_dev_t *sd = sd_blkdev();

    rc = blk_read(sd, 0, 1, sector);
    if (rc == 0) {
        rc = blk_write(sd, 1, 1, sector);
    }
    k_printf("copy: %s\n", blk_strerror(rc));
}
```

`examples/sdraw` reads and writes raw blocks from UART commands.
