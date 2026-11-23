# Memory maps

## Rules

- Each image has its own link address. Ranges never overlap within a chain.
- No image relocates itself. An image runs at the address it was linked for.
- Every image carries its own stacks in its own `.stack` section. No image uses a stack set up by the previous stage.
- `mem_size` in the image header covers code, data, BSS and stacks. The whole range must fit in the image's slot. An image that sets `ROBCO_SLOT_END` to the end of its slot fails to link when it does not fit.
- A heap stays inside a range its image owns. An image at `0x8000` puts its heap between `__heap_start` and the end of its slot, `robco_heap_slot()`. An image that owns the range from `0x01000000`, or runs alone, may use it with `robco_heap_high()`. See [the heap section](../guide/api/memory.md#heap-alloch).
- All addresses are ARM physical addresses. The MMU, when on, uses an identity map of 1 MiB sections. See the table below.
- A buffer the GPU or a DMA engine reads or writes is cleaned before and invalidated after, with `cache.h`, and is aligned and padded to 64 bytes, `CACHE_LINE_MAX`.
- RAM above the last listed range, up to `mem_base + mem_size` from the mailbox, belongs to the last owner in the table. RAM above that belongs to the GPU, including the framebuffer.

Memory attributes when an image turns the MMU on with `mmu_init()` and `mmu_enable()`:

| Range | Memory type | Execute |
|---|---|---|
| `mem_base` to `mem_base + mem_size`, ARM RAM from the mailbox | Normal, write-back write-allocate | Yes |
| Peripheral base to base + `0x00FFFFFF` | Shareable device | Never |
| `0x40000000`-`0x400FFFFF` on BCM2836 and BCM2837, the local peripherals | Shareable device | Never |
| The framebuffer, once `fb_init()` succeeded | Normal, non-cacheable | Never |
| Every other address | Unmapped. Any access faults. | No |

Address `0` is RAM, so a NULL read does not fault. The framebuffer type is pending hardware validation. See [the MMU section](../guide/api/memory.md#mmu-mmuh).

The lowest board in the set has 256 MiB of RAM, and `gpu_mem=64` leaves 192 MiB for the ARM. Both maps fit in 32 MiB, so every board can run them.

## Shared low memory

| Range | Owner | Use |
|---|---|---|
| `0x00000000`-`0x000000FF` | Firmware | Firmware data and secondary-core spin code on Pi 2 and Pi 3. Never written. |
| `0x00000100`-`0x00003FFF` | Firmware | ATAGS list from the firmware. Read once at boot. |
| `0x00004000`-`0x00007FFF` | Nobody | Unused on hardware. |

In QEMU tests a firmware stub plays the firmware's part in this range. Its code sits at `0x0` and `0x400`, it writes ATAGS at `0x100`, and the harness writes its parameter block at `0x7F00`. See [TESTING.md](./TESTING.md).

## Terminal chain

| Range | Size | Owner | Use |
|---|---|---|---|
| `0x00008000`-`0x000FFFFF` | 992 KiB | RETROS-BIOS | Image, stacks, BIOS heap |
| `0x00100000`-`0x00100FFF` | 4 KiB | Handoff | `robco_bootinfo_t` at `0x00100000` |
| `0x00101000`-`0x0011FFFF` | 124 KiB | Handoff | DTB or ATAGS copy |
| `0x00120000`-`0x001FFFFF` | 896 KiB | Nobody | Reserved. Must stay unused. |
| `0x00200000`-`0x003FFFFF` | 2 MiB | MFBootAgent | Image, stacks, agent heap |
| `0x00400000`-`0x00FFFFFF` | 12 MiB | OS slot | `UOS.SYS`, `MAINT.SYS` or `DIAG.SYS` |
| `0x01000000`-top of ARM RAM | rest | UOS | UOS heap |

After a stage jumps to the next one, the ranges of earlier stages become dead. In this version no later stage reuses them. The handoff range stays valid for the whole session.

## Pip-Boy chain

| Range | Size | Owner | Use |
|---|---|---|---|
| `0x00008000`-`0x003FFFFF` | 4064 KiB | Pip-OS | Kernel image, kernel stacks, kernel heap, Pip-OS boot info |
| `0x00400000`-`0x007FFFFF` | 4 MiB | ROM slot | `ROM1.ROM`, Deitrix |
| `0x00800000`-`0x00BFFFFF` | 4 MiB | Holotape slot | One `*.HTP` image at a time |
| `0x00C00000`-`0x00FFFFFF` | 4 MiB | Pip-OS | Back buffers for the display |
| `0x01000000`-`0x017FFFFF` | 8 MiB | ROM | ROM heap, ROM stack at the top |
| `0x01800000`-`0x01FFFFFF` | 8 MiB | Holotape | Holotape heap, holotape stack at the top |
| `0x02000000`-top of ARM RAM | rest | Nobody | Reserved for later use |

Pip-OS is the first stage of its chain. It keeps its `robco_bootinfo_t` inside its own range. It does not use the terminal chain's handoff address.

The ROM and holotape stacks are 64 KiB each, at the top of their regions. The rest of each region is heap. See [PIPOS_ABI.md](./PIPOS_ABI.md).

The slots and regions are whole megabytes, so each can be one or more 1 MiB MMU sections. RobCo-Core's [`umode.h`](../guide/api/umode.md) maps the regions a task names for USR mode while it runs and back to privileged-only afterwards. The default map from `mmu_init()` is privileged-only everywhere. `examples/umode` runs its test programs from the ROM slot at `0x00400000`, with the ROM heap and stack region at `0x01000000` as the writable region.

## Lore vs real

The games show numbers that do not fit real hardware. The firmware always loads the kernel at `0x8000`, so a ROM cannot live at `0x10000`. The table maps each lore number to its real counterpart for documentation. Each lore number is the real address shifted right by 6. No stage prints computed lore addresses today. The Pip-OS boot screen prints fixed text, such as `64K RAM SYSTEM`, and no address. If a stage shows such numbers, they are presentation only. No code uses the lore numbers.

| Lore number | Real value | Rule |
|---|---|---|
| 64K RAM | 4 MiB, the Pip-OS region `0x00000000`-`0x003FFFFF` | `0x00400000 >> 6 = 0x10000` bytes = 64 KiB |
| ROM at `0x10000` | ROM slot `0x00400000` | `0x00400000 >> 6` |
| Holotape at `0x20000` | Holotape slot `0x00800000` | `0x00800000 >> 6` |
| Buffers at `0x30000` | Back buffers `0x00C00000` | `0x00C00000 >> 6` |
| Heap at `0x40000` | ROM heap `0x01000000` | `0x01000000 >> 6` |
