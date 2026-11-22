/*
 * User-mode tasks: umode_run() and the C half of the system-call, fault
 * and kill paths. The assembly half, with the strong robco_vec_svc and
 * robco_vec_irq, is src/arch/umode_entry.S. Both objects stay in the archive
 * unless the image calls umode_run().
 */

#include <stdint.h>

#include <robco/board.h>
#include <robco/cache.h>
#include <robco/exc.h>
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
/* SVC sp of umode_run() after it saved the caller's registers. */
uint32_t robco_umode_kstack;
/* Non-zero kill reason, only while a task runs. Read by robco_vec_irq. */
volatile int robco_umode_kill;

extern char __image_start[], __image_mem_end[];

static struct {
    volatile int active;
    volatile int ended;
    umode_exit_t *out;
    umode_region_t regions[UMODE_MAX_REGIONS];
    unsigned count;
    uint32_t stub;
    umode_svc_fn fn;
    void *ctx;
    exc_hook_t prev_hook;
} U;

static void record(unsigned reason, int code, unsigned type, const exc_frame_t *f)
{
    umode_exit_t *o = U.out;

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

/* Fault hook, ABT or UND mode, IRQs masked. Only USR faults of the task. */
static int umode_hook(unsigned type, exc_frame_t *f)
{
    if (!U.active || (f->cpsr & MODE_MASK) != MODE_USR) {
        return U.prev_hook != 0 ? U.prev_hook(type, f) : EXC_NOT_HANDLED;
    }
    record(UMODE_FAULT, 0, type, f);
    to_kernel(f);
    return EXC_RESUME;
}

/* From robco_vec_svc, SVC mode, IRQs enabled, frame of the task. */
void robco_umode_svc_call(exc_frame_t *f)
{
    if (!U.active) {
        f->r[0] = (uint32_t)-UMODE_ENOSYS;  /* USR code not started by umode_run() */
        return;
    }
    if (f->cpsr & PSR_T) {
        /* Thumb state is not supported: end the task as a fault. */
        record(UMODE_FAULT, 0, ROBCO_EXC_SVC, f);
        U.ended = 1;
        return;
    }
    if (f->pc == U.stub + 4) {
        record(UMODE_RETURNED, (int)f->r[0], 0, f);
        U.ended = 1;
        return;
    }
    if (U.fn != 0) {
        U.fn(f, U.ctx);
    } else {
        f->r[0] = (uint32_t)-UMODE_ENOSYS;
    }
}

/*
 * From robco_vec_svc after the handler, IRQs masked. Returns non-zero
 * to end the task, 0 to resume it with a sanitised cpsr.
 */
int robco_umode_svc_return(exc_frame_t *f)
{
    if (!U.active) {
        return 0;
    }
    if (U.ended) {
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
    return U.active;
}

void umode_set_svc_handler(umode_svc_fn fn, void *ctx)
{
    U.fn = fn;
    U.ctx = ctx;
}

void umode_exit(exc_frame_t *frame, int code)
{
    if (U.active && !U.ended) {
        record(UMODE_EXITED, code, 0, frame);
        U.ended = 1;
    }
}

void umode_request_kill(int reason)
{
    if (U.active && robco_umode_kill == 0) {
        robco_umode_kill = reason != 0 ? reason : 1;
    }
}

int umode_range_ok(uintptr_t ptr, size_t len, int write)
{
    return U.active && umode_regions_range_ok(U.regions, U.count, ptr, len, write);
}

long umode_strnlen_ok(uintptr_t ptr, size_t max)
{
    return U.active ? umode_regions_strnlen(U.regions, U.count, ptr, max) : -1;
}

static int overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
    return a < b + blen && b < a + alen;
}

static int check_task(const umode_task_t *t, int mmu)
{
    const robco_board_t *b = board_get();
    uint32_t img = (uint32_t)(uintptr_t)__image_start;
    uint32_t img_len = (uint32_t)(uintptr_t)__image_mem_end - img;
    unsigned i;

    if (t->regions == 0 || t->region_count == 0 || t->region_count > UMODE_MAX_REGIONS) {
        return UMODE_ERR_ARG;
    }
    if ((t->entry & 3) != 0 || (t->sp & 7) != 0 || (t->stub & 3) != 0) {
        return UMODE_ERR_ARG;
    }
    for (i = 0; i < t->region_count; i++) {
        uint32_t s = t->regions[i].start, n = t->regions[i].size;

        if (n == 0 || s + (n - 1) < s || overlaps(s, n, img, img_len)) {
            return UMODE_ERR_ARG;
        }
        if (mmu && ((s | n) & (MMU_SECTION_SIZE - 1)) != 0) {
            return UMODE_ERR_ALIGN;
        }
        if (mmu && (s < b->mem_base || s - b->mem_base > b->mem_size ||
                    n > b->mem_size - (s - b->mem_base))) {
            return UMODE_ERR_ALIGN;
        }
    }
    if (!umode_regions_range_ok(t->regions, t->region_count, t->entry, 4, 0) ||
        !umode_regions_range_ok(t->regions, t->region_count, t->stub, UMODE_STUB_SIZE, 1)) {
        return UMODE_ERR_ARG;
    }
    return 0;
}

static void map_regions(int user)
{
    unsigned i;
    unsigned attr;

    for (i = 0; i < U.count; i++) {
        if (!user) {
            attr = MMU_ATTR_RAM;
        } else if (U.regions[i].flags & UMODE_REGION_WRITE) {
            attr = MMU_ATTR_RAM | MMU_USER_RW;
        } else {
            attr = MMU_ATTR_RO_CODE | MMU_USER_RO;
        }
        mmu_map(U.regions[i].start, U.regions[i].size, attr);
    }
}

int umode_run(const umode_task_t *task, umode_exit_t *out)
{
    exc_frame_t zero;
    uint32_t ctx[7];
    uint32_t cpsr;
    unsigned i;
    int mmu, rc;

    if (U.active) {
        return UMODE_ERR_BUSY;
    }
    if (task == 0 || out == 0) {
        return UMODE_ERR_ARG;
    }
    mmu = mmu_enabled();
    rc = check_task(task, mmu);
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < task->region_count; i++) {
        U.regions[i] = task->regions[i];
    }
    U.count = task->region_count;
    U.stub = task->stub;
    U.out = out;
    U.ended = 0;
    robco_umode_kill = 0;
    for (i = 0; i < sizeof(zero) / 4; i++) {
        ((uint32_t *)&zero)[i] = 0;
    }
    record(UMODE_KILLED, 0, 0, &zero);  /* overwritten by the real end */

    /* Return stub: lr of the task. Clean it to memory for the I-side. */
    ((volatile uint32_t *)(uintptr_t)U.stub)[0] = INSN_SVC0;
    ((volatile uint32_t *)(uintptr_t)U.stub)[1] = INSN_BSELF;
    cache_clean_range((const void *)(uintptr_t)U.stub, UMODE_STUB_SIZE);
    if (mmu) {
        map_regions(1);     /* TLB and branch predictor invalidated by mmu_map() */
    }
    cache_invalidate_icache();

    ctx[0] = task->entry;
    ctx[1] = task->sp;
    ctx[2] = task->r[0];
    ctx[3] = task->r[1];
    ctx[4] = task->r[2];
    ctx[5] = task->r[3];
    ctx[6] = U.stub;

    __asm__ volatile("mrs %0, cpsr\n cpsid if" : "=r"(cpsr) :: "memory");
    U.prev_hook = exc_set_hook(umode_hook);
    U.active = 1;
    robco_umode_enter(ctx);     /* returns once the task has ended, IRQs masked */
    U.active = 0;
    robco_umode_kill = 0;
    exc_set_hook(U.prev_hook);
    __asm__ volatile("msr cpsr_c, %0" :: "r"(cpsr) : "memory");

    if (mmu) {
        map_regions(0);
    }
    return 0;
}
