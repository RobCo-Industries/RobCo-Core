#include <stddef.h>
#include <stdint.h>

#include <robco/board.h>
#include <robco/gpio.h>
#include <robco/i2c.h>
#include <robco/mbox.h>
#include <robco/mmio.h>
#include <robco/timer.h>

#define BSC1_OFFSET     0x804000

#define REG_C           0x00
#define REG_S           0x04
#define REG_DLEN        0x08
#define REG_A           0x0c
#define REG_FIFO        0x10
#define REG_DIV         0x14
#define REG_CLKT        0x1c

#define C_I2CEN         (1u << 15)
#define C_ST            (1u << 7)
#define C_CLEAR         (3u << 4)
#define C_READ          (1u << 0)

#define S_CLKT          (1u << 9)
#define S_ERR           (1u << 8)
#define S_RXD           (1u << 5)
#define S_TXD           (1u << 4)
#define S_DONE          (1u << 1)
#define S_TA            (1u << 0)

#define MBOX_CLOCK_CORE 4u
#define SDA_PIN         2u
#define SCL_PIN         3u
#define DLEN_MAX        0xffffu

/* SCL periods a slave may stretch the clock before CLKT. */
#define CLKT_TOUT       0x40u

/* Fixed part of every deadline, for scheduling stalls on slow hosts. */
#define DEADLINE_BASE_US 20000u

/* Wait for the controller to send STOP after an error. */
#define ABORT_WAIT_US   1000u

static uint32_t base;
static uint32_t scl_hz;

static uint32_t reg(uint32_t off)
{
    return base + off;
}

/*
 * Clears the FIFO and every sticky status bit and leaves the controller
 * disabled. I2CEN stays 0 here: QEMU's model starts a transfer on any
 * write of C with I2CEN set, even without ST.
 */
static void reset_state(void)
{
    mmio_write(reg(REG_C), C_CLEAR);
    mmio_write(reg(REG_S), S_CLKT | S_ERR | S_DONE);
}

int i2c_init(uint32_t hz)
{
    const robco_board_t *b = board_get();
    uint32_t core = 0;
    uint32_t div;

    if (hz < I2C_HZ_MIN || hz > I2C_HZ_MAX) {
        return I2C_ERR_ARG;
    }
    if (b->periph_base == 0) {
        return I2C_ERR_INIT;
    }
    if (mbox_get_clock_rate(MBOX_CLOCK_CORE, &core) != 0 || core == 0) {
        core = I2C_CORE_FALLBACK_HZ;
    }
    /* Round up so the bus never runs faster than asked. CDIV must be even. */
    div = (core + hz - 1u) / hz;
    div = (div + 1u) & ~1u;
    if (div < 2u) {
        div = 2u;
    }
    if (div > 0xfffeu) {
        return I2C_ERR_ARG;
    }
    if (gpio_set_function(SDA_PIN, GPIO_FN_ALT0) < 0
        || gpio_set_function(SCL_PIN, GPIO_FN_ALT0) < 0) {
        return I2C_ERR_INIT;
    }
    base = b->periph_base + BSC1_OFFSET;
    mmio_write(reg(REG_C), 0);
    mmio_write(reg(REG_DIV), div);
    mmio_write(reg(REG_CLKT), CLKT_TOUT);
    reset_state();
    scl_hz = core / div;
    return 0;
}

uint32_t i2c_get_hz(void)
{
    return scl_hz;
}

/* Generous bound: 9 bits per byte plus address, times four, plus a fixed margin. */
static uint64_t deadline_for(size_t len)
{
    uint64_t bits = ((uint64_t)len + 2u) * 9u;

    return timer_now_us() + DEADLINE_BASE_US + bits * 4000000u / scl_hz;
}

/* Runs one transfer. `rbuf` for a read, `wbuf` for a write. */
static int transfer(uint8_t addr, const uint8_t *wbuf, uint8_t *rbuf, size_t len)
{
    uint64_t deadline;
    uint32_t s;
    size_t done = 0;
    int rc = 0;

    if (base == 0) {
        return I2C_ERR_INIT;
    }
    if (addr > I2C_ADDR_MAX || len == 0 || len > DLEN_MAX
        || (wbuf == NULL && rbuf == NULL)) {
        return I2C_ERR_ARG;
    }
    reset_state();
    mmio_write(reg(REG_A), addr);
    mmio_write(reg(REG_DLEN), (uint32_t)len);
    /*
     * Start first, then fill the FIFO from the loop. The controller holds
     * SCL low while the FIFO is empty, and QEMU drops FIFO writes made
     * before the transfer is active.
     */
    mmio_write(reg(REG_C), C_I2CEN | C_ST | (wbuf == NULL ? C_READ : 0u));
    deadline = deadline_for(len);
    for (;;) {
        s = mmio_read(reg(REG_S));
        if (s & (S_ERR | S_CLKT)) {
            break;
        }
        if (wbuf != NULL) {
            while (done < len && (s & S_TXD)) {
                mmio_write(reg(REG_FIFO), wbuf[done++]);
                s = mmio_read(reg(REG_S));
            }
        } else {
            while (done < len && (s & S_RXD)) {
                rbuf[done++] = (uint8_t)mmio_read(reg(REG_FIFO));
                s = mmio_read(reg(REG_S));
            }
        }
        if ((s & S_DONE) && (wbuf != NULL || done == len)) {
            break;
        }
        if (timer_expired(deadline)) {
            rc = I2C_ERR_TIMEOUT;
            break;
        }
    }
    s = mmio_read(reg(REG_S));
    if (s & S_CLKT) {
        rc = I2C_ERR_CLKT;
    } else if (s & S_ERR) {
        rc = I2C_ERR_NACK;
    } else if (rc == 0 && done != len) {
        rc = I2C_ERR_TIMEOUT;
    }
    if (rc != 0) {
        /*
         * After ERR or CLKT the controller sends STOP and sets DONE. Give
         * it a short bounded wait: QEMU leaves TA set and never sets DONE.
         */
        deadline = timer_now_us() + ABORT_WAIT_US;
        while (!(mmio_read(reg(REG_S)) & S_DONE) && !timer_expired(deadline)) {
        }
    }
    reset_state();
    return rc;
}

int i2c_write(uint8_t addr, const void *buf, size_t len)
{
    if (buf == NULL) {
        return I2C_ERR_ARG;
    }
    return transfer(addr, buf, NULL, len);
}

int i2c_read(uint8_t addr, void *buf, size_t len)
{
    if (buf == NULL) {
        return I2C_ERR_ARG;
    }
    return transfer(addr, NULL, buf, len);
}

int i2c_write_read(uint8_t addr, const void *wbuf, size_t wlen, void *rbuf, size_t rlen)
{
    int rc;

    if (wbuf == NULL || rbuf == NULL || rlen == 0) {
        return I2C_ERR_ARG;
    }
    rc = transfer(addr, wbuf, NULL, wlen);
    if (rc < 0) {
        return rc;
    }
    return transfer(addr, NULL, rbuf, rlen);
}

int i2c_probe(uint8_t addr)
{
    uint8_t b;
    int rc = i2c_read(addr, &b, 1);

    if (rc == 0) {
        return 1;
    }
    return rc == I2C_ERR_NACK ? 0 : rc;
}

const char *i2c_strerror(int code)
{
    switch (code) {
    case 0:                 return "ok";
    case I2C_ERR_ARG:       return "bad argument";
    case I2C_ERR_INIT:      return "not initialised";
    case I2C_ERR_NACK:      return "no acknowledge";
    case I2C_ERR_TIMEOUT:   return "timeout";
    case I2C_ERR_CLKT:      return "clock stretch timeout";
    default:                return "unknown error";
    }
}
