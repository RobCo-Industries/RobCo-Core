#include <stdint.h>

#include <robco/input.h>

/*
 * VT100/xterm input parser. Bytes come in one at a time. Recognised
 * sequences become key events, printable bytes and control codes become
 * CHAR events, and anything else is swallowed.
 */

enum {
    ST_GROUND = 0,
    ST_ESC,     /* ESC seen */
    ST_CSI,     /* ESC [ seen, collecting parameters */
    ST_SS3,     /* ESC O seen */
};

#define ESC         0x1b
#define PARAM_MAX   999u    /* stop growing before uint16_t overflows */

void input_ansi_init(input_ansi_t *p, uint32_t esc_timeout_ms)
{
    p->state = ST_GROUND;
    p->last_cr = 0;
    p->nparam = 0;
    p->param = 0;
    p->esc_ms = 0;
    p->timeout_ms = esc_timeout_ms;
}

static int emit(input_event_t *out, uint8_t type, uint16_t code)
{
    out->type = type;
    out->source = 0;
    out->code = code;
    out->value = type == INPUT_EV_KEY ? INPUT_PRESS : 0;
    out->t_ms = 0;
    return 1;
}

static int key(input_event_t *out, uint16_t code)
{
    return emit(out, INPUT_EV_KEY, code);
}

/* Arrow letters shared by CSI and SS3. */
static uint16_t arrow(uint8_t b)
{
    switch (b) {
    case 'A': return INPUT_KEY_UP;
    case 'B': return INPUT_KEY_DOWN;
    case 'C': return INPUT_KEY_RIGHT;
    case 'D': return INPUT_KEY_LEFT;
    default:  return INPUT_KEY_NONE;
    }
}

/* Byte in the ground state. */
static int ground(input_ansi_t *p, uint8_t b, uint32_t now_ms, input_event_t *out)
{
    int was_cr = p->last_cr;

    p->last_cr = 0;
    switch (b) {
    case ESC:
        p->state = ST_ESC;
        p->esc_ms = now_ms;
        return 0;
    case '\r':
        p->last_cr = 1;
        return key(out, INPUT_KEY_SELECT);
    case '\n':
        return was_cr ? 0 : key(out, INPUT_KEY_SELECT);
    case 0x08:
    case 0x7f:
        return key(out, INPUT_KEY_BACK);
    case 0x00:
        return 0;
    default:
        break;
    }
    if (b < 0x80) {
        return emit(out, INPUT_EV_CHAR, b);
    }
    return 0;   /* 8-bit bytes are not supported */
}

static int csi_final(input_ansi_t *p, uint8_t b, input_event_t *out)
{
    uint16_t a = arrow(b);

    if (a != INPUT_KEY_NONE) {
        return key(out, a);
    }
    if (b == '~' && p->nparam <= 1) {
        switch (p->param) {
        case 11: return key(out, INPUT_KEY_F1);
        case 12: return key(out, INPUT_KEY_F2);
        case 13: return key(out, INPUT_KEY_F3);
        case 14: return key(out, INPUT_KEY_F4);
        default: break;
        }
    }
    return 0;
}

int input_ansi_feed(input_ansi_t *p, uint8_t byte, uint32_t now_ms, input_event_t *out)
{
    int n;

    switch (p->state) {
    case ST_ESC:
        if (byte == '[') {
            p->state = ST_CSI;
            p->nparam = 0;
            p->param = 0;
            return 0;
        }
        if (byte == 'O') {
            p->state = ST_SS3;
            return 0;
        }
        if (byte >= 0x20 && byte < 0x7f) {
            /* Alt plus a key: not supported, swallowed. */
            p->state = ST_GROUND;
            return 0;
        }
        /* ESC followed by a control byte or another ESC: the ESC was alone. */
        p->state = ST_GROUND;
        n = key(out, INPUT_KEY_BACK);
        return n + ground(p, byte, now_ms, out + n);

    case ST_CSI:
        if (byte >= '0' && byte <= '9') {
            if (p->nparam == 0) {
                p->nparam = 1;
            }
            if (p->nparam == 1 && p->param <= PARAM_MAX) {
                p->param = (uint16_t)(p->param * 10 + (byte - '0'));
            }
            return 0;
        }
        if (byte == ';') {
            /* Only the first parameter counts; later ones are modifiers. */
            if (p->nparam < 255) {
                p->nparam++;
            }
            return 0;
        }
        if (byte >= 0x20 && byte <= 0x3f) {
            return 0;   /* other parameter or intermediate bytes */
        }
        p->state = ST_GROUND;
        if (byte >= 0x40 && byte <= 0x7e) {
            return csi_final(p, byte, out);
        }
        /* A control byte aborts the sequence and counts on its own. */
        return ground(p, byte, now_ms, out);

    case ST_SS3:
        p->state = ST_GROUND;
        if (arrow(byte) != INPUT_KEY_NONE) {
            return key(out, arrow(byte));
        }
        if (byte >= 'P' && byte <= 'S') {
            return key(out, (uint16_t)(INPUT_KEY_F1 + (byte - 'P')));
        }
        if (byte == 'M') {
            return key(out, INPUT_KEY_SELECT);  /* keypad Enter */
        }
        if (byte >= 0x20 && byte <= 0x7e) {
            return 0;
        }
        return ground(p, byte, now_ms, out);

    default:
        return ground(p, byte, now_ms, out);
    }
}

int input_ansi_poll(input_ansi_t *p, uint32_t now_ms, input_event_t *out)
{
    if (p->state == ST_GROUND || now_ms - p->esc_ms < p->timeout_ms) {
        return 0;
    }
    if (p->state == ST_ESC) {
        p->state = ST_GROUND;
        return key(out, INPUT_KEY_BACK);
    }
    /* An unfinished sequence timed out: drop it. */
    p->state = ST_GROUND;
    return 0;
}
