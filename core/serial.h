#ifndef HYPE_SERIAL_H
#define HYPE_SERIAL_H

#include <stdarg.h>
#include <stdint.h>

/*
 * Serial console driver (M1-5): a 16550-compatible UART on COM1, the
 * only output channel available once ExitBootServices() has removed
 * ConOut (M1-4).
 *
 * Same split as console.c: the actual byte-at-a-time transmit is real
 * PIO register access with nothing to unit test (AGENTS.md exemption,
 * same as halt.c) -- but *unlike* the GDT/IDT/paging load shims, the
 * higher-level formatting/newline-expansion/iteration logic doesn't
 * have to share that fate: it takes the one-byte "putc" as an injected
 * function pointer, exactly the way console.c already gets to unit
 * test hype_console_print by mocking ConOut->OutputString. Only the
 * one-line outb/inb calls in serial_hw.c are actually exempt.
 */

typedef void (*hype_serial_putc_fn)(char c);

/* 115200 / baud, e.g. 115200 -> 1, 9600 -> 12. Returns 0 (invalid) for
 * baud == 0 or a divisor that doesn't fit in the UART's 16-bit divisor
 * latch. Pure arithmetic. */
uint16_t hype_serial_divisor_for_baud(uint32_t baud);

/* Writes `s` through `putc`, expanding '\n' to "\r\n" (same convention
 * as console.c's ConOut, and for the same reason -- a raw terminal
 * needs the explicit CR). Pure iteration, testable via a mock putc. */
void hype_serial_write_via(hype_serial_putc_fn putc, const char *s);

/* Formats fmt/ap into a fixed-size stack buffer, then
 * hype_serial_write_via()s it. Testable via a mock putc. */
void hype_serial_vprint_via(hype_serial_putc_fn putc, const char *fmt, va_list ap);

/* va_start/va_end wrapper around hype_serial_vprint_via(). Testable via
 * a mock putc. */
void hype_serial_print_via(hype_serial_putc_fn putc, const char *fmt, ...);

/* ---- Real hardware (COM1, 16550 UART). Exempt from unit testing --
 * see serial_hw.c and halt.h for why. ---- */

#define HYPE_SERIAL_COM1 0x3F8

void hype_serial_init(uint16_t port, uint32_t baud);
void hype_serial_putc(char c);
void hype_serial_print(const char *fmt, ...);

#endif /* HYPE_SERIAL_H */
