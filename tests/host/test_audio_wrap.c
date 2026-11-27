/*
 * Plays in src/drv/audio.c that run past the 32-bit wrap of the
 * microsecond counter, about 71.6 minutes: the overlay's trace time, the
 * play's own pcm_next and pcm_end times, and the sample count. The test
 * builds the driver and the trace backend in, as test_audio_stop.c does.
 */
#include <stdint.h>
#include <stdlib.h>

#define ROBCO_STAGE_NAME "host"

/* Before the C library headers: macOS turns memcpy and friends into macros. */
#include "../../src/drv/audio.c"
#include "../../src/drv/audio_trace.c"

#include "audio_fake.h"

#define RATE        4000u                   /* 250 us a sample */
#define WRAP_US     4294967296ull
#define LONG_S      4300u                   /* past the wrap */

static uint8_t loop[RATE];
static uint8_t over[100];

/* Runs the IRQ for `us` 64-bit microseconds. */
static void run_long(uint64_t us)
{
    while (us) {
        uint32_t step = us > 1000000000u ? 1000000000u : (uint32_t)us;

        run_us(step);
        us -= step;
    }
}

/* A fresh driver with the counter 5 s under its wrap. */
static void start_near_wrap(void)
{
    fresh(AUDIO_BACKEND_TRACE);
    fake_us = 0xFFFFFFFFu - 5000000u;
    last_us = fake_us;
}

TEST(a_looped_play_past_the_wrap)
{
    uint64_t d = WRAP_US + 5000000u;        /* play time when the overlay joins */
    uint64_t join = (d + 249) / 250 * 250;
    uint32_t n;
    char want[512];

    k_memset(loop, 128, sizeof(loop));
    k_memset(over, 140, sizeof(over));
    start_near_wrap();
    CHECK_EQ(audio_play_pcm(loop, sizeof(loop), RATE, AUDIO_LOOP), 0);
    tick(10);
    run_long(d);
    CHECK(pcm.active);
    audio_pcm_samples(&n);
    CHECK_EQ(n, (uint32_t)(join / 250));
    CHECK_EQ(audio_pcm_mix(over, sizeof(over)), 0);
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=4000 len=4000 vol=100 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_mix len=100 crc=%08x t_ms=%u\n",
             (unsigned)crc32(loop, sizeof(loop)), (unsigned)crc32(over, sizeof(over)),
             (unsigned)(1010 + join / 1000));
    CHECK_STR(out, want);
    /* The overlay sounds there, all of it. */
    run_us(101 * 250);
    CHECK_EQ(audio_pcm_mix_busy(), 0);
    audio_stop();
}

TEST(a_long_play_and_its_queue_past_the_wrap)
{
    size_t len = (size_t)RATE * LONG_S;
    uint8_t *big = malloc(len);
    uint32_t n;
    char want[512];

    CHECK(big != 0);
    k_memset(big, 100, len);
    k_memset(over, 140, sizeof(over));
    start_near_wrap();
    CHECK_EQ(audio_play_pcm(big, len, RATE, 0), 0);
    tick(10);
    run_long((uint64_t)LONG_S * 1000000u - 1000u);
    CHECK_EQ(audio_pcm_queue(over, sizeof(over)), 0);
    run_long(1000000u);
    CHECK(!pcm.active);
    audio_pcm_samples(&n);
    CHECK_EQ(n, (uint32_t)(len + sizeof(over)));
    audio_trace_flush();
    snprintf(want, sizeof(want),
             "@RC stage=host ev=audio op=pcm rate=4000 len=%u vol=100 crc=%08x t_ms=1010\n"
             "@RC stage=host ev=audio op=pcm_next len=100 crc=%08x t_ms=%u\n"
             "@RC stage=host ev=audio op=pcm_end n=%u last=99 t_ms=%u\n",
             (unsigned)len, (unsigned)crc32(big, len), (unsigned)crc32(over, sizeof(over)),
             (unsigned)(1010 + LONG_S * 1000u),
             (unsigned)(len + sizeof(over)), (unsigned)(1010 + LONG_S * 1000u + 25u));
    CHECK_STR(out, want);
    free(big);
}
