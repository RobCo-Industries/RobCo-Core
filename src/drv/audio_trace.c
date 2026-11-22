/*
 * Trace backend: one marker per change. The tick callback only queues a
 * record. audio_poll() and audio_wait() print the queue from the main
 * context, because k_printf() and the UART driver have no lock and must
 * not run from IRQ context while the main flow prints.
 *
 * Single producer (tick) and single consumer (main), one core: the
 * indices are volatile and each side writes only its own.
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
void audio_trace_flush(void);

typedef struct {
    uint32_t hz;
    uint32_t vol;
    uint32_t t_ms;
} record_t;

static record_t queue[ROBCO_AUDIO_TRACE_QUEUE];
static volatile uint32_t head;      /* written by the tick */
static volatile uint32_t tail;      /* written by the main context */
static volatile uint32_t dropped;   /* written by the tick */
static uint32_t dropped_seen;

/* IRQ context. */
void audio_trace_out(uint32_t hz, unsigned vol, uint32_t t_ms)
{
    uint32_t h = head;

    if (h - tail >= ROBCO_AUDIO_TRACE_QUEUE) {
        dropped++;
        return;
    }
    queue[h % ROBCO_AUDIO_TRACE_QUEUE].hz = hz;
    queue[h % ROBCO_AUDIO_TRACE_QUEUE].vol = vol;
    queue[h % ROBCO_AUDIO_TRACE_QUEUE].t_ms = t_ms;
    head = h + 1;
}

/* Main context. */
void audio_trace_flush(void)
{
    uint32_t d;

    while (tail != head) {
        record_t r = queue[tail % ROBCO_AUDIO_TRACE_QUEUE];

        tail = tail + 1;
        k_printf("@RC stage=%s ev=audio op=%s hz=%u vol=%u t_ms=%u\n", ROBCO_STAGE_NAME,
                 r.hz ? "tone" : "stop", (unsigned)r.hz, (unsigned)r.vol, (unsigned)r.t_ms);
    }
    d = dropped;
    if (d != dropped_seen) {
        k_printf("@RC stage=%s ev=audio_drop n=%u\n", ROBCO_STAGE_NAME,
                 (unsigned)(d - dropped_seen));
        dropped_seen = d;
    }
}
