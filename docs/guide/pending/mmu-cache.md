# MMU and caches (`mmu.h`, `cache.h`)

This section belongs in [API.md](../API.md), after the mailbox section. The last part lists the sentences that change in API.md and in the specs.

## MMU (`mmu.h`)

`mmu.h` turns on the MMU, the data cache, the instruction cache and branch prediction with an identity map: every virtual address equals its physical address. Software rendering is several times faster with the data cache on. The map uses 1 MiB sections in one 16 KiB level-1 table inside the image's BSS. An image that never calls these functions does not link the table: `--gc-sections` drops it.

The same image runs on ARM1176 (Pi Zero, Pi 1), Cortex-A7 (Pi 2) and Cortex-A53 in AArch32 (Pi 3, Zero 2 W). The functions pick the CPU from the MIDR part number on each call.

### Init order

| Step | Call | Needs |
|---|---|---|
| 1 | Automatic: `board_get()` | Nothing. Gives the RAM size and peripheral base. |
| 2 | `mmu_init()` | Step 1. |
| 3 | `mmu_map()`, any number of times | Step 2. Also works after step 4. |
| 4 | `mmu_enable()` | Step 2. |

Call it early in `robco_main()`, before `irq_cpu_enable()`. `fb_init()` may come before or after: either order maps the framebuffer with `MMU_ATTR_FB`. Every other component works unchanged with the MMU on. `examples/mmu/main.c` shows the whole sequence. `examples/demo` and `examples/chain1` have `mmu` variants that run every component with the MMU on.

### Memory types

| Constant | Type | Cached | Execute | Access |
|---|---|---|---|---|
| `MMU_ATTR_NONE` | Unmapped | | | Any access faults |
| `MMU_ATTR_RAM` | Normal, write-back write-allocate | Yes | Yes | Read and write |
| `MMU_ATTR_DEVICE` | Shareable device | No | Never | Read and write |
| `MMU_ATTR_FB` | Normal, non-cacheable | No, but writes merge in the write buffer | Never | Read and write |
| `MMU_ATTR_RO_CODE` | Normal, write-back write-allocate | Yes | Yes | Read only at PL1, no user access |

The default map from `mmu_init()`:

| Range | Type |
|---|---|
| `mem_base` to `mem_base + mem_size`, from the mailbox | `MMU_ATTR_RAM` |
| Peripheral base to base + `0x00FFFFFF` | `MMU_ATTR_DEVICE` |
| `0x40000000`-`0x400FFFFF` on BCM2836 and BCM2837 | `MMU_ATTR_DEVICE` (core-local peripherals) |
| The framebuffer, if `fb_init()` succeeded | `MMU_ATTR_FB` |
| Everything else | `MMU_ATTR_NONE` |

The image, its vectors and its stacks are in RAM, so they stay executable. The first megabyte, address `0` included, is RAM: a null pointer read does not fault.

### Framebuffer attributes

The CPU writes the framebuffer and the GPU reads it. A console needs every write to reach the screen without a flush. Write-back caching with a periodic clean does not meet that rule, so the library does not use it.

| SoC | CPU | Framebuffer type | Why |
|---|---|---|---|
| BCM2835 | ARM1176 | Normal non-cacheable (TEX 001, C 0, B 0) | The ARM L1 is not coherent with the GPU. The VideoCore L2 sits behind the ARM, so a non-cacheable write reaches the GPU's view of memory once the write buffer drains. |
| BCM2836 | Cortex-A7 | Normal non-cacheable | The ARM L1 and L2 are not coherent with the GPU. Non-cacheable writes skip both. |
| BCM2837 | Cortex-A53 | Normal non-cacheable | As BCM2836. |

Writes need no flush. The write buffer drains on its own within a few cycles. Call `mmio_dsb()` when the next step depends on the GPU having seen every pixel, for example before a mailbox call that flips buffers. Reads from the framebuffer are slow, as with the cache off: draw in RAM and copy when you read back often. The choice is pending hardware validation.

### Functions

```c
int mmu_init(void);
```

Builds the default map in the table. Does not enable the MMU. Returns `0`, `MMU_ERR_BOARD` on an unknown board or a missing RAM size, or `MMU_ERR_ON` if the MMU is already on. Calling it again with the MMU off rebuilds the map from scratch and drops earlier `mmu_map()` changes. Main context.

```c
int mmu_map(uint32_t start, uint32_t size, unsigned attr);
```

Sets every 1 MiB section that overlaps `start` to `start + size - 1` to `attr`. The range is rounded out to whole sections. Returns `0`, or `MMU_ERR_ARG` for a size of `0`, a range past `0xFFFFFFFF` or an unknown `attr`. With the MMU off it only edits the table. With the MMU on it:

1. Cleans and invalidates the data cache over each section that stops being cacheable, while the old mapping still holds.
2. Writes the descriptors and cleans them to memory. Table walks do not read the data cache.
3. Invalidates the TLB and the branch predictor.
4. Invalidates the data cache over those sections again, through the new mapping. Cortex-A7 and A53 may have prefetched clean lines until step 3.

It masks IRQs while it runs. Main context. Do not unmap the section that holds the running code or the stacks.

```c
void mmu_enable(void);
```

Turns the MMU on with the table from `mmu_init()`. Does nothing if `mmu_init()` has not succeeded or the MMU is already on. In order, with IRQs masked:

1. On Cortex-A7 sets ACTLR.SMP (bit 6), on Cortex-A53 sets CPUECTLR.SMPEN (bit 6), only if clear. The write can trap when the secure firmware locks the register, so it runs through a probe that survives the trap.
2. Invalidates the data cache, the instruction cache, the branch predictor and the TLB. Cache contents are unknown after reset.
3. Sets DACR to domain 0 client, TTBCR to 0 and TTBR0 to the table with non-cacheable walks.
4. Sets SCTLR M, C, I and Z, and clears A, TRE and AFE. On ARM1176 it also sets XP, which selects the ARMv6 descriptor format with execute-never and the read-only AP encoding.

Main context.

```c
void mmu_disable(void);
```

Cleans and invalidates the whole data cache, then clears SCTLR M, C and I, then invalidates the caches, branch predictor and TLB again. It is written in assembly and touches no memory between the clean and the SCTLR write, so it is safe to call from cached code. The identity map keeps the next instruction at the same address after the MMU is off. It masks IRQs while it runs and restores the mask on return. Does nothing if the MMU and data cache are already off. `robco_handoff()` calls it.

```c
int mmu_enabled(void);
```

Returns non-zero when SCTLR.M is set. Any context.

```c
int mmu_smp(void);
```

Returns `1` if the SMP bit is set after `mmu_enable()`, `0` if it is clear because the write trapped or had no effect, and `-1` on ARM1176, which has no SMP bit. QEMU ignores the write, so it reports `0` on `raspi2b` and `raspi3b`.

### Context rules

| Rule | Why |
|---|---|
| Core 0 only. | `start.S` parks the other cores. There is one table and no TLB broadcast. |
| `mmu_init()`, `mmu_map()` and `mmu_enable()` from main context only. | They mask IRQs, but they are not reentrant. |
| Faults need the vectors in mapped memory. | The vectors are in the image, which is RAM. Keep it mapped. |

### Sharp edges

| Edge | What to do |
|---|---|
| A buffer that a DMA engine or the GPU reads or writes | Clean it before the device reads, invalidate it after the device writes. See `cache.h`. |
| Code written to memory, then run | Call `cache_clean_range()` over it, then `cache_invalidate_icache()`. |
| `mmio_probe()` of an unmapped address | Reports an abort, from a translation fault. |
| A store to an unmapped or read-only section outside a probe | Data abort. The exception line shows `dfar` and `dfsr`. Status `0x5` or `0x7` means translation, `0xd` or `0xf` permission. Bit 11 set means a write. |
| Next stage | `robco_handoff()` turns the MMU and caches off and cleans everything. The next stage must call `mmu_init()` and `mmu_enable()` again if it wants them. |

### Example

```c
#include <robco/console.h>
#include <robco/fb.h>
#include <robco/mmu.h>
#include <robco/uart.h>

void robco_main(void)
{
    uart_init(115200);
    if (mmu_init() == 0) {
        mmu_enable();
    }
    fb_init(640, 480, 32);      /* mapped with MMU_ATTR_FB by fb_init() */
    console_init(CONSOLE_SCHEME_GREEN);
    /* ... */
}
```

## Caches (`cache.h`)

`cache.h` holds data cache and instruction cache maintenance for buffers shared with the GPU or a DMA engine, and for code written at run time. `mbox_call()` and `robco_handoff()` already use it.

Every function reads MIDR. ARM1176 uses 32-byte lines and its whole-cache CP15 operations. Cortex-A7 and A53 read the smallest data line size from CTR for range operations and walk every cache level up to the point of coherency by set and way, from CLIDR and CCSIDR, for whole-cache operations. Range operations reach the point of coherency, so the A53 L2 is included.

Every data cache function does nothing while SCTLR.C is clear. All are callable with IRQs enabled and from IRQ context. `CACHE_LINE_MAX` (64) is the largest line of the three CPUs: align and size shared buffers to it.

```c
void cache_clean_range(const void *start, uint32_t size);
```

Writes dirty lines that overlap `start` to `start + size - 1` back to memory. Use it before a device reads the buffer.

```c
void cache_invalidate_range(const void *start, uint32_t size);
```

Discards lines in the range. A line only partly inside the range is cleaned and invalidated instead, so bytes next to the range are never lost. Use it after a device wrote the buffer. If code writes a byte that shares a line with the buffer while the device writes it, that byte or the device data can be lost: keep such buffers line-aligned.

```c
void cache_flush_range(const void *start, uint32_t size);
```

Cleans, then invalidates the range.

```c
void cache_clean_all(void);
```

Cleans the whole data cache. An IRQ that writes during the walk may leave lines dirty.

```c
void cache_invalidate_icache(void);
```

Invalidates the whole instruction cache and the branch predictor. On ARM1176 it applies the erratum 411920 workaround: four invalidates with interrupts masked, then eleven NOPs.

### Mailbox and framebuffer coherence

`mbox_call()` cleans and invalidates the property buffer before the request, so the GPU reads it from memory. After the answer it calls `cache_invalidate_range()`, so the CPU reads the GPU's answer, not a line prefetched during the call. The buffer stays caller-provided and 16-byte aligned. Its edge lines are cleaned, not dropped. Nothing else may write into the cache lines the buffer shares while the call runs. The library's own buffers are 64-byte aligned and padded to whole lines.

`fb_init()` calls `mmu_map(addr, size, MMU_ATTR_FB)` when the image links the MMU module. `mmu_init()` maps the framebuffer when `fb_init()` ran first.

## Changes to fold into the existing documents

### `docs/guide/API.md`

| Where | Now | Replace with |
|---|---|---|
| Conventions, Caches row | The library never turns on the MMU or the data cache. | The library turns on the MMU and caches only when the image calls `mmu_enable()`. Every component works with them on or off. |
| `robco_handoff()` requirements table, last row | Keep the MMU and data cache off, or clean every dirty line. / The function cleans the data cache only on ARM1176. On ARMv7 and later dirty lines are lost. | Remove the row. |
| `robco_handoff()` description | turns the MMU and data cache off if they are on | calls `mmu_disable()` if the MMU or data cache is on, which cleans the whole data cache on every CPU and turns the MMU and both caches off |
| `mbox_call()` | It assumes the data cache is off. | With the data cache on, it cleans the buffer before the request and invalidates it after the answer. Nothing else may write into the cache lines the buffer touches during the call. |
| Exceptions section, the exception line | `... dfsr=0x... dfar=0x...` | `... dfsr=0x... dfar=0x... ifsr=0x...`. `ifsr` is the prefetch abort status. |
| `mmio_probe()` | (add) | With the MMU on, a probe of an unmapped address reports an abort. |
| Header table | (add) | `mmu.h` and `cache.h` rows. |
| Initialisation order | (add) | `mmu_init()` and `mmu_enable()` between steps 3 and 4. |

### `docs/spec/BOOT_PROTOCOL.md`

| Now | Replace with |
|---|---|
| 4. Turns the MMU and D-cache off if they are on. Only on ARM1176 does it clean the D-cache first. | 4. If the MMU or D-cache is on, calls `mmu_disable()`: it cleans and invalidates the whole D-cache on every supported CPU, then turns the MMU, D-cache and I-cache off. |
| The caller must call `robco_handoff()` with the MMU and D-cache off, or with the D-cache already cleaned to memory. On ARMv7 and later the function does not clean the D-cache, so an image that turned it on must clean it, by set and way or by address range over everything it wrote, before the call. The library never turns the MMU or D-cache on, so an image that uses only the library meets this contract. The caller drains the UART with `uart_flush()`. | The caller may call `robco_handoff()` with the MMU and caches on or off. The next stage always starts with them off and every dirty line in memory. An image that turned the caches on by its own code, without `mmu.h`, must clean them first on ARMv7 and later. The caller drains the UART with `uart_flush()`. |
| CPU state table, MMU row: Off | Off, even if the previous stage had it on |
| CPU state table, D-cache row: Cleaned to memory, then off | Cleaned and invalidated, then off |
| CPU state table, I-cache row: Invalidated | Invalidated, then off |

### `docs/spec/MEMORY_MAP.md`

| Now | Replace with |
|---|---|
| All addresses are ARM physical addresses. The MMU, when on, uses an identity map. | All addresses are ARM physical addresses. The MMU, when on, uses an identity map of 1 MiB sections: ARM RAM is normal write-back memory, the peripheral window and the BCM2836/7 local peripherals at `0x40000000` are device memory and execute-never, the framebuffer is normal non-cacheable memory and execute-never, and every other address is unmapped and faults. |
| (add to Rules) | A buffer the GPU or a DMA engine reads or writes is cleaned before and invalidated after, with `cache.h`, and is aligned and padded to 64 bytes. |

## Pending hardware validation

QEMU does not model caches, cache timing, the SMP bit or memory types. These points need a real Pi Zero (ARM1176) and a real Pi 3 (Cortex-A53), and a Pi 2 for Cortex-A7:

- Fill and copy run faster with `caches=on` in `examples/mmu`.
- `mbox_call()` answers stay right in the 100-call loop, with the cache on.
- The framebuffer test pattern and the console show every write without a flush, with no stale or torn rows.
- `robco_handoff()` from a cached stage: the next stage trusts the boot info (its CRC matches) and runs the loaded image.
- `mmu_disable()` from cached code returns, and the mailbox works after it.
- `mmu_smp()` reports `1` under the real firmware. If it reports `0`, the firmware locked the register and data cache coherence must be checked again.
- The ARM1176 I-cache erratum workaround and the XP bit behave as described.
