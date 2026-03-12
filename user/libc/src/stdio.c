/*
 * stdio.c — Formatted output (printf family).
 *
 * Supports: %d %i %u %x %X %p %s %c %% %ld %li %lu %lx %lX
 * Width and zero-padding supported. No floating point.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* ── vsnprintf engine ─────────────────────────────────────────────────── */

static void out_char(char *buf, size_t size, size_t *pos, char c)
{
    if (*pos < size - 1)
        buf[*pos] = c;
    (*pos)++;
}

static void out_str(char *buf, size_t size, size_t *pos,
                    const char *s, int width, int pad_zero)
{
    int len = 0;
    while (s[len]) len++;
    int pad = (width > len) ? width - len : 0;
    char pc = pad_zero ? '0' : ' ';
    while (pad--) out_char(buf, size, pos, pc);
    for (int i = 0; i < len; i++)
        out_char(buf, size, pos, s[i]);
}

static void out_unsigned(char *buf, size_t size, size_t *pos,
                         uint64_t val, int base, int upper,
                         int width, int pad_zero)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[20];
    int i = 0;

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val) {
            tmp[i++] = digits[val % (uint64_t)base];
            val /= (uint64_t)base;
        }
    }

    int pad = (width > i) ? width - i : 0;
    char pc = pad_zero ? '0' : ' ';
    while (pad--) out_char(buf, size, pos, pc);
    while (i--) out_char(buf, size, pos, tmp[i]);
}

static void out_signed(char *buf, size_t size, size_t *pos,
                       int64_t val, int width, int pad_zero)
{
    if (val < 0) {
        out_char(buf, size, pos, '-');
        if (width > 0) width--;
        /* Handle INT64_MIN without overflow. */
        uint64_t uval = (uint64_t)(-(val + 1)) + 1;
        out_unsigned(buf, size, pos, uval, 10, 0, width, pad_zero);
    } else {
        out_unsigned(buf, size, pos, (uint64_t)val, 10, 0, width, pad_zero);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    if (size == 0) return 0;

    size_t pos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            out_char(buf, size, &pos, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Flags. */
        int pad_zero = 0;
        if (*fmt == '0') { pad_zero = 1; fmt++; }

        /* Width. */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Length modifier. */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            int64_t val = is_long ? va_arg(ap, long) : (int64_t)va_arg(ap, int);
            out_signed(buf, size, &pos, val, width, pad_zero);
            break;
        }
        case 'u': {
            uint64_t val = is_long ? va_arg(ap, unsigned long)
                                   : (uint64_t)va_arg(ap, unsigned int);
            out_unsigned(buf, size, &pos, val, 10, 0, width, pad_zero);
            break;
        }
        case 'x': {
            uint64_t val = is_long ? va_arg(ap, unsigned long)
                                   : (uint64_t)va_arg(ap, unsigned int);
            out_unsigned(buf, size, &pos, val, 16, 0, width, pad_zero);
            break;
        }
        case 'X': {
            uint64_t val = is_long ? va_arg(ap, unsigned long)
                                   : (uint64_t)va_arg(ap, unsigned int);
            out_unsigned(buf, size, &pos, val, 16, 1, width, pad_zero);
            break;
        }
        case 'p': {
            void *ptr = va_arg(ap, void *);
            out_char(buf, size, &pos, '0');
            out_char(buf, size, &pos, 'x');
            out_unsigned(buf, size, &pos, (uint64_t)(uintptr_t)ptr, 16, 0, 0, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            out_str(buf, size, &pos, s, width, 0);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            out_char(buf, size, &pos, c);
            break;
        }
        case '%':
            out_char(buf, size, &pos, '%');
            break;
        default:
            out_char(buf, size, &pos, '%');
            out_char(buf, size, &pos, *fmt);
            break;
        }
        fmt++;
    }

    /* Null-terminate. */
    if (pos < size)
        buf[pos] = '\0';
    else
        buf[size - 1] = '\0';

    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > 0) {
        size_t n = (size_t)len;
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        write(1, buf, n);
    }
    return len;
}

int putchar(int c)
{
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char *s)
{
    size_t len = strlen(s);
    write(1, s, len);
    write(1, "\n", 1);
    return 0;
}

int getchar(void)
{
    char c;
    long n = read(0, &c, 1);
    if (n <= 0) return EOF;
    return (unsigned char)c;
}

char *gets_s(char *buf, size_t size)
{
    if (!buf || size == 0) return (void *)0;

    size_t pos = 0;
    while (pos < size - 1) {
        int c = getchar();
        if (c == EOF) {
            if (pos == 0) return (void *)0;
            break;
        }

        if (c == 0x03) {
            /* Ctrl+C — print ^C and abort input. */
            write(1, "^C\n", 3);
            buf[0] = '\0';
            return (void *)0;
        }

        if (c == '\n' || c == '\r') {
            putchar('\n');
            break;
        }

        if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                /* Erase character on screen. */
                write(1, "\b \b", 3);
            }
            continue;
        }

        buf[pos++] = (char)c;
        putchar(c);  /* Echo */
    }

    buf[pos] = '\0';
    return buf;
}
