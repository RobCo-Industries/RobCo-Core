/*
 * Note table, RTTTL parser and PWM register maths. Integer only.
 */
#include <stddef.h>
#include <stdint.h>

#include <robco/audio.h>

/* Octave 4 in millihertz, equal temperament from A4 = 440 Hz. */
static const uint32_t octave4_mhz[12] = {
    261626, 277183, 293665, 311127, 329628, 349228,
    369994, 391995, 415305, 440000, 466164, 493883,
};

uint32_t audio_note_hz(unsigned semitone, unsigned octave)
{
    uint32_t mhz;

    if (semitone > 11 || octave > 8) {
        return 0;
    }
    mhz = octave4_mhz[semitone];
    if (octave >= 4) {
        mhz <<= octave - 4;
    } else {
        unsigned sh = 4 - octave;

        mhz = (mhz + (1u << (sh - 1))) >> sh;
    }
    return (mhz + 500) / 1000;
}

/* Parser state. */
typedef struct {
    const char *text;
    size_t i;
} cursor_t;

static char peek(const cursor_t *c)
{
    return c->text[c->i];
}

static void skip_ws(cursor_t *c)
{
    while (peek(c) == ' ' || peek(c) == '\t' || peek(c) == '\n' || peek(c) == '\r') {
        c->i++;
    }
}

static char lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

/* Reads up to 4 decimal digits. Returns -1 if there are none or too many. */
static int number(cursor_t *c)
{
    int v = 0;
    int n = 0;

    while (peek(c) >= '0' && peek(c) <= '9') {
        if (++n > 4) {
            return -1;
        }
        v = v * 10 + (peek(c) - '0');
        c->i++;
    }
    return n ? v : -1;
}

static int valid_duration(int d)
{
    return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32;
}

/* Semitone of a note letter, or -1. */
static int letter_semitone(char ch)
{
    switch (ch) {
    case 'c': return 0;
    case 'd': return 2;
    case 'e': return 4;
    case 'f': return 5;
    case 'g': return 7;
    case 'a': return 9;
    case 'b': return 11;
    default:  return -1;
    }
}

static int fail(const cursor_t *c, size_t *err_pos, int code)
{
    if (err_pos) {
        *err_pos = c->i;
    }
    return code;
}

int audio_rtttl_parse(const char *text, audio_note_t *out, size_t max, size_t *err_pos)
{
    cursor_t c = { text, 0 };
    int def_d = 4;
    int def_o = 6;
    int bpm = 63;
    size_t n = 0;

    if (!text) {
        if (err_pos) {
            *err_pos = 0;
        }
        return AUDIO_ERR_ARG;
    }

    /* Name: anything up to the first colon. */
    while (peek(&c) && peek(&c) != ':') {
        c.i++;
    }
    if (peek(&c) != ':') {
        return fail(&c, err_pos, AUDIO_ERR_PARSE);
    }
    c.i++;

    /* Defaults. */
    skip_ws(&c);
    while (peek(&c) != ':') {
        char key = lower(peek(&c));
        int v;

        if (key != 'd' && key != 'o' && key != 'b') {
            return fail(&c, err_pos, AUDIO_ERR_PARSE);
        }
        c.i++;
        skip_ws(&c);
        if (peek(&c) != '=') {
            return fail(&c, err_pos, AUDIO_ERR_PARSE);
        }
        c.i++;
        skip_ws(&c);
        v = number(&c);
        if ((key == 'd' && !valid_duration(v)) || (key == 'o' && (v < 0 || v > 8)) ||
            (key == 'b' && (v < 1 || v > 900))) {
            return fail(&c, err_pos, AUDIO_ERR_PARSE);
        }
        if (key == 'd') {
            def_d = v;
        } else if (key == 'o') {
            def_o = v;
        } else {
            bpm = v;
        }
        skip_ws(&c);
        if (peek(&c) == ',') {
            c.i++;
            skip_ws(&c);
        } else if (peek(&c) != ':') {
            return fail(&c, err_pos, AUDIO_ERR_PARSE);
        }
    }
    c.i++;

    /* Notes. */
    skip_ws(&c);
    if (!peek(&c)) {
        return 0;
    }
    for (;;) {
        size_t start = c.i;
        int d = def_d;
        int o = def_o;
        int semi;
        int dots = 0;
        uint32_t hz = 0;
        uint32_t ms;
        char ch;

        skip_ws(&c);
        if (peek(&c) >= '0' && peek(&c) <= '9') {
            d = number(&c);
            if (!valid_duration(d)) {
                return fail(&c, err_pos, AUDIO_ERR_PARSE);
            }
        }
        ch = lower(peek(&c));
        if (ch == 'p') {
            semi = -1;
            c.i++;
        } else {
            semi = letter_semitone(ch);
            if (semi < 0) {
                return fail(&c, err_pos, AUDIO_ERR_PARSE);
            }
            c.i++;
            if (peek(&c) == '#') {
                if (ch == 'e' || ch == 'b') {
                    return fail(&c, err_pos, AUDIO_ERR_PARSE);
                }
                semi++;
                c.i++;
            }
        }
        if (peek(&c) == '.') {
            dots++;
            c.i++;
        }
        if (peek(&c) >= '0' && peek(&c) <= '9') {
            o = number(&c);
            if (o < 0 || o > 8) {
                return fail(&c, err_pos, AUDIO_ERR_PARSE);
            }
        }
        if (peek(&c) == '.') {
            if (dots) {
                return fail(&c, err_pos, AUDIO_ERR_PARSE);
            }
            dots++;
            c.i++;
        }
        if (semi >= 0) {
            hz = audio_note_hz((unsigned)semi, (unsigned)o);
            if (hz < AUDIO_HZ_MIN || hz > AUDIO_HZ_MAX) {
                c.i = start;
                return fail(&c, err_pos, AUDIO_ERR_RANGE);
            }
        }
        /* A whole note lasts four beats: 240000 / bpm ms. */
        ms = 240000u / ((uint32_t)bpm * (uint32_t)d);
        if (dots) {
            ms += ms / 2;
        }
        if (n >= max) {
            c.i = start;
            return fail(&c, err_pos, AUDIO_ERR_FULL);
        }
        if (out) {
            out[n].hz = hz;
            out[n].ms = ms;
        }
        n++;

        skip_ws(&c);
        if (!peek(&c)) {
            break;
        }
        if (peek(&c) != ',') {
            return fail(&c, err_pos, AUDIO_ERR_PARSE);
        }
        c.i++;
    }
    return (int)n;
}

int audio_pwm_calc(uint32_t src_hz, uint32_t hz, unsigned vol, audio_pwm_cfg_t *out)
{
    uint32_t divi;
    uint32_t clock;
    uint32_t rng;
    uint32_t half;

    if (!out || src_hz < AUDIO_PWM_DIVI_MIN) {
        return AUDIO_ERR_ARG;
    }
    if (hz < AUDIO_HZ_MIN || hz > AUDIO_HZ_MAX) {
        return AUDIO_ERR_RANGE;
    }
    divi = src_hz / AUDIO_PWM_CLOCK_TARGET;
    if (src_hz % AUDIO_PWM_CLOCK_TARGET >= AUDIO_PWM_CLOCK_TARGET / 2) {
        divi++;
    }
    if (divi < AUDIO_PWM_DIVI_MIN) {
        divi = AUDIO_PWM_DIVI_MIN;
    }
    if (divi > AUDIO_PWM_DIVI_MAX) {
        divi = AUDIO_PWM_DIVI_MAX;
    }
    clock = src_hz / divi;
    rng = (clock + hz / 2) / hz;
    if (rng < 2) {
        return AUDIO_ERR_RANGE;
    }
    if (vol > 100) {
        vol = 100;
    }
    out->divi = divi;
    out->clock_hz = clock;
    out->rng = rng;
    /*
     * 50% duty at full volume. Stays in 32 bits so it is cheap in IRQ
     * context. Ranges too large for the product lose the rounding only.
     */
    half = rng / 2;
    if (half <= (UINT32_MAX - 50) / 100) {
        out->dat = (half * vol + 50) / 100;
    } else {
        out->dat = half / 100 * vol;
    }
    return 0;
}
