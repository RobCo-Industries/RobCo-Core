# User mode

[Back to the API reference](../API.md)

## User-mode tasks (`umode.h`)

`umode.h` runs an untrusted program in the CPU's unprivileged USR mode. The kernel serves its `svc #0` system calls, survives its crashes and gets control back when it ends. [`examples/umode`](../../../examples/umode/main.c) shows the whole flow, and `tests/target/test_umode.py` checks it on every board.

### The model

- One task at a time, on core 0. `umode_run()` blocks until the task ends, then returns to its caller in SVC mode, on the caller's stack.
- A task ends in one of four ways: it returns from its entry function, a system-call handler calls `umode_exit()`, it faults, or an IRQ handler asks for a kill.
- Only ARM state is supported. A task that switches to Thumb runs until it faults or makes a system call, and either one ends it with `UMODE_FAULT`, with the right `pc`, for 16-bit and 32-bit Thumb instructions alike.
- The task runs with IRQs enabled and FIQ masked. The tick, the UART and every other IRQ keep working.
- The component costs nothing when unused. Its objects stay in the archive unless the image calls `umode_run()`.

### What is protected

| | MMU on | MMU off |
|---|---|---|
| Kernel memory, other RAM | Faults from USR mode | No protection: the task can read and write it |
| Peripherals | Faults from USR mode | No protection |
| The task's read-only regions | Writes fault | Writable |
| Privileged instructions (CP15 access, `mrs`/`msr` of banked registers) | Undefined instruction fault | Undefined instruction fault |
| System-call pointers | `umode_range_ok()` | `umode_range_ok()` |
| CPU time | A watchdog on the tick can kill the task | Same |

With the MMU off there is no memory protection at all. Use it only for trusted code or for bring-up.

Some privileged instructions do not fault in USR mode. They are ignored instead, on ARMv6 and ARMv7 alike: `cpsid` and `cpsie` do nothing, and `msr cpsr_c` leaves the mode and masks alone. Only the flags of an `msr cpsr_f` take effect. A task cannot raise its privilege with them, but a test must use an instruction that traps, such as `mrc p15`.

### Constants

| Constant | Value | Meaning |
|---|---|---|
| `UMODE_MAX_REGIONS` | `8` | Largest region list |
| `UMODE_REGION_WRITE` | `1u` | Region flag: writable by the task |
| `UMODE_RETURNED` | `1` | Reason: the entry function returned. `code` is its `r0`. |
| `UMODE_EXITED` | `2` | Reason: a handler called `umode_exit()`. `code` is its code. |
| `UMODE_FAULT` | `3` | Reason: undefined instruction, abort, or `svc` in Thumb state. `type` says which. |
| `UMODE_KILLED` | `4` | Reason: `umode_request_kill()`. `code` is its reason. |
| `UMODE_ERR_BUSY` | `-1` | `umode_run()` while a task runs |
| `UMODE_ERR_ARG` | `-2` | NULL argument, no regions, more than `UMODE_MAX_REGIONS`, or unknown region flags |
| `UMODE_ERR_ALIGN` | `-3` | MMU on and a region start or size is not whole megabytes |
| `UMODE_ERR_RANGE` | `-4` | A region of size `0`, or one that runs past the top of the address space |
| `UMODE_ERR_OVERLAP` | `-5` | Two regions share a byte |
| `UMODE_ERR_SECTION` | `-6` | MMU on and two regions share a megabyte section |
| `UMODE_ERR_RAM` | `-7` | A region is not entirely inside board RAM |
| `UMODE_ERR_KERNEL` | `-8` | A region covers the kernel image or its mode stacks |
| `UMODE_ERR_TABLE` | `-9` | A region covers the level-1 translation table |
| `UMODE_ERR_VECTORS` | `-10` | A region covers the exception vectors |
| `UMODE_ERR_PERIPH` | `-11` | A region covers the peripheral window or the core-local peripherals |
| `UMODE_ERR_ENTRY` | `-12` | `entry` not word-aligned or not inside a region |
| `UMODE_ERR_SP` | `-13` | `sp` not 8-byte aligned, or the 8 bytes below it are not writable |
| `UMODE_ERR_STUB` | `-14` | `stub` not word-aligned, not inside writable memory, or on the entry word |
| `UMODE_ENOSYS` | `38` | A call with no handler installed returns `-38` |
| `UMODE_STUB_SIZE` | `8u` | Bytes of the return stub |

### Types

```c
typedef struct {
    uintptr_t start;
    size_t size;
    unsigned flags;     /* UMODE_REGION_* */
} umode_region_t;

typedef struct {
    uint32_t entry;     /* ARM code, word-aligned, inside a region */
    uint32_t sp;        /* initial USR sp, 8-byte aligned */
    uint32_t r[4];      /* r0..r3 at entry */
    uint32_t stub;      /* UMODE_STUB_SIZE bytes inside a writable region */
    const umode_region_t *regions;
    unsigned region_count;  /* 1..UMODE_MAX_REGIONS */
} umode_task_t;

typedef struct {
    unsigned reason;    /* UMODE_* reason */
    int code;           /* return value, exit code or kill reason */
    unsigned type;      /* ROBCO_EXC_* for UMODE_FAULT, else 0 */
    uint32_t pc;        /* frame.pc */
    uint32_t addr;      /* fault address for aborts, else 0 */
    uint32_t status;    /* DFSR or IFSR for aborts, else 0 */
    exc_frame_t frame;  /* the task's registers when it ended */
} umode_exit_t;

typedef void (*umode_svc_fn)(exc_frame_t *frame, void *ctx);
```

`exc_frame_t` is the frame from [`exc.h`](../API.md#exception-frames-and-the-fault-hook). For a fault, `pc` is the faulting instruction. For a system call or a return, it is the instruction after the `svc`. For a kill, it is the instruction the task would have run next.

### The return stub

A task returns through `lr`, so `lr` must point at code the task can run. The kernel's own code is privileged-only, so the stub must live in the task's memory. The caller names 8 bytes of writable memory in `task->stub`. `umode_run()` writes an `svc #0` and a branch to itself there, cleans them to memory, and sets the task's `lr` to the stub. The system-call entry recognises a call from that address and ends the task with `UMODE_RETURNED`. `umode_run()` refuses a stub that is not word-aligned, not entirely writable or on the entry word, so a wrong address fails at once instead of crashing later. The task must not use those 8 bytes. If it overwrites them, returning from the entry function no longer ends it cleanly.

The task owns the stub, so it can rewrite it or run its `svc` on purpose with any `r0` and `r7`. That only ends the task with `UMODE_RETURNED` and the `r0` it chose. The handler never sees the call. Treat `code` as a value from the task, never as a trusted status.

### Functions

```c
int umode_run(const umode_task_t *task, umode_exit_t *out);
```

Runs the task in USR mode until it ends, fills `*out` and returns `0`. On entry the task has `r0` to `r3` from `task->r`, `sp` from `task->sp`, `lr` at the stub, every other register `0`, IRQs on and FIQ masked. It returns in SVC mode on the caller's stack, with `r4` to `r11` and the IRQ and FIQ masks as they were. It can be called again any number of times. It returns `UMODE_ERR_BUSY` while a task runs, including from a system-call handler: no nesting. Otherwise it validates the whole task first (see [Task validation](#task-validation)) and returns the first error found. On any error nothing is mapped, nothing is written and nothing runs.

With the MMU on, it maps each region before entry: writable ones as `MMU_ATTR_RAM | MMU_USER_RW`, the others as `MMU_ATTR_RO_CODE | MMU_USER_RO`. It maps them before it writes the stub, so a region the caller left unmapped or read-only with `mmu_map()` still works. After the task ends it maps them back as `MMU_ATTR_RAM`, privileged-only, on every way out: return, exit, fault and kill. `mmu_map()` does the TLB and branch predictor maintenance on every CPU type. Load the program while the regions are privileged RAM, then clean it to memory with `cache_clean_range()`. `umode_run()` invalidates the instruction cache before entry. Main context only.

```c
int umode_active(void);
```

Non-zero while a task runs, including inside its system calls.

```c
void umode_set_svc_handler(umode_svc_fn fn, void *ctx);
```

Installs the system-call handler. `0` makes every call return `-UMODE_ENOSYS`. The handler reads the call number from `frame->r[7]` and the arguments from `frame->r[0]` to `frame->r[3]`, and writes its result into `frame->r[0]`.

```c
void umode_exit(exc_frame_t *frame, int code);
```

From the handler only. Ends the task with `UMODE_EXITED` and `code` when the handler returns, instead of resuming it. The first end wins: a later `umode_exit()` or kill request does nothing.

```c
void umode_request_kill(int reason);
```

Ends the running task with `UMODE_KILLED` and code `reason` (`0` counts as `1`). Callable from IRQ context, for example from a tick callback that acts as a watchdog, or from a key handler. Ignored when no task runs. The first request wins.

```c
int umode_range_ok(uintptr_t ptr, size_t len, int write);
long umode_strnlen_ok(uintptr_t ptr, size_t max);
```

For handlers. `umode_range_ok()` is non-zero when `ptr` to `ptr + len - 1` lies inside the running task's regions, in writable ones if `write`. A buffer may cross from one region into an adjacent one. A zero-length buffer is valid at any address inside a region or exactly at its end. Arithmetic never wraps: a range past the top of the address space fails. `umode_strnlen_ok()` returns the length of the string at `ptr`, reading only inside the regions, or `-1` when it finds no NUL in the first `max` bytes or meets a byte outside the regions. Both return `0` or `-1` when no task runs. They check the task's declared regions, so they work the same with the MMU off.

```c
int umode_regions_range_ok(const umode_region_t *regions, unsigned count,
                           uintptr_t ptr, size_t len, int write);
long umode_regions_strnlen(const umode_region_t *regions, unsigned count,
                           uintptr_t ptr, size_t max);
```

The pure forms on any region list, in `src/lib/umode_check.c`. They ignore a region whose last byte would wrap past the top of the address space. A range may end exactly at the top. `tests/host/test_umode_check.c` covers empty ranges, ranges that cross a region end, adjacent regions, wraparound at the top of the address space, regions that wrap, zero-length buffers at a region boundary and read-only regions.

```c
int umode_task_check(const umode_task_t *task, const umode_limits_t *lim);
```

The whole validation, pure, in `src/lib/umode_check.c`. `umode_run()` calls it with board RAM, the MMU section size (or `0` with the MMU off) and the kernel's forbidden ranges, each with its own error code. Returns `0` or the first `UMODE_ERR_*` found.

### Task validation

`umode_task_check()` checks, in this order:

1. The task, its region list and the limits are not NULL, and there are 1 to `UMODE_MAX_REGIONS` regions with no flag but `UMODE_REGION_WRITE` (`UMODE_ERR_ARG`).
2. Each region has a size and its last byte does not wrap past the top of the address space (`UMODE_ERR_RANGE`).
3. No two regions share a byte (`UMODE_ERR_OVERLAP`).
4. With the MMU on, no two regions share a megabyte section, even with disjoint bytes (`UMODE_ERR_SECTION`), and every start and size is whole megabytes (`UMODE_ERR_ALIGN`).
5. No region touches kernel memory: the level-1 table (`UMODE_ERR_TABLE`), the vectors (`UMODE_ERR_VECTORS`), the image from `__image_start` to `__image_mem_end`, which includes `.bss` and the mode stacks (`UMODE_ERR_KERNEL`), and the peripheral window plus the core-local peripherals on BCM2836/7 (`UMODE_ERR_PERIPH`).
6. Each region lies inside board RAM (`UMODE_ERR_RAM`).
7. `entry` is word-aligned and its word lies in a region (`UMODE_ERR_ENTRY`).
8. `sp` is 8-byte aligned and `[sp - 8, sp)` is writable, so a full-descending stack may start at a region's end (`UMODE_ERR_SP`).
9. The stub is word-aligned, its 8 bytes are writable, and it does not overlap the entry word (`UMODE_ERR_STUB`).

The initial registers `r0` to `r3` are data and need no check. Adjacent regions in different sections may have different permissions. The kernel checks hold with the MMU off too, so `umode_range_ok()` never approves kernel memory.

After validation each byte, and with the MMU on each section, belongs to at most one region. The descriptor `umode_run()` writes for a section and the answer of `umode_range_ok()` for any byte in it come from the same region, so the two views cannot disagree. A host test walks every section of an accepted layout to show it.

### Invariants

- The region list and the mappings do not change while a task runs. `umode_run()` copies the list and maps it before entry, and only unmaps it after the task ends. A nested `umode_run()` fails with `UMODE_ERR_BUSY`. There is one task, and it does not run while its handler does. So a pointer that `umode_range_ok()` approves stays valid for the rest of that call.
- Every return to USR mode uses a status word the library built: `USR | F` at entry, the sanitised frame value after a system call, and the CPU's own saved value after an IRQ. The task cannot mask IRQs, unmask FIQ, enter Thumb through a handler, or reach a privileged mode. A test spins with IRQs "masked" and the watchdog still kills it.
- The task never sees kernel registers. At entry `r4` to `r12` and the flags are `0`. A system call restores every register from the frame. The kernel's banked `sp` and `lr` are never visible in USR mode.
- A kill request counts only while a task runs, and `umode_run()` clears it before entry and after exit, with IRQs masked. A request that lands during a task's last call cannot kill the next task.
- If a handler changes the MMU state, the library prints `ev=umode_mmu_changed nr=<call>` and halts before the task resumes.
- A fault in a handler prints `ev=umode_call nr=<call>` before the fatal `ev=exception` line.

### The system-call path

1. The task runs `svc #0` with the number in `r7`. The CPU enters SVC mode with IRQs masked.
2. The entry saves `r0` to `r12`, the task's banked `sp` and `lr`, the return address and the task's `cpsr` in an `exc_frame_t` on the kernel stack.
3. It enables IRQs and calls the handler. The handler runs in SVC mode on the kernel stack, always with IRQs enabled, so a long call does not stop the tick. An IRQ that arrives during the handler runs and returns to the handler. It never switches tasks.
4. It masks IRQs again, then ends the task if the handler called `umode_exit()` or a kill request is pending.
5. Otherwise it restores every register from the frame and returns to the task.

Preservation rules, stricter than the Pip-OS ABI needs:

| State | After the call |
|---|---|
| `r0` | The handler's result |
| `r1` to `r12`, `sp`, `lr` | Unchanged unless the handler wrote them in the frame |
| N, Z, C, V, Q, GE[3:0], E | Unchanged unless the handler wrote them in `frame->cpsr` |
| Mode, I, F, A, T, J, IT | Forced back to USR mode, ARM state, IRQs on, FIQ masked, whatever the handler wrote |

An `svc` in a privileged mode is not a system call. It stays fatal: the library prints `ev=exception type=svc` and halts.

### Faults

`umode_run()` installs a fault hook (see [`exc_set_hook()`](../API.md#exception-frames-and-the-fault-hook)) while the task runs and restores the previous one afterwards. A fault taken in USR mode ends the task with `UMODE_FAULT`. The exit record holds the type (`ROBCO_EXC_UNDEF`, `ROBCO_EXC_PABT` or `ROBCO_EXC_DABT`), the faulting `pc`, and for aborts the fault address and status. A fault in a privileged mode, for example in a handler, goes to the previous hook and then to the fatal path, unchanged. `mmio_probe()` keeps working while a task runs.

### The kill mechanism

`umode_request_kill()` sets a flag. Two places check it:

- The IRQ exit. When the interrupted mode is USR and the flag is set, the IRQ entry does not return to the task. It saves the task's registers in a frame, records the kill and returns to `umode_run()`'s caller.
- The system-call exit, with IRQs masked, before it returns to the task.

Latency: a task in USR mode ends at the end of the IRQ that made the request, or at the next IRQ. A task inside a system call ends when the handler returns. A handler that never returns cannot be killed, so keep handlers short.

### Context rules

| Rule | Why |
|---|---|
| `umode_run()` from main context only, never from IRQ context or a handler. | It switches stacks and modes; a nested call returns `UMODE_ERR_BUSY`. |
| `umode_exit()`, `umode_range_ok()` and `umode_strnlen_ok()` from the handler. | They act on the running task. |
| `umode_request_kill()` from anywhere. | It only sets a flag. |
| Handlers may sleep with `wfi`. | IRQs are on and return to the handler. |

### What the kernel author must still get right

- Call `umode_run()` from main context only, never from an IRQ handler.
- Never call `mmu_map()`, `mmu_enable()` or `mmu_disable()` while a task runs, from a handler or from an IRQ handler. The library catches a change of the MMU state at the next system-call exit only.
- Check every pointer with `umode_range_ok()` or `umode_strnlen_ok()` before using it. A handler that dereferences a bad pointer faults in SVC mode, and that is fatal.
- Keep task memory away from kernel data the library does not know about: the heap, DMA buffers, the framebuffer and mailbox buffers.
- Never put a kernel pointer or secret into `frame->r[0]` or any other frame register.
- Treat `ex.code` and `ex.frame` as data from the task.

### Sharp edges

| Edge | What happens |
|---|---|
| Section granularity. | With the MMU on, regions are whole 1 MiB sections, and no two regions share one. |
| Regions return to `MMU_ATTR_RAM`. | After the task, each region is privileged RAM, whatever it was before. Regions must lie in RAM. |
| Thumb is not supported. | A task in Thumb state is killed at its first fault or system call. |
| Handlers are not preemptible by the kill. | A request during a handler waits for the handler to return. |
| The task's own memory is unprotected from itself. | A task can overwrite its own stub, stack or heap. |
| FIQ stays masked in the task. | The library has no FIQ handler. |
| Hardware validation. | QEMU enforces the MMU permissions. Real fault status values and real-silicon behaviour of privileged instructions in USR mode are in [HARDWARE_VALIDATION.md](../../HARDWARE_VALIDATION.md). |

### Example

```c
static void on_svc(exc_frame_t *f, void *ctx)
{
    switch (f->r[7]) {
    case 1: /* write(ptr, len) */
        if (!umode_range_ok(f->r[0], f->r[1], 0)) {
            f->r[0] = (uint32_t)-14;
            break;
        }
        /* print f->r[1] bytes from f->r[0] */
        f->r[0] = f->r[1];
        break;
    case 4: /* exit(code) */
        umode_exit(f, (int)f->r[0]);
        break;
    default:
        f->r[0] = (uint32_t)-38;
    }
}

static const umode_region_t regions[] = {
    { 0x00400000, 0x00100000, 0 },                   /* code, read-only */
    { 0x01000000, 0x00100000, UMODE_REGION_WRITE },  /* heap and stack */
};

umode_task_t t = {
    .entry = 0x00400000, .sp = 0x01100000, .stub = 0x01000000,
    .regions = regions, .region_count = 2,
};
umode_exit_t ex;

umode_set_svc_handler(on_svc, 0);
/* copy the program to 0x00400000, then: */
cache_clean_range((void *)0x00400000, size);
if (umode_run(&t, &ex) == 0 && ex.reason == UMODE_FAULT) {
    k_printf("killed at 0x%08x\n", (unsigned)ex.pc);
}
```
