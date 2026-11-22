/*
 * Runs the test programs of user/prog.S in USR mode, one task per case,
 * first with the MMU off (the cases that do not need protection), then
 * with the MMU on (every case). Prints one marker per task:
 *
 *   @RC stage=umode ev=task name=<n> mmu=<0|1> reason=<r> code=<n>
 *       type=<t> pc=0x... addr=0x... status=0x... ticks=<n> canary=<ok|bad>
 *
 * and at the end "ev=done tasks=<n>", then echoes UART input.
 */

#include <stdint.h>

#include <robco/board.h>
#include <robco/boot.h>
#include <robco/cache.h>
#include <robco/exc.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/mmu.h>
#include <robco/tick.h>
#include <robco/uart.h>
#include <robco/umode.h>

#define TICK_HZ         100
#define WATCHDOG_TICKS  30          /* 0.3 s of task time */
#define KILL_WATCHDOG   1

#define CODE_BASE       0x00400000u /* ROM slot: the programs, read-only to the task */
#define DATA_BASE       0x01000000u /* ROM heap and stack, writable */
#define REGION_SIZE     0x00100000u
#define STUB_ADDR       DATA_BASE   /* return stub: first 8 bytes of the data region */
#define SCRATCH_ADDR    (DATA_BASE + 0x10)
#define USER_SP         (DATA_BASE + REGION_SIZE)
#define UNMAPPED_ADDR   0xe0000000u
#define CANARY          0x5afec0deu

#define SYS_WRITE       1
#define SYS_TICKS       2
#define SYS_SLEEP       3
#define SYS_EXIT        4
#define SYS_NEST        5
#define SYS_KFAULT      6
#define E_FAULT         14

extern const char user_prog_start[], user_prog_end[];

static volatile uint32_t canary = CANARY;
static volatile uint32_t watch_start;
static volatile int watch_on;

/* Needs: 0 runs with the MMU off and on, 1 only with the MMU on. */
enum { ARG_NONE, ARG_SCRATCH, ARG_KERNEL, ARG_CANARY, ARG_PERIPH, ARG_OWN_CODE, ARG_UNMAPPED };

static const struct {
    const char *name;
    unsigned slot;
    int arg;
    int need_mmu;
    int watchdog;
} cases[] = {
    { "return",     0,  ARG_NONE,     0, 0 },
    { "exit",       1,  ARG_NONE,     0, 0 },
    { "regs",       2,  ARG_SCRATCH,  0, 0 },
    { "badptr",     3,  ARG_KERNEL,   0, 0 },
    { "priv",       4,  ARG_NONE,     0, 0 },
    { "undef",      5,  ARG_NONE,     0, 0 },
    { "rdkern",     6,  ARG_CANARY,   0, 0 },
    { "wrkern",     7,  ARG_CANARY,   1, 0 },
    { "rdperiph",   8,  ARG_PERIPH,   0, 0 },
    { "wrro",       9,  ARG_OWN_CODE, 1, 0 },
    { "jump",       10, ARG_UNMAPPED, 1, 0 },
    { "stack",      11, ARG_NONE,     1, 0 },
    { "spin",       12, ARG_NONE,     0, 1 },
    { "svcspin",    13, ARG_NONE,     0, 1 },
    { "thumb",      14, ARG_NONE,     0, 0 },
};

static const umode_region_t regions[] = {
    { CODE_BASE, REGION_SIZE, 0 },
    { DATA_BASE, REGION_SIZE, UMODE_REGION_WRITE },
};

/* Tick callback, IRQ context: the watchdog. */
static void on_tick(void)
{
    if (watch_on && umode_active() && tick_count() - watch_start >= WATCHDOG_TICKS) {
        umode_request_kill(KILL_WATCHDOG);
    }
}

static umode_task_t nested;
static umode_exit_t nested_exit;

/* System calls, SVC mode with IRQs on. */
static void on_svc(exc_frame_t *f, void *ctx)
{
    uint32_t start, i;
    long n;

    (void)ctx;
    switch (f->r[7]) {
    case SYS_WRITE:
        if (!umode_range_ok(f->r[0], f->r[1], 0)) {
            f->r[0] = (uint32_t)-E_FAULT;
            break;
        }
        uart_puts("@RC stage=umode ev=write text=");
        for (i = 0; i < f->r[1]; i++) {
            char c = ((const char *)(uintptr_t)f->r[0])[i];
            uart_putc(c == ' ' ? '_' : c);
        }
        uart_putc('\n');
        f->r[0] = f->r[1];
        break;
    case SYS_TICKS:
        f->r[0] = tick_count();
        break;
    case SYS_SLEEP:
        start = tick_count();
        while (tick_count() - start < f->r[0]) {
            __asm__ volatile("wfi");
        }
        f->r[0] = 0;
        break;
    case SYS_EXIT:
        umode_exit(f, (int)f->r[0]);
        break;
    case SYS_NEST:
        /* umode_run() from a handler: must fail with UMODE_ERR_BUSY. */
        f->r[0] = (uint32_t)umode_run(&nested, &nested_exit);
        break;
#ifdef UMODE_KFAULT
    case SYS_KFAULT:
        /* A fault in a handler is a kernel fault: the hook declines it. */
        __asm__ volatile(".word 0xe7f000f0");
        break;
#endif
    default:
        n = -38;
        f->r[0] = (uint32_t)n;
        break;
    }
}

static const char *reason_name(unsigned r)
{
    switch (r) {
    case UMODE_RETURNED: return "returned";
    case UMODE_EXITED:   return "exit";
    case UMODE_FAULT:    return "fault";
    case UMODE_KILLED:   return "killed";
    default:             return "?";
    }
}

static const char *type_name(const umode_exit_t *e)
{
    if (e->reason != UMODE_FAULT) {
        return "none";
    }
    switch (e->type) {
    case ROBCO_EXC_UNDEF: return "undef";
    case ROBCO_EXC_SVC:   return "svc";
    case ROBCO_EXC_PABT:  return "prefetch_abort";
    case ROBCO_EXC_DABT:  return "data_abort";
    default:              return "?";
    }
}

static uint32_t arg_for(int arg)
{
    switch (arg) {
    case ARG_SCRATCH:  return SCRATCH_ADDR;
    case ARG_KERNEL:   return 0x8000u;
    case ARG_CANARY:   return (uint32_t)(uintptr_t)&canary;
    case ARG_PERIPH:   return board_get()->periph_base + 0x3004u;   /* system timer CLO */
    case ARG_OWN_CODE: return CODE_BASE + 0x800u;
    case ARG_UNMAPPED: return UNMAPPED_ADDR;
    default:           return 0;
    }
}

static unsigned run_pass(int mmu)
{
    static umode_exit_t ex;
    umode_task_t t;
    unsigned i, n = 0;
    uint32_t len = (uint32_t)(user_prog_end - user_prog_start);
    int rc;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].need_mmu && !mmu) {
            continue;
        }
        /* Fresh copy each time: the regions are privileged RAM between tasks. */
        k_memcpy((void *)(uintptr_t)CODE_BASE, user_prog_start, len);
        cache_clean_range((const void *)(uintptr_t)CODE_BASE, len);

        k_memset(&t, 0, sizeof(t));
        t.entry = CODE_BASE + cases[i].slot * 0x40u;
        t.sp = USER_SP;
        t.r[0] = arg_for(cases[i].arg);
        t.r[1] = 0xbadbad00u;
        t.stub = STUB_ADDR;
        t.regions = regions;
        t.region_count = 2;

        watch_start = tick_count();
        watch_on = cases[i].watchdog;
        rc = umode_run(&t, &ex);
        watch_on = 0;
        if (rc != 0) {
            k_printf("@RC stage=umode ev=error name=%s rc=%d\n", cases[i].name, rc);
            continue;
        }
        n++;
        k_printf("@RC stage=umode ev=task name=%s mmu=%d reason=%s code=%d type=%s pc=0x%08x "
                 "addr=0x%08x status=0x%08x ticks=%u canary=%s\n",
                 cases[i].name, mmu, reason_name(ex.reason), ex.code, type_name(&ex),
                 (unsigned)ex.pc, (unsigned)ex.addr, (unsigned)ex.status,
                 (unsigned)tick_count(), canary == CANARY ? "ok" : "bad");
    }
    return n;
}

/* Runs one task with the given regions, entry at slot 0. Returns umode_run()'s result. */
static int try_regions(const umode_region_t *r, unsigned n, uint32_t entry, uint32_t sp,
                       uint32_t stub)
{
    static umode_exit_t ex;
    umode_task_t t;

    k_memset(&t, 0, sizeof(t));
    t.entry = entry;
    t.sp = sp;
    t.stub = stub;
    t.regions = r;
    t.region_count = n;
    return umode_run(&t, &ex);
}

/*
 * Error cases of umode_run() itself, then a task that reads the canary:
 * the refused layouts left nothing mapped for USR mode.
 */
static void check_errors(void)
{
    static umode_exit_t ex;
    static const umode_region_t odd[] = { { CODE_BASE + 0x1000, REGION_SIZE, UMODE_REGION_WRITE } };
    /* Read-only code, then the same megabyte writable: last-wins would make it writable. */
    static const umode_region_t overlap[] = {
        { CODE_BASE, REGION_SIZE, 0 },
        { CODE_BASE, REGION_SIZE, UMODE_REGION_WRITE },
    };
    static const umode_region_t kernel[] = { { 0, REGION_SIZE, UMODE_REGION_WRITE } };
    static const umode_region_t outside[] = { { 0xe0000000u, REGION_SIZE, UMODE_REGION_WRITE } };
    umode_region_t periph[1] = { { board_get()->periph_base, REGION_SIZE, UMODE_REGION_WRITE } };
    umode_task_t t;
    int bad_stub, bad_align, bad_overlap, bad_kernel, bad_periph, bad_ram;

    bad_stub = try_regions(regions, 2, CODE_BASE, USER_SP, CODE_BASE);  /* read-only stub */
    bad_align = try_regions(odd, 1, CODE_BASE + 0x1000, CODE_BASE + 0x3000, CODE_BASE + 0x2000);
    bad_overlap = try_regions(overlap, 2, CODE_BASE, CODE_BASE + REGION_SIZE, CODE_BASE + 0x800);
    bad_kernel = try_regions(kernel, 1, 0x80000, REGION_SIZE, 0x80000 + 0x800);
    bad_periph = try_regions(periph, 1, periph[0].start, periph[0].start + REGION_SIZE,
                             periph[0].start + 0x800);
    bad_ram = try_regions(outside, 1, 0xe0000000u, 0xe0000000u + REGION_SIZE, 0xe0000800u);
    k_printf("@RC stage=umode ev=errors stub=%d align=%d overlap=%d kernel=%d periph=%d ram=%d "
             "mmu=%d\n", bad_stub, bad_align, bad_overlap, bad_kernel, bad_periph, bad_ram,
             mmu_enabled());

    if (!mmu_enabled()) {
        return;
    }
    k_memcpy((void *)(uintptr_t)CODE_BASE, user_prog_start,
             (uint32_t)(user_prog_end - user_prog_start));
    cache_clean_range((const void *)(uintptr_t)CODE_BASE,
                      (uint32_t)(user_prog_end - user_prog_start));
    k_memset(&t, 0, sizeof(t));
    t.entry = CODE_BASE + 7 * 0x40u;    /* wrkern */
    t.sp = USER_SP;
    t.r[0] = (uint32_t)(uintptr_t)&canary;
    t.stub = STUB_ADDR;
    t.regions = regions;
    t.region_count = 2;
    umode_run(&t, &ex);
    k_printf("@RC stage=umode ev=refused_after reason=%s type=%s addr=0x%08x canary=%s\n",
             reason_name(ex.reason), type_name(&ex), (unsigned)ex.addr,
             canary == CANARY ? "ok" : "bad");
}

/* A kill request while no task runs is ignored: the next task returns normally. */
static void check_stale_kill(void)
{
    static umode_exit_t ex;
    umode_task_t t;
    int rc;

    umode_request_kill(9);
    k_memset(&t, 0, sizeof(t));
    t.entry = CODE_BASE;    /* case 0: returns 42 */
    t.sp = USER_SP;
    t.stub = STUB_ADDR;
    t.regions = regions;
    t.region_count = 2;
    rc = umode_run(&t, &ex);
    k_printf("@RC stage=umode ev=stale rc=%d reason=%s code=%d\n", rc, reason_name(ex.reason),
             ex.code);
}

void robco_main(void)
{
    unsigned tasks = 0;
    int c, rc;

    uart_init(115200);
    k_printf("@RC stage=umode ev=boot canary_addr=0x%08x\n", (unsigned)(uintptr_t)&canary);
    irq_init();
    tick_register(on_tick);
    tick_init(TICK_HZ);
    irq_cpu_enable();
    umode_set_svc_handler(on_svc, 0);

    tasks += run_pass(0);
    check_errors();
    check_stale_kill();

    rc = mmu_init();
    if (rc != 0) {
        k_printf("@RC stage=umode ev=error name=mmu_init rc=%d\n", rc);
    }
    mmu_enable();
    k_printf("@RC stage=umode ev=mmu on=%d\n", mmu_enabled());
    tasks += run_pass(1);
    check_errors();

#ifdef UMODE_KFAULT
    {
        static umode_exit_t ex;
        umode_task_t t;

        k_memset(&t, 0, sizeof(t));
        t.entry = CODE_BASE + 15 * 0x40u;
        t.sp = USER_SP;
        t.stub = STUB_ADDR;
        t.regions = regions;
        t.region_count = 2;
        k_printf("@RC stage=umode ev=kfault\n");
        umode_run(&t, &ex);     /* does not return: fatal */
    }
#endif
#ifdef UMODE_KSVC
    k_printf("@RC stage=umode ev=ksvc\n");
    __asm__ volatile("svc #0" ::: "memory");   /* privileged svc: fatal */
#endif
    k_printf("@RC stage=umode ev=done tasks=%u ticks=%u canary=%s\n", tasks,
             (unsigned)tick_count(), canary == CANARY ? "ok" : "bad");
    for (;;) {
        c = uart_poll();
        if (c >= 0) {
            uart_putc(c == '\r' ? '\n' : (char)c);
        }
    }
}
