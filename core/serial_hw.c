#include "serial.h"
#include "logbuf.h"
#include "format.h"

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
 * #346/#338 ROOT CAUSE of every "lost line" on real hardware: this wrote ONLY to the physical
 * COM port. The serial-less validation laptop has no such port, so every hype_serial_print --
 * deliberately the LOUD variant, used for stream verdicts, refusals, budget expiries -- was
 * invisible in \HYPEFULL.LOG while hype_debug_print lines (which tee into the logbuf) landed
 * fine. QEMU masked it completely: its -serial capture IS the COM port. One evening of
 * hardware runs was diagnosed around holes this created.
 *
 * So: format once, emit to the port, AND tee into the logbuf. hype_debug_print now writes the
 * port via the _via form directly (it tees the logbuf itself), so nothing double-appends. The
 * panic path (halt.c) keeps raw putc + append_unlocked, so no lock-order change there.
 */
void hype_serial_print(const char *fmt, ...) {
    char msg[512];
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = hype_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    (void)hype_format_mark_truncated(msg, sizeof(msg), n);
    hype_serial_print_via(hype_serial_putc, "%s", msg);
    hype_logbuf_append(msg);
}
