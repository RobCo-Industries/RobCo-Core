/*
 * User-mode tasks: umode_run() and the C half of the system-call, fault
 * and kill paths. The assembly half, with the strong robco_vec_svc and
 * robco_vec_irq, is src/arch/umode_entry.S. Both objects stay in the archive
 * unless the image calls umode_run().
 */

#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/cache.h>
#include <robco/exc.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/mmu.h>
#include <robco/umode.h>

#define MODE_MASK   0x1fu
#define MODE_USR    0x10u
#define MODE_SVC    0x13u
#define PSR_T       (1u << 5)
#define PSR_F       (1u << 6)
#define PSR_I       (1u << 7)
/* Restored to the task after a system call: N Z C V Q, GE[3:0], E. */
#define PSR_USER_KEEP 0xf80f0200u

#define INSN_SVC0   0xef000000u     /* svc #0 */
#define INSN_BSELF  0xeafffffeu     /* b . */

/* Assembly side, src/arch/umode_entry.S. */
int robco_umode_enter(const uint32_t ctx[7]);
void robco_umode_leave(void);
/* SVC sp of the innermost umode_run() after it saved the caller's registers. */
uint32_t robco_umode_kstack;
/* Kill reason of the innermost task, non-zero only while it runs. Read by robco_vec_irq. */
volatile int robco_umode_kill;

extern char __image_start[], __image_mem_end[], robco_vectors[];
/* Address of the level-1 table, src/arch/mmu.c. */
uintptr_t robco_mmu_table(void);

/* One record per running task: T[0] the outer task, T[1] the inner one. */
typedef struct {
    volatile int ended;
    volatile int in_call;   /* a system-call handler of this task runs */
    uint32_t call;          /* its number, frame r7 */
    int mmu;                /* MMU state at entry; handlers must not change it */
    umode_exit_t *out;
    umode_region_t regions[UMODE_MAX_REGIONS];
    unsigned count;
    uint32_t stub;
    uint32_t kstack;        /* robco_umode_kstack of the task around this one */
    int kill;               /* robco_umode_kill of the outer task, saved while this one runs */
} task_t;

static task_t T[UMODE_MAX_DEPTH];
static volatile int depth;      /* running tasks, 0..UMODE_MAX_DEPTH */
static umode_svc_fn svc_fn;
static void *svc_ctx;
static exc_hook_t prev_hook;    /* installed by the outermost umode_run() */

/* Innermost running task. Only while depth > 0. */
static task_t *cur(void)
{
    return &T[depth - 1];
}

static void record(unsigned reason, int code, unsigned type, const exc_frame_t *f)
{
    umode_exit_t *o = cur()->out;

    o->reason = reason;
    o->code = code;
    o->type = type;
    o->frame = *f;
    o->pc = f->pc;
    if (type == ROBCO_EXC_DABT || type == ROBCO_EXC_PABT) {
        o->addr = f->far;
        o->status = f->fsr;
    } else {
        o->addr = 0;
        o->status = 0;
    }
}

/*
 * Turns a frame into "return to robco_umode_leave in SVC mode with IRQs
 * and FIQs masked". The exception exit restores it, and the leave code
 * reloads the caller's stack and registers.
 */
static void to_kernel(exc_frame_t *f)
{
    f->r[0] = 0;
    f->sp = robco_umode_kstack;
    f->lr = 0;
    f->pc = (uint32_t)(uintptr_t)robco_umode_leave;
    f->cpsr = MODE_SVC | PSR_I | PSR_F;
}

/* Innermost task inside a system call, or 0. */
static task_t *calling(void)
{
    int d;

    for (d = depth; d > 0; d--) {
        if (T[d - 1].in_call) {
            return &T[d - 1];
        }
    }
    return 0;
}

/* Fault hook, ABT or UND mode, IRQs masked. Only USR faults of the innermost task. */
static int umode_hook(unsigned type, exc_frame_t *f)
{
    task_t *c;
    int rc;

    if (depth == 0 || (f->cpsr & MODE_MASK) != MODE_USR) {
        rc = prev_hook != 0 ? prev_hook(type, f) : EXC_NOT_HANDLED;
        c = depth != 0 ? calling() : 0;
        if (rc != EXC_RESUME && c != 0) {
            /* A handler faulted: name the call before the fatal line. */
            k_printf("\n@RC stage=%s ev=umode_call nr=%u\n", ROBCO_STAGE_NAME,
                     (unsigned)c->call);
        }
        return rc;
    }
    record(UMODE_FAULT, 0, type, f);
    to_kernel(f);
    return EXC_RESUME;
}

/* From robco_vec_svc, SVC mode, IRQs enabled, frame of the innermost task. */
void robco_umode_svc_call(exc_frame_t *f)
{
    task_t *t;

    if (depth == 0) {
        f->r[0] = (uint32_t)-UMODE_ENOSYS;  /* USR code not started by umode_run() */
        return;
    }
    t = cur();
    if (f->cpsr & PSR_T) {
        /* Thumb state is not supported: end the task as a fault. */
        record(UMODE_FAULT, 0, ROBCO_EXC_SVC, f);
        t->ended = 1;
        return;
    }
    if (f->pc == t->stub + 4) {
        record(UMODE_RETURNED, (int)f->r[0], 0, f);
        t->ended = 1;
        return;
    }
    t->call = f->r[7];
    t->in_call = 1;
    if (svc_fn != 0) {
        svc_fn(f, svc_ctx);     /* may run a nested task, then T[1] is free again */
    } else {
        f->r[0] = (uint32_t)-UMODE_ENOSYS;
    }
    t->in_call = 0;
}

static void mmu_changed(unsigned call)
{
    /* The task's protection is gone or half-built: a kernel bug. Fatal. */
    k_printf("\n@RC stage=%s ev=umode_mmu_changed nr=%u\n", ROBCO_STAGE_NAME, call);
    robco_halt();
}

/*
 * From robco_vec_svc after the handler, IRQs masked. Returns non-zero
 * to end the task, 0 to resume it with a sanitised cpsr.
 */
int robco_umode_svc_return(exc_frame_t *f)
{
    task_t *t;

    if (depth == 0) {
        return 0;
    }
    t = cur();
    if (mmu_enabled() != t->mmu) {
        mmu_changed((unsigned)t->call);
    }
    if (t->ended) {
        return 1;
    }
    if (robco_umode_kill != 0) {
        record(UMODE_KILLED, robco_umode_kill, 0, f);
        return 1;
    }
    /* Whatever the handler did: USR mode, ARM state, IRQs on, FIQ off. */
    f->cpsr = (f->cpsr & PSR_USER_KEEP) | MODE_USR | PSR_F;
    return 0;
}

/* From robco_vec_irq when it diverts a USR task, IRQ mode, IRQs masked. */
void robco_umode_irq_kill(exc_frame_t *f)
{
    record(UMODE_KILLED, robco_umode_kill, 0, f);
}

int umode_active(void)
{
    return depth != 0;
}

int umode_depth(void)
{
    return depth;
}

void umode_set_svc_handler(umode_svc_fn fn, void *ctx)
{
    svc_fn = fn;
    svc_ctx = ctx;
}

void umode_exit(exc_frame_t *frame, int code)
{
    uint32_t s = irq_save();

    if (depth != 0 && !cur()->ended) {
        record(UMODE_EXITED, code, 0, frame);
        cur()->ended = 1;
    }
    irq_restore(s);
}

/* IRQs masked: an IRQ request cannot land between the check and the store. */
void umode_request_kill(int reason)
{
    uint32_t s = irq_save();

    if (depth != 0 && robco_umode_kill == 0) {
        robco_umode_kill = reason != 0 ? reason : 1;
    }
    irq_restore(s);
}

int umode_range_ok(uintptr_t ptr, size_t len, int write)
{
    return depth != 0 && umode_regions_range_ok(cur()->regions, cur()->count, ptr, len, write);
}

long umode_strnlen_ok(uintptr_t ptr, size_t max)
{
    return depth != 0 ? umode_regions_strnlen(cur()->regions, cur()->count, ptr, max) : -1;
}

#define LOCAL_PERIPH    0x40000000u /* BCM2836/7 core-local peripherals */
#define PERIPH_SIZE     0x01000000u
#define VECTORS_SIZE    0x40u       /* 8 vectors and their literal pool */

/*
 * Kernel memory the task must never get, then umode_task_check(). The
 * specific ranges come first, so their own error wins over the image's.
 */
static int check_task(const umode_task_t *t, int mmu)
{
    const robco_board_t *b = board_get();
    uintptr_t img = (uintptr_t)__image_start;
    umode_forbidden_t f[5];
    umode_limits_t lim;
    unsigned n = 0;

    f[n++] = (umode_forbidden_t){ robco_mmu_table(), 4096 * 4, UMODE_ERR_TABLE };
    f[n++] = (umode_forbidden_t){ (uintptr_t)robco_vectors, VECTORS_SIZE, UMODE_ERR_VECTORS };
    f[n++] = (umode_forbidden_t){ img, (uintptr_t)__image_mem_end - img, UMODE_ERR_KERNEL };
    f[n++] = (umode_forbidden_t){ b->periph_base, b->periph_base != 0 ? PERIPH_SIZE : 0,
                                  UMODE_ERR_PERIPH };
    if (b->soc == ROBCO_SOC_BCM2836 || b->soc == ROBCO_SOC_BCM2837) {
        f[n++] = (umode_forbidden_t){ LOCAL_PERIPH, MMU_SECTION_SIZE, UMODE_ERR_PERIPH };
    }
    lim.ram_start = b->mem_base;
    lim.ram_size = b->mem_size;
    lim.section = mmu ? MMU_SECTION_SIZE : 0;
    lim.forbidden = f;
    lim.forbidden_count = n;
    return umode_task_check(t, &lim);
}

static int map_one(uint32_t start, uint32_t size, unsigned attr)
{
    return mmu_map(start, size, attr);
}

static void map_regions(const task_t *t, int user)
{
    /* TLB and branch predictor invalidated by mmu_map(). */
    umode_map_regions(t->regions, t->count, user, map_one);
}

static int nest_allowed(void)
{
    uint32_t cpsr;

    if (depth == 0) {
        return 1;
    }
    if (depth >= UMODE_MAX_DEPTH) {
        return 0;
    }
    /* Only from a system-call handler of the running task: not from an IRQ or the hook. */
    __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
    return (cpsr & MODE_MASK) == MODE_SVC && cur()->in_call;
}

int umode_run(const umode_task_t *task, umode_exit_t *out)
{
    exc_frame_t zero;
    uint32_t ctx[7];
    uint32_t cpsr;
    unsigned i;
    int mmu, rc, d = depth;
    task_t *t, *outer = d != 0 ? &T[d - 1] : 0;

    if (!nest_allowed()) {
        return UMODE_ERR_BUSY;
    }
    if (task == 0 || out == 0) {
        return UMODE_ERR_ARG;
    }
    mmu = mmu_enabled();
    if (outer != 0 && mmu != outer->mmu) {
        mmu_changed((unsigned)outer->call);
    }
    rc = check_task(task, mmu);
    if (rc != 0) {
        return rc;
    }
    /* Nested: the outer task's sections become privileged-only before the scan. */
    if (mmu && outer != 0) {
        map_regions(outer, 0);
    }
    /* A user mapping left by the caller outside the regions would leak memory to the task. */
    if (mmu && umode_stray_section(task->regions, task->region_count, mmu_section_user) >= 0) {
        if (outer != 0) {
            map_regions(outer, 1);  /* back as they were */
        }
        return UMODE_ERR_USERMAP;
    }
    t = &T[d];
    for (i = 0; i < task->region_count; i++) {
        t->regions[i] = task->regions[i];
    }
    t->count = task->region_count;
    t->mmu = mmu;
    t->in_call = 0;
    t->call = 0;
    t->stub = task->stub;
    t->out = out;
    t->ended = 0;
    for (i = 0; i < sizeof(zero) / 4; i++) {
        ((uint32_t *)&zero)[i] = 0;
    }
    {
        /* record() writes the innermost task's record: T[d] is not running yet. */
        umode_exit_t *o = out;

        o->reason = UMODE_KILLED;   /* overwritten by the real end */
        o->code = 0;
        o->type = 0;
        o->frame = zero;
        o->pc = 0;
        o->addr = 0;
        o->status = 0;
    }

    /*
     * Map first: the caller may have left a region unmapped or read-only.
     * The stub is in a writable region, so the kernel can write it then.
     * A section both tasks list now has the inner task's permission.
     */
    if (mmu) {
        map_regions(t, 1);
    }
    /* Return stub: lr of the task. Clean it to memory for the I-side. */
    ((volatile uint32_t *)(uintptr_t)t->stub)[0] = INSN_SVC0;
    ((volatile uint32_t *)(uintptr_t)t->stub)[1] = INSN_BSELF;
    cache_clean_range((const void *)(uintptr_t)t->stub, UMODE_STUB_SIZE);
    cache_invalidate_icache();

    ctx[0] = task->entry;
    ctx[1] = task->sp;
    ctx[2] = task->r[0];
    ctx[3] = task->r[1];
    ctx[4] = task->r[2];
    ctx[5] = task->r[3];
    ctx[6] = t->stub;

    __asm__ volatile("mrs %0, cpsr\n cpsid if" : "=r"(cpsr) :: "memory");
    if (d == 0) {
        prev_hook = exc_set_hook(umode_hook);
    }
    /* The outer task's pending kill waits for its own system call to return. */
    t->kill = robco_umode_kill;
    t->kstack = robco_umode_kstack;
    robco_umode_kill = 0;
    depth = d + 1;
    robco_umode_enter(ctx);     /* returns once the task has ended, IRQs masked */
    depth = d;
    robco_umode_kill = t->kill;
    robco_umode_kstack = t->kstack;
    if (d == 0) {
        robco_umode_kill = 0;
        exc_set_hook(prev_hook);
    }

    /* Every way out comes here: no USR mapping outlives the task. */
    if (mmu) {
        map_regions(t, 0);
        if (outer != 0) {
            map_regions(outer, 1);  /* the outer task's view, exactly as before */
        }
    }
    if (outer != 0) {
        /* The inner task may have written memory the outer task runs. */
        cache_invalidate_icache();
    }
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");
    return 0;
}
