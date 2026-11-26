/*
 * PCM position engine. Pure: the caller passes the time, the engine says
 * which sample is due. The position comes from the clock, so a late call
 * skips samples instead of slowing the sound, and nothing drifts.
 *
 * Cost: one IRQ calls audio_pcm_poll() and audio_pcm_next_us(). Both use
 * 32x32 to 64-bit multiplies. A looped buffer adds a 32-bit modulo per
 * sample after its first pass, and a rebase adds a 32-bit divide every
 * 2^26 us at most. A switch to a chained buffer is a few subtractions. The 64-bit divides run in audio_pcm_start() only.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/audio.h>

#define US_PER_S        1000000u

/* Rebase the origin once this much time has passed: keeps products in 64 bits. */
#define REBASE_US       (1u << 26)

int audio_pcm_check(const uint8_t *data, size_t len, uint32_t rate_hz, unsigned flags)
{
    if (!data || len == 0 || len > AUDIO_PCM_LEN_MAX ||
        (flags & ~(AUDIO_LOOP | AUDIO_PRIORITY))) {
        return AUDIO_ERR_ARG;
    }
    if (rate_hz < AUDIO_PCM_RATE_MIN || rate_hz > AUDIO_PCM_RATE_MAX) {
        return AUDIO_ERR_RANGE;
    }
    return 0;
}

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;

        a = b;
        b = t;
    }
    return a;
}

void audio_pcm_start(audio_pcm_t *p, const uint8_t *data, size_t len, uint32_t rate_hz,
                     unsigned flags, uint32_t now_us)
{
    uint32_t g = gcd(rate_hz, US_PER_S);

    p->data = data;
    p->len = (uint32_t)len;
    p->rate = rate_hz;
    p->flags = flags & AUDIO_LOOP;
    p->mul = (uint32_t)(((uint64_t)rate_hz << 32) / US_PER_S);
    p->inv = (uint32_t)((((uint64_t)US_PER_S << 24) + rate_hz - 1) / rate_hz);
    p->unit_us = US_PER_S / g;
    p->unit_n = rate_hz / g;
    p->origin_us = now_us;
    p->origin_pos = 0;
    p->next_q = 0;
    p->last = 0;
    p->n = 0;
    p->next = 0;
    p->next_len = 0;
    p->switches = 0;
    p->active = 1;
}

/* floor(elapsed * rate / 10^6): an estimate from `mul`, low by at most 1. */
static uint32_t samples_at(const audio_pcm_t *p, uint32_t elapsed)
{
    uint32_t q = (uint32_t)(((uint64_t)elapsed * p->mul) >> 32);

    if ((uint64_t)(q + 1) * US_PER_S <= (uint64_t)elapsed * p->rate) {
        q++;
    }
    return q;
}

/* Smallest t with t * rate >= m * 10^6: when sample m after the origin starts. */
static uint32_t time_of(const audio_pcm_t *p, uint32_t m)
{
    uint64_t want = (uint64_t)m * US_PER_S;
    uint32_t t = (uint32_t)(((uint64_t)m * p->inv) >> 24);

    while ((uint64_t)t * p->rate < want) {
        t++;
    }
    while (t && (uint64_t)(t - 1) * p->rate >= want) {
        t--;
    }
    return t;
}

/*
 * Hands over to the waiting buffer: positions in it are pos - len. The
 * origin time does not move, so the grid stays exact.
 */
static void take_next(audio_pcm_t *p)
{
    p->origin_pos -= p->len;
    p->data = p->next;
    p->len = p->next_len;
    p->next = 0;
    p->next_len = 0;
    p->switches++;
}

/*
 * Moves the origin forward by whole units, where time and samples are both
 * exact. After a switch, origin_pos can be below 0 as a signed value: the
 * origin lies in the previous buffer. len is at most 2^31 - 1, so it fits.
 * A one-shot origin that passes the end switches to the waiting buffer
 * first, so a late call keeps its position. Only then does it clamp to the
 * end, and the caller sees END.
 */
static void rebase(audio_pcm_t *p, uint32_t elapsed)
{
    uint32_t k = elapsed / p->unit_us;
    uint32_t dn = k * p->unit_n;

    p->origin_us += k * p->unit_us;
    p->next_q = p->next_q > dn ? p->next_q - dn : 0;
    if (p->flags & AUDIO_LOOP) {
        p->origin_pos = (p->origin_pos + dn) % p->len;
        return;
    }
    if (p->next && (int64_t)(int32_t)p->origin_pos + dn >= (int64_t)p->len) {
        take_next(p);
    }
    if ((int64_t)(int32_t)p->origin_pos + dn > (int64_t)p->len) {
        p->origin_pos = p->len;
    } else {
        p->origin_pos += dn;
    }
}

int audio_pcm_chain(audio_pcm_t *p, const uint8_t *data, size_t len)
{
    if (!p->active || (p->flags & AUDIO_LOOP)) {
        return AUDIO_ERR_STATE;
    }
    if (p->next) {
        return AUDIO_ERR_BUSY;
    }
    p->next_len = (uint32_t)len;
    p->next = data;
    return 0;
}

int audio_pcm_poll(audio_pcm_t *p, uint32_t now_us, uint32_t *pos)
{
    uint32_t elapsed;
    uint32_t q;
    uint32_t abs;

    if (!p->active) {
        return AUDIO_PCM_NONE;
    }
    elapsed = now_us - p->origin_us;
    /*
     * Before the origin: audio_pcm_chain_at() can put it a little ahead of
     * now. A real elapsed time stays far below 2^31 us, since the IRQ polls
     * every sample and the rebase below keeps it under 2^26 us.
     */
    if ((int32_t)elapsed < 0) {
        return AUDIO_PCM_NONE;
    }
    if (elapsed >= REBASE_US) {
        rebase(p, elapsed);
        elapsed = now_us - p->origin_us;
    }
    q = samples_at(p, elapsed);
    abs = p->origin_pos + q;
    /*
     * Past the end of a one-shot buffer: the waiting buffer takes over.
     * origin_pos is signed after a switch, and its sum with q can pass
     * 2^31, so the compare runs on 64 bits. It adds and compares only.
     */
    while (!(p->flags & AUDIO_LOOP) &&
           (int64_t)(int32_t)p->origin_pos + q >= (int64_t)p->len) {
        if (!p->next) {
            p->active = 0;
            return AUDIO_PCM_END;
        }
        abs -= p->len;
        take_next(p);
    }
    if (q < p->next_q) {
        return AUDIO_PCM_NONE;
    }
    if (abs >= p->len) {
        abs %= p->len;
    }
    p->next_q = q + 1;
    p->last = abs;
    p->n++;
    if (pos) {
        *pos = abs;
    }
    return AUDIO_PCM_SAMPLE;
}

uint32_t audio_pcm_next_us(const audio_pcm_t *p)
{
    return p->origin_us + time_of(p, p->next_q);
}

/*
 * The first grid point at or after now_us that is not before the next
 * sample due. The products stay in 64 bits: elapsed is under 2^32 and q
 * under 2^32 * 22050 / 10^6.
 */
uint32_t audio_pcm_join_us(const audio_pcm_t *p, uint32_t now_us)
{
    uint32_t elapsed = now_us - p->origin_us;
    uint32_t q;

    if ((int32_t)elapsed < 0) {
        elapsed = 0;
    }
    q = samples_at(p, elapsed);
    if (time_of(p, q) < elapsed) {
        q++;
    }
    if (q < p->next_q) {
        q = p->next_q;
    }
    return p->origin_us + time_of(p, q);
}

int audio_pcm_chain_at(audio_pcm_t *p, const uint8_t *data, size_t len, uint32_t now_us)
{
    uint32_t join;
    uint32_t end;
    int r = audio_pcm_chain(p, data, len);

    if (r) {
        return r;
    }
    /*
     * The current buffer ends at sample len. When that time has passed and
     * no poll has switched yet, the new buffer starts at the next grid
     * point instead, so its first sample is not skipped.
     */
    join = audio_pcm_join_us(p, now_us);
    end = p->origin_us + time_of(p, p->len - p->origin_pos);
    if ((int32_t)(end - join) < 0) {
        p->origin_us = join;
        p->origin_pos = p->len;
        p->next_q = 0;
    }
    return 0;
}

uint8_t audio_pcm_scale(uint8_t s, unsigned vol)
{
    int d;

    if (vol > 100) {
        vol = 100;
    }
    d = ((int)s - 128) * (int)vol;
    /* Nearest step, a half step away from the midpoint: the same on both sides. */
    d = d >= 0 ? (d + 50) / 100 : -((50 - d) / 100);
    return (uint8_t)(128 + d);
}

uint32_t audio_pcm_duty(uint8_t s, uint32_t rng)
{
    return (uint32_t)(((uint64_t)s * rng) >> 8);
}
