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

void audio_trace_out(uint32_t hz, unsigned vol, uint32_t t_ms);
void audio_trace_pcm(uint32_t rate, uint32_t len, unsigned vol, uint32_t crc, uint32_t t_ms);
void audio_trace_pcm_end(uint32_t n, uint32_t last, uint32_t t0_ms, uint64_t dur_us);
void audio_trace_flush(void);

#define OP_TONE     0       /* a tone, or a stop when hz is 0 */
#define OP_PCM      1
#define OP_PCM_END  2

typedef struct {
    uint32_t op;
    uint32_t a;             /* tone: hz.  pcm: rate.  pcm_end: n */
    uint32_t b;             /* tone: vol. pcm: len.   pcm_end: last */
    uint32_t c;             /* tone, pcm: t_ms. pcm_end: start t_ms */
    uint32_t d;             /* pcm: vol.  pcm_end: duration in us, low word */
    uint32_t e;             /* pcm: crc.  pcm_end: duration in us, high word */
} record_t;

static record_t queue[ROBCO_AUDIO_TRACE_QUEUE];
static volatile uint32_t head;      /* written by IRQ handlers */
static volatile uint32_t tail;      /* written by the main context */
static volatile uint32_t dropped;   /* written by IRQ handlers */
static uint32_t dropped_seen;

/* IRQ context. Handlers do not nest, so the tick and the PCM IRQ never race here. */
static void put(uint32_t op, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
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
    head = h + 1;
}

/* IRQ context. */
void audio_trace_out(uint32_t hz, unsigned vol, uint32_t t_ms)
{
    put(OP_TONE, hz, vol, t_ms, 0, 0);
}

/* IRQ context. */
void audio_trace_pcm(uint32_t rate, uint32_t len, unsigned vol, uint32_t crc, uint32_t t_ms)
{
    put(OP_PCM, rate, len, t_ms, vol, crc);
}

/* IRQ context. The division by 1000 waits for the main context. */
void audio_trace_pcm_end(uint32_t n, uint32_t last, uint32_t t0_ms, uint64_t dur_us)
{
    put(OP_PCM_END, n, last, t0_ms, (uint32_t)dur_us, (uint32_t)(dur_us >> 32));
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
            k_printf("@RC stage=%s ev=audio op=pcm rate=%u len=%u vol=%u crc=%08x t_ms=%u\n",
                     ROBCO_STAGE_NAME, (unsigned)r.a, (unsigned)r.b, (unsigned)r.d,
                     (unsigned)r.e, (unsigned)r.c);
            break;
        case OP_PCM_END:
            dur = ((uint64_t)r.e << 32) | r.d;
            k_printf("@RC stage=%s ev=audio op=pcm_end n=%u last=%u t_ms=%u\n",
                     ROBCO_STAGE_NAME, (unsigned)r.a, (unsigned)r.b,
                     (unsigned)(r.c + (uint32_t)(dur / 1000u)));
            break;
        default:
            k_printf("@RC stage=%s ev=audio op=%s hz=%u vol=%u t_ms=%u\n", ROBCO_STAGE_NAME,
                     r.a ? "tone" : "stop", (unsigned)r.a, (unsigned)r.b, (unsigned)r.c);
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
