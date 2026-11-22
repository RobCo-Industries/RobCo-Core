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

static void k_out_padded(k_out_t *o, const char *s, int len, int width, char pad)
{
    while (width-- > len) {
        k_out(o, pad);
    }
    while (len--) {
        k_out(o, *s++);
    }
}

static void k_out_uint(k_out_t *o, uint32_t value, uint32_t radix, int upper,
                       int negative, int width, char pad)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[12];
    int len = 0;

    do {
        tmp[sizeof(tmp) - 1 - len] = digits[value % radix];
        len++;
        value /= radix;
    } while (value != 0);

    if (negative) {
        if (pad == '0') {
            k_out(o, '-');
            width--;
        } else {
            tmp[sizeof(tmp) - 1 - len] = '-';
            len++;
        }
    }
    k_out_padded(o, &tmp[sizeof(tmp) - len], len, width, pad);
}

static void k_format(k_out_t *o, const char *fmt, va_list ap)
{
    const char *s;
    int sv;
    int width;
    char pad;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            k_out(o, *fmt);
            continue;
        }
        fmt++;
        pad = ' ';
        width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        switch (*fmt) {
        case 's':
            s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            k_out_padded(o, s, (int)k_strlen(s), width, ' ');
            break;
        case 'c':
            k_out(o, (char)va_arg(ap, int));
            break;
        case 'd':
            sv = va_arg(ap, int);
            if (sv < 0) {
                k_out_uint(o, 0u - (uint32_t)sv, 10, 0, 1, width, pad);
            } else {
                k_out_uint(o, (uint32_t)sv, 10, 0, 0, width, pad);
            }
            break;
        case 'u':
            k_out_uint(o, va_arg(ap, unsigned int), 10, 0, 0, width, pad);
            break;
        case 'x':
            k_out_uint(o, va_arg(ap, unsigned int), 16, 0, 0, width, pad);
            break;
        case 'X':
            k_out_uint(o, va_arg(ap, unsigned int), 16, 1, 0, width, pad);
            break;
        case 'p':
            k_out(o, '0');
            k_out(o, 'x');
            k_out_uint(o, (uint32_t)(uintptr_t)va_arg(ap, void *), 16, 0, 0, 8, '0');
            break;
        case '%':
            k_out(o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            k_out(o, '%');
            k_out(o, *fmt);
            break;
        }
    }
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
