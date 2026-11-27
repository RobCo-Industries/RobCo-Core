/*
 * The pcm_voice case of examples/audio: its spans and overlay, built by the
 * example's own voice_demo.h, rendered through the voice engine. Checks
 * that the output has no step anywhere, at span edges or where the
 * overlay's burst starts and ends, and that it starts and ends at 128.
 */
#include <stdint.h>

#include <robco/audio.h>

#include "rc_test.h"

#include "../../examples/audio/voice_demo.h"

/*
 * Largest change between two neighbouring output samples, in steps.
 * A sine of peak A at f Hz moves at most A * 2 pi f / rate per sample.
 * - Fundamental: A 40 steps (2621250 >> 16), f at most 2000.0004 Hz:
 *   40 * 2 pi * 2000 / 37500 = 13.41.
 * - Second partial: A * 8192 / 32768 = 10 steps at 4000 Hz: 6.71.
 * - Overlay: 30 steps at 500 Hz: 2.52, plus its envelope, 30 / 749: 0.04.
 * - Level ramp: 40 steps over 1875 samples: 0.03.
 * - The table reads the phase on 10 bits: each sample is off by at most
 *   (40 + 10 * 2) * 2 pi / 1024 = 0.37, so 0.74 on a difference.
 * - The rounding of the output is off by half a step each side: 1. The
 *   overlay's own truncation is under a step each side: 2.
 * Sum: 26.8. A step of the partial or of the level at a span edge, or
 * an overlay that starts away from 128, adds to the slope it lands on.
 */
#define STEP_BOUND  27

static audio_voice_span_t spans[VOICE_SPANS];
static uint8_t mix[VOICE_MIX_LEN];
static uint8_t out[VOICE_LEN];

static void render(void)
{
    audio_voice_t v;
    uint32_t i;

    voice_demo_spans(spans);
    voice_demo_mix(mix);
    CHECK_EQ(audio_voice_check(spans, VOICE_SPANS, VOICE_RATE, 0, 0), 0);
    audio_voice_start(&v, spans, VOICE_SPANS);
    for (i = 0; i < VOICE_LEN; i++) {
        out[i] = audio_voice_out(audio_voice_at(&v, i), i < VOICE_MIX_LEN ? mix[i] : 128, 100);
    }
}

TEST(spans_carry_and_share_p2)
{
    uint32_t len = 0;
    size_t k;

    render();
    CHECK_EQ(audio_voice_check(spans, VOICE_SPANS, VOICE_RATE, 0, &len), 0);
    CHECK_EQ(len, VOICE_LEN);
    CHECK_EQ(spans[0].amp, 0);
    CHECK_EQ(spans[1].inc, VOICE_INC);
    /* About 2000 Hz at the end of the glide: twice the start, within 0.01 %. */
    CHECK(spans[2].inc > 2u * VOICE_INC - 2u * VOICE_INC / 10000u);
    CHECK(spans[2].inc < 2u * VOICE_INC + 2u * VOICE_INC / 10000u);
    /* The level after the last sample is 0. */
    CHECK_EQ(spans[2].amp + (int32_t)spans[2].n * spans[2].damp, 0);
    for (k = 0; k + 1 < VOICE_SPANS; k++) {
        audio_voice_span_t next;

        voice_demo_carry(&spans[k], &next);
        CHECK_EQ(spans[k + 1].phase, next.phase);
        CHECK_EQ(spans[k + 1].inc, next.inc);
        CHECK_EQ(spans[k + 1].amp, next.amp);
        CHECK_EQ(spans[k + 1].p2, spans[k].p2);
    }
}

TEST(overlay_starts_and_ends_at_128)
{
    uint32_t k;
    int peak = 0;

    render();
    for (k = 0; k <= VOICE_MIX_DELAY; k++) {
        CHECK_EQ(mix[k], 128);
    }
    CHECK_EQ(mix[VOICE_MIX_LEN - 1], 128);
    for (k = 0; k < VOICE_MIX_LEN; k++) {
        int d = mix[k] > 128 ? mix[k] - 128 : 128 - mix[k];

        peak = d > peak ? d : peak;
    }
    CHECK(peak >= VOICE_MIX_PEAK - 2 && peak <= VOICE_MIX_PEAK);
}

TEST(no_step_in_the_output)
{
    uint32_t i;
    int worst = 0;
    int lo = 255;
    int hi = 0;

    render();
    CHECK_EQ(out[0], 128);
    CHECK_EQ(out[VOICE_LEN - 1], 128);
    for (i = 0; i < VOICE_LEN; i++) {
        lo = out[i] < lo ? out[i] : lo;
        hi = out[i] > hi ? out[i] : hi;
        if (i > 0) {
            int d = out[i] > out[i - 1] ? out[i] - out[i - 1] : out[i - 1] - out[i];

            if (d > STEP_BOUND) {
                RC_FAIL("samples %u and %u differ by %d", (unsigned)i - 1, (unsigned)i, d);
                return;
            }
            worst = d > worst ? d : worst;
        }
    }
    /* No clamp, and the voice is loud enough to hear. */
    CHECK(lo > 0 && hi < 255);
    CHECK(hi - 128 >= 40);
    CHECK(worst > 0);
}
