/*
 * PWM backend: a square wave on PWM channel 1 and, when a second pin is
 * set, the same wave on channel 2. Mark-space mode, no FIFO, no DMA.
 *
 * Silence is DAT = 0 with the channel left enabled, so the output sits
 * low and does not jump when a tone starts or stops.
 *
 * PCM mode: the range is fixed at AUDIO_PCM_PWM_RANGE, a carrier above
 * hearing, and each sample IRQ writes the duty. A tone sets its own range
 * again, so each start picks its mode.
 */
#include <stdint.h>

#include <robco/audio.h>
#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/mmio.h>
#include <robco/timer.h>

#ifndef ROBCO_AUDIO_CLOCK_SRC
#define ROBCO_AUDIO_CLOCK_SRC   AUDIO_CLK_SRC_OSC
#endif

#if ROBCO_AUDIO_CLOCK_SRC == AUDIO_CLK_SRC_OSC
#define SRC_HZ                  19200000u
#elif ROBCO_AUDIO_CLOCK_SRC == AUDIO_CLK_SRC_PLLD
#define SRC_HZ                  500000000u
#else
#error "ROBCO_AUDIO_CLOCK_SRC must be AUDIO_CLK_SRC_OSC or AUDIO_CLK_SRC_PLLD"
#endif

#define PWM_OFFSET      0x20C000
#define PWM_CTL         0x00
#define PWM_STA         0x04
#define PWM_RNG1        0x10
#define PWM_DAT1        0x14
#define PWM_RNG2        0x20
#define PWM_DAT2        0x24

#define CTL_PWEN1       (1u << 0)
#define CTL_MSEN1       (1u << 7)
#define CTL_PWEN2       (1u << 8)
#define CTL_MSEN2       (1u << 15)
#define STA_ERRORS      0x1fcu      /* WERR1, RERR1, GAPO1-4, BERR: write 1 to clear */

#define CM_OFFSET       0x101000
#define CM_PWMCTL       0xA0
#define CM_PWMDIV       0xA4
#define CM_PASSWD       0x5A000000u
#define CM_ENAB         (1u << 4)
#define CM_KILL         (1u << 5)
#define CM_BUSY         (1u << 7)
#define CM_WAIT_US      1000u

#define PROBE_PATTERN   0x0000A5A5u

int audio_pwm_probe(void);
int audio_pwm_setup(int pin0, unsigned fn0, int pin1, unsigned fn1);
void audio_pwm_out(uint32_t hz, unsigned vol);
void audio_pwm_pcm_mode(void);
void audio_pwm_sample(uint32_t dat);

static uint32_t pwm;
static uint32_t cm;
static int ch1, ch2;

/* Waits until BUSY reads `want`. Returns 0, or -1 after CM_WAIT_US. */
static int cm_wait_busy(uint32_t want)
{
    uint32_t t0 = timer_now_lo();

    while ((mmio_read(cm + CM_PWMCTL) & CM_BUSY) != want) {
        if (timer_now_lo() - t0 >= CM_WAIT_US) {
            return -1;
        }
    }
    return 0;
}

/* Stops the clock, sets the divisor and source, starts it. Returns 0 or -1. */
static int clock_setup(void)
{
    audio_pwm_cfg_t c;
    int ret = 0;

    audio_pwm_calc(SRC_HZ, 1000, 100, &c);
    mmio_dmb();
    mmio_write(cm + CM_PWMCTL, CM_PASSWD | ROBCO_AUDIO_CLOCK_SRC);
    if (cm_wait_busy(0)) {
        /* KILL stops the generator at once, at the cost of a glitch. */
        mmio_write(cm + CM_PWMCTL, CM_PASSWD | CM_KILL | ROBCO_AUDIO_CLOCK_SRC);
        if (cm_wait_busy(0)) {
            ret = -1;
        }
        mmio_write(cm + CM_PWMCTL, CM_PASSWD | ROBCO_AUDIO_CLOCK_SRC);
    }
    mmio_write(cm + CM_PWMDIV, CM_PASSWD | (c.divi << 12));
    mmio_write(cm + CM_PWMCTL, CM_PASSWD | ROBCO_AUDIO_CLOCK_SRC);
    mmio_write(cm + CM_PWMCTL, CM_PASSWD | CM_ENAB | ROBCO_AUDIO_CLOCK_SRC);
    if (cm_wait_busy(CM_BUSY)) {
        ret = -1;
    }
    mmio_dmb();
    return ret;
}

int audio_pwm_probe(void)
{
    const robco_board_t *b = board_get();
    uint32_t v;
    uint32_t saved;

    if (b->periph_base == 0) {
        return AUDIO_PROBE_ABORT;
    }
    pwm = b->periph_base + PWM_OFFSET;
    cm = b->periph_base + CM_OFFSET;
    /* Read every register first: none is written if any read aborts. */
    if (mmio_probe(pwm + PWM_CTL, &v) || mmio_probe(pwm + PWM_RNG1, &saved) ||
        mmio_probe(cm + CM_PWMCTL, &v) || mmio_probe(cm + CM_PWMDIV, &v)) {
        return AUDIO_PROBE_ABORT;
    }
    if (clock_setup()) {
        return AUDIO_PROBE_IGNORED;
    }
    mmio_write(pwm + PWM_RNG1, PROBE_PATTERN);
    mmio_dmb();
    v = mmio_read(pwm + PWM_RNG1);
    mmio_write(pwm + PWM_RNG1, saved);
    mmio_dmb();
    return v == PROBE_PATTERN ? AUDIO_PROBE_OK : AUDIO_PROBE_IGNORED;
}

int audio_pwm_setup(int pin0, unsigned fn0, int pin1, unsigned fn1)
{
    uint32_t ctl = 0;
    int ret = 0;

    ch1 = pin0 != AUDIO_PIN_NONE;
    ch2 = pin1 != AUDIO_PIN_NONE;
    mmio_dmb();
    mmio_write(pwm + PWM_CTL, 0);
    mmio_write(pwm + PWM_STA, STA_ERRORS);
    mmio_write(pwm + PWM_RNG1, 2);
    mmio_write(pwm + PWM_DAT1, 0);
    mmio_write(pwm + PWM_RNG2, 2);
    mmio_write(pwm + PWM_DAT2, 0);
    if (ch1) {
        ctl |= CTL_PWEN1 | CTL_MSEN1;
    }
    if (ch2) {
        ctl |= CTL_PWEN2 | CTL_MSEN2;
    }
    mmio_write(pwm + PWM_CTL, ctl);
    mmio_dmb();
    /* Mux the pins last: the channels already hold the output low. */
    if (ch1 && gpio_set_function((unsigned)pin0, fn0)) {
        ret = -1;
    }
    if (ch2 && gpio_set_function((unsigned)pin1, fn1)) {
        ret = -1;
    }
    mmio_dmb();
    return ret;
}

/* IRQ context. hz 0 silences. */
void audio_pwm_out(uint32_t hz, unsigned vol)
{
    audio_pwm_cfg_t c;

    if (hz == 0 || audio_pwm_calc(SRC_HZ, hz, vol, &c)) {
        c.rng = 2;
        c.dat = 0;
    }
    mmio_dmb();
    if (c.dat == 0) {
        mmio_write(pwm + PWM_DAT1, 0);
        mmio_write(pwm + PWM_DAT2, 0);
    } else {
        if (ch1) {
            mmio_write(pwm + PWM_RNG1, c.rng);
            mmio_write(pwm + PWM_DAT1, c.dat);
        }
        if (ch2) {
            mmio_write(pwm + PWM_RNG2, c.rng);
            mmio_write(pwm + PWM_DAT2, c.dat);
        }
    }
    mmio_dmb();
}

/* IRQ context. Switches to PCM mode at the midpoint, the PCM silence. */
void audio_pwm_pcm_mode(void)
{
    uint32_t mid = audio_pcm_duty(128, AUDIO_PCM_PWM_RANGE);

    mmio_dmb();
    if (ch1) {
        mmio_write(pwm + PWM_RNG1, AUDIO_PCM_PWM_RANGE);
        mmio_write(pwm + PWM_DAT1, mid);
    }
    if (ch2) {
        mmio_write(pwm + PWM_RNG2, AUDIO_PCM_PWM_RANGE);
        mmio_write(pwm + PWM_DAT2, mid);
    }
    mmio_dmb();
}

/* IRQ context, once per sample. `dat` comes from audio_pcm_duty(). */
void audio_pwm_sample(uint32_t dat)
{
    mmio_dmb();
    if (ch1) {
        mmio_write(pwm + PWM_DAT1, dat);
    }
    if (ch2) {
        mmio_write(pwm + PWM_DAT2, dat);
    }
    mmio_dmb();
}
