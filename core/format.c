#include "format.h"

static void put_char(char *buf, unsigned long long bufsz, unsigned long long *written, char c) {
    if (*written + 1 < bufsz) {
        buf[*written] = c;
    }
    (*written)++;
}

static void put_str(char *buf, unsigned long long bufsz, unsigned long long *written, const char *s) {
    while (*s) {
        put_char(buf, bufsz, written, *s);
        s++;
    }
}

/* Emits `v` in `base`, left-padded to `width` columns with `pad` (' ' or
 * '0'). width==0 / pad==' ' reproduce the original unpadded behaviour, so
 * %x/%u/%llx are unchanged; %02x/%08x/%016llx now work as C printf does --
 * previously the width/flag chars fell through the parser's default case,
 * emitting the literal "%02x" AND consuming no argument, which silently
 * shifted every following vararg (a latent wrong-pointer-to-%s crash). */
static void put_uint(char *buf, unsigned long long bufsz, unsigned long long *written,
                      unsigned long long v, unsigned int base, unsigned int width, char pad) {
    char tmp[32];
    int i = 0;
    unsigned int n;
    static const char digits[] = "0123456789abcdef";

    if (v == 0) {
        tmp[i++] = '0';
    } else {
        while (v > 0) {
            tmp[i++] = digits[v % base];
            v /= base;
        }
    }
    for (n = (unsigned int)i; n < width; n++) {
        put_char(buf, bufsz, written, pad);
    }
    while (i > 0) {
        put_char(buf, bufsz, written, tmp[--i]);
    }
}

static void put_int(char *buf, unsigned long long bufsz, unsigned long long *written, long long v,
                     unsigned int width, char pad) {
    unsigned long long uv;
    int negative = 0;

    if (v < 0) {
        negative = 1;
        /* Avoid overflow negating LLONG_MIN. */
        uv = (unsigned long long)(-(v + 1)) + 1ULL;
    } else {
        uv = (unsigned long long)v;
    }
    /* For zero-pad, the sign precedes the zeros ("-007"); for space-pad the
     * whole field (sign included) is right-justified via put_uint's padding. */
    if (negative && pad == '0') {
        put_char(buf, bufsz, written, '-');
        put_uint(buf, bufsz, written, uv, 10, width > 0 ? width - 1u : 0u, pad);
    } else if (negative) {
        put_char(buf, bufsz, written, '-');
        put_uint(buf, bufsz, written, uv, 10, 0u, pad);
    } else {
        put_uint(buf, bufsz, written, uv, 10, width, pad);
    }
}

int hype_vsnprintf(char *buf, unsigned long long bufsz, const char *fmt, va_list ap) {
    unsigned long long written = 0;

    while (*fmt) {
        if (*fmt != '%') {
            put_char(buf, bufsz, &written, *fmt);
            fmt++;
            continue;
        }
        fmt++;
        /* Optional zero-pad flag + decimal field width (e.g. %02x, %016llx,
         * %5d). Only these two, since that is all the project's diagnostics
         * use; other flags/precision are still passed through the default
         * case below. */
        {
        unsigned int width = 0;
        char pad = ' ';
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10u + (unsigned int)(*fmt - '0');
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            put_str(buf, bufsz, &written, s ? s : "(null)");
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            put_char(buf, bufsz, &written, c);
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            put_int(buf, bufsz, &written, v, width, pad);
            break;
        }
        case 'u': {
            unsigned int v = va_arg(ap, unsigned int);
            put_uint(buf, bufsz, &written, v, 10, width, pad);
            break;
        }
        case 'x': {
            unsigned int v = va_arg(ap, unsigned int);
            put_uint(buf, bufsz, &written, v, 16, width, pad);
            break;
        }
        case 'p': {
            void *p = va_arg(ap, void *);
            put_str(buf, bufsz, &written, "0x");
            put_uint(buf, bufsz, &written, (unsigned long long)p, 16, 0u, ' ');
            break;
        }
        case 'l': {
            /* %ll[uxd] -- 64-bit variants; a hypervisor deals in 64-bit
             * addresses/counts (EPT entries, LBAs, page counts, ...)
             * constantly, so this earns its keep over just %p everywhere. */
            if (fmt[1] != 'l') {
                put_char(buf, bufsz, &written, '%');
                put_char(buf, bufsz, &written, 'l');
                break;
            }
            fmt++;
            switch (fmt[1]) {
            case 'u': {
                unsigned long long v = va_arg(ap, unsigned long long);
                put_uint(buf, bufsz, &written, v, 10, width, pad);
                fmt++;
                break;
            }
            case 'x': {
                unsigned long long v = va_arg(ap, unsigned long long);
                put_uint(buf, bufsz, &written, v, 16, width, pad);
                fmt++;
                break;
            }
            case 'd': {
                long long v = va_arg(ap, long long);
                put_int(buf, bufsz, &written, v, width, pad);
                fmt++;
                break;
            }
            default:
                put_str(buf, bufsz, &written, "%ll");
                break;
            }
            break;
        }
        case '%':
            put_char(buf, bufsz, &written, '%');
            break;
        case '\0':
            put_char(buf, bufsz, &written, '%');
            goto done;
        default:
            put_char(buf, bufsz, &written, '%');
            put_char(buf, bufsz, &written, *fmt);
            break;
        }
        }
        fmt++;
    }
done:
    if (bufsz > 0) {
        unsigned long long term = (written < bufsz) ? written : bufsz - 1;
        buf[term] = '\0';
    }
    return (int)written;
}

int hype_snprintf(char *buf, unsigned long long bufsz, const char *fmt, ...) {
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = hype_vsnprintf(buf, bufsz, fmt, ap);
    va_end(ap);
    return r;
}
