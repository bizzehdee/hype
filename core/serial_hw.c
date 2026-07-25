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

void hype_serial_print(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    hype_serial_vprint_via(hype_serial_putc, fmt, ap);
    va_end(ap);
}
