/*
 * Test-only model of an ILI9341 panel, written from the data sheet and
 * not from the driver. It takes the bytes on the bus with their DC level
 * and the reset line, and keeps the 240x320 frame memory.
 *
 * What it models: SWRESET and the reset line, SLPIN/SLPOUT, DISPON/
 * DISPOFF, COLMOD (only 16 bits per pixel moves pixels), MADCTL MY/MX/MV,
 * CASET/PASET, RAMWR and the write pointer: it runs from SC to EC, then
 * goes to SC of the next page, and after EP back to SP. Data bytes that
 * follow no RAMWR are dropped. Pixels written while asleep or with the
 * display off still land in memory, as in the data sheet.
 *
 * The view assumes the glass is mounted so that MX gives an upright
 * portrait picture, which is what the common 2.4" and 2.8" modules do.
 * That is an assumption, not a hardware result.
 */
#ifndef ILI9341_MODEL_H
#define ILI9341_MODEL_H

#include <stdint.h>
#include <string.h>

#define MODEL_W 240u
#define MODEL_H 320u

typedef struct {
    uint16_t mem[MODEL_H][MODEL_W];     /* [row][column] in glass order */
    int asleep;
    int display_on;
    int bgr;
    uint8_t madctl;
    uint8_t colmod;
    unsigned sc, ec, sp, ep;
    unsigned c, p;          /* write pointer: column and page address */
    uint8_t cmd;            /* command being received, 0 for none */
    unsigned nparam;        /* parameter bytes received for it */
    uint8_t param[4];
    int writing;            /* after RAMWR */
    int have_hi;
    uint8_t hi;
    unsigned dropped;       /* data bytes with no command to take them */
    unsigned pixels;        /* pixels stored */
} model_t;

/* Power-on and reset state: sleep in, display off, MADCTL 0, full window. */
static inline void model_reset(model_t *m)
{
    m->asleep = 1;
    m->display_on = 0;
    m->madctl = 0;
    m->bgr = 0;
    m->colmod = 0x66;       /* 18 bits per pixel after reset */
    m->sc = 0;
    m->ec = MODEL_W - 1u;
    m->sp = 0;
    m->ep = MODEL_H - 1u;
    m->c = m->p = 0;
    m->cmd = 0;
    m->nparam = 0;
    m->writing = 0;
    m->have_hi = 0;
}

static inline void model_init(model_t *m)
{
    memset(m, 0, sizeof(*m));
    model_reset(m);
}

/* Stores one pixel at the write pointer, then moves the pointer. */
static inline void model_pixel(model_t *m, uint16_t v)
{
    unsigned col = m->c;
    unsigned row = m->p;
    unsigned t;

    if (m->madctl & 0x20u) {        /* MV: column and page exchanged */
        t = col;
        col = row;
        row = t;
    }
    if (m->madctl & 0x40u) {        /* MX: column address order */
        col = MODEL_W - 1u - col;
    }
    if (m->madctl & 0x80u) {        /* MY: row address order */
        row = MODEL_H - 1u - row;
    }
    if (col < MODEL_W && row < MODEL_H) {
        m->mem[row][col] = v;
        m->pixels++;
    }
    if (m->c == m->ec) {
        m->c = m->sc;
        m->p = m->p == m->ep ? m->sp : m->p + 1u;
    } else {
        m->c++;
    }
}

static inline void model_param(model_t *m, uint8_t v)
{
    if (m->nparam < sizeof(m->param)) {
        m->param[m->nparam] = v;
    }
    m->nparam++;
    switch (m->cmd) {
    case 0x2a:          /* CASET: SC high, SC low, EC high, EC low */
    case 0x2b:          /* PASET: SP high, SP low, EP high, EP low */
        if (m->nparam == 4) {
            unsigned s = (unsigned)m->param[0] << 8 | m->param[1];
            unsigned e = (unsigned)m->param[2] << 8 | m->param[3];

            if (m->cmd == 0x2a) {
                m->sc = s;
                m->ec = e;
            } else {
                m->sp = s;
                m->ep = e;
            }
        }
        break;
    case 0x36:          /* MADCTL */
        if (m->nparam == 1) {
            m->madctl = v;
            m->bgr = (v & 0x08u) != 0;
        }
        break;
    case 0x3a:          /* COLMOD */
        if (m->nparam == 1) {
            m->colmod = v;
        }
        break;
    default:
        m->dropped++;
        break;
    }
}

/* One byte from the bus. `dc` is 0 for a command, 1 for data. */
static inline void model_byte(model_t *m, int dc, uint8_t v)
{
    if (!dc) {
        m->cmd = v;
        m->nparam = 0;
        m->writing = 0;
        m->have_hi = 0;
        switch (v) {
        case 0x01: model_reset(m); break;           /* SWRESET */
        case 0x10: m->asleep = 1; break;            /* SLPIN */
        case 0x11: m->asleep = 0; break;            /* SLPOUT */
        case 0x28: m->display_on = 0; break;        /* DISPOFF */
        case 0x29: m->display_on = 1; break;        /* DISPON */
        case 0x2c:                                  /* RAMWR */
            m->writing = 1;
            m->c = m->sc;
            m->p = m->sp;
            break;
        default: break;
        }
        return;
    }
    if (!m->writing) {
        model_param(m, v);
        return;
    }
    if ((m->colmod & 0x0fu) != 0x05u) {
        m->dropped++;
        return;
    }
    if (!m->have_hi) {
        m->hi = v;
        m->have_hi = 1;
        return;
    }
    m->have_hi = 0;
    model_pixel(m, (uint16_t)(m->hi << 8 | v));
}

/* The reset line: low resets the panel. */
static inline void model_reset_pin(model_t *m, int level)
{
    if (!level) {
        model_reset(m);
    }
}

/* What the glass shows at portrait position (vx, vy), upright with MX set. */
static inline uint16_t model_glass(const model_t *m, unsigned vx, unsigned vy)
{
    return m->mem[vy][MODEL_W - 1u - vx];
}

/*
 * The picture as seen by someone holding the panel turned by `rotation`
 * degrees: 320x240 for 90 and 270, 240x320 otherwise. `out` gets w*h
 * pixels, row by row. Returns the width.
 */
static inline unsigned model_view(const model_t *m, unsigned rotation, uint16_t *out)
{
    unsigned w = (rotation == 90 || rotation == 270) ? MODEL_H : MODEL_W;
    unsigned h = (rotation == 90 || rotation == 270) ? MODEL_W : MODEL_H;
    unsigned x, y;
    uint16_t v;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            switch (rotation) {
            case 90: v = model_glass(m, MODEL_W - 1u - y, x); break;
            case 180: v = model_glass(m, MODEL_W - 1u - x, MODEL_H - 1u - y); break;
            case 270: v = model_glass(m, y, MODEL_H - 1u - x); break;
            default: v = model_glass(m, x, y); break;
            }
            out[y * w + x] = v;
        }
    }
    return w;
}

#endif
