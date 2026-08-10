/*
 * #371: the smallest possible UEFI application, to answer one question.
 *
 * Roughly 1 QEMU boot in 4 leaves a 113-byte serial log and never reaches hype. Two explanations
 * fit that equally well from the outside:
 *
 *   (a) the firmware never loads or never starts the application -- a QEMU/OVMF problem, and
 *       nothing hype can fix;
 *   (b) hype starts and dies before its first serial write -- a hype problem, and a serious one,
 *       because it would mean a 1-in-4 silent failure on real hardware too.
 *
 * The serial log cannot separate those: both produce exactly the same bytes. So boot something
 * that is definitely not hype, the same way hype is booted, and compare the rates.
 *
 * Deliberately minimal, and deliberately NOT linked against anything in core/: the whole point is
 * that a failure here cannot be hype's code. It touches only COM1, does not use boot services, and
 * never exits -- so the harness kills it on the same timer that bounds a hype run.
 *
 * Build:  tools/371/build-hello.sh
 */

typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned char uint8_t;

#define COM1 0x3F8u

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/*
 * Initialise COM1 exactly as hype's core/serial_hw.c does -- 115200 8N1, FIFOs on.
 *
 * It matters that this matches: if the difference between a booting run and a silent one were the
 * UART setup, an experiment using a different setup would answer a different question.
 */
static void serial_init(void) {
    outb(COM1 + 1, 0x00); /* interrupts off */
    outb(COM1 + 3, 0x80); /* DLAB */
    outb(COM1 + 0, 0x01); /* 115200 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1 */
    outb(COM1 + 2, 0xC7); /* FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B); /* DTR/RTS/OUT2 */
}

static void serial_putc(char c) {
    unsigned spins = 0;
    while ((inb(COM1 + 5) & 0x20u) == 0u) {
        if (++spins > 1000000u) return; /* never hang the experiment on a dead UART */
    }
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_putu(uint64_t v) {
    char buf[24];
    int i = 0;
    if (v == 0) {
        serial_putc('0');
        return;
    }
    while (v != 0 && i < 24) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i-- > 0) serial_putc(buf[i]);
}

/*
 * The UEFI entry point. The signature is (EFI_HANDLE, EFI_SYSTEM_TABLE *), but neither is used --
 * taking them as void * avoids pulling in any headers, and keeps this file dependency-free.
 */
uint64_t efi_main(void *image_handle, void *system_table) {
    uint64_t tick = 0;

    (void)image_handle;
    (void)system_table;

    serial_init();
    /* The banner the harness scores. Emitted as the FIRST thing this program does, so "no banner"
     * means the application never ran at all -- which is precisely the discrimination #371 needs. */
    serial_puts("hello: build 371-probe -- minimal EFI app, not hype\n");

    /*
     * Stay alive and keep printing, so a run that starts and then dies is distinguishable from one
     * that starts and keeps going. A single banner could not tell those apart, and "it printed once
     * and stopped" is a real possible outcome worth being able to see.
     */
    for (;;) {
        uint64_t spin;
        serial_puts("hello: tick=");
        serial_putu(tick++);
        serial_puts("\n");
        /* Crude delay -- no timer services, none needed. Roughly a second on this class of host;
         * exactness does not matter, only that the log grows steadily. */
        for (spin = 0; spin < 300000000ull; spin++) {
            __asm__ volatile("pause");
        }
    }
}
