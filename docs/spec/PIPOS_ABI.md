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
| lr | Exit trampoline | Exit trampoline |

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

Pip-OS writes the struct and the exit trampoline at the bottom of the image's stack area, at `stack_top - 0x10000`. Both sit in the image's own memory, so the image can overwrite them. Pip-OS never reads them back. An image that overwrites the trampoline must end with `exit`.

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
| `0x40` | `get_time` | `uint32 *unix_seconds` | 0, or `-PIP_ENODEV` when no clock is set | stable |
| `0x41` | `get_battery` | none | Percent, 0 to 100, or `-PIP_ENODEV` | stable |
| `0x42` | `ticks_ms` | none | Milliseconds since boot, wraps at 2^31 | draft |
| `0x43` | `sleep_ms` | `uint32 ms` | 0, or `-PIP_ERANGE` above `0x7FFFFFFF` | draft |
| `0x44` | `exit` | `int32 status` | Does not return. The exit status is the low 8 bits. | draft |
| `0x45` | `sys_info` | `pip_sys_info_t *out`: board, ABI, Pip-OS version | 0 | draft |
| `0x46` | `mem_info` | `pip_mem_info_t *out`: heap base, size | 0 | draft |
| `0x47` | `power` | `uint32 op`: 0 reboot, 1 halt, 2 screen off, 3 screen on | 0, does not return, or `-PIP_EINVAL` | draft |
| `0x48` | `log` | `const char *text` | 0. Prints on the PL011. | draft |
| `0x49` | `rand` | none | Random value, 0 to 2^31 - 1, or `-PIP_EIO` | draft |
| `0x4A` | `set_time` | `uint32 unix_seconds` | 0. ROM only. | draft |
| `0x50` | `read_save` | `const char *name, void *buf, uint32 size` | Bytes read | stable number, draft arguments |
| `0x51` | `write_save` | `const char *name, const void *buf, uint32 size` | Bytes written | stable number, draft arguments |
| `0x52` | `save_info` | `const char *name, uint32 *size` | 0 | draft |
| `0x53` | `save_delete` | `const char *name` | 0 | draft |
| `0x54` | `file_open` | `const char *path` | Handle `>= 0`. Read only. | draft |
| `0x55` | `file_read` | `int32 handle, void *buf, uint32 size` | Bytes read, 0 at end | draft |
| `0x56` | `file_close` | `int32 handle` | 0 | draft |
| `0x57` | `dir_list` | `const char *path, pip_dirent_t *out, uint32 max, uint32 start` | Entries written | draft |
| `0x60` | `holo_enumerate` | `pip_holo_info_t *out, uint32 max` | Entries written | draft |
| `0x61` | `holo_launch` | `const char *name, const char *args` | Holotape exit status, 0 to 255. ROM only. | draft |
| `0x62` | `holo_last_status` | none | Result of the last `holo_launch`, or `-PIP_ENOENT` | draft |
| `0x63` | `holo_eject_event` | none | 1 if a holotape was inserted or removed since the last call. Always 0 in ABI 1.0. | draft |

The existing Pip-OS calls `read_save` and `write_save` took a byte offset. They now take a save name, so their argument lists are draft.

Buttons for `read_buttons`: bit 0 up, 1 down, 2 left, 3 right, 4 select, 5 dial press. Sensors for `read_sensor`: 0 RTC, 1 battery, 2 geiger, 3 GPS, 4 accelerometer, 5 temperature, 6 heart rate. These values come from Pip-OS and are stable.

Holotapes cannot call `holo_launch`, `power` or `set_time`. They get `-PIP_EPERM`.

A call that this version of Pip-OS does not implement returns `-PIP_ENOSYS`, like an unknown number.

## Display

Every image draws on a logical surface of 320x240 pixels. `gfx_info` reports it.

```c
typedef struct __attribute__((packed)) pip_gfx_info {
    uint16_t width;    /* 320 */
    uint16_t height;   /* 240 */
    uint32_t format;   /* 0: 0x00RRGGBB */
    uint32_t backend;  /* 0 HDMI framebuffer, 1 SPI TFT */
    uint32_t fg_default; /* colour of the configured screen scheme */
} pip_gfx_info_t;     /* 16 bytes */
```

| Offset | Size | Field |
|---|---|---|
| `0x00` | 2 | `width` |
| `0x02` | 2 | `height` |
| `0x04` | 4 | `format` |
| `0x08` | 4 | `backend` |
| `0x0C` | 4 | `fg_default` |

Drawing goes to a back buffer. `present` shows it. On HDMI, Pip-OS scales the surface by 2 into the 640x480 framebuffer. On the SPI TFT it copies 1:1. The surface is the same on both backends, so one set of golden images covers both.

### Drawing state

Each image has its own drawing state. Pip-OS resets it when the image starts.

| State | Set by | Start value |
|---|---|---|
| Foreground colour | `set_color` | `fg_default` from `gfx_info` |
| Background colour | `set_color` | `PIP_COLOR_NONE` |
| Clip rectangle | `set_clip` | The whole surface |
| Font | `set_font` | 0 |

`PIP_COLOR_NONE` is `0x01000000`. As a background it means transparent.

Every call clips to the clip rectangle and to the surface. Coordinates outside are legal and draw nothing. The back buffer starts black.

### Drawing calls

| Call | Rules |
|---|---|
| `draw_line` | Foreground colour. Both end points are drawn. |
| `draw_point` | Uses the colour argument, not the foreground colour. |
| `draw_text` | Glyphs in the foreground colour. If the background is not `PIP_COLOR_NONE`, each glyph cell is filled with it first. `x, y` is the top left corner. `scale` is 1 to 8. The text is at most 255 bytes. Bytes outside printable ASCII draw as `?`. Returns the width in pixels. |
| `text_metrics` | Same limits as `draw_text`. Draws nothing. |
| `clear` | Fills the whole surface with the colour argument. Ignores the clip rectangle. |
| `rect` | `fill` 0 draws the outline, 1 fills. Foreground colour. A zero width or height draws nothing. |
| `polyline` | `xy` holds `count` pairs of `int16 x, y`. `count` is 2 to 256. `closed` 1 joins the last point to the first. Foreground colour. |
| `arc` | See `pip_arc_t`. Foreground colour. |
| `blit_1bpp` | See `pip_blit_t`. Rows are packed most significant bit first. Set bits draw in `color`. Clear bits draw nothing. |
| `blit_rgb` | See `pip_blit_t`. Each pixel is a `uint32` `0x00RRGGBB`. `color` is ignored. |
| `set_clip` | `wh = 0` resets the clip to the whole surface. The rectangle is cut to the surface. |
| `set_font` | Font 0 is the 8x16 system font. ABI 1.0 has no other font, so any other value returns `-PIP_EINVAL`. |
| `present` | Copies the back buffer to the display. The back buffer keeps its content. |

A bad limit, such as `scale` 9 or `count` 1, returns `-PIP_EINVAL`.

```c
typedef struct __attribute__((packed)) pip_arc {
    int16_t  cx, cy;      /* center */
    uint16_t radius;      /* 1 to 512 */
    uint16_t start_deg;   /* 0 to 359 */
    uint16_t end_deg;     /* 0 to 360 */
    uint16_t fill;        /* 0 outline, 1 filled slice */
} pip_arc_t;              /* 12 bytes */

typedef struct __attribute__((packed)) pip_blit {
    int16_t  x, y;        /* top left corner on the surface */
    uint16_t w, h;        /* 1 to 320, 1 to 240 */
    uint16_t stride;      /* bytes from one row to the next */
    uint16_t flags;       /* 0 */
    uint32_t data;        /* pointer to the first row */
    uint32_t color;       /* blit_1bpp only */
} pip_blit_t;             /* 20 bytes */
```

Angles are in degrees, clockwise on the screen, from the positive x axis. So 90 points down. The arc runs clockwise from `start_deg` to `end_deg`. If `end_deg` is not above `start_deg`, it passes through 0. `start_deg` 0 with `end_deg` 360 is a full circle.

A row of `blit_1bpp` is `(w + 7) / 8` bytes. A row of `blit_rgb` is `4 * w` bytes. `stride` must be at least one row. Pip-OS checks the range from `data` to the end of the last row.

## Input

`read_buttons` returns the buttons that are down now. `read_dial` returns the dial position in degrees, 0 to 359. One detent of the dial is 15 degrees.

Events come from a queue of 32. When the queue is full, the oldest event is dropped. The queue is emptied when an image starts.

```c
typedef struct __attribute__((packed)) pip_event {
    uint32_t type;      /* PIP_EV_* */
    uint32_t code;
    int32_t  value;
    uint32_t time_ms;   /* ticks_ms when the event happened */
} pip_event_t;          /* 16 bytes */
```

| `type` | Name | `code` | `value` |
|---|---|---|---|
| 1 | `PIP_EV_BUTTON` | Button number, the bit position from `read_buttons` | 1 press, 0 release, 2 repeat while held |
| 2 | `PIP_EV_DIAL` | 0 | Signed detents. Positive is clockwise. |

`wait_event` blocks until an event arrives or `timeout_ms` passes. A timeout of 0 does not block. `0xFFFFFFFF` waits forever. Any other value above `0x7FFFFFFF` returns `-PIP_ERANGE`.

## Audio

There is one voice. `tone`, `sample` and `play_seq` return at once and the sound plays in the background. A new sound replaces the one that is playing.

| Call | Rules |
|---|---|
| `tone` | `freq_hz` 20 to 20000. `duration_ms` 1 to 60000. |
| `sample` | 8-bit unsigned mono at 8000 Hz. At most 65536 bytes. Pip-OS copies the data before it returns. |
| `play_seq` | `count` is 1 to 256. Pip-OS copies the notes before it returns. |
| `volume` | 0 to 100. Returns the previous level. The start value comes from `PIPOS.CFG`. |
| `audio_status` | 1 while a sound plays. |
| `audio_stop` | Stops the sound. |

A value outside these limits returns `-PIP_EINVAL`.

```c
typedef struct __attribute__((packed)) pip_note {
    uint16_t freq_hz;      /* 20 to 20000, or 0 for a rest */
    uint16_t duration_ms;  /* 1 to 60000 */
} pip_note_t;              /* 4 bytes */
```

## Time, sensors and power

Pip-OS keeps a software clock in Unix seconds. At boot it reads a real-time clock chip if one answers. `set_time` sets the software clock. It does not write the chip. With no chip and no `set_time`, the clock is not set and `get_time` returns `-PIP_ENODEV`.

`read_sensor` with sensor 0 returns the same value as `get_time`. No other sensor exists on the current hardware, so sensors 1 to 6 return `-PIP_ENODEV`, and so does `get_battery`. `sensor_available` returns 1 only when `read_sensor` would succeed.

`power` with 2 turns the screen black until 3 turns it back on. Drawing and `present` keep working on the back buffer in between. Any other `op` above 3 returns `-PIP_EINVAL`.

## System information

```c
typedef struct __attribute__((packed)) pip_sys_info {
    uint32_t size;           /* 32 */
    uint32_t abi_version;    /* (major << 16) | minor */
    uint32_t pipos_version;  /* (major << 16) | (minor << 8) | patch */
    uint32_t board_rev;      /* firmware board revision code */
    uint32_t soc;            /* value from BOARD_SUPPORT.md */
    uint32_t mem_size;       /* RAM in bytes */
    uint32_t reserved[2];    /* 0 */
} pip_sys_info_t;            /* 32 bytes */

typedef struct __attribute__((packed)) pip_mem_info {
    uint32_t heap_base;
    uint32_t heap_size;
    uint32_t stack_top;
    uint32_t stack_size;     /* 0x00010000 */
} pip_mem_info_t;            /* 16 bytes */
```

`log` takes at most 255 bytes of text. With no NUL in the first 256 bytes it returns `-PIP_EINVAL`. Bytes other than printable ASCII, newline and tab print as `?`.

`sleep_ms` sleeps at least `ms` milliseconds.

`rand` returns `-PIP_EIO` when the hardware generator fails.

## Save files

- Path: `/PIPBOY/SAVE/<NAME>.SAV`. `NAME` is 1 to 8 characters from `A-Z`, `0-9` and `_`. Pip-OS upper-cases it. Anything else returns `-PIP_EINVAL`.
- A save is at most 64 KiB.
- `write_save` replaces the file with RobCo-Core's atomic write. A power cut leaves the old save or the new one. A larger `size` returns `-PIP_ERANGE`.
- The atomic write keeps work files named `~<NAME>.SAV` and `#<NAME>.SAV` in the folder. Pip-OS recovers from them before it reads a save. The developer guide in RobCo-Core describes the steps.
- `read_save` copies at most `size` bytes and returns the count. A missing save returns `-PIP_ENOENT`.
- `save_info` stores the save's size. `save_delete` removes the save and its work files.
- ROMs and holotapes share the save folder. A holotape should use its own name prefix.

## Files

File calls are read only.

- A path is absolute, uses `/`, and is at most 127 bytes. Names follow the 8.3 rule. Pip-OS upper-cases the path.
- A path in `/PIPBOY/SAVE` returns `-PIP_EPERM`.
- An image can hold 8 open files. Handles are 0 to 7. `file_open` returns `-PIP_ENOMEM` when all are in use. Pip-OS closes an image's files when it ends.
- `file_read` returns the bytes read, and 0 at the end of the file.
- `dir_list` writes at most `max` entries, starting at entry number `start`, and returns how many it wrote. It returns 0 past the end. `max` is 1 to 64.

```c
typedef struct __attribute__((packed)) pip_dirent {
    char     name[13];   /* 8.3 name, NUL terminated */
    uint8_t  flags;      /* bit 0: directory */
    uint16_t reserved;   /* 0 */
    uint32_t size;       /* bytes, 0 for a directory */
} pip_dirent_t;          /* 20 bytes */
```

## Holotapes

A holotape is a file `/PIPBOY/HOLO/<NAME>.HTP` in the `ROBCO78` image format.

`holo_enumerate` lists the files whose header passes the magic, version and size checks. It does not check the CRC. It writes at most `max` entries, 1 to 32, and returns the count.

```c
typedef struct __attribute__((packed)) pip_holo_info {
    char     file[13];        /* 8.3 file name, for holo_launch */
    uint8_t  subtype;         /* 0 game, 1 utility, 2 data */
    uint16_t reserved;        /* 0 */
    uint32_t size;            /* file size in bytes */
    uint32_t image_version;
    char     name[16];        /* from the image header */
    char     title[64];       /* from the META block, or empty */
    uint8_t  icon[128];       /* from the META block, or zero */
} pip_holo_info_t;            /* 232 bytes */
```

`holo_launch` takes a file name from `holo_enumerate`, without a folder, and an argument string of at most 99 bytes. It runs these steps:

1. Load and validate the file, with every step from [IMAGE_FORMAT.md](./IMAGE_FORMAT.md). A failure returns `-PIP_ENOENT` for a missing file and `-PIP_ENOEXEC` for a bad one.
2. Stop the sound, empty the event queue, and give the holotape a fresh drawing state.
3. Run the holotape. The ROM waits inside `holo_launch`.
4. When the holotape ends, close its files, stop the sound, empty the event queue and restore the ROM's drawing state.

`holo_launch` returns the holotape's exit status, 0 to 255, or `-PIP_EFAULT` when Pip-OS killed it. The back buffer holds whatever the holotape drew, so the ROM redraws its screen.

`holo_last_status` returns what the last `holo_launch` returned, or `-PIP_ENOENT` when none ran since boot.

Holotapes are files on the card, so nothing can be inserted or removed while Pip-OS runs. `holo_eject_event` returns 0. It is kept for hardware with a tape slot.

## Sandbox

Pip-OS runs every image in USR mode behind an identity map of 1 MiB sections.

| Memory | ROM | Holotape |
|---|---|---|
| Pip-OS kernel, back buffers | No access | No access |
| Peripherals (MMIO) | No access | No access |
| ROM slot | Read and write | Read only |
| ROM heap and stack | Read and write | No access |
| Holotape slot | No access | Read and write |
| Holotape heap and stack | No access | Read and write |

- Pip-OS checks the image's CRC before it runs it.
- An image that faults is ended. A fault is a bad memory access or an undefined instruction.
- When a holotape faults, control returns to the ROM and `holo_launch` returns `-PIP_EFAULT`.
- When the ROM faults, Pip-OS shows the fault on screen and stops running the ROM.

The sandbox does not protect an image from itself: its code, heap and stack are all writable. It sets no limit on CPU time, so an image that loops forever holds the device.

The map depends on the cache and MMU setup proven by the framebuffer spike. Its behaviour on real BCM2837 hardware is pending hardware validation.

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
