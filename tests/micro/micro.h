#ifndef HYPE_MICRO_H
#define HYPE_MICRO_H

#include <stdint.h>

/*
 * #535/#534: the guest side of a hype micro-kernel.
 *
 * These kernels are guests, not part of hype: nothing here may include a hype header or link
 * against hype code. They are freestanding, have no libc, and their only output channel is the
 * emulated 16550 at COM1 -- which hype already relays into that VM's own log, the same path a
 * real guest's serial console takes.
 *
 * Everything is static inline in this header rather than a shared .c: a micro-kernel is a few
 * hundred bytes of code, and one translation unit per artifact keeps the build a single compile
 * with nothing to link against.
 */

#define MICRO_COM1 0x3F8u
#define MICRO_COM1_LSR (MICRO_COM1 + 5u)
#define MICRO_LSR_THRE 0x20u

static inline void micro_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t micro_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void micro_putc(char c) {
    /*
     * Bounded, not a bare `while`. A UART model that never raises THRE would otherwise hang the
     * guest with no output at all, which reads exactly like a wedge in the kernel under test --
     * and the whole point of this channel is to be able to tell those two apart. If the wait
     * expires, write anyway and let the missing character be the symptom.
     */
    unsigned int spins = 100000u;
    while (spins-- != 0u && (micro_inb(MICRO_COM1_LSR) & MICRO_LSR_THRE) == 0u) {
    }
    micro_outb(MICRO_COM1, (uint8_t)c);
}

static inline void micro_puts(const char *s) {
    while (*s != '\0') {
        if (*s == '\n') {
            micro_putc('\r'); /* the log's line splitter expects CRLF from a serial guest */
        }
        micro_putc(*s++);
    }
}

static inline void micro_put_hex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[19];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xFull];
    }
    buf[18] = '\0';
    micro_puts(buf);
}

static inline void micro_put_uint(uint64_t v) {
    char buf[21];
    int i = 20;
    buf[20] = '\0';
    if (v == 0ull) {
        micro_puts("0");
        return;
    }
    while (v != 0ull && i > 0) {
        buf[--i] = (char)('0' + (int)(v % 10ull));
        v /= 10ull;
    }
    micro_puts(&buf[i]);
}

/*
 * Verdict discipline (#282's rule, one level down): the verdict is a LINE IN THE LOG, not an exit
 * code. A harness greps for these. Absence of either is a failure -- a kernel that wedges or
 * triple-faults prints neither, and treating silence as success is how a dead test passes.
 */
static inline void micro_pass(const char *name) {
    micro_puts("MICRO PASS: ");
    micro_puts(name);
    micro_puts("\n");
}

static inline void micro_fail(const char *name, const char *what) {
    micro_puts("MICRO FAIL: ");
    micro_puts(name);
    micro_puts(": ");
    micro_puts(what);
    micro_puts("\n");
}

/*
 * #546: the kernel command line hype was told to pass this VM, or 0 if there was none.
 *
 * boot_params.hdr.cmd_line_ptr is at offset 0x228 of the zero page (the setup header starts at
 * 0x1F1 and cmd_line_ptr is at 0x228 within the file layout core/linux_boot.h transcribes). A zero
 * pointer means no command line AT ALL, which is distinct from an empty string at a valid address
 * -- `cmdline =` in the config means "explicitly nothing", and a test that cannot tell those apart
 * cannot report which one it got.
 */
static inline const char *micro_cmdline(uint64_t zero_page_gpa) {
    uint32_t ptr;
    if (zero_page_gpa == 0ull) {
        return 0;
    }
    ptr = *(const volatile uint32_t *)(uintptr_t)(zero_page_gpa + 0x228ull);
    return (ptr == 0u) ? 0 : (const char *)(uintptr_t)ptr;
}

/*
 * Find `key=` in a command line and return the value, or 0 if absent. Matches only at a word
 * boundary, so `verify=strict` is not found by looking for `ify=`.
 *
 * A microtest that reads a parameter must FAIL on one it does not understand rather than ignore it:
 * an ignored parameter is a test that silently did not do what the config asked, which is the same
 * class of defect as a config key that parses and has no effect.
 */
static inline const char *micro_cmdline_value(const char *cmdline, const char *key) {
    const char *p = cmdline;

    if (cmdline == 0 || key == 0) {
        return 0;
    }
    while (*p != '\0') {
        const char *k = key;
        const char *q = p;

        while (*k != '\0' && *q == *k) {
            k++;
            q++;
        }
        if (*k == '\0' && *q == '=') {
            return q + 1;
        }
        /* Advance to the start of the next space-separated word. */
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
    }
    return 0;
}

/* Stop this guest. Halting with interrupts left as the loader set them parks the vCPU in a HLT
 * exit, which hype reports as an idle guest rather than as a fault. */
static inline void micro_halt(void) {
    for (;;) {
        __asm__ volatile("hlt");
    }
}

#endif /* HYPE_MICRO_H */
