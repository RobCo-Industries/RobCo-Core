/*
 * Trace backend: one marker per change. The tick callback and the PCM
 * IRQ only queue a record. audio_poll() and audio_wait() print the queue
 * from the main context, because k_printf() and the UART driver have no
 * lock and must not run from IRQ context while the main flow prints.
 *
 * Producers are IRQ handlers, which do not nest, and the consumer is the
 * main context, on one core: the indices are volatile and each side
 * writes only its own.
 */
#include <stdint.h>

#include <robco/audio.h>
#include <robco/klibc.h>

#ifndef ROBCO_STAGE_NAME
#define ROBCO_STAGE_NAME        "robco"
#endif
#ifndef ROBCO_AUDIO_TRACE_QUEUE
#define ROBCO_AUDIO_TRACE_QUEUE 64
#endif

void audio_trace_out(uint32_t hz, uint32_t gain, uint32_t t_ms);
void audio_trace_pcm(uint32_t rate, uint32_t len, uint32_t gain, uint32_t crc, uint32_t t_ms,
                     unsigned loop);
void audio_trace_pcm_end(uint32_t n, uint32_t last, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_pcm_next(uint32_t len, uint32_t crc, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_pcm_mix(uint32_t len, uint32_t crc, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_voice(uint32_t rate, uint32_t spans, uint32_t len, uint32_t gain, uint32_t crc,
                       uint32_t t_ms);
void audio_trace_gain(uint32_t gain, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_flush(void);

#define OP_TONE     0       /* a tone, or a stop when hz is 0 */
#define OP_PCM      1
#define OP_PCM_END  2
#define OP_PCM_NEXT 3
#define OP_PCM_MIX  4
#define OP_VOICE    5
#define OP_GAIN     6

typedef struct {
    uint32_t op;
    uint32_t a;             /* tone: hz.  pcm, voice: rate. pcm_end: n. pcm_next, pcm_mix: len.
                               gain: gain */
    uint32_t b;             /* tone: gain. pcm, voice: len. pcm_end: last. pcm_next, pcm_mix: crc */
    uint32_t c;             /* tone, pcm, voice: t_ms. gain, pcm_end, pcm_next, pcm_mix: start t_ms */
    uint32_t d;             /* pcm, voice: gain. others: time since start in us, low word */
    uint32_t e;             /* pcm, voice: crc. others: time since start in us, high word */
    uint32_t f;             /* voice: span count. pcm: 1 when the play loops */
} record_t;

static record_t queue[ROBCO_AUDIO_TRACE_QUEUE];
static volatile uint32_t head;      /* written by IRQ handlers */
static volatile uint32_t tail;      /* written by the main context */
static volatile uint32_t dropped;   /* written by IRQ handlers */
static uint32_t dropped_seen;

/*
 * IRQ context, or the main context with IRQs masked. Handlers do not nest
 * and there is one core, so no two producers ever race here.
 */
static void put6(uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
                 uint32_t f)
{
    uint32_t h = head;
    record_t *r;

    if (h - tail >= ROBCO_AUDIO_TRACE_QUEUE) {
        dropped++;
        return;
    }
    r = &queue[h % ROBCO_AUDIO_TRACE_QUEUE];
    r->op = op;
    r->a = a;
    r->b = b;
    r->c = c;
    r->d = d;
    r->e = e;
    r->f = f;
    head = h + 1;
}

static void put(uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
{
    put6(op, a, b, c, d, e, 0);
}

/*
 * Start lines print vol=, the gain in whole percent rounded down. A gain
 * that is not a whole percent adds gain=<0..10000> at the end of the line.
 */

/* IRQ context. */
void audio_trace_out(uint32_t hz, uint32_t gain, uint32_t t_ms)
{
    put(OP_TONE, hz, gain, t_ms, 0, 0);
}

/* IRQ context. `loop` is 1 for a play that loops: its line ends with loop=1. */
void audio_trace_pcm(uint32_t rate, uint32_t len, uint32_t gain, uint32_t crc, uint32_t t_ms,
                     unsigned loop)
{
    put6(OP_PCM, rate, len, t_ms, gain, crc, loop);
}

/* IRQ context. A voice starts. */
void audio_trace_voice(uint32_t rate, uint32_t spans, uint32_t len, uint32_t gain, uint32_t crc,
                       uint32_t t_ms)
{
    put6(OP_VOICE, rate, len, t_ms, gain, crc, spans);
}

/*
 * IRQ context, or IRQs masked. The gain's target changed while a play
 * sounds. The division waits as for pcm_end.
 */
void audio_trace_gain(uint32_t gain, uint32_t t0_ms, uint64_t dur_us)
{
    put(OP_GAIN, gain, 0, t0_ms, (uint32_t)dur_us, (uint32_t)(dur_us >> 32));
}

/* Prints " gain=<g>" when g is not a whole percent. Main context. */
static void gain_suffix(uint32_t g)
{
    if (g % 100u) {
        k_printf(" gain=%u", (unsigned)g);
    }
}

/* IRQ context. The division by 1000 waits for the main context. */
void audio_trace_pcm_end(uint32_t n, uint32_t last, uint32_t t0_ms, uint64_t dur_us)
{
    put(OP_PCM_END, n, last, t0_ms, (uint32_t)dur_us, (uint32_t)(dur_us >> 32));
}

/* IRQ context. A queued buffer took over; the division waits as for pcm_end. */
void audio_trace_pcm_next(uint32_t len, uint32_t crc, uint32_t t0_ms, uint64_t dur_us)
{
    put(OP_PCM_NEXT, len, crc, t0_ms, (uint32_t)dur_us, (uint32_t)(dur_us >> 32));
}

/* IRQ context, or IRQs masked. An overlay started; the division waits as for pcm_end. */
void audio_trace_pcm_mix(uint32_t len, uint32_t crc, uint32_t t0_ms, uint64_t dur_us)
{
    put(OP_PCM_MIX, len, crc, t0_ms, (uint32_t)dur_us, (uint32_t)(dur_us >> 32));
}

/* Main context. */
void audio_trace_flush(void)
{
    uint32_t d;

    while (tail != head) {
        record_t r = queue[tail % ROBCO_AUDIO_TRACE_QUEUE];
        uint64_t dur;

        tail = tail + 1;
        switch (r.op) {
        case OP_PCM:
            k_printf("@RC stage=%s ev=audio op=pcm rate=%u len=%u vol=%u crc=%08x t_ms=%u%s",
                     ROBCO_STAGE_NAME, (unsigned)r.a, (unsigned)r.b, (unsigned)(r.d / 100u),
                     (unsigned)r.e, (unsigned)r.c, r.f ? " loop=1" : "");
            gain_suffix(r.d);
            k_printf("\n");
            break;
        case OP_VOICE:
            k_printf("@RC stage=%s ev=audio op=voice rate=%u spans=%u len=%u vol=%u crc=%08x "
                     "t_ms=%u",
                     ROBCO_STAGE_NAME, (unsigned)r.a, (unsigned)r.f, (unsigned)r.b,
                     (unsigned)(r.d / 100u), (unsigned)r.e, (unsigned)r.c);
            gain_suffix(r.d);
            k_printf("\n");
            break;
        case OP_GAIN:
            dur = ((uint64_t)r.e << 32) | r.d;
            k_printf("@RC stage=%s ev=audio op=gain gain=%u t_ms=%u\n", ROBCO_STAGE_NAME,
                     (unsigned)r.a, (unsigned)(r.c + (uint32_t)(dur / 1000u)));
            break;
        case OP_PCM_END:
            dur = ((uint64_t)r.e << 32) | r.d;
            k_printf("@RC stage=%s ev=audio op=pcm_end n=%u last=%u t_ms=%u\n",
                     ROBCO_STAGE_NAME, (unsigned)r.a, (unsigned)r.b,
                     (unsigned)(r.c + (uint32_t)(dur / 1000u)));
            break;
        case OP_PCM_NEXT:
            dur = ((uint64_t)r.e << 32) | r.d;
            k_printf("@RC stage=%s ev=audio op=pcm_next len=%u crc=%08x t_ms=%u\n",
                     ROBCO_STAGE_NAME, (unsigned)r.a, (unsigned)r.b,
                     (unsigned)(r.c + (uint32_t)(dur / 1000u)));
            break;
        case OP_PCM_MIX:
            dur = ((uint64_t)r.e << 32) | r.d;
            k_printf("@RC stage=%s ev=audio op=pcm_mix len=%u crc=%08x t_ms=%u\n",
                     ROBCO_STAGE_NAME, (unsigned)r.a, (unsigned)r.b,
                     (unsigned)(r.c + (uint32_t)(dur / 1000u)));
            break;
        default:
            /* A stop line is no start line: no gain= on it. */
            k_printf("@RC stage=%s ev=audio op=%s hz=%u vol=%u t_ms=%u", ROBCO_STAGE_NAME,
                     r.a ? "tone" : "stop", (unsigned)r.a, (unsigned)(r.b / 100u), (unsigned)r.c);
            if (r.a) {
                gain_suffix(r.b);
            }
            k_printf("\n");
            break;
        }
    }
    d = dropped;
    if (d != dropped_seen) {
        k_printf("@RC stage=%s ev=audio_drop n=%u\n", ROBCO_STAGE_NAME,
                 (unsigned)(d - dropped_seen));
        dropped_seen = d;
    }
}
