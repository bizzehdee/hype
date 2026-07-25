#include <stdarg.h>

#include "serial.h"
#include "format.h"

uint16_t hype_serial_divisor_for_baud(uint32_t baud) {
    uint32_t divisor;

    if (baud == 0) {
        return 0;
    }
    divisor = 115200u / baud;
    if (divisor == 0 || divisor > 0xFFFFu) {
        return 0;
    }
    return (uint16_t)divisor;
}

void hype_serial_write_via(hype_serial_putc_fn putc, const char *s) {
    while (*s) {
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s);
        s++;
    }
}

int hype_serial_format_record(char *buf, unsigned long long bufsz, const char *fmt, va_list ap) {
    /* Ends in '\n' on purpose -- see the header comment: a truncated record
     * that loses its newline makes the next record share its line. */
    static const char marker[] = "...[TRUNCATED]\n";
    const unsigned long long mlen = sizeof(marker) - 1u; /* excludes the NUL */
    unsigned long long i;
    int would;

    if (buf == 0 || bufsz == 0) {
        return 0;
    }

    would = hype_vsnprintf(buf, bufsz, fmt, ap);
    if (would < 0 || (unsigned long long)would < bufsz) {
        return 0; /* fitted -- vsnprintf's return excludes the NUL */
    }

    /* Overwrite the tail with the marker rather than appending it (there is by
     * definition no room to append). Only possible if the marker itself fits;
     * a buffer that small can't say anything useful, so leave it be. */
    if (bufsz > mlen) {
        for (i = 0; i < mlen; i++) {
            buf[bufsz - 1u - mlen + i] = marker[i];
        }
    }
    buf[bufsz - 1u] = '\0';
    return 1;
}

void hype_serial_vprint_via(hype_serial_putc_fn putc, const char *fmt, va_list ap) {
    char buf[HYPE_LOG_RECORD_MAX];

    hype_serial_format_record(buf, sizeof(buf), fmt, ap);
    hype_serial_write_via(putc, buf);
}

void hype_serial_print_via(hype_serial_putc_fn putc, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    hype_serial_vprint_via(putc, fmt, ap);
    va_end(ap);
}
