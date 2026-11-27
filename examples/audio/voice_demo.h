/*
 * The voice of the pcm_voice case, built at run time. The example and
 * tests/host/test_audio_voice_demo.c share this file, so the host test
 * renders the same spans and overlay the board plays.
 *
 * 200 ms at 37500 Hz in three spans: 50 ms at 1000 Hz that fades in from
 * 0, 100 ms that glides up to 2000 Hz, 50 ms at 2000 Hz that fades out to
 * 0. One p2 for all spans, so the partial never steps between spans. Each
 * span starts with the phase, step and level the last one would have had
 * at its sample n, from the engine's own closed form.
 *
 * The overlay is a 500 Hz burst of 40 ms that starts 80 ms in, during the
 * glide. It rises from 128 and falls back to 128 on a triangle envelope.
 */
#ifndef VOICE_DEMO_H
#define VOICE_DEMO_H

#include <stdint.h>

#include <robco/audio.h>

#define VOICE_RATE      37500u
_Static_assert(VOICE_RATE == AUDIO_PCM_RATE_MAX, "the voice plays at the top rate");
#define VOICE_SPANS     3u
#define VOICE_FADE_N    1875u       /* 50 ms */
#define VOICE_GLIDE_N   3750u       /* 100 ms */
#define VOICE_LEN       (2u * VOICE_FADE_N + VOICE_GLIDE_N)
/* floor(1000 * 2^32 / 37500): 1000 Hz. */
#define VOICE_INC       114532461u
/* floor(VOICE_INC / VOICE_GLIDE_N): the step doubles over the glide. */
#define VOICE_DINC      30542
/* The fade-in reaches 1875 * 1398 = 2621250, about 40 steps of peak. */
#define VOICE_DAMP      1398
/* The second partial at a quarter of the fundamental. */
#define VOICE_P2        8192u

/* floor(500 * 2^32 / 37500): 500 Hz. */
#define VOICE_MIX_INC   57266230u
#define VOICE_MIX_DELAY 3000u       /* 80 ms of 128 first */
#define VOICE_MIX_BURST 1500u       /* 40 ms */
#define VOICE_MIX_PEAK  30          /* steps, at the middle of the burst */
#define VOICE_MIX_LEN   (VOICE_MIX_DELAY + VOICE_MIX_BURST)

/* Fills next with the phase, step and level of span s at its sample n. */
static inline void voice_demo_carry(const audio_voice_span_t *s, audio_voice_span_t *next)
{
    uint32_t n = s->n;

    next->phase = s->phase + n * s->inc + (uint32_t)s->dinc * (n * (n - 1u) / 2u);
    next->inc = s->inc + n * (uint32_t)s->dinc;
    next->amp = s->amp + (int32_t)n * s->damp;
}

static inline void voice_demo_spans(audio_voice_span_t s[VOICE_SPANS])
{
    s[0].n = VOICE_FADE_N;
    s[0].phase = 0;
    s[0].inc = VOICE_INC;
    s[0].dinc = 0;
    s[0].amp = 0;
    s[0].damp = VOICE_DAMP;
    s[0].p2 = VOICE_P2;

    voice_demo_carry(&s[0], &s[1]);
    s[1].n = VOICE_GLIDE_N;
    s[1].dinc = VOICE_DINC;
    s[1].damp = 0;
    s[1].p2 = VOICE_P2;

    voice_demo_carry(&s[1], &s[2]);
    s[2].n = VOICE_FADE_N;
    s[2].dinc = 0;
    s[2].damp = -VOICE_DAMP;
    s[2].p2 = VOICE_P2;
}

/*
 * The overlay, VOICE_MIX_LEN bytes: 128 for VOICE_MIX_DELAY bytes, then
 * the burst. Burst byte j is 128 + sin * e * 2 * PEAK / (BURST - 1), with
 * e = min(j, BURST - 1 - j). The divide truncates toward 0, so the first
 * and the last burst bytes are 128.
 */
static inline void voice_demo_mix(uint8_t *buf)
{
    uint32_t k;

    for (k = 0; k < VOICE_MIX_DELAY; k++) {
        buf[k] = 128;
    }
    for (k = 0; k < VOICE_MIX_BURST; k++) {
        uint32_t e = k < VOICE_MIX_BURST - 1u - k ? k : VOICE_MIX_BURST - 1u - k;
        int32_t s = (int32_t)audio_sine[(k * VOICE_MIX_INC) >> 22] * (int32_t)e;
        int32_t v = s * (2 * VOICE_MIX_PEAK) / (int32_t)((VOICE_MIX_BURST - 1u) * 32768u);

        buf[VOICE_MIX_DELAY + k] = (uint8_t)(128 + v);
    }
}

#endif
