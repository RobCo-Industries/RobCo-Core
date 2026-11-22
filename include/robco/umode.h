#ifndef ROBCO_UMODE_H
#define ROBCO_UMODE_H

#include <stddef.h>
#include <stdint.h>

#include <robco/exc.h>

/*
 * User-mode tasks: run one untrusted program in USR mode, serve its
 * `svc #0` calls, end it on return, exit, fault or kill request, and
 * return to the caller in SVC mode. One task at a time, core 0 only,
 * ARM state only. See docs/guide/api/umode.md.
 *
 * With the MMU on, umode_run() maps the task's regions for USR mode and
 * everything else stays privileged-only, so the task can touch nothing
 * else. With the MMU off there is NO memory protection: the task can
 * read and write any RAM and any peripheral. The pointer checks below
 * still apply to system-call arguments in both cases.
 */

#define UMODE_MAX_REGIONS   8

/* umode_region_t.flags */
#define UMODE_REGION_WRITE  1u  /* writable by the task; otherwise read-only */

/* umode_exit_t.reason */
#define UMODE_RETURNED      1   /* the entry function returned; code = r0 */
#define UMODE_EXITED        2   /* a handler called umode_exit(); code = its code */
#define UMODE_FAULT         3   /* undef, abort, or svc in Thumb state; see type */
#define UMODE_KILLED        4   /* umode_request_kill(); code = its reason */

/* umode_run() errors. */
#define UMODE_ERR_BUSY      (-1)    /* a task already runs */
#define UMODE_ERR_ARG       (-2)    /* bad task, region, entry, sp or stub */
#define UMODE_ERR_ALIGN     (-3)    /* MMU on and a region is not whole megabytes in RAM */

/* r0 set by the library when no handler is installed (as -38). */
#define UMODE_ENOSYS        38

/* Return stub: two words umode_run() writes at task->stub. */
#define UMODE_STUB_SIZE     8u

typedef struct {
    uintptr_t start;
    size_t size;
    unsigned flags;     /* UMODE_REGION_* */
} umode_region_t;

typedef struct {
    uint32_t entry;     /* ARM code, word-aligned, inside a region */
    uint32_t sp;        /* initial USR sp, 8-byte aligned */
    uint32_t r[4];      /* r0..r3 at entry */
    /*
     * Address of UMODE_STUB_SIZE bytes inside a writable region.
     * umode_run() writes the return stub there (an `svc #0` and a
     * branch to itself) and sets the task's lr to it. The task must not
     * use these bytes. umode_run() fails with UMODE_ERR_ARG if the stub
     * is not word-aligned or not inside one writable region.
     */
    uint32_t stub;
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

/*
 * Called in SVC mode for each `svc` from the task, with the frame of the
 * task (call number in frame->r[7], arguments in r[0]..r[3]). Write the
 * result into frame->r[0]. Every other register of the task, and its
 * cpsr flags, are restored from the frame as they are. IRQs are enabled.
 */
typedef void (*umode_svc_fn)(exc_frame_t *frame, void *ctx);

/*
 * Runs the task in USR mode (IRQs on, FIQ masked) until it ends, then
 * fills *out and returns 0, in SVC mode on the caller's stack with the
 * IRQ and FIQ masks as they were. With the MMU on, the regions must be
 * whole megabytes inside RAM: they are mapped MMU_ATTR_RAM | MMU_USER_RW
 * (writable) or MMU_ATTR_RO_CODE | MMU_USER_RO (read-only) before entry
 * and MMU_ATTR_RAM after exit. The caller cleans loaded code to memory
 * (cache_clean_range()); umode_run() invalidates the I-cache.
 * Main context only. Returns UMODE_ERR_* without running anything.
 */
int umode_run(const umode_task_t *task, umode_exit_t *out);

/* Non-zero while a task runs (including inside its system calls). */
int umode_active(void);

/* Installs the system-call handler. 0 makes every call return -UMODE_ENOSYS. */
void umode_set_svc_handler(umode_svc_fn fn, void *ctx);

/*
 * From the handler only: ends the task with UMODE_EXITED and `code`
 * when the handler returns, instead of resuming it.
 */
void umode_exit(exc_frame_t *frame, int code);

/*
 * Ends the running task with UMODE_KILLED and code `reason` (0 counts
 * as 1): at once if an IRQ interrupted USR mode, otherwise when the
 * current system call returns. Callable from IRQ context. Ignored when
 * no task runs. The first request wins.
 */
void umode_request_kill(int reason);

/*
 * For handlers: non-zero when [ptr, ptr + len) lies inside the running
 * task's regions (adjacent regions chain), writable ones if `write`.
 */
int umode_range_ok(uintptr_t ptr, size_t len, int write);

/*
 * For handlers: the length of the NUL-terminated string at ptr, read
 * only inside the task's regions, or -1 when no NUL is found in the
 * first `max` bytes or a byte lies outside the regions.
 */
long umode_strnlen_ok(uintptr_t ptr, size_t max);

/* Pure forms, src/lib/umode_check.c, on any region list. */
int umode_regions_range_ok(const umode_region_t *regions, unsigned count,
                           uintptr_t ptr, size_t len, int write);
long umode_regions_strnlen(const umode_region_t *regions, unsigned count,
                           uintptr_t ptr, size_t max);

#endif
