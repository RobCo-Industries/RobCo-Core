/*
 * Gain rules of the audio driver. Pure: no hardware, so the host tests
 * and the driver share them.
 *
 * A gain is in units of 1/10000, 0 to AUDIO_GAIN_FULL. A gain of
 * vol * 100 gives the same byte as the volume rules of audio_pcm.c,
 * audio_voice.c and audio_square.c at volume vol. The gain is never above
 * 1, so no rule can leave 0 to 255.
 *
 * Bounds: a sample rule multiplies at most 128 by 10000. The voice rule
 * multiplies its fine value, under 98000 in size, by 10000, which stays
 * under 2^31. The square rule multiplies at most 127 by 10000.
 */
#include <stdint.h>

#include <robco/audio.h>

static uint32_t cap(uint32_t g)
{
    return g > AUDIO_GAIN_FULL ? AUDIO_GAIN_FULL : g;
}

uint8_t audio_pcm_scale_g(uint8_t s, uint32_t gain)
{
    int32_t d = ((int32_t)s - 128) * (int32_t)cap(gain);

    /* Nearest step, a half step away from the midpoint, as audio_pcm_scale(). */
    d = d >= 0 ? (d + 5000) / 10000 : -((5000 - d) / 10000);
    return (uint8_t)(128 + d);
}

uint8_t audio_voice_out_g(int32_t q, uint8_t mix, uint32_t gain)
{
    int32_t x = (q + ((int32_t)mix - 128) * 256) * (int32_t)cap(gain);
    int32_t r = x >= 0 ? (x + 1280000) / 2560000 : -((1280000 - x) / 2560000);

    r += 128;
    return (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
}

unsigned audio_square_amp_g(unsigned peak, uint32_t gain)
{
    return (unsigned)((peak * cap(gain) + 5000u) / 10000u);
}

unsigned audio_gain_pulse(uint32_t gain)
{
    return (unsigned)((cap(gain) + 50u) / 100u);
}

uint32_t audio_gain_step(uint32_t rate)
{
    if (rate == 0) {
        return AUDIO_GAIN_FULL;
    }
    return (AUDIO_GAIN_SLEW + rate - 1u) / rate;
}

uint32_t audio_gain_slew(uint32_t cur, uint32_t target, uint32_t step)
{
    if (cur < target) {
        return target - cur > step ? cur + step : target;
    }
    return cur - target > step ? cur - step : target;
}

uint32_t audio_gain_slew_n(uint32_t cur, uint32_t target, uint32_t step, uint32_t n)
{
    uint32_t d = cur < target ? target - cur : cur - target;
    uint64_t move;

    if (n == 0 || step == 0 || d == 0) {
        return cur;
    }
    /* n steps of at least 1 cover d: done, before any product. */
    if (n >= d) {
        return target;
    }
    /* n < d, so the product of two 32-bit values fits in 64 bits. */
    move = (uint64_t)step * n;
    if (move >= d) {
        return target;
    }
    return cur < target ? cur + (uint32_t)move : cur - (uint32_t)move;
}

void audio_gain_follow(uint32_t *cur, uint32_t *n, uint32_t target, uint32_t step, uint32_t at)
{
    uint32_t d = at - *n;

    /* at wraps with the grid: a point at or before *n moves nothing. */
    if ((int32_t)d <= 0) {
        return;
    }
    *cur = audio_gain_slew_n(*cur, target, step, d);
    *n = at;
}
