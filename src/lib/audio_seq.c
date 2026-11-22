/*
 * Note sequencer. Pure state machine: the caller passes the time, the
 * sequencer says what should sound. Times wrap at 2^32 ms; only the
 * difference between two times is used.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/audio.h>

/* Index of the first step at or after `pos` with a non-zero length, or count. */
static size_t next_audible(const audio_seq_t *s, size_t pos)
{
    while (pos < s->count && s->notes[pos].ms == 0) {
        pos++;
    }
    return pos;
}

static void enter(audio_seq_t *s, size_t pos)
{
    s->pos = pos;
    s->dur_ms = s->notes[pos].ms;
    s->hz = s->notes[pos].hz;
    s->step++;
}

int audio_seq_start(audio_seq_t *s, const audio_note_t *notes, size_t count,
                    unsigned flags, uint32_t now_ms)
{
    size_t i;
    uint32_t sum = 0;
    size_t first;

    s->notes = notes;
    s->count = notes ? count : 0;
    s->flags = flags;
    s->start_ms = now_ms;
    s->active = 0;
    s->hz = 0;
    s->dur_ms = 0;
    for (i = 0; i < s->count; i++) {
        uint32_t ms = notes[i].ms;

        sum = sum > UINT32_MAX - ms ? UINT32_MAX : sum + ms;
    }
    s->period_ms = sum;
    first = next_audible(s, 0);
    if (first >= s->count) {
        s->step++;
        return 0;
    }
    s->active = 1;
    enter(s, first);
    return 1;
}

void audio_seq_stop(audio_seq_t *s)
{
    if (s->active || s->hz) {
        s->step++;
    }
    s->active = 0;
    s->hz = 0;
}

int audio_seq_poll(audio_seq_t *s, uint32_t now_ms, uint32_t *at_ms)
{
    uint32_t elapsed;
    size_t pos;

    if (!s->active) {
        return 0;
    }
    elapsed = now_ms - s->start_ms;
    if (elapsed < s->dur_ms) {
        return 0;
    }
    /*
     * A looped sequence far behind skips whole loops at once, so a long
     * stall costs at most one pass of steps.
     */
    if ((s->flags & AUDIO_LOOP) && s->period_ms != UINT32_MAX &&
        elapsed - s->dur_ms >= s->period_ms) {
        s->start_ms += (elapsed - s->dur_ms) / s->period_ms * s->period_ms;
    }
    s->start_ms += s->dur_ms;
    if (at_ms) {
        *at_ms = s->start_ms;
    }
    pos = next_audible(s, s->pos + 1);
    if (pos >= s->count) {
        if (!(s->flags & AUDIO_LOOP)) {
            audio_seq_stop(s);
            return 1;
        }
        pos = next_audible(s, 0);
    }
    enter(s, pos);
    return 1;
}
