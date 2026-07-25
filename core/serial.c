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

void hype_serial_vprint_via(hype_serial_putc_fn putc, const char *fmt, va_list ap) {
    char buf[256];

    hype_vsnprintf(buf, sizeof(buf), fmt, ap);
    hype_serial_write_via(putc, buf);
}

void hype_serial_print_via(hype_serial_putc_fn putc, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    hype_serial_vprint_via(putc, fmt, ap);
    va_end(ap);
}
