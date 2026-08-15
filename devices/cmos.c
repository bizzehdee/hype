#include "cmos.h"

#include "../core/rtc.h"

void hype_cmos_reset(hype_cmos_t *cmos) {
    unsigned int i;

    cmos->index = 0;
    for (i = 0; i < HYPE_CMOS_SIZE; i++) {
        cmos->registers[i] = 0;
    }
    /* #286: zero is not a legal power-on state for these -- see cmos.h. */
    cmos->registers[HYPE_CMOS_REG_STATUS_A] = HYPE_CMOS_STATUS_A_RESET;
    cmos->registers[HYPE_CMOS_REG_STATUS_B] = HYPE_CMOS_STATUS_B_RESET;
    cmos->registers[HYPE_CMOS_REG_STATUS_D] = HYPE_CMOS_STATUS_D_RESET;
    cmos->periodic_ns = 0;
    cmos->periodic_owed = 0;
    cmos->base_valid = 0;
    cmos->base_year = 0;
    cmos->base_month = 0;
    cmos->base_day = 0;
    cmos->base_hour = 0;
    cmos->base_minute = 0;
    cmos->base_second = 0;
}

/* Binary -> BCD for one two-digit field. */
static uint8_t bin_to_bcd(unsigned int v) {
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

/* Write one wall clock into the time/date registers, honouring register B's DM bit. */
static void encode_time(hype_cmos_t *cmos, unsigned int year, unsigned int month,
                        unsigned int day, unsigned int hour, unsigned int minute,
                        unsigned int second) {
    int binary = (cmos->registers[HYPE_CMOS_REG_STATUS_B] & HYPE_CMOS_STATUS_B_BINARY) != 0;

    if (binary) {
        cmos->registers[HYPE_CMOS_REG_SECONDS] = (uint8_t)second;
        cmos->registers[HYPE_CMOS_REG_MINUTES] = (uint8_t)minute;
        cmos->registers[HYPE_CMOS_REG_HOURS] = (uint8_t)hour;
        cmos->registers[HYPE_CMOS_REG_DAY] = (uint8_t)day;
        cmos->registers[HYPE_CMOS_REG_MONTH] = (uint8_t)month;
        cmos->registers[HYPE_CMOS_REG_YEAR] = (uint8_t)(year % 100u);
        cmos->registers[HYPE_CMOS_REG_CENTURY] = (uint8_t)(year / 100u);
    } else {
        cmos->registers[HYPE_CMOS_REG_SECONDS] = bin_to_bcd(second);
        cmos->registers[HYPE_CMOS_REG_MINUTES] = bin_to_bcd(minute);
        cmos->registers[HYPE_CMOS_REG_HOURS] = bin_to_bcd(hour);
        cmos->registers[HYPE_CMOS_REG_DAY] = bin_to_bcd(day);
        cmos->registers[HYPE_CMOS_REG_MONTH] = bin_to_bcd(month);
        cmos->registers[HYPE_CMOS_REG_YEAR] = bin_to_bcd(year % 100u);
        cmos->registers[HYPE_CMOS_REG_CENTURY] = bin_to_bcd(year / 100u);
    }
    /* Day-of-week is 1-based with Sunday = 1. Nothing in this project's scope reads it,
     * but a zero there is another "invalid" a validator can trip on. */
    cmos->registers[HYPE_CMOS_REG_DAY_OF_WEEK] = 1u;
}

void hype_cmos_advance_to(hype_cmos_t *cmos, uint64_t elapsed_seconds) {
    hype_rtc_time_t base, now;

    if (cmos == 0 || !cmos->base_valid) {
        return;
    }
    base.year = cmos->base_year;
    base.month = cmos->base_month;
    base.day = cmos->base_day;
    base.hour = cmos->base_hour;
    base.minute = cmos->base_minute;
    base.second = cmos->base_second;
    hype_rtc_advance(&base, elapsed_seconds, &now);
    if (now.year == 0u) {
        /*
         * hype_rtc_advance() rejected the base. Unreachable from outside this file --
         * hype_cmos_set_time() range-checks before storing, so a stored base is always
         * valid -- and kept anyway because the alternative is writing zeros into the
         * guest's date registers, which is exactly the day-0/month-0 state #286 had to
         * fix. This is the one branch coverage cannot reach here (89.5% on this file).
         */
        return;
    }
    encode_time(cmos, now.year, now.month, now.day, now.hour, now.minute, now.second);
}

int hype_cmos_set_time(hype_cmos_t *cmos, unsigned int year, unsigned int month,
                       unsigned int day, unsigned int hour, unsigned int minute,
                       unsigned int second) {
    if (cmos == 0) {
        return -1;
    }
    /*
     * Refused rather than clamped. A month or day of 0 fails EDK2's RtcTimeFieldsValid()
     * and is what made a DEBUG OVMF dead-loop, so writing a nonsense date here would
     * reproduce the bug this function exists to fix -- and a silently corrected date is
     * worse than a caller that learns its clock read failed.
     */
    if (year < 1980u || year > 2099u || month < 1u || month > 12u || day < 1u || day > 31u ||
        hour > 23u || minute > 59u || second > 59u) {
        return -1;
    }
    encode_time(cmos, year, month, day, hour, minute, second);
    /* #304: keep the base so a later hype_cmos_advance_to() can re-encode without going
     * back to the host clock -- which an AP must never do (#229/#239). */
    cmos->base_year = (uint16_t)year;
    cmos->base_month = (uint8_t)month;
    cmos->base_day = (uint8_t)day;
    cmos->base_hour = (uint8_t)hour;
    cmos->base_minute = (uint8_t)minute;
    cmos->base_second = (uint8_t)second;
    cmos->base_valid = 1;
    return 0;
}

void hype_cmos_set_extended_memory_above_16mb(hype_cmos_t *cmos, uint16_t size_64kb_units) {
    cmos->registers[HYPE_CMOS_REG_EXTMEM_LOW] = (uint8_t)size_64kb_units;
    cmos->registers[HYPE_CMOS_REG_EXTMEM_HIGH] = (uint8_t)(size_64kb_units >> 8);
}

void hype_cmos_index_write(hype_cmos_t *cmos, uint8_t value) {
    cmos->index = value & (uint8_t)HYPE_CMOS_INDEX_MASK;
}

uint8_t hype_cmos_data_read(hype_cmos_t *cmos) {
    uint8_t value = cmos->registers[cmos->index];

    /* Register C is an acknowledge-on-read interrupt-status latch. Leaving
     * its flags set makes a guest that polls for the acknowledge condition
     * spin forever and prevents the next periodic IRQ from being raised. */
    if (cmos->index == HYPE_CMOS_REG_STATUS_C) {
        cmos->registers[cmos->index] = 0;
    }
    return value;
}

void hype_cmos_data_write(hype_cmos_t *cmos, uint8_t value) {
    /*
     * #286: some RTC status bits are READ-ONLY on real hardware, and treating the whole
     * register file as plain storage is what stopped a DEBUG OVMF from booting.
     *
     * EDK2's PcRtcInit() does exactly this, in this order:
     *
     *     RtcWrite (RTC_ADDRESS_REGISTER_D, PcdInitialValueRtcRegisterD);  // 0x00
     *     Status = RtcWaitToUpdate (...);   // requires RegisterD.Bits.Vrt != 0
     *
     * On a real MC146818 that write cannot clear VRT -- the bit reflects whether the CMOS
     * battery has kept the time valid, and it is read-only. With a read/write model the
     * firmware destroyed the very bit it was about to require, RtcWaitToUpdate returned
     * EFI_DEVICE_ERROR, and PcRtcEntry.c:181's ASSERT_EFI_ERROR dead-looped the DEBUG
     * build. A release build takes the same error and carries on -- so its RTC was quietly
     * broken too, and every guest EFI GetTime() was answering from a dead clock.
     */
    switch (cmos->index) {
        case HYPE_CMOS_REG_STATUS_A:
            /* UIP (bit 7) is set by the update cycle, not by software. Held clear: the
             * register file is only touched between guest accesses, so an update is never
             * genuinely in progress and a guest polling for it always makes progress. */
            cmos->registers[cmos->index] = (uint8_t)(value & 0x7Fu);
            return;
        case HYPE_CMOS_REG_STATUS_C:
            /* Interrupt-flags register: read-only, and cleared by reading. Nothing to
             * store, and letting a guest set flags would invent interrupts. */
            return;
        case HYPE_CMOS_REG_STATUS_D:
            /* VRT preserved; everything else is reserved and reads 0. */
            cmos->registers[cmos->index] =
                (uint8_t)(cmos->registers[cmos->index] & HYPE_CMOS_STATUS_D_RESET);
            return;
        default:
            cmos->registers[cmos->index] = value;
            return;
    }
}

uint32_t hype_cmos_periodic_hz(const hype_cmos_t *cmos) {
    /* Register A bits 3:0. The hardware's own table: 0 = disabled, 1 and 2 are
     * aliases for 256 Hz, and 3..15 run 8192 Hz down to 2 Hz by halving. */
    uint8_t rate = (uint8_t)(cmos->registers[HYPE_CMOS_REG_STATUS_A] & 0x0Fu);
    if (rate == 0u) {
        return 0u;
    }
    if (rate == 1u || rate == 2u) {
        return 256u;
    }
    return 32768u >> (rate - 1u);
}

int hype_cmos_advance(hype_cmos_t *cmos, uint64_t elapsed_ns) {
    uint32_t hz = hype_cmos_periodic_hz(cmos);
    uint64_t period_ns;

    if (hz == 0u || (cmos->registers[HYPE_CMOS_REG_STATUS_B] & HYPE_CMOS_STATUS_B_PIE) == 0u) {
        /* No rate selected, or the guest has not enabled the interrupt: time
         * still passes, but nothing is owed. Keep the accumulators from growing
         * without bound so enabling it later starts from now. */
        cmos->periodic_ns = 0;
        cmos->periodic_owed = 0;
        return 0;
    }
    period_ns = 1000000000ull / (uint64_t)hz;
    if (period_ns == 0u) {
        return 0;
    }
    cmos->periodic_ns += elapsed_ns;
    if (cmos->periodic_ns >= period_ns) {
        /* #94: every elapsed period is OWED an interrupt, not just the most
         * recent one. Windows uses this line as its clock and adds the period
         * to InterruptTime per interrupt received; the old drop-the-backlog
         * behaviour (periodic_ns %= period) made guest relative time run
         * measurably slow (RTCRATE: ~1730 delivered/s against a programmed
         * 2048 Hz), stretching every timeout until service starts and OOBE
         * fell over. Cap the backlog at one second's worth so a paused/wedged
         * guest gets a bounded catch-up burst, exactly like QEMU's RTC
         * reinjection. */
        uint64_t elapsed_periods = cmos->periodic_ns / period_ns;
        uint32_t cap = (hz > 0u) ? hz : 1u;
        cmos->periodic_ns -= elapsed_periods * period_ns;
        if (elapsed_periods > (uint64_t)cap) {
            elapsed_periods = cap;
        }
        if (cmos->periodic_owed > cap - (uint32_t)elapsed_periods) {
            cmos->periodic_owed = cap;
        } else {
            cmos->periodic_owed += (uint32_t)elapsed_periods;
        }
    }
    if (cmos->periodic_owed == 0u) {
        return 0;
    }
    if ((cmos->registers[HYPE_CMOS_REG_STATUS_C] & HYPE_CMOS_STATUS_C_IRQF) != 0u) {
        return 0; /* still asserted; the backlog drains as the guest acks */
    }
    cmos->periodic_owed--;
    cmos->registers[HYPE_CMOS_REG_STATUS_C] |=
        (uint8_t)(HYPE_CMOS_STATUS_C_PF | HYPE_CMOS_STATUS_C_IRQF);
    return 1;
}
