#include "rtc.h"

/*
 * CMOS RTC port I/O. Exempt from unit testing per AGENTS.md, same as
 * serial_hw.c / pit_hw.c -- real inb/outb, nothing to observe without hardware.
 * All the decoding this delegates to (hype_rtc_decode) IS unit-tested.
 */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

#define RTC_INDEX_PORT 0x70u
#define RTC_DATA_PORT 0x71u

/* Bit 7 of the index port is the NMI-disable line on a PC. Preserving it
 * matters: writing a bare register number would clear it and re-enable NMIs as
 * a side effect of reading the clock. Read-modify-write is not possible (0x70
 * is write-only), so keep the bit set -- hype masks NMIs post-EBS anyway, and
 * leaving them masked is the safe direction. */
#define RTC_NMI_DISABLE 0x80u

static uint8_t rtc_reg(uint8_t reg) {
    outb(RTC_INDEX_PORT, (uint8_t)(reg | RTC_NMI_DISABLE));
    return inb(RTC_DATA_PORT);
}

/* Spin until the update-in-progress flag clears, bounded so a machine with no
 * RTC (or one wedged with UIP stuck high) cannot hang the boot. The RTC's update
 * cycle is under 2ms, so this bound is generous by orders of magnitude. */
static int rtc_wait_ready(void) {
    unsigned long spins;
    for (spins = 0; spins < 1000000ul; spins++) {
        if ((rtc_reg(HYPE_RTC_REG_STATUS_A) & HYPE_RTC_STATUS_A_UIP) == 0u) {
            return 0;
        }
    }
    return -1;
}

int hype_rtc_read(hype_rtc_time_t *out) {
    unsigned attempt;

    if (out == 0) {
        return -1;
    }

    /*
     * Read twice and accept only a matching pair. The RTC can roll over between
     * two register reads -- reading 01:59:59 and then the new hour gives
     * 02:59:59, an hour into the future. Waiting for !UIP first shrinks that
     * window but does not close it, because the wait can finish microseconds
     * before an update begins. Comparing two full reads does close it: a
     * rollover cannot produce two identical snapshots.
     */
    for (attempt = 0; attempt < 4u; attempt++) {
        uint8_t s1, m1, h1, d1, mo1, y1, c1, b1;
        uint8_t s2, m2, h2, d2, mo2, y2, c2;

        if (rtc_wait_ready() != 0) {
            return -1;
        }
        b1 = rtc_reg(HYPE_RTC_REG_STATUS_B);
        s1 = rtc_reg(HYPE_RTC_REG_SECOND);
        m1 = rtc_reg(HYPE_RTC_REG_MINUTE);
        h1 = rtc_reg(HYPE_RTC_REG_HOUR);
        d1 = rtc_reg(HYPE_RTC_REG_DAY);
        mo1 = rtc_reg(HYPE_RTC_REG_MONTH);
        y1 = rtc_reg(HYPE_RTC_REG_YEAR);
        c1 = rtc_reg(HYPE_RTC_REG_CENTURY);

        if (rtc_wait_ready() != 0) {
            return -1;
        }
        s2 = rtc_reg(HYPE_RTC_REG_SECOND);
        m2 = rtc_reg(HYPE_RTC_REG_MINUTE);
        h2 = rtc_reg(HYPE_RTC_REG_HOUR);
        d2 = rtc_reg(HYPE_RTC_REG_DAY);
        mo2 = rtc_reg(HYPE_RTC_REG_MONTH);
        y2 = rtc_reg(HYPE_RTC_REG_YEAR);
        c2 = rtc_reg(HYPE_RTC_REG_CENTURY);

        if (s1 == s2 && m1 == m2 && h1 == h2 && d1 == d2 && mo1 == mo2 && y1 == y2 && c1 == c2) {
            return hype_rtc_decode(s1, m1, h1, d1, mo1, y1, c1, b1, out);
        }
    }
    return -1;
}
