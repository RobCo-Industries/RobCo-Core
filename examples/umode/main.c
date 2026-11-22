/*
 * Runs the test programs of user/prog.S in USR mode, one task per case,
 * first with the MMU off (the cases that do not need protection), then
 * with the MMU on (every case). Prints one marker per task:
 *
 *   @RC stage=umode ev=task name=<n> mmu=<0|1> reason=<r> code=<n>
 *       type=<t> pc=0x... thumb=<0|1> addr=0x... status=0x... ticks=<n>
 *       canary=<ok|bad>
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
#define BACKSTOP_TICKS  1000        /* 10 s: every other task, in case it never ends */
#define KILL_WATCHDOG   1
#define KILL_BACKSTOP   3

#define CODE_BASE       0x00400000u /* ROM slot: the programs, read-only to the task */
#define DATA_BASE       0x01000000u /* ROM heap and stack, writable */
#define DATA2_BASE      0x01100000u /* data region of the "peek" task */
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
#define SYS_BADPSR      7
#define SYS_KILLEXIT    8
#define SYS_KMMU        9
#define SYS_KILLFIRST   10
#define E_FAULT         14

extern const char user_prog_start[], user_prog_end[];

static volatile uint32_t canary = CANARY;
static volatile uint32_t watch_start;
static volatile uint32_t watch_limit;
static volatile int watch_kill;
static volatile int kill_armed, kill_seen;

/* Needs: 0 runs with the MMU off and on, 1 only with the MMU on. */
enum { ARG_NONE, ARG_SCRATCH, ARG_KERNEL, ARG_CANARY, ARG_PERIPH, ARG_OWN_CODE, ARG_UNMAPPED,
       ARG_DATA };

static const struct {
    const char *name;
    unsigned slot;
    int arg;
    int need_mmu;
    int watchdog;
    int thumb2;     /* needs Thumb-2: skipped on ARM1176 */
} cases[] = {
    { "return",     0,  ARG_NONE,     0, 0, 0 },
    { "exit",       1,  ARG_NONE,     0, 0, 0 },
    { "regs",       2,  ARG_SCRATCH,  0, 0, 0 },
    { "badptr",     3,  ARG_KERNEL,   0, 0, 0 },
    { "priv",       4,  ARG_NONE,     0, 0, 0 },
    { "undef",      5,  ARG_NONE,     0, 0, 0 },
    { "rdkern",     6,  ARG_CANARY,   0, 0, 0 },
    { "wrkern",     7,  ARG_CANARY,   1, 0, 0 },
    { "rdperiph",   8,  ARG_PERIPH,   0, 0, 0 },
    { "wrro",       9,  ARG_OWN_CODE, 1, 0, 0 },
    { "jump",       10, ARG_UNMAPPED, 1, 0, 0 },
    { "stack",      11, ARG_NONE,     1, 0, 0 },
    { "spin",       12, ARG_NONE,     0, 1, 0 },
    { "svcspin",    13, ARG_NONE,     0, 1, 0 },
    { "thumb",      14, ARG_NONE,     0, 0, 0 },
    { "thumb2",     22, ARG_NONE,     0, 0, 1 },
    { "init",       16, ARG_NONE,     0, 0, 0 },
    { "psr",        17, ARG_NONE,     0, 0, 0 },
    { "cpsid",      18, ARG_NONE,     0, 1, 0 },
    { "stubsvc",    19, ARG_NONE,     0, 0, 0 },
    { "killexit",   20, ARG_NONE,     0, 0, 0 },
    { "afterkill",  0,  ARG_NONE,     0, 0, 0 },
    { "killfirst",  23, ARG_NONE,     0, 0, 0 },
    /* Reads the previous tasks' data region without declaring it. */
    { "peek",       6,  ARG_DATA,     1, 0, 0 },
};

static const umode_region_t regions[] = {
    { CODE_BASE, REGION_SIZE, 0 },
    { DATA_BASE, REGION_SIZE, UMODE_REGION_WRITE },
};

static const umode_region_t regions2[] = {
    { CODE_BASE, REGION_SIZE, 0 },
    { DATA2_BASE, REGION_SIZE, UMODE_REGION_WRITE },
};

/* Tick callback, IRQ context: the watchdog. */
static void on_tick(void)
{
    if (umode_active() && tick_count() - watch_start >= watch_limit) {
        umode_request_kill(watch_kill);
    }
    if (kill_armed && umode_active()) {
        kill_armed = 0;
        umode_request_kill(2);
        kill_seen = 1;
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
    case SYS_BADPSR:
        f->cpsr = 0xffffffffu;      /* the return path must sanitise it */
        f->r[0] = 0;
        break;
    case SYS_KILLEXIT:
        /* A kill request lands during the task's last call: the exit wins. */
        kill_seen = 0;
        kill_armed = 1;
        while (!kill_seen) {
            __asm__ volatile("wfi");
        }
        umode_exit(f, 5);
        break;
    case SYS_KILLFIRST:
        /* The tick requests reason 2 first: the handler's later request is ignored. */
        kill_seen = 0;
        kill_armed = 1;
        while (!kill_seen) {
            __asm__ volatile("wfi");
        }
        umode_request_kill(7);
        break;
#ifdef UMODE_KMMU
    case SYS_KMMU:
        mmu_disable();              /* forbidden: the library halts */
        break;
#endif
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
    case ARG_DATA:     return DATA_BASE;
    default:           return 0;
    }
}

/*
 * umode_run() with the watchdog armed. A case marked `watchdog` is meant
 * to spin and gets the short limit; every other task gets the backstop.
 */
static int run_task(const umode_task_t *t, umode_exit_t *ex, int spins)
{
    int rc;

    watch_limit = spins ? WATCHDOG_TICKS : BACKSTOP_TICKS;
    watch_kill = spins ? KILL_WATCHDOG : KILL_BACKSTOP;
    watch_start = tick_count();
    rc = umode_run(t, ex);
    watch_limit = BACKSTOP_TICKS;
    watch_kill = KILL_BACKSTOP;
    return rc;
}

/* Fresh copy of the programs at CODE_BASE: the regions are privileged RAM between tasks. */
static void load_programs(void)
{
    uint32_t len = (uint32_t)(user_prog_end - user_prog_start);

    k_memcpy((void *)(uintptr_t)CODE_BASE, user_prog_start, len);
    cache_clean_range((const void *)(uintptr_t)CODE_BASE, len);
}

/* ARM1176 (part 0xb76) is the only supported CPU without Thumb-2. */
static int has_thumb2(void)
{
    return ((board_get()->midr >> 4) & 0xfffu) != 0xb76u;
}

static unsigned run_pass(int mmu)
{
    static umode_exit_t ex;
    umode_task_t t;
    unsigned i, n = 0;
    int rc;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if ((cases[i].need_mmu && !mmu) || (cases[i].thumb2 && !has_thumb2())) {
            continue;
        }
        load_programs();

        k_memset(&t, 0, sizeof(t));
        t.entry = CODE_BASE + cases[i].slot * 0x40u;
        t.sp = USER_SP;
        t.r[0] = arg_for(cases[i].arg);
        t.r[1] = 0xbadbad00u;
        t.stub = STUB_ADDR;
        t.regions = regions;
        t.region_count = 2;
        if (cases[i].arg == ARG_DATA) {
            t.sp = DATA2_BASE + REGION_SIZE;
            t.stub = DATA2_BASE;
            t.regions = regions2;
        }

        rc = run_task(&t, &ex, cases[i].watchdog);
        if (rc != 0) {
            k_printf("@RC stage=umode ev=error name=%s rc=%d\n", cases[i].name, rc);
            continue;
        }
        n++;
        k_printf("@RC stage=umode ev=task name=%s mmu=%d reason=%s code=%d type=%s pc=0x%08x "
                 "thumb=%u addr=0x%08x status=0x%08x ticks=%u canary=%s\n",
                 cases[i].name, mmu, reason_name(ex.reason), ex.code, type_name(&ex),
                 (unsigned)ex.pc, (unsigned)(ex.frame.cpsr >> 5) & 1u,
                 (unsigned)ex.addr, (unsigned)ex.status,
                 (unsigned)tick_count(), canary == CANARY ? "ok" : "bad");
    }
    return n;
}

/*
 * Error cases of umode_run() itself, then a task that reads the canary:
 * the refused layouts left nothing mapped for USR mode. Every case enters
 * at slot 0 of the loaded programs, which returns 42, so a case that is
 * wrongly accepted runs a real program that ends. The watchdog backstop
 * covers it too. A case that ran prints "ran" or "killed" instead of its
 * error code. Cases refused only with the MMU on are skipped without it
 * and listed in "skipped".
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
    const struct {
        const char *name;
        const umode_region_t *regions;
        unsigned count;
        uint32_t stub;
        int need_mmu;       /* refused only with the MMU on */
    } bad[] = {
        { "stub",    regions, 2, CODE_BASE, 0 },    /* read-only stub */
        { "align",   odd,     1, STUB_ADDR, 1 },    /* alignment is checked with the MMU on */
        { "overlap", overlap, 2, STUB_ADDR, 0 },
        { "kernel",  kernel,  1, STUB_ADDR, 0 },
        { "periph",  periph,  1, STUB_ADDR, 0 },
        { "ram",     outside, 1, STUB_ADDR, 0 },
    };
    umode_task_t t;
    unsigned i, skipped = 0;
    int mmu = mmu_enabled(), rc;

    k_printf("@RC stage=umode ev=errors mmu=%d", mmu);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (bad[i].need_mmu && !mmu) {
            continue;
        }
        load_programs();
        k_memset(&t, 0, sizeof(t));
        t.entry = CODE_BASE;
        t.sp = USER_SP;
        t.stub = bad[i].stub;
        t.regions = bad[i].regions;
        t.region_count = bad[i].count;
        rc = run_task(&t, &ex, 0);
        if (rc != 0) {
            k_printf(" %s=%d", bad[i].name, rc);
        } else {
            k_printf(" %s=%s", bad[i].name, ex.reason == UMODE_KILLED ? "killed" : "ran");
        }
    }
    k_printf(" skipped=");
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (bad[i].need_mmu && !mmu) {
            k_printf("%s%s", skipped++ ? "," : "", bad[i].name);
        }
    }
    k_printf("%s\n", skipped ? "" : "none");

    if (!mmu) {
        return;
    }
    load_programs();
    k_memset(&t, 0, sizeof(t));
    t.entry = CODE_BASE + 7 * 0x40u;    /* wrkern */
    t.sp = USER_SP;
    t.r[0] = (uint32_t)(uintptr_t)&canary;
    t.stub = STUB_ADDR;
    t.regions = regions;
    t.region_count = 2;
    run_task(&t, &ex, 0);
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
    load_programs();
    rc = run_task(&t, &ex, 0);
    k_printf("@RC stage=umode ev=stale rc=%d reason=%s code=%d\n", rc, reason_name(ex.reason),
             ex.code);
}

/*
 * MMU on: the caller left the task's sections privileged read-only or
 * unmapped. umode_run() maps them before it writes the stub, so the task
 * still runs and returns 42.
 */
static void check_premapped(void)
{
    static umode_exit_t ex;
    static const struct {
        const char *name;
        unsigned data_attr, code_attr;
    } pre[] = {
        { "ro",   MMU_ATTR_RO_CODE, MMU_ATTR_RO_CODE },
        { "none", MMU_ATTR_NONE,    MMU_ATTR_NONE },
    };
    umode_task_t t;
    unsigned i;
    int rc;

    for (i = 0; i < sizeof(pre) / sizeof(pre[0]); i++) {
        load_programs();
        mmu_map(DATA_BASE, REGION_SIZE, pre[i].data_attr);
        mmu_map(CODE_BASE, REGION_SIZE, pre[i].code_attr);
        k_memset(&t, 0, sizeof(t));
        t.entry = CODE_BASE;    /* case 0: returns 42 */
        t.sp = USER_SP;
        t.stub = STUB_ADDR;
        t.regions = regions;
        t.region_count = 2;
        rc = run_task(&t, &ex, 0);
        k_printf("@RC stage=umode ev=premapped name=%s rc=%d reason=%s code=%d\n", pre[i].name,
                 rc, reason_name(ex.reason), ex.code);
    }
}

void robco_main(void)
{
    unsigned tasks = 0;
    int c, rc;

    uart_init(115200);
    k_printf("@RC stage=umode ev=boot canary_addr=0x%08x\n", (unsigned)(uintptr_t)&canary);
    irq_init();
    watch_limit = BACKSTOP_TICKS;
    watch_kill = KILL_BACKSTOP;
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
    check_premapped();

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
        load_programs();
        run_task(&t, &ex, 0);   /* does not return: fatal */
    }
#endif
#ifdef UMODE_KMMU
    {
        static umode_exit_t ex;
        umode_task_t t;

        k_memset(&t, 0, sizeof(t));
        t.entry = CODE_BASE + 21 * 0x40u;
        t.sp = USER_SP;
        t.stub = STUB_ADDR;
        t.regions = regions;
        t.region_count = 2;
        k_printf("@RC stage=umode ev=kmmu\n");
        load_programs();
        run_task(&t, &ex, 0);   /* does not return: fatal */
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
