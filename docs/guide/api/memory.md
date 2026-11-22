# Memory, MMU and caches

This page covers the heap, the MMU with its identity map, and cache maintenance. See [Conventions](../API.md#conventions) for error codes, contexts and init rules.

## Heap (`alloc.h`)

`alloc.h` is a first-fit allocator over one memory region you give it. Blocks are 8-byte aligned. Headers carry a checksum, so a stray or double free is counted instead of corrupting the heap. The code lives in `src/lib/` and runs on the host in the unit tests.

```c
void k_heap_init(void *base, size_t size);
```

Takes over `size` bytes at `base`, trimmed to 8-byte alignment. A second call starts a new empty heap. A NULL `base` or a region too small for one block leaves the heap empty, with no error. `k_heap_init()` does not check the region against the memory map. Get it from one of the two helpers below.

Heap rule: a heap stays inside a range its image owns in [the memory map](../../spec/MEMORY_MAP.md). For an image at `0x8000` the slot ends at `0x00100000`, where the boot info handoff area starts, so a heap there is less than 1 MiB.

| Constant | Value | Meaning |
|---|---|---|
| `ROBCO_HEAP_HIGH_BASE` | `0x01000000u` | Start of the OS heap range in the memory map |

```c
size_t robco_heap_slot(void **base);
size_t robco_heap_high(void **base);
```

Both live in `src/board/heap_region.c`. Each stores the start in `*base` and returns the size in bytes, or `0` when there is no such region. `robco_heap_slot()` gives everything from `__heap_start` up to the end of the image's slot, `ROBCO_SLOT_END`. It returns `0` when the image does not set `ROBCO_SLOT_END`. `robco_heap_high()` gives `ROBCO_HEAP_HIGH_BASE` up to the top of ARM RAM from the mailbox. Use it only in the image that owns that range in its chain, the OS in the terminal chain, or in an image that runs alone. From `examples/heap/main.c`:

```c
size = robco_heap_slot(&base);
k_heap_init(base, size);
```

```c
void *k_malloc(size_t n);
void *k_calloc(size_t n, size_t size);
void *k_realloc(void *p, size_t n);
void k_free(void *p);
```

Standard contracts. `k_malloc(0)` returns NULL. `k_free(NULL)` does nothing. `k_realloc(NULL, n)` is `k_malloc(n)`, and `k_realloc(p, 0)` frees `p` and returns NULL. `k_calloc()` returns NULL when `n * size` overflows. `k_free()` and `k_realloc()` ignore a pointer the heap did not hand out or one already freed, and count it in `bad_frees`. All allocators return NULL when no block is large enough or before `k_heap_init()`. Before `k_heap_init()`, `k_free()` counts every pointer as a bad free.

```c
typedef struct {
    size_t total, used, free, largest_free, allocs;
    size_t bad_frees;
} k_heap_stats_t;

void k_heap_stats(k_heap_stats_t *out);
```

`total` is the whole region, headers included. `used`, `free` and `largest_free` count payload bytes only. `allocs` is the number of live blocks.

```c
int k_heap_check(void);
```

Walks every block and the free list. Returns `0` if consistent, a negative code otherwise.

The heap has no lock. If any IRQ handler or tick callback can touch it, mask IRQs around every call, as `examples/demo/main.c` does:

```c
static void *heap_alloc(size_t n)
{
    uint32_t s = irq_save();
    void *p = k_malloc(n);

    irq_restore(s);
    return p;
}
```

## MMU (`mmu.h`)

`mmu.h` turns on the MMU, the data cache, the instruction cache and branch prediction with an identity map: every virtual address equals its physical address. Software rendering is several times faster with the data cache on. The map uses 1 MiB sections in one 16 KiB level-1 table in the image's BSS. An image that never calls these functions does not link the table: `--gc-sections` drops it.

The same image runs on ARM1176 (Pi Zero, Pi 1), Cortex-A7 (Pi 2) and Cortex-A53 in AArch32 (Pi 3, Zero 2 W). The functions pick the CPU from the MIDR part number.

### Init order

| Step | Call | Needs |
|---|---|---|
| 1 | Automatic: `board_get()` | Nothing. Gives the RAM size and peripheral base. |
| 2 | `mmu_init()` | Step 1. |
| 3 | `mmu_map()`, any number of times | Step 2. Also works after step 4. |
| 4 | `mmu_enable()` | Step 2. |

Call them early in `robco_main()`, before `irq_cpu_enable()`. `fb_init()` may come before or after: either order maps the framebuffer with `MMU_ATTR_FB`. Every other component works unchanged with the MMU on. [`examples/mmu/main.c`](../../../examples/mmu/main.c) shows the whole sequence. `examples/demo` (`build/mmu`, `-DDEMO_MMU=1`) and `examples/chain1` (`build/mmu` and `build/mmufb`, `-DCHAIN1_MMU=1`) have variants that run every component with the MMU on.

### Memory types

| Constant | Value | Type | Cached | Execute | Access |
|---|---|---|---|---|---|
| `MMU_ATTR_NONE` | `0` | Unmapped | | | Any access faults |
| `MMU_ATTR_RAM` | `1` | Normal, write-back write-allocate (TEX 001, C 1, B 1) | Yes | Yes | Read and write at PL1, no user access |
| `MMU_ATTR_DEVICE` | `2` | Shareable device (TEX 000, C 0, B 1) | No | Never | Read and write at PL1, no user access |
| `MMU_ATTR_FB` | `3` | Normal, non-cacheable (TEX 001, C 0, B 0) | No, writes merge in the write buffer | Never | Read and write at PL1, no user access |
| `MMU_ATTR_RO_CODE` | `4` | Normal, write-back write-allocate (TEX 001, C 1, B 1) | Yes | Yes | Read only at PL1, no user access |

Every section uses domain 0. TEX remap and the access flag are off.

### Permissions

Every section is privileged-only by default: code in USR mode faults on any access. The library runs in SVC mode, so this changes nothing for it. To give USR mode access, OR one permission flag into the type passed to `mmu_map()`. [`umode.h`](umode.md) does this for a task's regions.

| Constant | Value | Meaning |
|---|---|---|
| `MMU_ATTR_TYPE_MASK` | `0xffu` | The memory type bits of `attr` |
| `MMU_USER_RW` | `0x100u` | USR mode may read and write |
| `MMU_USER_RO` | `0x200u` | USR mode may read |

The access permission bits AP[2:0] (APX and AP[1:0]) per attribute. They mean the same on ARMv6 (with SCTLR.XP) and ARMv7.

| Attribute | AP[2:0] | Privileged | User |
|---|---|---|---|
| `MMU_ATTR_RAM` | `001` | Read and write | None |
| `MMU_ATTR_RAM \| MMU_USER_RO` | `010` | Read and write | Read |
| `MMU_ATTR_RAM \| MMU_USER_RW` | `011` | Read and write | Read and write |
| `MMU_ATTR_RO_CODE` | `101` | Read | None |
| `MMU_ATTR_RO_CODE \| MMU_USER_RO` | `110` | Read | Read |
| `MMU_ATTR_DEVICE`, `MMU_ATTR_FB` | `001` | Read and write | None |

`mmu_map()` returns `MMU_ERR_ARG` for a user flag on device or framebuffer memory, for `MMU_ATTR_RO_CODE | MMU_USER_RW`, for both flags at once and for unknown bits. RAM and read-only code with a user flag stay executable from USR mode.

| Constant | Value | Meaning |
|---|---|---|
| `MMU_SECTION_SIZE` | `0x00100000u` | One section, 1 MiB |
| `MMU_ERR_BOARD` | `-1` | Unknown board or no RAM size from the mailbox |
| `MMU_ERR_ARG` | `-2` | Bad range or attribute |
| `MMU_ERR_ON` | `-3` | `mmu_init()` while the MMU is on |

The default map from `mmu_init()`:

| Range | Type |
|---|---|
| `mem_base` to `mem_base + mem_size - 1`, ARM RAM from the mailbox | `MMU_ATTR_RAM` |
| Peripheral base to base + `0x00FFFFFF` | `MMU_ATTR_DEVICE` |
| `0x40000000` to `0x400FFFFF` on BCM2836 and BCM2837 | `MMU_ATTR_DEVICE` (core-local peripherals) |
| The framebuffer, if `fb_init()` succeeded first | `MMU_ATTR_FB` |
| Everything else | `MMU_ATTR_NONE` |

The image, its vectors and its stacks are in RAM, so they stay executable. The first megabyte, address `0` included, is RAM: a null pointer read does not fault.

### Framebuffer attributes

The CPU writes the framebuffer and the GPU reads it. A console needs every write to reach the screen without a flush. Write-back caching with a periodic clean does not meet that rule, so the library does not use it.

| SoC | CPU | Framebuffer type | Why |
|---|---|---|---|
| BCM2835 | ARM1176 | Normal non-cacheable | The ARM L1 is not coherent with the GPU. The VideoCore L2 sits behind the ARM, so a non-cacheable write reaches the GPU's view of memory once the write buffer drains. |
| BCM2836 | Cortex-A7 | Normal non-cacheable | The ARM L1 and L2 are not coherent with the GPU. Non-cacheable writes skip both. |
| BCM2837 | Cortex-A53 | Normal non-cacheable | As BCM2836. |

Writes need no flush. The write buffer drains on its own within a few cycles. Call `mmio_dsb()` when the next step depends on the GPU having seen every pixel, for example before a mailbox call that flips buffers. Reads from the framebuffer are slow, as with the cache off: draw in RAM and copy when you read back often.

### Functions

```c
int mmu_init(void);
```

Builds the default map in the table. Does not enable the MMU. Returns `0`, `MMU_ERR_BOARD` when the board has no peripheral base or no RAM size, or `MMU_ERR_ON` if the MMU is already on. Calling it again with the MMU off rebuilds the map from scratch and drops earlier `mmu_map()` changes. Main context.

```c
int mmu_map(uint32_t start, uint32_t size, unsigned attr);
```

Sets every 1 MiB section that overlaps `start` to `start + size - 1` to `attr`. The range is rounded out to whole sections. Returns `0`, or `MMU_ERR_ARG` for a size of `0`, a range past `0xFFFFFFFF` or an unknown or invalid `attr` (see [permissions](#permissions)). It does not need `mmu_init()` first, but `mmu_init()` clears the table. With the MMU off it only edits the table. With the MMU on it:

1. Cleans and invalidates the data cache over each section that stops being cacheable, while the old mapping still holds.
2. Writes the descriptors and cleans them to memory. Table walks do not read the data cache.
3. Invalidates the TLB and the branch predictor.
4. If step 1 ran, cleans and invalidates the data cache again over every section of the range that is still mapped, through the new mapping. Cortex-A7 and A53 may have prefetched clean lines until step 3.

It masks IRQs and FIQs while it runs. Main context. Do not unmap the section that holds the running code or the stacks.

```c
void mmu_enable(void);
```

Turns the MMU on with the table from `mmu_init()`. Does nothing if `mmu_init()` has not succeeded or the MMU is already on. After `mmu_disable()` it turns the MMU on again with the same table. In order, with IRQs and FIQs masked:

1. On Cortex-A7 sets ACTLR.SMP (bit 6), on Cortex-A53 sets CPUECTLR.SMPEN (bit 6), only if clear. The access can trap when the secure firmware locks the register, so it runs through a probe that survives the trap.
2. Invalidates the data cache, the instruction cache, the branch predictor and the TLB. Cache contents are unknown after reset.
3. Sets DACR to domain 0 client, TTBCR to 0 and TTBR0 to the table with non-cacheable walks.
4. Sets SCTLR M, C, I and Z, and clears A, TRE and AFE. On ARM1176 it also sets XP, which selects the ARMv6 descriptor format with execute-never and the read-only AP encoding.

Main context.

```c
void mmu_disable(void);
```

Cleans and invalidates the whole data cache, then clears SCTLR M, C and I, then invalidates the data cache, the instruction cache, the branch predictor and the TLB again. It is written in assembly and touches no memory between the clean and the SCTLR write, so it is safe to call from cached code. The identity map keeps the next instruction at the same address after the MMU is off. It masks IRQs and FIQs while it runs and restores the mask on return. Does nothing if the MMU and data cache are already off. `robco_handoff()` calls it. It lives in `src/arch/cache.S`, so an image that uses `mmu.h` or `cache.h` links it.

```c
int mmu_enabled(void);
```

Returns non-zero when SCTLR.M is set. Any context.

```c
int mmu_smp(void);
```

Returns `1` if the SMP bit was set after `mmu_enable()`, `0` if it was clear because the access trapped or the write had no effect, and `-1` on ARM1176, which has no SMP bit. Before `mmu_enable()` it returns `0` on Cortex-A7 and A53. QEMU ignores the write, so it reports `0` on `raspi2b` and `raspi3b`.

### Context rules

| Rule | Why |
|---|---|
| Core 0 only. | `start.S` parks the other cores. There is one table and no TLB broadcast. |
| `mmu_init()`, `mmu_map()` and `mmu_enable()` from main context only. | They mask IRQs, but they are not reentrant. |
| Faults need the vectors in mapped memory. | The vectors are in the image, which is RAM. Keep it mapped. |

### Sharp edges

| Edge | What to do |
|---|---|
| A buffer that a DMA engine or the GPU reads or writes | Clean it before the device reads, invalidate it after the device writes. See [Caches](#caches-cacheh). |
| Code written to memory, then run | Call `cache_clean_range()` over it, then `cache_invalidate_icache()`. |
| `mmio_probe()` of an unmapped address | Reports an abort, from a translation fault. |
| A store to an unmapped or read-only section outside a probe | Data abort. The exception line shows `dfar` and `dfsr`. Status `0x5` or `0x7` means translation, `0xd` or `0xf` permission. Bit 11 set means a write. |
| Next stage | `robco_handoff()` turns the MMU and caches off and cleans everything. The next stage must call `mmu_init()` and `mmu_enable()` again if it wants them. |

### Pending hardware validation

QEMU does not model caches, cache timing, the SMP bit or memory types. These points need real boards. See [HARDWARE_VALIDATION.md](../../HARDWARE_VALIDATION.md).

| Check | How |
|---|---|
| Fill and copy run faster with the caches on | `ev=bench` lines of `examples/mmu` |
| Mailbox answers stay right with the cache on | `ev=mbox phase=on mismatches=0` and `ev=guard bad_guard=0 bad_reply=0` in `examples/mmu` |
| The framebuffer shows every write without a flush | Test pattern of `examples/mmu`, console of `examples/demo` `build/mmu` |
| `robco_handoff()` from a cached stage | `examples/chain1` `build/mmu` |
| `mmu_disable()` from cached code returns | `ev=mmu_off` and `ev=mbox phase=after` in `examples/mmu` |
| `mmu_smp()` reports `1` under the real firmware | `smp=` on the `ev=mmu` line, Pi 2 and Pi 3 |
| ARM1176 XP bit and I-cache erratum workaround | `examples/mmu` and its `fault_ro` variant on a Pi Zero or Pi 1 |

### Example

From `examples/demo/main.c`, `-DDEMO_MMU=1`:

```c
rc = mmu_init();
if (rc != 0) {
    fail("mmu_init", rc);
}
mmu_enable();
k_printf("@RC stage=demo ev=mmu on=%d\n", mmu_enabled());

rc = fb_init(640, 480, 32);     /* mapped with MMU_ATTR_FB by fb_init() */
```

## Caches (`cache.h`)

`cache.h` holds data cache and instruction cache maintenance for buffers shared with the GPU or a DMA engine, and for code written at run time. `mbox_call()`, `mmu_map()` and `mmu_disable()` already use it.

Every function reads MIDR. ARM1176 uses 32-byte lines and its whole-cache CP15 operations. Cortex-A7 and A53 read the smallest data line size from CTR for range operations, and walk every cache level up to the point of coherency by set and way, from CLIDR and CCSIDR, for whole-cache operations. Range operations reach the point of coherency, so the A53 L2 is included.

Every data cache function does nothing while SCTLR.C is clear. Range functions do nothing for a size of `0`. All are callable with IRQs enabled and from IRQ context.

| Constant | Value | Meaning |
|---|---|---|
| `CACHE_LINE_MAX` | `64` | Largest data cache line of the three CPUs. Align and size shared buffers to it. |

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

Cleans the whole data cache. It does not invalidate. An IRQ that writes during the walk may leave lines dirty.

```c
void cache_invalidate_icache(void);
```

Invalidates the whole instruction cache and the branch predictor. It applies the ARM1176 erratum 411920 workaround on every CPU: four invalidates with IRQ, FIQ and asynchronous aborts masked, then eleven NOPs.

### Mailbox and framebuffer coherence

The GPU reads and writes the property buffer in memory, past the ARM caches. A clean after the answer would write the CPU's copy of a shared line over the GPU's reply, so `mbox_call()` never runs maintenance on a line that holds other data. With the data cache on, it uses the caller's buffer in place only when the buffer starts on a 64-byte boundary and fills whole 64-byte lines. Otherwise it copies the message into a bounce buffer that is aligned to `CACHE_LINE_MAX` and sized in whole lines. It cleans those lines before the request and invalidates them after the answer, then copies the reply back. Nothing else lives in those lines, so the invalidate loses nothing. A message larger than `ROBCO_MBOX_BOUNCE_SIZE` that is not line-isolated fails with `MBOX_ERR_SIZE`. The bounce buffer and this code come with `mmu_enable()`, so images that never turn the cache on do not link them.

The framebuffer itself is mapped non-cacheable, so it has no such hazard.

`fb_init()` calls `mmu_map(addr, size, MMU_ATTR_FB)` when the image links the MMU module. `mmu_init()` maps the framebuffer when `fb_init()` ran first.
