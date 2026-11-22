# Pip-OS ABI

This is the contract between Pip-OS and the code it runs: the Deitrix ROM and holotapes. The ABI version is 1.0, `abi_version = 0x00010000` in the image header.

Rows marked "draft" in a status column may change before 1.0 is frozen. Rows marked "stable" will not change within major version 1.

## Entry state

Pip-OS loads the image as described in [BOOT_PROTOCOL.md](./BOOT_PROTOCOL.md) and enters it at `load_addr`.

| Item | ROM | Holotape |
|---|---|---|
| Entry address | `0x00400000` | `0x00800000` |
| Mode | USR | USR |
| IRQ | Enabled | Enabled |
| FIQ | Masked | Masked |
| sp | `0x01800000` | `0x02000000` |
| r0 | Pointer to `pipos_startup_t` | Pointer to `pipos_startup_t` |
| r1 to r3 | 0 | 0 |
| lr | Exit trampoline in Pip-OS | Exit trampoline in Pip-OS |

Returning from the entry function goes to the exit trampoline. It acts like `exit(r0)`.

The image's `crt0.S` zeroes BSS, then calls `pip_main(const pipos_startup_t *)`.

```c
#include <stdint.h>

typedef struct __attribute__((packed)) pipos_startup {
    uint32_t abi_version;
    uint32_t size;
    uint32_t heap_base;
    uint32_t heap_size;
    uint32_t stack_top;
    uint32_t kind;
    uint32_t flags;
    char     args[100];
} pipos_startup_t;

_Static_assert(sizeof(pipos_startup_t) == 128, "pipos_startup_t must be 128 bytes");
```

| Offset | Size | Field | Meaning | Status |
|---|---|---|---|---|
| `0x00` | 4 | `abi_version` | ABI Pip-OS implements, `(major << 16) \| minor` | stable |
| `0x04` | 4 | `size` | `128` | stable |
| `0x08` | 4 | `heap_base` | ROM `0x01000000`, holotape `0x01800000` | stable |
| `0x0C` | 4 | `heap_size` | `0x007F0000`, the region minus the 64 KiB stack | stable |
| `0x10` | 4 | `stack_top` | Same value as sp at entry | stable |
| `0x14` | 4 | `kind` | 0 ROM, 1 holotape | stable |
| `0x18` | 4 | `flags` | Reserved, 0 | draft |
| `0x1C` | 100 | `args` | NUL-terminated argument string from the launcher | draft |
| | 128 | | Total | |

The struct lives in Pip-OS memory. Pip-OS maps it readable from USR mode in sandbox v2.

## Syscall convention

| Item | Rule |
|---|---|
| Instruction | `svc #0` |
| Number | r7 |
| Arguments | r0 to r3. Calls with more arguments take a pointer to a struct. |
| Return | r0. `>= 0` is success. A negative value is `-PIP_E*`. |
| Unknown number | Returns `-PIP_ENOSYS` |
| Preserved | r4 to r11, sp, lr |
| Clobbered | r0 to r3, r12, flags |
| Pointers | Pip-OS checks every pointer and length. The whole range must lie in the caller's image or heap region. Otherwise it returns `-PIP_EFAULT`. |

`libpip.a` wraps each call as a C function with the name `pip_<name>`.

## Error codes

```c
#define PIP_EPERM      1   /* not allowed for this caller */
#define PIP_ENOENT     2   /* file, save or holotape not found */
#define PIP_EIO        5   /* SD card or device error */
#define PIP_EBADF      9   /* bad file handle */
#define PIP_ENOMEM    12   /* out of memory or handles */
#define PIP_EFAULT    14   /* bad pointer or range */
#define PIP_EBUSY     16   /* resource in use */
#define PIP_EEXIST    17   /* already exists */
#define PIP_EINVAL    22   /* bad argument */
#define PIP_ENOSPC    28   /* card full */
#define PIP_ERANGE    34   /* value out of range */
#define PIP_ENOSYS    38   /* unknown syscall */
#define PIP_ENOEXEC   40   /* holotape failed validation */
#define PIP_ENODEV    41   /* hardware not present */
#define PIP_ETIMEDOUT 42   /* wait timed out */
```

## Syscall table

Numbers 0x01 to 0x04, 0x10, 0x11, 0x20, 0x21, 0x30, 0x40, 0x41, 0x50 and 0x51 keep the values Pip-OS already uses. Colours are `0x00RRGGBB`. Coordinates are in the logical 320x240 surface.

| Number | Name | Arguments | Return | Status |
|---|---|---|---|---|
| `0x01` | `draw_line` | `int32 x0, int32 y0, int32 x1, int32 y1` | 0 | stable |
| `0x02` | `draw_point` | `int32 x, int32 y, uint32 color` | 0 | stable |
| `0x03` | `draw_text` | `int32 x, int32 y, const char *text, uint32 scale` | Width drawn in pixels | stable |
| `0x04` | `clear` | `uint32 color` | 0 | stable |
| `0x05` | `gfx_info` | `pip_gfx_info_t *out` | 0 | draft |
| `0x06` | `rect` | `int32 x, int32 y, uint32 wh, uint32 fill`. `wh` is `(w << 16) \| h`. | 0 | draft |
| `0x07` | `polyline` | `const int16 *xy, uint32 count, uint32 closed` | 0 | draft |
| `0x08` | `arc` | `const pip_arc_t *a`: center, radius, start and end angle in degrees, fill | 0 | draft |
| `0x09` | `blit_1bpp` | `const pip_blit_t *b`: x, y, w, h, stride, data, color | 0 | draft |
| `0x0A` | `blit_rgb` | `const pip_blit_t *b`: x, y, w, h, stride, data as `0x00RRGGBB` | 0 | draft |
| `0x0B` | `set_clip` | `int32 x, int32 y, uint32 wh`. `wh = 0` clears the clip. | 0 | draft |
| `0x0C` | `set_color` | `uint32 fg, uint32 bg` | 0 | draft |
| `0x0D` | `text_metrics` | `const char *text, uint32 scale` | `(width << 16) \| height` | draft |
| `0x0E` | `present` | none | 0. Copies the back buffer to the display. | draft |
| `0x0F` | `set_font` | `uint32 font_id` | 0, or `-PIP_EINVAL` | draft |
| `0x10` | `read_buttons` | none | Bit mask of pressed buttons | stable |
| `0x11` | `read_dial` | none | Dial position, 0 to 359 | stable |
| `0x12` | `poll_event` | `pip_event_t *out` | 1 if an event was read, 0 if none | draft |
| `0x13` | `wait_event` | `pip_event_t *out, uint32 timeout_ms` | 1, or `-PIP_ETIMEDOUT` | draft |
| `0x20` | `tone` | `uint32 freq_hz, uint32 duration_ms` | 0 | stable |
| `0x21` | `sample` | `const uint8 *data, uint32 length` | 0. 8-bit unsigned, 8000 Hz. | stable |
| `0x22` | `audio_stop` | none | 0 | draft |
| `0x23` | `volume` | `uint32 level`, 0 to 100 | Previous level | draft |
| `0x24` | `audio_status` | none | 1 if playing, 0 if idle | draft |
| `0x25` | `play_seq` | `const pip_note_t *notes, uint32 count` | 0 | draft |
| `0x30` | `read_sensor` | `uint32 sensor, uint32 *value` | 0, or `-PIP_ENODEV` | stable |
| `0x31` | `sensor_available` | `uint32 sensor` | 1 or 0 | stable |
| `0x40` | `get_time` | `uint32 *unix_seconds` | 0 | stable |
| `0x41` | `get_battery` | none | Percent, 0 to 100, or `-PIP_ENODEV` | stable |
| `0x42` | `ticks_ms` | none | Milliseconds since boot, wraps at 2^31 | draft |
| `0x43` | `sleep_ms` | `uint32 ms` | 0 | draft |
| `0x44` | `exit` | `int32 status` | Does not return | draft |
| `0x45` | `sys_info` | `pip_sys_info_t *out`: board, ABI, Pip-OS version | 0 | draft |
| `0x46` | `mem_info` | `pip_mem_info_t *out`: heap base, size | 0 | draft |
| `0x47` | `power` | `uint32 op`: 0 reboot, 1 halt, 2 screen off, 3 screen on | 0, or does not return | draft |
| `0x48` | `log` | `const char *text` | 0. Prints on the PL011. | draft |
| `0x49` | `rand` | none | Random value, 0 to 2^31 - 1 | draft |
| `0x4A` | `set_time` | `uint32 unix_seconds` | 0, or `-PIP_EPERM` for holotapes | draft |
| `0x50` | `read_save` | `const char *name, void *buf, uint32 size` | Bytes read | stable number, draft arguments |
| `0x51` | `write_save` | `const char *name, const void *buf, uint32 size` | Bytes written | stable number, draft arguments |
| `0x52` | `save_info` | `const char *name, uint32 *size` | 0 | draft |
| `0x53` | `save_delete` | `const char *name` | 0 | draft |
| `0x54` | `file_open` | `const char *path` | Handle `>= 0`. Read only. | draft |
| `0x55` | `file_read` | `int32 handle, void *buf, uint32 size` | Bytes read, 0 at end | draft |
| `0x56` | `file_close` | `int32 handle` | 0 | draft |
| `0x57` | `dir_list` | `const char *path, pip_dirent_t *out, uint32 max, uint32 start` | Entries written | draft |
| `0x60` | `holo_enumerate` | `pip_holo_info_t *out, uint32 max` | Entries written | draft |
| `0x61` | `holo_launch` | `const char *name, const char *args` | Holotape exit status. ROM only. | draft |
| `0x62` | `holo_last_status` | none | Last exit status, or `-PIP_ENOENT` | draft |
| `0x63` | `holo_eject_event` | none | 1 if a holotape was inserted or removed since the last call | draft |

The existing Pip-OS calls `read_save` and `write_save` took a byte offset. They now take a save name, so their argument lists are draft.

Buttons for `read_buttons`: bit 0 up, 1 down, 2 left, 3 right, 4 select, 5 dial press. Sensors for `read_sensor`: 0 RTC, 1 battery, 2 geiger, 3 GPS, 4 accelerometer, 5 temperature, 6 heart rate. These values come from Pip-OS and are stable.

Holotapes cannot call `holo_launch` or `power`. They get `-PIP_EPERM`.

File calls are read only. They reach the whole boot partition except `/PIPBOY/SAVE/`, which only the save calls touch.

## Display

Every image draws on a logical surface of 320x240 pixels. `gfx_info` reports it.

```c
typedef struct __attribute__((packed)) pip_gfx_info {
    uint16_t width;    /* 320 */
    uint16_t height;   /* 240 */
    uint32_t format;   /* 0: 0x00RRGGBB */
    uint32_t backend;  /* 0 HDMI framebuffer, 1 SPI TFT */
    uint32_t reserved;
} pip_gfx_info_t;     /* 16 bytes */
```

| Offset | Size | Field |
|---|---|---|
| `0x00` | 2 | `width` |
| `0x02` | 2 | `height` |
| `0x04` | 4 | `format` |
| `0x08` | 4 | `backend` |
| `0x0C` | 4 | `reserved` |

Drawing goes to a back buffer. `present` shows it. On HDMI, Pip-OS scales the surface by 2 into the 640x480 framebuffer. On the SPI TFT it copies 1:1. The surface is the same on both backends, so one set of golden images covers both.

## Save files

- Path: `/PIPBOY/SAVE/<NAME>.SAV`. `NAME` is 1 to 8 characters from `A-Z`, `0-9` and `_`. Pip-OS upper-cases it. Anything else returns `-PIP_EINVAL`.
- A save is at most 64 KiB.
- `write_save` writes `/PIPBOY/SAVE/<NAME>.TMP`, syncs it, deletes the old `.SAV` and renames `.TMP` to `.SAV`. A power cut leaves either the old save or the new one.
- At boot, Pip-OS deletes leftover `.TMP` files whose `.SAV` exists, and renames them to `.SAV` otherwise.
- ROMs and holotapes share the save folder. A holotape should use its own name prefix.

## Sandbox

The sandbox is weak in v1. This section says what it really does.

| Level | What it gives | What it does not give |
|---|---|---|
| v1 | Code runs in USR mode. Pip-OS checks the CRC before running it. Code is expected to use only syscalls. | No memory protection. USR code can read and write any RAM and any peripheral. A crashing holotape stops the device. |
| v2 | An identity map of 1 MiB sections. Kernel and MMIO are privileged only. The ROM slot is read only to holotapes. A holotape that faults is killed, and control returns to Deitrix with `-PIP_EFAULT` from `holo_launch`. | No protection between a ROM and its own heap. No limit on CPU time. |

v2 depends on the cache and MMU setup proven by the framebuffer spike. Its behaviour on real BCM2837 hardware is pending hardware validation.

RobCo-Core provides the mechanism for both levels in [`umode.h`](../guide/api/umode.md): `umode_run()` runs a program in USR mode and returns on exit, fault or kill, the `svc #0` entry hands each call to a handler, `umode_range_ok()` checks pointers, and with the MMU on the task's regions are the only user-accessible memory. A watchdog built on `umode_request_kill()` can also limit CPU time. Pip-OS decides how it uses them: the syscall table, the regions per level and the policy are its own. The library's system-call path preserves more than this ABI asks: every register but r0, and the flags.

## SDK

Pip-OS ships an SDK in its `sdk/` folder.

| File | Use |
|---|---|
| `include/pipos/*.h` | Syscall numbers, structs, error codes, `pip_*` wrappers |
| `crt0.S` | Branch stub, `rimg` header, META area, BSS clear, call to `pip_main` |
| `rom.ld` | Links at `0x00400000` |
| `holotape.ld` | Links at `0x00800000` |
| `libpip.a` | Syscall wrappers and a small libc |
| `pipos-app.mk` | Make fragment: flags, link, `rimg.py seal` |

A ROM project, such as Deitrix, adds Pip-OS as its only submodule at `external/pip-os`. It reaches RobCo-Core through Pip-OS's own submodule, so both use the same RobCo-Core commit. Its Makefile looks like this:

```make
PIPOS_SDK := external/pip-os/sdk
IMAGE     := ROM1.ROM
KIND      := rom            # or holotape
NAME      := DEITRIX
SRCS      := src/main.c src/ui.c
include $(PIPOS_SDK)/pipos-app.mk
```

`pipos-app.mk` sets magic and subtype from `KIND`, builds with the RobCo-Core flags, links with `rom.ld` or `holotape.ld`, and calls `tools/rimg.py seal`. ROM and holotape code must not include RobCo-Core driver headers. It talks to hardware only through syscalls.
