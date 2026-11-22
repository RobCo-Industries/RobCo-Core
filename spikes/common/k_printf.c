#include <stdarg.h>
#include <stdint.h>

#include "k_printf.h"
#include "uart_pl011.h"

static void k_put_padded(const char *s, int len, int width, char pad)
{
    while (width-- > len) {
        uart_putc(pad);
    }
    while (len--) {
        uart_putc(*s++);
    }
}

static void k_put_uint(uint32_t value, uint32_t radix, int negative, int width, char pad)
{
    char buf[12];
    int len = 0;
    uint32_t digit;

    do {
        digit = value % radix;
        buf[sizeof(buf) - 1 - len] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        len++;
        value /= radix;
    } while (value != 0);

    if (negative) {
        if (pad == '0') {
            uart_putc('-');
            width--;
        } else {
            buf[sizeof(buf) - 1 - len] = '-';
            len++;
        }
    }
    k_put_padded(&buf[sizeof(buf) - len], len, width, pad);
}

void k_printf(const char *fmt, ...)
{
    va_list ap;
    const char *s;
    int sv;
    int width;
    int len;
    char pad;

    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            uart_putc(*fmt);
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
            if (s == 0) {
                s = "(null)";
            }
            for (len = 0; s[len]; len++) {
            }
            k_put_padded(s, len, width, ' ');
            break;
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case 'd':
            sv = va_arg(ap, int);
            if (sv < 0) {
                k_put_uint(0u - (uint32_t)sv, 10, 1, width, pad);
            } else {
                k_put_uint((uint32_t)sv, 10, 0, width, pad);
            }
            break;
        case 'u':
            k_put_uint(va_arg(ap, unsigned int), 10, 0, width, pad);
            break;
        case 'x':
            k_put_uint(va_arg(ap, unsigned int), 16, 0, width, pad);
            break;
        case '%':
            uart_putc('%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
    }
    va_end(ap);
}
