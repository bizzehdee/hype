#include "logbuf.h"
#include "serial.h"

/*
 * Real 16550 UART register I/O. Exempt from unit testing per
 * AGENTS.md -- outb/inb only make sense with a real CPU/device and the
 * only way to observe their effect is a real serial line. All the
 * decision logic (baud-to-divisor, formatting, newline expansion) lives
 * in the tested serial.c; this file is deliberately just register
 * twiddling.
 */

static uint16_t g_serial_port = HYPE_SERIAL_COM1;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void hype_serial_init(uint16_t port, uint32_t baud) {
    uint16_t divisor = hype_serial_divisor_for_baud(baud);
    if (divisor == 0) {
        divisor = 1; /* fastest standard rate, rather than a nonsensical 0 */
    }
    g_serial_port = port;

    outb((uint16_t)(port + 1), 0x00); /* disable interrupts */
    outb((uint16_t)(port + 3), 0x80); /* DLAB=1 to set the baud divisor */
    outb((uint16_t)(port + 0), (uint8_t)(divisor & 0xFFu));
    outb((uint16_t)(port + 1), (uint8_t)((divisor >> 8) & 0xFFu));
    outb((uint16_t)(port + 3), 0x03); /* 8 bits, no parity, 1 stop bit, DLAB=0 */
    outb((uint16_t)(port + 2), 0xC7); /* enable + clear FIFO, 14-byte threshold */
    outb((uint16_t)(port + 4), 0x0B); /* IRQs disabled, RTS/DSR set */
}

void hype_serial_putc(char c) {
    while ((inb((uint16_t)(g_serial_port + 5)) & 0x20) == 0) {
        /* spin until the Transmitter Holding Register is empty */
    }
    outb(g_serial_port, (uint8_t)c);
}

/*
 * #238: also tee into the in-memory capture, not just the UART.
 *
 * On the real target there IS no serial port, so \HYPEFULL.LOG (and the RT-3
 * variable tail) is the only place a record can be read afterwards. Every
 * caller that reached for hype_serial_print() instead of hype_debug_print()
 * was therefore writing to a channel nobody could read -- which is exactly
 * how the m3-5/m4-3/m4-4/m4-5 microtest confirmations went missing from the
 * 2026-07-25 AMD validation run while every neighbouring test's lines
 * survived: those four tests, alone in the battery, print via this function.
 * Teeing here fixes all 27 such call sites at once instead of converting
 * them one by one and re-introducing the same asymmetry with the next one.
 *
 * Deliberately NOT also mirrored to the GOP: hype_debug_print() owns that
 * (with its own deferral/enable gating for the framebuffer cost and for
 * rendering isolation against the dashboard), and a serial print should not
 * silently start painting the screen.
 */
void hype_serial_print(const char *fmt, ...) {
    char msg[HYPE_LOG_RECORD_MAX];
    va_list ap;

    va_start(ap, fmt);
    hype_serial_format_record(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    hype_serial_write_via(hype_serial_putc, msg);
    hype_logbuf_append(msg);
}
