/*
 * Centred square engine: the tone that plays once the pin rests at the
 * midpoint. Pure: no hardware, so the host tests and the driver share it.
 *
 * Integer rule, the same in any tool that renders a tone:
 * - half = max(256, (rate * 128 + hz / 2) / hz), the half period in
 *   1/256 of a sample, taken once per note. A rest, hz 0, has half 0 and
 *   writes 128. The floor of 256 keeps a half period at one sample or
 *   more, so a pitch above rate / 2 plays at rate / 2: the samples
 *   alternate 128 + amp and 128 - amp. Without it a pitch of exactly
 *   rate / 2 has half 128 and every sample averages to 128, silence.
 * - A note starts at an edge going up: the sign is +1 and the next edge
 *   lies half units ahead.
 * - Each sample walks 256 units. Before each edge it meets, it adds its
 *   sign times the units up to that edge, then flips the sign, and the
 *   next edge lies half units further. Then it adds the sign times the
 *   units left. An edge exactly at the end of a sample flips there. So
 *   acc lies in -256 to 256, and a sample that holds an edge takes the
 *   mean of the two levels by the edge's place in it.
 * - out = 128 + round_half_away(amp * acc / 256), where
 *   round_half_away(x / 256) is (x + 128) / 256 for x >= 0 and
 *   -((128 - x) / 256) below 0, with C's truncating divide.
 *
 * half is at least 256, so one sample meets at most one edge. The walk
 * still loops over edges: a skip walks up to AUDIO_SQUARE_SKIP_MAX
 * samples at once, and the loop keeps walk() right for any half above 0.
 * No divide runs per sample.
 *
 * Bounds, at the extremes: rate * 128 is at most 37500 * 128 = 4800000.
 * The largest half is 20 Hz on the 37500 grid: 4800010 / 20 = 240000
 * units, which fits int32_t. walk() adds q->left to acc only when it is
 * at most units, and units is at most 64 * 256 = 16384 for a skip or 256
 * for a sample, so acc stays within 16384 and the 256 of a sample. In
 * audio_square_acc() skip is at most 64 before skip * 256, so 16384.
 * amp * acc is at most 127 * 256 = 32512. No step overflows.
 */
#include <stdint.h>

#include <robco/audio.h>

unsigned audio_square_amp(unsigned peak, unsigned vol)
{
    if (vol > 100) {
        vol = 100;
    }
    return (peak * vol + 50) / 100;
}

void audio_square_start(audio_square_t *q, uint32_t hz, uint32_t rate, unsigned amp)
{
    uint32_t half = hz ? (rate * 128u + hz / 2u) / hz : 0;

    q->half = hz && half < 256u ? 256u : half;
    q->left = q->half;
    q->sign = 1;
    q->amp = (int32_t)amp;
    q->fresh = 1;
    q->next = 0;
}

/* Walks `units` of 1/256 sample and returns the signed area. half must not be 0. */
static int32_t walk(audio_square_t *q, uint32_t units)
{
    int32_t acc = 0;

    while (units >= q->left) {
        acc += q->sign * (int32_t)q->left;
        units -= q->left;
        q->sign = -q->sign;
        q->left = q->half;
    }
    acc += q->sign * (int32_t)units;
    q->left -= units;
    return acc;
}

int32_t audio_square_acc(audio_square_t *q, uint32_t pos)
{
    uint32_t skip = pos - q->next;

    q->next = pos + 1u;
    if (q->half == 0) {
        return 0;
    }
    /*
     * A skip of more than AUDIO_SQUARE_SKIP_MAX samples restarts the wave
     * at an edge going up, as a new note. The reason: catching up walks
     * skip * 256 units, and the walk must stay short and bounded in the
     * sample IRQ, with no divide and no overflow, whatever the skip. A skip
     * that long already held one level on the pin for over 1.7 ms at
     * 37500 Hz, a gap the ear hears, so keeping the phase across it buys
     * nothing. A grid sample that goes backwards counts as a long skip.
     */
    if (q->fresh || skip > AUDIO_SQUARE_SKIP_MAX) {
        q->fresh = 0;
        q->sign = 1;
        q->left = q->half;
    } else if (skip) {
        (void)walk(q, skip * 256u);
    }
    return walk(q, 256u);
}

uint8_t audio_square_out(int32_t acc, int32_t amp)
{
    int32_t x = amp * acc;
    int32_t r = x >= 0 ? (x + 128) / 256 : -((128 - x) / 256);

    r += 128;
    return (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
}

uint8_t audio_square_at(audio_square_t *q, uint32_t pos)
{
    return audio_square_out(audio_square_acc(q, pos), q->amp);
}
