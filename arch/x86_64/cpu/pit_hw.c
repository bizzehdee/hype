#include "pit.h"

/* Channel 0, lobyte/hibyte access, mode 2 (rate generator), binary:
 * 00 11 010 0 = 0x34. */
#define PIT_CMD_CH0_MODE2_BINARY 0x34

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * Programs the PIT. Exempt from unit testing per AGENTS.md, same as
 * serial_hw.c/pic.c -- real outb calls, nothing to observe without a
 * real device. hype_pit_divisor_for_frequency() in pit.c holds the
 * actual arithmetic and is fully tested; falls back to the fastest
 * valid rate (divisor 1) on an invalid hz, same convention as
 * serial_hw.c's hype_serial_init().
 */
void hype_pit_init(uint32_t hz) {
    uint16_t divisor = hype_pit_divisor_for_frequency(hz);
    if (divisor == 0) {
        divisor = 1;
    }

    outb(HYPE_PIT_COMMAND, PIT_CMD_CH0_MODE2_BINARY);
    outb(HYPE_PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFFu));
    outb(HYPE_PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFFu));
}
