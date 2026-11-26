/*
 * What "something plays" means in src/drv/audio.c: one table of driver
 * states against every call that checks it. The test builds the driver
 * and the trace backend in, as test_audio_stop.c does.
 */
#include <stdint.h>

#define ROBCO_STAGE_NAME "host"

/* Before the C library headers: macOS turns memcpy and friends into macros. */
#include "../../src/drv/audio.c"
#include "../../src/drv/audio_trace.c"

#include "audio_fake.h"

static uint8_t play[800];       /* 100 ms at 8000 Hz */
static uint8_t over[100];
static const audio_note_t notes[] = { { 440, 1000 } };
/* 100 ms at 8000 Hz, as `play`. */
static const audio_voice_span_t spans[] = { { 800, 0, 0x10000000u, 0, 20 << 16, 0, 0 } };

enum {
    S_IDLE,         /* nothing plays, duty 0 rest */
    S_TONE_WAIT,    /* a tone accepted, before its tick */
    S_TONE_ON,      /* a tone sounds */
    S_PCM_WAIT,     /* a sample accepted, before its tick */
    S_PCM_ON,       /* a sample plays */
    S_STOP_TONE,    /* audio_stop() while a tone sounds, before the tick */
    S_STOP_PCM,     /* audio_stop() cut a sample, before the tick */
    S_RAMP_WAIT,    /* audio_rest_mid() accepted, before its tick */
    S_RAMP_ON,      /* the ramp plays */
    S_REST,         /* nothing plays, midpoint rest */
    S_VOICE_WAIT,   /* a voice accepted, before its tick */
    S_VOICE_ON,     /* a voice plays */
    S_COUNT
};

static const char *const state_name[S_COUNT] = {
    "idle", "tone_wait", "tone_on", "pcm_wait", "pcm_on",
    "stop_tone", "stop_pcm", "ramp_wait", "ramp_on", "rest", "voice_wait", "voice_on",
};

enum {
    C_BUSY,         /* audio_busy() */
    C_PLAY,         /* audio_play() without AUDIO_PRIORITY */
    C_PCM,          /* audio_play_pcm() without AUDIO_PRIORITY */
    C_RTTTL,        /* audio_play_rtttl() without AUDIO_PRIORITY */
    C_PRIO,         /* audio_play_pcm() with AUDIO_PRIORITY */
    C_REST,         /* audio_rest_mid() */
    C_MIX,          /* audio_pcm_mix() */
    C_QUEUE,        /* audio_pcm_queue() */
    C_VOICE,        /* audio_play_voice() without AUDIO_PRIORITY */
    C_COUNT
};

static const char *const call_name[C_COUNT] = {
    "busy", "play", "play_pcm", "rtttl", "priority", "rest_mid", "mix", "queue", "voice",
};

#define B AUDIO_ERR_BUSY
#define S AUDIO_ERR_STATE

static const int want[S_COUNT][C_COUNT] = {
    /*                 busy play pcm rtttl prio rest mix queue voice */
    [S_IDLE]       = { 0,   0,   0,  0,    0,   0,   S,  S,    0 },
    [S_TONE_WAIT]  = { 1,   B,   B,  B,    0,   B,   S,  S,    B },
    [S_TONE_ON]    = { 1,   B,   B,  B,    0,   B,   S,  S,    B },
    [S_PCM_WAIT]   = { 1,   B,   B,  B,    0,   B,   0,  0,    B },
    [S_PCM_ON]     = { 1,   B,   B,  B,    0,   B,   0,  0,    B },
    [S_STOP_TONE]  = { 1,   0,   0,  0,    0,   B,   S,  S,    0 },
    [S_STOP_PCM]   = { 1,   0,   0,  0,    0,   B,   S,  S,    0 },
    [S_RAMP_WAIT]  = { 1,   B,   B,  B,    0,   0,   S,  S,    B },
    [S_RAMP_ON]    = { 1,   B,   B,  B,    0,   0,   S,  S,    B },
    [S_REST]       = { 0,   0,   0,  0,    0,   0,   S,  S,    0 },
    [S_VOICE_WAIT] = { 1,   B,   B,  B,    0,   B,   0,  S,    B },
    [S_VOICE_ON]   = { 1,   B,   B,  B,    0,   B,   0,  S,    B },
};

static void enter(int st)
{
    fresh(AUDIO_BACKEND_TRACE);
    switch (st) {
    case S_TONE_WAIT:
    case S_TONE_ON:
    case S_STOP_TONE:
        CHECK_EQ(audio_tone(440, 1000), 0);
        if (st != S_TONE_WAIT) {
            tick(10);
        }
        break;
    case S_PCM_WAIT:
    case S_PCM_ON:
    case S_STOP_PCM:
        CHECK_EQ(audio_play_pcm(play, sizeof(play), 8000, 0), 0);
        if (st != S_PCM_WAIT) {
            tick(10);
            run_us(1000);
        }
        break;
    case S_RAMP_WAIT:
    case S_RAMP_ON:
    case S_REST:
        CHECK_EQ(audio_rest_mid(), 0);
        if (st != S_RAMP_WAIT) {
            tick(10);
            run_us(1000);
        }
        if (st == S_REST) {
            run_us(50000);
            tick(10);
        }
        break;
    case S_VOICE_WAIT:
    case S_VOICE_ON:
        CHECK_EQ(audio_play_voice(spans, 1, 8000, 0), 0);
        if (st != S_VOICE_WAIT) {
            tick(10);
            run_us(1000);
        }
        break;
    default:
        break;
    }
    if (st == S_STOP_TONE || st == S_STOP_PCM) {
        audio_stop();
    }
}

static int call(int c)
{
    switch (c) {
    case C_BUSY:  return audio_busy() != 0;
    case C_PLAY:  return audio_play(notes, 1, 0);
    case C_PCM:   return audio_play_pcm(play, sizeof(play), 8000, 0);
    case C_RTTTL: return audio_play_rtttl("t:d=4,o=5,b=120:a", 0);
    case C_PRIO:  return audio_play_pcm(play, sizeof(play), 8000, AUDIO_PRIORITY);
    case C_REST:  return audio_rest_mid();
    case C_MIX:   return audio_pcm_mix(over, sizeof(over));
    case C_VOICE: return audio_play_voice(spans, 1, 8000, 0);
    default:      return audio_pcm_queue(over, sizeof(over));
    }
}

TEST(state_against_caller)
{
    int st;
    int c;

    k_memset(play, 128, sizeof(play));
    k_memset(over, 140, sizeof(over));
    for (st = 0; st < S_COUNT; st++) {
        for (c = 0; c < C_COUNT; c++) {
            int r;

            enter(st);
            r = call(c);
            if (r != want[st][c]) {
                RC_FAIL("%s in %s: %d, want %d", call_name[c], state_name[st], r,
                        want[st][c]);
            }
        }
    }
}

/* Busy holds from the accept to the rest: never 0 while the pin is not at rest. */
TEST(busy_until_the_pin_rests)
{
    int st;

    k_memset(play, 128, sizeof(play));
    for (st = 0; st < S_COUNT; st++) {
        enter(st);
        CHECK_EQ(audio_busy() != 0, st != S_IDLE && st != S_REST);
    }
}

/* audio_rest_mid() with a stop pending: BUSY, and the stop still ends the tone at its tick. */
TEST(rest_mid_waits_for_a_pending_stop)
{
    enter(S_STOP_TONE);
    clear_logs();
    CHECK_EQ(audio_rest_mid(), AUDIO_ERR_BUSY);
    CHECK_EQ(rest_mid, 0);
    tick(10);
    CHECK(!audio_busy());
    audio_trace_flush();
    CHECK_STR(out, "@RC stage=host ev=audio op=stop hz=0 vol=100 t_ms=1020\n");
    /* Once at rest, the ramp starts from rest. */
    CHECK_EQ(audio_rest_mid(), 0);
    tick(10);
    audio_trace_flush();
    CHECK(strstr(out, "op=pcm rate=22050 len=882 vol=100") != 0);
}
