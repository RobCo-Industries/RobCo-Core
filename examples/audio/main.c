#include <stdint.h>

#include <robco/audio.h>
#include <robco/board.h>
#include <robco/irq.h>
#include <robco/klibc.h>
#include <robco/tick.h>
#include <robco/timer.h>
#include <robco/uart.h>

#define TICK_HZ     100

/* A rising C major arpeggio. */
static const audio_note_t arpeggio[] = { { 523, 120 }, { 659, 120 }, { 784, 200 } };

/* Two short chirps and a gap, looped. */
static const audio_note_t alarm[] = { { 660, 60 }, { 0, 40 }, { 990, 60 } };

static const audio_note_t high = { 880, 300 };

/* A short original jingle. */
static const char jingle[] = "terminal:d=8,o=5,b=200:c,g,e,p,4c6,a,4g.";

/* PCM buffer, filled at run time. One second at the highest rate. */
#define PCM_BUF     22050
static uint8_t pcm_buf[PCM_BUF];

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
    int busy, prio, tbusy, tprio;

    case_begin("pcm_swap");
    pcm_fill(8000);
    audio_tone(440, 3000);
    sleep_ms(100);
    busy = audio_play_pcm(pcm_buf, 8000, 8000, 0);
    prio = audio_play_pcm(pcm_buf, 8000, 8000, AUDIO_PRIORITY);
    sleep_ms(100);
    tbusy = audio_play(&high, 1, 0);
    tprio = audio_play(&high, 1, AUDIO_PRIORITY);
    k_printf("@RC stage=audio ev=pcm_swap busy=%d prio=%d tbusy=%d tprio=%d\n",
             busy, prio, tbusy, tprio);
    case_end("pcm_swap", tprio);
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
    play_pcm_stop();
    play_pcm_swap();
    play_pcm_loop();
    play_pcm_tick();
    play_pcm_args();
}

static void play_all(void)
{
    play_beep();
    play_seq();
    play_tune();
    play_loop();
    play_interrupt();
    play_pcm_all();
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
    k_printf("@RC stage=audio ev=ready keys=b,s,t,l,i,p,a\n");
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
        case 'a': play_all(); break;
        default: break;
        }
        if (c < 0) {
            __asm__ volatile("wfi");
        }
    }
}
