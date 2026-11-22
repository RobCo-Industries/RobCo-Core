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

static void play_all(void)
{
    play_beep();
    play_seq();
    play_tune();
    play_loop();
    play_interrupt();
}

void robco_main(void)
{
    const robco_board_t *b = board_get();
    int pin0, pin1;
    unsigned fn0, fn1;
    int early, preinit, err;
    int c;

    uart_init(115200);
    k_printf("\n@RC stage=audio ev=boot soc=%s base=0x%08x board_rev=0x%08x\n",
             b->soc_name, (unsigned)b->periph_base, (unsigned)b->board_rev);

    /* Both must fail cleanly before the IRQs and the tick run. */
    early = audio_init();
    preinit = audio_tone(440, 100);
    k_printf("@RC stage=audio ev=order init=%d tone=%d\n", early, preinit);

    irq_init();
    tick_init(TICK_HZ);
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
    k_printf("@RC stage=audio ev=ready keys=b,s,t,l,i,a\n");
    for (;;) {
        audio_poll();
        c = uart_poll();
        switch (c) {
        case 'b': play_beep(); break;
        case 's': play_seq(); break;
        case 't': play_tune(); break;
        case 'l': play_loop(); break;
        case 'i': play_interrupt(); break;
        case 'a': play_all(); break;
        default: break;
        }
        if (c < 0) {
            __asm__ volatile("wfi");
        }
    }
}
