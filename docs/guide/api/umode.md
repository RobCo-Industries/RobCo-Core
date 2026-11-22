# User mode

[Back to the API reference](../API.md)

## User-mode tasks (`umode.h`)

`umode.h` runs an untrusted program in the CPU's unprivileged USR mode. The kernel serves its `svc #0` system calls, survives its crashes and gets control back when it ends. [`examples/umode`](../../../examples/umode/main.c) shows the whole flow, and `tests/target/test_umode.py` checks it on every board.

### The model

- One task at a time, on core 0. `umode_run()` blocks until the task ends, then returns to its caller in SVC mode, on the caller's stack.
- A task ends in one of four ways: it returns from its entry function, a system-call handler calls `umode_exit()`, it faults, or an IRQ handler asks for a kill.
- Only ARM state is supported. A task that switches to Thumb runs until it faults or makes a system call, and either one ends it with `UMODE_FAULT`, with the right `pc`.
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
| `UMODE_ERR_ARG` | `-2` | Bad task: see `umode_run()` |
| `UMODE_ERR_ALIGN` | `-3` | MMU on and a region is not whole megabytes inside RAM |
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

A task returns through `lr`, so `lr` must point at code the task can run. The kernel's own code is privileged-only, so the stub must live in the task's memory. The caller names 8 bytes inside a writable region in `task->stub`. `umode_run()` writes an `svc #0` and a branch to itself there, cleans them to memory, and sets the task's `lr` to the stub. The system-call entry recognises a call from that address and ends the task with `UMODE_RETURNED`. `umode_run()` refuses a stub that is not word-aligned or not entirely inside one writable region, so a wrong address fails at once instead of crashing later. The task must not use those 8 bytes. If it overwrites them, returning from the entry function no longer ends it cleanly.

### Functions

```c
int umode_run(const umode_task_t *task, umode_exit_t *out);
```

Runs the task in USR mode until it ends, fills `*out` and returns `0`. On entry the task has `r0` to `r3` from `task->r`, `sp` from `task->sp`, `lr` at the stub, every other register `0`, IRQs on and FIQ masked. It returns in SVC mode on the caller's stack, with `r4` to `r11` and the IRQ and FIQ masks as they were. It can be called again any number of times. Returns, without running anything:

- `UMODE_ERR_BUSY` while a task runs, including from a system-call handler: no nesting.
- `UMODE_ERR_ARG` for a NULL argument, no regions or more than `UMODE_MAX_REGIONS`, a region of size `0` or past `0xFFFFFFFF`, a region that overlaps the image, an `entry` that is not word-aligned or not inside a region, an `sp` not 8-byte aligned, or a bad stub.
- `UMODE_ERR_ALIGN` with the MMU on, for a region that is not whole megabytes or not inside board RAM.

With the MMU on, it maps each region before entry: writable ones as `MMU_ATTR_RAM | MMU_USER_RW`, the others as `MMU_ATTR_RO_CODE | MMU_USER_RO`. After the task ends it maps them back as `MMU_ATTR_RAM`, privileged-only. `mmu_map()` does the TLB and branch predictor maintenance on every CPU type. Load the program while the regions are privileged RAM, then clean it to memory with `cache_clean_range()`. `umode_run()` invalidates the instruction cache before entry. Main context only.

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

The pure forms on any region list, in `src/lib/umode_check.c`. `tests/host/test_umode_check.c` covers empty ranges, ranges that cross a region end, adjacent regions, wraparound at the top of the address space, zero-length buffers at a region boundary and read-only regions.

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

### Sharp edges

| Edge | What happens |
|---|---|
| Section granularity. | With the MMU on, regions are whole 1 MiB sections. Everything else in the same megabyte belongs to the task. |
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
