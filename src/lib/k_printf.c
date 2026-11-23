#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <robco/klibc.h>

typedef struct {
    void (*putc)(char);     /* console output, or NULL for a buffer */
    char *buf;
    size_t size;
    size_t len;             /* characters produced, even past size */
} k_out_t;

static void (*k_console)(char);

void k_console_set(void (*putc)(char))
{
    k_console = putc;
}

/* Used by drivers: installs `putc` only if no console is set yet. */
void k_console_set_default(void (*putc)(char))
{
    if (k_console == NULL) {
        k_console = putc;
    }
}

static void k_out(k_out_t *o, char c)
{
    if (o->putc != NULL) {
        o->putc(c);
    } else if (o->len + 1 < o->size) {
        o->buf[o->len] = c;
    }
    o->len++;
}

/*
 * Writes one field: optional sign, `zeros` leading zeros, then `len` bytes of
 * `s`, padded to `width`. `left` pads on the right with spaces. Otherwise
 * `pad` fills on the left; a '0' pad goes after the sign.
 */
static void k_out_field(k_out_t *o, char sign, int zeros, const char *s, int len,
                        int width, int left, char pad)
{
    int fill = width - len - zeros - (sign != 0);

    if (sign != 0 && pad == '0') {
        k_out(o, sign);
    }
    if (!left) {
        for (; fill > 0; fill--) {
            k_out(o, pad);
        }
    }
    if (sign != 0 && pad != '0') {
        k_out(o, sign);
    }
    for (; zeros > 0; zeros--) {
        k_out(o, '0');
    }
    while (len-- > 0) {
        k_out(o, *s++);
    }
    for (; fill > 0; fill--) {
        k_out(o, ' ');
    }
}

/*
 * Longest digit string k_out_uint() builds. The sign and the precision zeros
 * go out separately, so only the digits count. Decimal only formats an
 * unsigned int (%d after negation, %u): ceil(bits * log10(2)) digits, which
 * 5 * bytes / 2 bounds (10 for 32 bits). Hex formats an unsigned int or a
 * uintptr_t (%p): 2 digits per byte. The target needs 10, a 64-bit host 16.
 */
#define K_DEC_DIGITS ((sizeof(unsigned int) * 5 + 1) / 2)
#define K_HEX_DIGITS (2 * sizeof(uintptr_t))
#define K_DIGITS_MAX (K_DEC_DIGITS > K_HEX_DIGITS ? K_DEC_DIGITS : K_HEX_DIGITS)

/*
 * uintptr_t so %p prints full pointers on a 64-bit host. 32 bits on the target.
 * `prec` is the minimum digit count, or -1 for none. Precision 0 prints no
 * digits for the value 0.
 */
static void k_out_uint(k_out_t *o, uintptr_t value, unsigned radix, int upper,
                       char sign, int prec, int width, int left, char pad)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[3 * sizeof(uintptr_t)];
    _Static_assert(sizeof(tmp) >= K_DIGITS_MAX, "tmp must hold the longest digit string");
    int len = 0;

    while (value != 0 || (len == 0 && prec != 0)) {
        tmp[sizeof(tmp) - 1 - len] = digits[value % radix];
        len++;
        value /= radix;
    }
    k_out_field(o, sign, prec > len ? prec - len : 0, &tmp[sizeof(tmp) - len],
                len, width, left, pad);
}

/* Reads a decimal number or '*' at *fmtp. Returns -1 for a negative '*'. */
static int k_read_num(const char **fmtp, va_list *ap, int *negative)
{
    const char *f = *fmtp;
    int n = 0;

    *negative = 0;
    if (*f == '*') {
        n = va_arg(*ap, int);
        *fmtp = f + 1;
        if (n < 0) {
            *negative = 1;
            n = (int)(0u - (unsigned)n);
        }
        return n;
    }
    while (*f >= '0' && *f <= '9') {
        n = n * 10 + (*f - '0');
        f++;
    }
    *fmtp = f;
    return n;
}

static void k_format(k_out_t *o, const char *fmt, va_list ap_in)
{
    const char *spec;
    const char *s;
    va_list ap;
    int sv;
    int width;
    int prec;
    int left;
    int zero;
    int neg;
    int len;
    char pad;

    va_copy(ap, ap_in);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            k_out(o, *fmt);
            continue;
        }
        spec = fmt;
        fmt++;
        left = 0;
        zero = 0;
        for (;; fmt++) {
            if (*fmt == '-') {
                left = 1;
            } else if (*fmt == '0') {
                zero = 1;
            } else {
                break;
            }
        }
        width = k_read_num(&fmt, &ap, &neg);
        if (neg) {
            left = 1;   /* a negative '*' width means '-' */
        }
        prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = k_read_num(&fmt, &ap, &neg);
            if (neg) {
                prec = -1;  /* a negative '*' precision means none */
            }
        }
        /* '-' beats '0'. A precision turns '0' off for integers. */
        pad = (zero && !left && prec < 0) ? '0' : ' ';
        switch (*fmt) {
        case 's':
            s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            for (len = 0; (prec < 0 || len < prec) && s[len] != '\0'; len++) {
            }
            k_out_field(o, 0, 0, s, len, width, left, ' ');
            break;
        case 'c':
            sv = va_arg(ap, int);
            k_out_field(o, 0, 0, &(char){ (char)sv }, 1, width, left, ' ');
            break;
        case 'd':
            sv = va_arg(ap, int);
            if (sv < 0) {
                k_out_uint(o, 0u - (uint32_t)sv, 10, 0, '-', prec, width, left, pad);
            } else {
                k_out_uint(o, (uint32_t)sv, 10, 0, 0, prec, width, left, pad);
            }
            break;
        case 'u':
            k_out_uint(o, va_arg(ap, unsigned int), 10, 0, 0, prec, width, left, pad);
            break;
        case 'x':
            k_out_uint(o, va_arg(ap, unsigned int), 16, 0, 0, prec, width, left, pad);
            break;
        case 'X':
            k_out_uint(o, va_arg(ap, unsigned int), 16, 1, 0, prec, width, left, pad);
            break;
        case 'p':
            /* Fixed form: flags, width and precision are ignored. */
            k_out(o, '0');
            k_out(o, 'x');
            k_out_uint(o, (uintptr_t)va_arg(ap, void *), 16, 0, 0,
                       (int)(2 * sizeof(void *)), 0, 0, ' ');
            break;
        case '%':
            k_out(o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            /* Unknown conversion: print the whole spec as text. */
            for (; spec <= fmt; spec++) {
                k_out(o, *spec);
            }
            break;
        }
    }
    va_end(ap);
}

int k_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    k_out_t o = { NULL, buf, size, 0 };

    k_format(&o, fmt, ap);
    if (size != 0) {
        buf[o.len < size ? o.len : size - 1] = '\0';
    }
    return (int)o.len;
}

int k_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = k_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void k_printf(const char *fmt, ...)
{
    k_out_t o = { k_console, NULL, 0, 0 };
    va_list ap;

    if (o.putc == NULL) {
        return;
    }
    va_start(ap, fmt);
    k_format(&o, fmt, ap);
    va_end(ap);
}
