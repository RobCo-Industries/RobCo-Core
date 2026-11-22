#include <stddef.h>
#include <stdint.h>

#include <robco/audio.h>

#include "rc_test.h"

TEST(note_table)
{
    CHECK_EQ(audio_note_hz(9, 4), 440);
    CHECK_EQ(audio_note_hz(9, 5), 880);
    CHECK_EQ(audio_note_hz(0, 4), 262);
    CHECK_EQ(audio_note_hz(9, 0), 28);
    CHECK_EQ(audio_note_hz(0, 8), 4186);
    CHECK_EQ(audio_note_hz(11, 8), 7902);
    CHECK_EQ(audio_note_hz(1, 4), 277);
    CHECK_EQ(audio_note_hz(12, 4), 0);
    CHECK_EQ(audio_note_hz(0, 9), 0);
}

TEST(note_table_monotonic)
{
    uint32_t prev = 0;
    unsigned o, n;

    for (o = 1; o <= 8; o++) {
        for (n = 0; n < 12; n++) {
            uint32_t hz = audio_note_hz(n, o);

            CHECK(hz > prev);
            prev = hz;
        }
    }
}

TEST(rtttl_basic)
{
    audio_note_t out[8];
    size_t pos = 99;
    int n = audio_rtttl_parse("t:d=4,o=5,b=120:c,8e.,p,2g6", out, 8, &pos);

    CHECK_EQ(n, 4);
    CHECK_EQ(out[0].hz, 523);
    CHECK_EQ(out[0].ms, 500);
    CHECK_EQ(out[1].hz, 659);
    CHECK_EQ(out[1].ms, 375);
    CHECK_EQ(out[2].hz, 0);
    CHECK_EQ(out[2].ms, 500);
    CHECK_EQ(out[3].hz, 1568);
    CHECK_EQ(out[3].ms, 1000);
    CHECK_EQ(pos, 99);
}

TEST(rtttl_defaults_and_spacing)
{
    audio_note_t out[4];

    /* No settings: d=4, o=6, b=63. */
    CHECK_EQ(audio_rtttl_parse("x::a", out, 4, NULL), 1);
    CHECK_EQ(out[0].hz, 1760);
    CHECK_EQ(out[0].ms, 952);
    CHECK_EQ(audio_rtttl_parse(" name with spaces : B = 240 , d=8 : 16C#5 , A.4 , 4p. ", out, 4, NULL), 3);
    CHECK_EQ(out[0].hz, 554);
    CHECK_EQ(out[0].ms, 62);
    CHECK_EQ(out[1].hz, 440);
    CHECK_EQ(out[1].ms, 187);
    CHECK_EQ(out[2].hz, 0);
    CHECK_EQ(out[2].ms, 375);
    /* Dot after the octave works too. */
    CHECK_EQ(audio_rtttl_parse("x:b=60:4a4.", out, 4, NULL), 1);
    CHECK_EQ(out[0].ms, 1500);
}

TEST(rtttl_tempo)
{
    audio_note_t out[1];

    CHECK_EQ(audio_rtttl_parse("x:b=900:32c", out, 1, NULL), 1);
    CHECK_EQ(out[0].ms, 8);
    CHECK_EQ(audio_rtttl_parse("x:b=1:1c", out, 1, NULL), 1);
    CHECK_EQ(out[0].ms, 240000);
    CHECK_EQ(audio_rtttl_parse("x:b=901:c", out, 1, NULL), AUDIO_ERR_PARSE);
    CHECK_EQ(audio_rtttl_parse("x:b=0:c", out, 1, NULL), AUDIO_ERR_PARSE);
}

TEST(rtttl_empty_and_count_only)
{
    CHECK_EQ(audio_rtttl_parse("x:d=4:", NULL, 0, NULL), 0);
    CHECK_EQ(audio_rtttl_parse("x:d=4:  ", NULL, 0, NULL), 0);
    CHECK_EQ(audio_rtttl_parse("x::c,d,e", NULL, 3, NULL), 3);
}

TEST(rtttl_full)
{
    audio_note_t out[2];
    size_t pos = 0;

    CHECK_EQ(audio_rtttl_parse("x::c,d,e", out, 2, &pos), AUDIO_ERR_FULL);
    CHECK_EQ(pos, 7);
}

TEST(rtttl_malformed)
{
    static const struct {
        const char *text;
        int err;
        size_t pos;
    } bad[] = {
        { "", AUDIO_ERR_PARSE, 0 },
        { "no colon", AUDIO_ERR_PARSE, 8 },
        { "x:d=4", AUDIO_ERR_PARSE, 5 },
        { "x:d=3:c", AUDIO_ERR_PARSE, 5 },
        { "x:q=3:c", AUDIO_ERR_PARSE, 2 },
        { "x:d4:c", AUDIO_ERR_PARSE, 3 },
        { "x:o=9:c", AUDIO_ERR_PARSE, 5 },
        { "x:d=4;o=5:c", AUDIO_ERR_PARSE, 5 },
        { "x::h", AUDIO_ERR_PARSE, 3 },
        { "x::c,,d", AUDIO_ERR_PARSE, 5 },
        { "x::c,", AUDIO_ERR_PARSE, 5 },
        { "x::e#", AUDIO_ERR_PARSE, 4 },
        { "x::c..", AUDIO_ERR_PARSE, 5 },
        { "x::c.5.", AUDIO_ERR_PARSE, 6 },
        { "x::c9", AUDIO_ERR_PARSE, 5 },
        { "x::3c", AUDIO_ERR_PARSE, 4 },
        { "x::c d", AUDIO_ERR_PARSE, 5 },
        { "x::12345c", AUDIO_ERR_PARSE, 7 },
        { "x::c0", AUDIO_ERR_RANGE, 3 },
    };
    audio_note_t out[4];
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        size_t pos = 999;
        int r = audio_rtttl_parse(bad[i].text, out, 4, &pos);

        if (r != bad[i].err || pos != bad[i].pos) {
            RC_FAIL("\"%s\": got %d at %zu, want %d at %zu", bad[i].text, r, pos,
                    bad[i].err, bad[i].pos);
        }
    }
    CHECK_EQ(audio_rtttl_parse(rc_launder(NULL), out, 4, NULL), AUDIO_ERR_ARG);
}

TEST(pwm_osc_square)
{
    audio_pwm_cfg_t c;

    CHECK_EQ(audio_pwm_calc(19200000, 440, 100, &c), 0);
    CHECK_EQ(c.divi, 2);
    CHECK_EQ(c.clock_hz, 9600000);
    CHECK_EQ(c.rng, 21818);
    CHECK_EQ(c.dat, 10909);
    CHECK_EQ(audio_pwm_calc(19200000, 1000, 50, &c), 0);
    CHECK_EQ(c.rng, 9600);
    CHECK_EQ(c.dat, 2400);
    CHECK_EQ(audio_pwm_calc(19200000, 1000, 0, &c), 0);
    CHECK_EQ(c.dat, 0);
    CHECK_EQ(audio_pwm_calc(19200000, 1000, 250, &c), 0);
    CHECK_EQ(c.dat, 4800);
}

TEST(pwm_plld)
{
    audio_pwm_cfg_t c;

    CHECK_EQ(audio_pwm_calc(500000000, 440, 100, &c), 0);
    CHECK_EQ(c.divi, 52);
    CHECK_EQ(c.clock_hz, 9615384);
    CHECK_EQ(c.rng, 21853);
}

TEST(pwm_rounding)
{
    audio_pwm_cfg_t c;

    /* 9600000 / 7003 = 1370.8: rounds up, not down. */
    CHECK_EQ(audio_pwm_calc(19200000, 7003, 100, &c), 0);
    CHECK_EQ(c.rng, 1371);
    /* 9600000 / 7004 = 1370.6, 9600000 / 7008 = 1369.9. */
    CHECK_EQ(audio_pwm_calc(19200000, 7008, 100, &c), 0);
    CHECK_EQ(c.rng, 1370);
    /* An odd range gives the lower half for 50%. */
    CHECK_EQ(c.dat, 685);
}

TEST(pwm_limits)
{
    audio_pwm_cfg_t c;

    CHECK_EQ(audio_pwm_calc(19200000, AUDIO_HZ_MIN - 1, 100, &c), AUDIO_ERR_RANGE);
    CHECK_EQ(audio_pwm_calc(19200000, AUDIO_HZ_MAX + 1, 100, &c), AUDIO_ERR_RANGE);
    CHECK_EQ(audio_pwm_calc(0, 440, 100, &c), AUDIO_ERR_ARG);
    CHECK_EQ(audio_pwm_calc(19200000, 440, 100, rc_launder(NULL)), AUDIO_ERR_ARG);
    /* A slow source clamps the divisor at 2; too slow for the tone fails. */
    CHECK_EQ(audio_pwm_calc(1000000, 440, 100, &c), 0);
    CHECK_EQ(c.divi, AUDIO_PWM_DIVI_MIN);
    CHECK_EQ(audio_pwm_calc(50000, 20000, 100, &c), AUDIO_ERR_RANGE);
    /* The largest source still fits the 12-bit divisor without overflow. */
    CHECK_EQ(audio_pwm_calc(UINT32_MAX, 20, 100, &c), 0);
    CHECK_EQ(c.divi, 447);
    CHECK_EQ(c.dat, c.rng / 2);
}

TEST(pwm_fits_registers_over_audible_range)
{
    static const uint32_t srcs[] = { 19200000, 500000000 };
    audio_pwm_cfg_t c;
    size_t i;
    uint32_t hz;
    unsigned vol;

    for (i = 0; i < 2; i++) {
        for (hz = AUDIO_HZ_MIN; hz <= AUDIO_HZ_MAX; hz += 7) {
            for (vol = 0; vol <= 100; vol += 25) {
                if (audio_pwm_calc(srcs[i], hz, vol, &c) != 0) {
                    RC_FAIL("src %u hz %u failed", (unsigned)srcs[i], (unsigned)hz);
                    continue;
                }
                CHECK(c.divi >= AUDIO_PWM_DIVI_MIN && c.divi <= AUDIO_PWM_DIVI_MAX);
                CHECK(c.rng >= 2);
                CHECK(c.dat <= c.rng / 2);
                /* rng is clock / hz rounded to nearest: off by half a count at most. */
                {
                    int64_t err = (int64_t)c.rng * hz - (int64_t)c.clock_hz;

                    CHECK(2 * (err < 0 ? -err : err) <= (int64_t)hz);
                }
            }
        }
    }
}
