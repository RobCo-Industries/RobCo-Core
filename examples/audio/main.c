#include <stdint.h>

#include <robco/audio.h>
#include <robco/board.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>

#include "voice_demo.h"

#define TICK_HZ     100

/* A rising C major arpeggio. */
static const audio_note_t arpeggio[] = { { 523, 120 }, { 659, 120 }, { 784, 200 } };

/* Two short chirps and a gap, looped. */
static const audio_note_t alarm[] = { { 660, 60 }, { 0, 40 }, { 990, 60 } };

static const audio_note_t high = { 880, 300 };
/* A long rest in the middle, so a stop lands in it. */
static const audio_note_t rest_seq[] = { { 440, 50 }, { 0, 500 }, { 880, 50 } };
/* Runs of each trace rule case: the trace must be the same every time. */
#define RULE_RUNS 5

/* A short original jingle. */
static const char jingle[] = "terminal:d=8,o=5,b=200:c,g,e,p,4c6,a,4g.";

/* PCM buffer, filled at run time. One second at the highest rate. */
#define PCM_BUF     22050
static uint8_t pcm_buf[PCM_BUF];

/* Length of a busy call that must not scan its buffer. */
#define BUSY_BIG_LEN    (16u << 20)

/* Tick callback runs, to check the tick keeps its rhythm during PCM. */
static volatile uint32_t tick_runs;

static void count_tick(void)
{
    tick_runs++;
}

/*
 * A rising chirp as a triangle wave: the phase step grows with i. Sample
 * i is a pure function of i, so a test can rebuild the buffer and its CRC.
 */
static void pcm_fill(uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t v = ((i * (64 + (i >> 6))) >> 4) & 0xff;

        pcm_buf[i] = (uint8_t)(v < 128 ? v * 2 : (255 - v) * 2 + 1);
    }
}

/* Sleeps in the main loop, printing trace records as they come. */
static void sleep_ms(uint32_t ms)
{
    uint64_t end = timer_now_us() + (uint64_t)ms * 1000;

    while (!timer_expired(end)) {
        audio_poll();
        __asm__ volatile("wfi");
    }
    audio_poll();
}

static void case_begin(const char *name)
{
    k_printf("@RC stage=audio ev=case name=%s\n", name);
}

static void case_end(const char *name, int ret)
{
    audio_wait();
    k_printf("@RC stage=audio ev=case_end name=%s ret=%d\n", name, ret);
}

static void play_beep(void)
{
    case_begin("beep");
    case_end("beep", audio_beep());
}

static void play_seq(void)
{
    case_begin("seq");
    case_end("seq", audio_play(arpeggio, 3, 0));
}

static void play_tune(void)
{
    int ret;

    case_begin("tune");
    audio_set_volume(60);
    ret = audio_play_rtttl(jingle, 0);
    audio_wait();
    audio_set_volume(100);
    case_end("tune", ret);
}

static void play_loop(void)
{
    int ret;

    case_begin("loop");
    ret = audio_play(alarm, 3, AUDIO_LOOP);
    sleep_ms(700);
    audio_stop();
    case_end("loop", ret);
}

static void play_interrupt(void)
{
    int busy;
    int prio;

    case_begin("interrupt");
    audio_tone(440, 3000);
    sleep_ms(200);
    busy = audio_play(&high, 1, 0);
    prio = audio_play(&high, 1, AUDIO_PRIORITY);
    k_printf("@RC stage=audio ev=interrupt busy=%d prio=%d\n", busy, prio);
    case_end("interrupt", prio);
}

/* 800 samples at 8000 Hz: 100 ms. */
static void play_pcm(void)
{
    case_begin("pcm");
    pcm_fill(800);
    case_end("pcm", audio_play_pcm(pcm_buf, 800, 8000, 0));
}

/* 2205 samples at 22050 Hz and half volume: 100 ms. */
static void play_pcm_fast(void)
{
    int ret;

    case_begin("pcm_fast");
    pcm_fill(2205);
    audio_set_volume(50);
    ret = audio_play_pcm(pcm_buf, 2205, 22050, 0);
    audio_wait();
    audio_set_volume(100);
    case_end("pcm_fast", ret);
}

/* 3750 samples at 37500 Hz, the top rate: 100 ms. */
static void play_pcm_top(void)
{
    int ret;

    case_begin("pcm_top");
    pcm_fill(3750);
    ret = audio_play_pcm(pcm_buf, 3750, AUDIO_PCM_RATE_MAX, 0);
    audio_wait();
    case_end("pcm_top", ret);
}

/* One second, stopped after 200 ms. */
static void play_pcm_stop(void)
{
    int ret;

    case_begin("pcm_stop");
    pcm_fill(8000);
    ret = audio_play_pcm(pcm_buf, 8000, 8000, 0);
    sleep_ms(200);
    audio_stop();
    audio_wait();
    k_printf("@RC stage=audio ev=pcm_stop busy=%d\n", audio_busy());
    case_end("pcm_stop", ret);
}

/* PCM and tones replace each other only with AUDIO_PRIORITY. */
static void play_pcm_swap(void)
{
    int busy, prio, tbusy, tprio, big;
    uint64_t t0;
    uint32_t big_us;

    case_begin("pcm_swap");
    pcm_fill(8000);
    audio_tone(440, 3000);
    sleep_ms(100);
    busy = audio_play_pcm(pcm_buf, 8000, 8000, 0);
    /*
     * A busy call returns before it reads the buffer. 16 MB runs past
     * pcm_buf into RAM; a CRC over it would take far longer than this.
     */
    t0 = timer_now_us();
    big = audio_play_pcm(pcm_buf, BUSY_BIG_LEN, 8000, 0);
    big_us = (uint32_t)(timer_now_us() - t0);
    prio = audio_play_pcm(pcm_buf, 8000, 8000, AUDIO_PRIORITY);
    sleep_ms(100);
    tbusy = audio_play(&high, 1, 0);
    tprio = audio_play(&high, 1, AUDIO_PRIORITY);
    k_printf("@RC stage=audio ev=pcm_swap busy=%d prio=%d tbusy=%d tprio=%d big=%d big_us=%u\n",
             busy, prio, tbusy, tprio, big, (unsigned)big_us);
    case_end("pcm_swap", tprio);
}

/*
 * Plays 0.5 s from the first half of the buffer, cuts it after 50 ms
 * with `kind`, then overwrites that half. Prints how many samples the
 * play output before the cut, and how many the old buffer gave after.
 */
static void pcm_cut_one(int kind)
{
    static const char *const names[] = { "stop", "tone", "pcm" };
    uint8_t *old = pcm_buf;
    uint8_t *next = pcm_buf + PCM_BUF / 2;
    uint32_t b0, t0, t1, p1;
    uint32_t s;
    int ret, cut;

    pcm_fill(PCM_BUF);
    ret = audio_play_pcm(old, PCM_BUF / 2, 22050, 0);
    sleep_ms(50);
    /* Masked so the tick cannot start the new sound before the count. */
    s = irq_save();
    (void)audio_pcm_samples(&b0);
    if (kind == 0) {
        audio_stop();
        cut = 0;
    } else if (kind == 1) {
        cut = audio_tone(440, 50);
    } else {
        cut = audio_play_pcm(next, PCM_BUF / 2, 22050, AUDIO_PRIORITY);
    }
    t0 = audio_pcm_samples(0);
    irq_restore(s);
    k_memset(old, 0x5a, PCM_BUF / 2);
    sleep_ms(30);
    t1 = audio_pcm_samples(&p1);
    if (kind != 2) {
        p1 = 0;
    }
    audio_stop();
    audio_wait();
    k_printf("@RC stage=audio ev=pcm_cut kind=%s ret=%d cut=%d before=%u after=%d\n",
             names[kind], ret, cut, (unsigned)b0, (int)(t1 - t0 - p1));
}

/* A stop or a replacement stops reading the PCM buffer before it returns. */
static void play_pcm_cut(void)
{
    case_begin("pcm_cut");
    pcm_cut_one(0);
    pcm_cut_one(1);
    pcm_cut_one(2);
    case_end("pcm_cut", 0);
}

/* 25 ms looped for 300 ms. */
static void play_pcm_loop(void)
{
    int ret;
    int busy;

    case_begin("pcm_loop");
    pcm_fill(200);
    ret = audio_play_pcm(pcm_buf, 200, 8000, AUDIO_LOOP);
    sleep_ms(300);
    busy = audio_busy();
    audio_stop();
    audio_wait();
    k_printf("@RC stage=audio ev=pcm_loop busy=%d\n", busy);
    case_end("pcm_loop", ret);
}

/* One second at 22050 Hz while the tick counts. */
static void play_pcm_tick(void)
{
    uint32_t c0, r0, m0;
    uint64_t t0;
    int ret;

    case_begin("pcm_tick");
    pcm_fill(PCM_BUF);
    c0 = tick_count();
    r0 = tick_runs;
    m0 = tick_missed();
    t0 = timer_now_us();
    ret = audio_play_pcm(pcm_buf, PCM_BUF, 22050, 0);
    audio_wait();
    k_printf("@RC stage=audio ev=pcm_tick ticks=%u runs=%u missed=%u us=%u\n",
             (unsigned)(tick_count() - c0), (unsigned)(tick_runs - r0),
             (unsigned)(tick_missed() - m0), (unsigned)(timer_now_us() - t0));
    case_end("pcm_tick", ret);
}

/* Chunks of pcm_buf for the queue cases, at 8000 Hz. */
#define CH_A_OFF    0u
#define CH_A_LEN    800u
#define CH_B_OFF    800u
#define CH_B_LEN    600u
#define CH_C_OFF    1400u
#define CH_C_LEN    1000u

/*
 * Three chunks of 100, 75 and 125 ms played back to back with
 * audio_pcm_queue(). Prints the queue results, audio_pcm_can_queue() at
 * each step, and how often audio_busy() read 0 before the end.
 */
static void play_pcm_chain(void)
{
    int ret, can0, q1, can1, qbusy, can2, q2, can3, can4, under;
    uint32_t idle = 0;

    case_begin("pcm_chain");
    pcm_fill(PCM_BUF);
    ret = audio_play_pcm(pcm_buf + CH_A_OFF, CH_A_LEN, 8000, 0);
    /* The play waits for the tick: queueing works already. */
    can0 = audio_pcm_can_queue();
    q1 = audio_pcm_queue(pcm_buf + CH_B_OFF, CH_B_LEN);
    can1 = audio_pcm_can_queue();
    qbusy = audio_pcm_queue(pcm_buf + CH_C_OFF, CH_C_LEN);
    /* Turns 1 when B takes over: A is free then. */
    while (!audio_pcm_can_queue()) {
        if (!audio_busy()) {
            idle++;
            break;
        }
        audio_poll();
    }
    can2 = audio_pcm_can_queue();
    q2 = audio_pcm_queue(pcm_buf + CH_C_OFF, CH_C_LEN);
    can3 = audio_pcm_can_queue();
    while (!audio_pcm_can_queue()) {
        if (!audio_busy()) {
            idle++;
            break;
        }
        audio_poll();
    }
    audio_wait();
    can4 = audio_pcm_can_queue();
    /* The play has ended: nothing to queue behind. */
    under = audio_pcm_queue(pcm_buf + CH_A_OFF, CH_A_LEN);
    k_printf("@RC stage=audio ev=pcm_chain can0=%d q1=%d can1=%d qbusy=%d can2=%d q2=%d "
             "can3=%d can4=%d under=%d idle=%u\n",
             can0, q1, can1, qbusy, can2, q2, can3, can4, under, (unsigned)idle);
    case_end("pcm_chain", ret);
}

/* audio_pcm_queue() errors: bad arguments, nothing playing, a looped play. */
static void play_pcm_qerr(void)
{
    int idle, null, zero, loop, lcan, ret;

    case_begin("pcm_qerr");
    pcm_fill(200);
    idle = audio_pcm_queue(pcm_buf, 100);
    null = audio_pcm_queue(0, 100);
    zero = audio_pcm_queue(pcm_buf, 0);
    ret = audio_play_pcm(pcm_buf, 200, 8000, AUDIO_LOOP);
    loop = audio_pcm_queue(pcm_buf, 100);
    lcan = audio_pcm_can_queue();
    sleep_ms(50);
    audio_stop();
    audio_wait();
    k_printf("@RC stage=audio ev=pcm_qerr idle=%d null=%d zero=%d loop=%d lcan=%d\n",
             idle, null, zero, loop, lcan);
    case_end("pcm_qerr", ret);
}

/*
 * Plays 0.25 s from the first quarter of the buffer with the second
 * quarter queued, cuts after 50 ms with `kind`, then overwrites both
 * quarters at once. Prints how many samples the play output before the
 * cut, and how many either buffer gave after.
 */
static void pcm_qcut_one(int kind)
{
    static const char *const names[] = { "stop", "tone" };
    uint8_t *a = pcm_buf;
    uint8_t *b = pcm_buf + PCM_BUF / 4;
    uint32_t b0, t0, t1;
    uint32_t s;
    int ret, q, cut, can;

    pcm_fill(PCM_BUF);
    ret = audio_play_pcm(a, PCM_BUF / 4, 22050, 0);
    q = audio_pcm_queue(b, PCM_BUF / 4);
    sleep_ms(50);
    s = irq_save();
    (void)audio_pcm_samples(&b0);
    if (kind == 0) {
        audio_stop();
        cut = 0;
    } else {
        cut = audio_tone(440, 50);
    }
    t0 = audio_pcm_samples(0);
    irq_restore(s);
    k_memset(a, 0x5a, PCM_BUF / 2);
    can = audio_pcm_can_queue();
    /* Longer than A: a buffer that still waited would have taken over. */
    sleep_ms(300);
    t1 = audio_pcm_samples(0);
    audio_stop();
    audio_wait();
    k_printf("@RC stage=audio ev=pcm_qcut kind=%s ret=%d q=%d cut=%d can=%d before=%u "
             "after=%d\n",
             names[kind], ret, q, cut, can, (unsigned)b0, (int)(t1 - t0));
}

/* A stop or a tone drops the waiting buffer and stops reading both. */
static void play_pcm_qcut(void)
{
    case_begin("pcm_qcut");
    pcm_qcut_one(0);
    pcm_qcut_one(1);
    case_end("pcm_qcut", 0);
}

/* Underrun: the play ends, a queue call fails, a new play works. */
static void play_pcm_under(void)
{
    int ret, under, can, again;

    case_begin("pcm_under");
    pcm_fill(PCM_BUF);
    ret = audio_play_pcm(pcm_buf, 80, 8000, 0);
    audio_wait();
    under = audio_pcm_queue(pcm_buf + 80, 80);
    can = audio_pcm_can_queue();
    again = audio_play_pcm(pcm_buf + 80, 80, 8000, 0);
    audio_wait();
    k_printf("@RC stage=audio ev=pcm_under under=%d can=%d again=%d\n", under, can, again);
    case_end("pcm_under", ret);
}

/*
 * An overlay over a play: one on a play that waits for its tick, one on a
 * running play that is longer than what is left of it. Prints the codes.
 */
static void play_pcm_mix(void)
{
    int idle, ret, mix0, busy0, again, null, ret2, mix1, live, done;

    case_begin("pcm_mix");
    pcm_fill(PCM_BUF);
    idle = audio_pcm_mix(pcm_buf + 2000, 200);
    ret = audio_play_pcm(pcm_buf, 800, 8000, 0);
    mix0 = audio_pcm_mix(pcm_buf + 2000, 200);
    busy0 = audio_pcm_mix_busy();
    again = audio_pcm_mix(pcm_buf + 2000, 200);
    null = audio_pcm_mix(0, 200);
    audio_wait();
    ret2 = audio_play_pcm(pcm_buf, 800, 8000, 0);
    sleep_ms(50);
    mix1 = audio_pcm_mix(pcm_buf + 4000, 2000);
    live = audio_pcm_mix_busy();
    audio_wait();
    done = audio_pcm_mix_busy();
    k_printf("@RC stage=audio ev=pcm_mix idle=%d mix0=%d busy0=%d again=%d null=%d ret2=%d "
             "mix1=%d live=%d done=%d\n",
             idle, mix0, busy0, again, null, ret2, mix1, live, done);
    case_end("pcm_mix", ret);
}

/*
 * The voice of voice_demo.h with its overlay, given while the voice waits
 * for its tick, so both start on its first sample. The overlay's burst
 * starts 80 ms in. Prints the codes of the calls around it.
 */
static void play_pcm_voice(void)
{
    static audio_voice_span_t spans[VOICE_SPANS];
    int loop, none, ret, mix, queue, can, busy;

    case_begin("pcm_voice");
    voice_demo_spans(spans);
    voice_demo_mix(pcm_buf);
    loop = audio_play_voice(spans, VOICE_SPANS, VOICE_RATE, AUDIO_LOOP);
    none = audio_play_voice(spans, 0, VOICE_RATE, 0);
    ret = audio_play_voice(spans, VOICE_SPANS, VOICE_RATE, 0);
    mix = audio_pcm_mix(pcm_buf, VOICE_MIX_LEN);
    queue = audio_pcm_queue(pcm_buf, VOICE_MIX_LEN);
    can = audio_pcm_can_queue();
    busy = audio_play_voice(spans, VOICE_SPANS, VOICE_RATE, 0);
    audio_wait();
    k_printf("@RC stage=audio ev=pcm_voice loop=%d none=%d mix=%d queue=%d can=%d busy=%d\n",
             loop, none, mix, queue, can, busy);
    case_end("pcm_voice", ret);
}

/* Bad arguments return their codes and play nothing. */
static void play_pcm_args(void)
{
    case_begin("pcm_args");
    k_printf("@RC stage=audio ev=pcm_args low=%d high=%d null=%d zero=%d flags=%d\n",
             audio_play_pcm(pcm_buf, 10, AUDIO_PCM_RATE_MIN - 1, 0),
             audio_play_pcm(pcm_buf, 10, AUDIO_PCM_RATE_MAX + 1, 0),
             audio_play_pcm(0, 10, 8000, 0),
             audio_play_pcm(pcm_buf, 0, 8000, 0),
             audio_play_pcm(pcm_buf, 10, 8000, 4));
    case_end("pcm_args", 0);
}

static void play_pcm_all(void)
{
    play_pcm();
    play_pcm_fast();
    play_pcm_top();
    play_pcm_stop();
    play_pcm_swap();
    play_pcm_cut();
    play_pcm_loop();
    play_pcm_tick();
    play_pcm_args();
    play_pcm_chain();
    play_pcm_qerr();
    play_pcm_qcut();
    play_pcm_under();
    play_pcm_mix();
    play_pcm_voice();
}

/* A stop, then PCM before the tick runs: IRQs stay masked across both calls. */
static void play_stop_pcm(void)
{
    uint32_t s;
    int ret;

    case_begin("stop_pcm");
    pcm_fill(800);
    audio_tone(440, 3000);
    sleep_ms(50);
    s = irq_save();
    audio_stop();
    ret = audio_play_pcm(pcm_buf, 800, 8000, 0);
    irq_restore(s);
    case_end("stop_pcm", ret);
}

/* A stop in the 500 ms rest of a sequence. */
static void play_stop_rest(void)
{
    int ret;

    case_begin("stop_rest");
    ret = audio_play(rest_seq, 3, 0);
    sleep_ms(250);
    audio_stop();
    case_end("stop_rest", ret);
}

static void play_rule(void)
{
    int i;

    for (i = 0; i < RULE_RUNS; i++) {
        play_stop_pcm();
    }
    for (i = 0; i < RULE_RUNS; i++) {
        play_stop_rest();
    }
}

/*
 * The rest level moves to the midpoint with a ramp; a second call does
 * nothing. A tone then still sounds, now as a centred square on the
 * sample path, so the sample count grows while it plays. Last, as the
 * level stays there.
 */
static void play_rest(void)
{
    int ret, again, tone;
    uint32_t s0, s1;

    case_begin("rest");
    audio_set_volume(50);
    ret = audio_rest_mid();
    audio_wait();
    again = audio_rest_mid();
    s0 = audio_pcm_samples(0);
    tone = audio_tone(440, 50);
    audio_wait();
    s1 = audio_pcm_samples(0);
    audio_set_volume(100);
    k_printf("@RC stage=audio ev=rest again=%d tone=%d grew=%u\n", again, tone,
             (unsigned)(s1 - s0));
    case_end("rest", ret);
}

static void play_all(void)
{
    play_beep();
    play_seq();
    play_tune();
    play_loop();
    play_interrupt();
    play_pcm_all();
    play_rule();
    play_rest();
}

void robco_main(void)
{
    const robco_board_t *b = board_get();
    int pin0, pin1;
    unsigned fn0, fn1;
    int early, preinit, prepcm, err;
    int c;

    uart_init(115200);
    k_printf("\n@RC stage=audio ev=boot soc=%s base=0x%08x board_rev=0x%08x\n",
             b->soc_name, (unsigned)b->periph_base, (unsigned)b->board_rev);

    /* Both must fail cleanly before the IRQs and the tick run. */
    early = audio_init();
    preinit = audio_tone(440, 100);
    prepcm = audio_play_pcm(pcm_buf, 1, 8000, 0);
    k_printf("@RC stage=audio ev=order init=%d tone=%d pcm=%d\n", early, preinit, prepcm);

    irq_init();
    tick_init(TICK_HZ);
    tick_register(count_tick);
    irq_cpu_enable();

    err = audio_init();
    audio_get_pins(&pin0, &fn0, &pin1, &fn1);
    k_printf("@RC stage=audio ev=backend name=%s probe_pwm=%s err=%d pin0=%d fn0=%u "
             "pin1=%d fn1=%u\n",
             audio_backend(), audio_probe_name(audio_probe_result()), err,
             pin0, fn0, pin1, fn1);

    play_all();
    k_printf("@RC stage=audio ev=done\n");

    /* Replay on request, for listening tests on real hardware. */
    k_printf("@RC stage=audio ev=ready keys=b,s,t,l,i,p,c,v,a\n");
    for (;;) {
        audio_poll();
        c = uart_poll();
        switch (c) {
        case 'b': play_beep(); break;
        case 's': play_seq(); break;
        case 't': play_tune(); break;
        case 'l': play_loop(); break;
        case 'i': play_interrupt(); break;
        case 'p': play_pcm(); break;
        case 'c': play_pcm_chain(); break;
        case 'v': play_pcm_voice(); break;
        case 'a': play_all(); break;
        default: break;
        }
        if (c < 0) {
            __asm__ volatile("wfi");
        }
    }
}
