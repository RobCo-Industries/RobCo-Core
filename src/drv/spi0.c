#include <stddef.h>
#include <stdint.h>

#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/mbox.h>
#include <robco/mmio.h>
#include <robco/spi.h>
#include <robco/timer.h>

#define SPI0_OFFSET     0x204000

#define REG_CS          0x00
#define REG_FIFO        0x04
#define REG_CLK         0x08

#define CS_CPHA         (1u << 2)
#define CS_CPOL         (1u << 3)
#define CS_CLEAR        (3u << 4)
#define CS_CSPOL        (1u << 6)
#define CS_TA           (1u << 7)
#define CS_DONE         (1u << 16)
#define CS_RXD          (1u << 17)
#define CS_TXD          (1u << 18)
#define CS_CSPOL0       (1u << 21)

#define MBOX_CLOCK_CORE 4u
#define CDIV_MAX        0xfffeu

/* Bytes in flight. Keeps the RX FIFO from overflowing on any FIFO depth. */
#define IN_FLIGHT_MAX   16u

/*
 * Progress rule: a wait fails only after this long with no byte moved,
 * plus four times the wire time of one byte at the line's rate. The
 * window restarts at every byte, so a slow but healthy transfer of any
 * length completes.
 */
#define STALL_BASE_US   20000u

static const uint8_t pins[] = { 7, 8, 9, 10, 11 };

typedef struct {
    uint32_t cdiv;
    unsigned mode;
} line_t;

static uint32_t base;
static uint32_t core_hz;
static line_t lines[SPI_CS_COUNT];
static int active = -1;     /* chip select between begin and end, or -1 */

/* CSPOLn bits for every line, so each idles at its own inactive level. */
static uint32_t cspol_bits(void)
{
    uint32_t v = 0;
    unsigned i;

    for (i = 0; i < SPI_CS_COUNT; i++) {
        if (lines[i].mode & SPI_CS_HIGH) {
            v |= CS_CSPOL0 << i;
        }
    }
    return v;
}

static uint32_t cdiv_for(uint32_t hz)
{
    uint32_t div;

    if (hz == 0) {
        return 0;
    }
    div = core_hz / hz + (core_hz % hz != 0);
    div = (div + 1u) & ~1u;
    if (div < 2u) {
        div = 2u;
    }
    return div > CDIV_MAX ? 0 : div;
}

int spi_init(void)
{
    const robco_board_t *b = board_get();
    uint32_t core = 0;
    unsigned i;

    if (b->periph_base == 0) {
        return SPI_ERR_INIT;
    }
    for (i = 0; i < sizeof(pins); i++) {
        if (gpio_set_function(pins[i], GPIO_FN_ALT0) < 0) {
            return SPI_ERR_INIT;
        }
    }
    if (mbox_get_clock_rate(MBOX_CLOCK_CORE, &core) != 0 || core == 0) {
        core = SPI_CORE_FALLBACK_HZ;
    }
    core_hz = core;
    for (i = 0; i < SPI_CS_COUNT; i++) {
        lines[i].cdiv = cdiv_for(SPI_DEFAULT_HZ);
        lines[i].mode = SPI_MODE_0;
    }
    base = b->periph_base + SPI0_OFFSET;
    active = -1;
    mmio_write(base + REG_CS, cspol_bits() | CS_CLEAR);
    return 0;
}

int spi_configure(unsigned cs, uint32_t hz, unsigned mode)
{
    uint32_t div;

    if (base == 0) {
        return SPI_ERR_INIT;
    }
    if (cs >= SPI_CS_COUNT || mode > (SPI_MODE_3 | SPI_CS_HIGH)) {
        return SPI_ERR_ARG;
    }
    div = cdiv_for(hz);
    if (div == 0) {
        return SPI_ERR_ARG;
    }
    if (active >= 0) {
        return SPI_ERR_BUSY;
    }
    lines[cs].cdiv = div;
    lines[cs].mode = mode;
    mmio_write(base + REG_CS, cspol_bits());
    return 0;
}

uint32_t spi_get_hz(unsigned cs)
{
    if (base == 0 || cs >= SPI_CS_COUNT) {
        return 0;
    }
    return core_hz / lines[cs].cdiv;
}

int spi_begin(unsigned cs)
{
    const line_t *l;
    uint32_t v;

    if (base == 0) {
        return SPI_ERR_INIT;
    }
    if (cs >= SPI_CS_COUNT) {
        return SPI_ERR_ARG;
    }
    if (active >= 0) {
        return active == (int)cs ? 0 : SPI_ERR_BUSY;
    }
    l = &lines[cs];
    v = cspol_bits() | cs;
    if (l->mode & 1u) {
        v |= CS_CPHA;
    }
    if (l->mode & 2u) {
        v |= CS_CPOL;
    }
    if (l->mode & SPI_CS_HIGH) {
        v |= CS_CSPOL;
    }
    mmio_write(base + REG_CLK, l->cdiv);
    /* Set mode and clear the FIFOs while idle, then start. */
    mmio_write(base + REG_CS, v | CS_CLEAR);
    mmio_write(base + REG_CS, v | CS_TA);
    active = (int)cs;
    return 0;
}

/* Stall window for `cs`, in microseconds. */
static uint32_t stall_us(unsigned cs)
{
    return STALL_BASE_US + (uint32_t)(8u * 4000000ull / spi_get_hz(cs));
}

/* Releases chip select and clears the FIFOs. */
static void stop(void)
{
    mmio_write(base + REG_CS, cspol_bits() | CS_CLEAR);
    active = -1;
}

int spi_end(unsigned cs)
{
    uint64_t deadline;

    if (base == 0) {
        return SPI_ERR_INIT;
    }
    if (cs >= SPI_CS_COUNT) {
        return SPI_ERR_ARG;
    }
    if (active < 0) {
        return 0;
    }
    if (active != (int)cs) {
        return SPI_ERR_BUSY;
    }
    deadline = timer_now_us() + stall_us(cs);
    while (!(mmio_read(base + REG_CS) & CS_DONE)) {
        /* Read once more after the deadline: a host pause is not a stall. */
        if (timer_expired(deadline) && !(mmio_read(base + REG_CS) & CS_DONE)) {
            stop();
            return SPI_ERR_TIMEOUT;
        }
    }
    stop();
    return 0;
}

int spi_transfer(unsigned cs, const void *tx, void *rx, size_t len)
{
    const uint8_t *t = tx;
    uint8_t *r = rx;
    size_t sent = 0;
    size_t got = 0;
    uint64_t deadline;
    uint32_t window;
    uint32_t s;
    uint32_t b;
    int own;
    int rc;

    if (base == 0) {
        return SPI_ERR_INIT;
    }
    if (cs >= SPI_CS_COUNT) {
        return SPI_ERR_ARG;
    }
    if (active >= 0 && active != (int)cs) {
        return SPI_ERR_BUSY;
    }
    if (len == 0) {
        return 0;
    }
    own = active < 0;
    if (own) {
        rc = spi_begin(cs);
        if (rc < 0) {
            return rc;
        }
    }
    window = stall_us(cs);
    deadline = timer_now_us() + window;
    while (got < len) {
        s = mmio_read(base + REG_CS);
        if (sent < len && sent - got < IN_FLIGHT_MAX && (s & CS_TXD)) {
            mmio_write(base + REG_FIFO, t != NULL ? t[sent] : 0u);
            sent++;
        }
        if (s & CS_RXD) {
            b = mmio_read(base + REG_FIFO);
            if (r != NULL) {
                r[got] = (uint8_t)b;
            }
            got++;
            deadline = timer_now_us() + window;
        } else if (timer_expired(deadline) && !(mmio_read(base + REG_CS) & CS_RXD)) {
            /* No byte for a whole window, checked once more after it. */
            stop();
            return SPI_ERR_TIMEOUT;
        }
    }
    return own ? spi_end(cs) : 0;
}

int spi_write(unsigned cs, const void *tx, size_t len)
{
    if (tx == NULL && len != 0) {
        return SPI_ERR_ARG;
    }
    return spi_transfer(cs, tx, NULL, len);
}
