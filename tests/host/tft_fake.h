/*
 * Fake SPI, GPIO and timer under the ILI9341 driver. The driver source is
 * included here and calls these functions instead of the hardware. Every
 * call that reaches the panel lands in a transcript: chip select edges,
 * each byte with its DC level, reset and backlight edges, delays, rate
 * changes and reads. Include this file in one test_*.c only.
 */
#ifndef TFT_FAKE_H
#define TFT_FAKE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <robco/gpio.h>
#include <robco/spi.h>
#include <robco/timer.h>

enum {
    EV_CS_LO = 1,   /* a = chip select */
    EV_CS_HI,       /* a = chip select */
    EV_CMD,         /* a = byte sent with DC low */
    EV_DATA,        /* a = byte sent with DC high */
    EV_PIN,         /* a = pin, b = level */
    EV_DELAY,       /* b = microseconds */
    EV_RATE,        /* a = chip select, b = rate asked for */
    EV_READ,        /* b = bytes read */
};

typedef struct {
    uint8_t kind;
    uint16_t a;
    uint32_t b;
} ev_t;

/* Shorthands for expected transcripts. */
#define CSL     { EV_CS_LO, 0, 0 }
#define CSH     { EV_CS_HI, 0, 0 }
#define C(x)    { EV_CMD, (x), 0 }
#define D(x)    { EV_DATA, (x), 0 }
#define PIN(p, l) { EV_PIN, (p), (l) }
#define US(n)   { EV_DELAY, 0, (n) }
#define MS(n)   { EV_DELAY, 0, (n) * 1000u }
#define RATE(hz) { EV_RATE, 0, (hz) }

#define FAKE_MAX_EV 400000u
#define FAKE_CORE_HZ 250000000u

static ev_t fake_ev[FAKE_MAX_EV];
static size_t fake_n;
static int fake_overflow;
static int fake_dc_pin = -1;     /* DC changes are carried by each byte, not logged */
static int fake_dc;
static int fake_spi_up;
static uint32_t fake_hz[SPI_CS_COUNT];
static int fake_open = -1;
static int fake_timer_frozen;
static uint32_t fake_clock;
static int fake_write_fail;     /* spi_write() fails when this is non-zero */
static unsigned fake_write_fail_at; /* when non-zero, the Nth spi_write() from now fails once */
static unsigned fake_pin_fail_at;   /* when non-zero, the Nth pin write from now fails once */
static int fake_spi_init_calls;
static uint8_t fake_rx[4];
static unsigned fake_writes;    /* spi_write() calls */
static size_t fake_max_write;   /* longest spi_write() */

static void fake_log(uint8_t kind, unsigned a, uint32_t b)
{
    if (fake_n == FAKE_MAX_EV) {
        fake_overflow = 1;
        return;
    }
    fake_ev[fake_n].kind = kind;
    fake_ev[fake_n].a = (uint16_t)a;
    fake_ev[fake_n].b = b;
    fake_n++;
}

/* Clears the transcript only. */
static void fake_clear(void)
{
    fake_n = 0;
    fake_overflow = 0;
    fake_writes = 0;
    fake_max_write = 0;
}

/* Back to power-on: no SPI, timer running, no failures. */
static void fake_reset(int dc_pin)
{
    fake_clear();
    fake_dc_pin = dc_pin;
    fake_dc = 1;
    fake_spi_up = 0;
    memset(fake_hz, 0, sizeof(fake_hz));
    fake_open = -1;
    fake_timer_frozen = 0;
    fake_write_fail = 0;
    fake_write_fail_at = 0;
    fake_pin_fail_at = 0;
    fake_spi_init_calls = 0;
    memset(fake_rx, 0, sizeof(fake_rx));
}

/* The SPI0 divider rule: core clock over an even divider, never faster. */
static uint32_t fake_round(uint32_t hz)
{
    uint32_t div = FAKE_CORE_HZ / hz + (FAKE_CORE_HZ % hz != 0);

    div = (div + 1u) & ~1u;
    return FAKE_CORE_HZ / (div < 2u ? 2u : div);
}

int spi_init(void)
{
    fake_spi_init_calls++;
    fake_spi_up = 1;
    fake_hz[0] = fake_hz[1] = fake_round(SPI_DEFAULT_HZ);
    return 0;
}

int spi_configure(unsigned cs, uint32_t hz, unsigned mode)
{
    if (!fake_spi_up) {
        return SPI_ERR_INIT;
    }
    if (cs >= SPI_CS_COUNT || mode != SPI_MODE_0 || hz == 0) {
        return SPI_ERR_ARG;
    }
    if (fake_open >= 0) {
        return SPI_ERR_BUSY;
    }
    fake_hz[cs] = fake_round(hz);
    fake_log(EV_RATE, cs, hz);
    return 0;
}

uint32_t spi_get_hz(unsigned cs)
{
    return fake_spi_up && cs < SPI_CS_COUNT ? fake_hz[cs] : 0;
}

int spi_begin(unsigned cs)
{
    if (!fake_spi_up || cs >= SPI_CS_COUNT || fake_open >= 0) {
        return SPI_ERR_ARG;
    }
    fake_open = (int)cs;
    fake_log(EV_CS_LO, cs, 0);
    return 0;
}

int spi_end(unsigned cs)
{
    if (fake_open != (int)cs) {
        return SPI_ERR_BUSY;
    }
    fake_open = -1;
    fake_log(EV_CS_HI, cs, 0);
    return 0;
}

int spi_write(unsigned cs, const void *tx, size_t len)
{
    const uint8_t *p = tx;
    size_t i;

    if (fake_open != (int)cs || tx == NULL) {
        return SPI_ERR_ARG;
    }
    if (fake_write_fail_at != 0 && --fake_write_fail_at == 0) {
        return SPI_ERR_TIMEOUT;
    }
    if (fake_write_fail) {
        return SPI_ERR_TIMEOUT;
    }
    fake_writes++;
    if (len > fake_max_write) {
        fake_max_write = len;
    }
    for (i = 0; i < len; i++) {
        fake_log(fake_dc ? EV_DATA : EV_CMD, p[i], 0);
    }
    return 0;
}

int spi_transfer(unsigned cs, const void *tx, void *rx, size_t len)
{
    if (fake_open != (int)cs || tx != NULL || rx == NULL || len > sizeof(fake_rx)) {
        return SPI_ERR_ARG;
    }
    memcpy(rx, fake_rx, len);
    fake_log(EV_READ, 0, (uint32_t)len);
    return 0;
}

static int fake_pin(unsigned pin, int level)
{
    if (pin >= GPIO_PIN_COUNT) {
        return GPIO_ERR_PIN;
    }
    if (fake_pin_fail_at != 0 && --fake_pin_fail_at == 0) {
        return GPIO_ERR_PIN;
    }
    if ((int)pin == fake_dc_pin) {
        fake_dc = level != 0;
    } else {
        fake_log(EV_PIN, pin, level != 0);
    }
    return 0;
}

int gpio_write(unsigned pin, int level)
{
    return fake_pin(pin, level);
}

int gpio_config_output(unsigned pin, int initial_level)
{
    return fake_pin(pin, initial_level);
}

uint32_t timer_now_lo(void)
{
    if (!fake_timer_frozen) {
        fake_clock++;
    }
    return fake_clock;
}

void timer_delay_us(uint32_t us)
{
    fake_log(EV_DELAY, 0, us);
}

void timer_delay_ms(uint32_t ms)
{
    fake_log(EV_DELAY, 0, ms * 1000u);
}

#include "../../src/drv/tft_ili9341.c"

#endif
