#include "rtc.h"

/* Days per month, non-leap. February is fixed up by the caller for leap years. */
static const uint8_t g_days_in_month[12] = {31u, 28u, 31u, 30u, 31u, 30u,
                                            31u, 31u, 30u, 31u, 30u, 31u};

static int is_leap(uint16_t y) {
    if ((y % 4u) != 0u) {
        return 0;
    }
    if ((y % 100u) != 0u) {
        return 1;
    }
    return (y % 400u) == 0u;
}

uint8_t hype_rtc_bcd_to_bin(uint8_t v) {
    return (uint8_t)(((v >> 4) * 10u) + (v & 0x0Fu));
}

int hype_rtc_time_valid(const hype_rtc_time_t *t) {
    uint8_t dim;

    if (t == 0) {
        return 0;
    }
    /* 1980 is the floor for both FAT and exFAT; 2107 is the last year FAT's
     * 7-bit year field can hold (1980 + 127). */
    if (t->year < 1980u || t->year > 2107u) {
        return 0;
    }
    if (t->month < 1u || t->month > 12u) {
        return 0;
    }
    dim = g_days_in_month[t->month - 1u];
    if (t->month == 2u && is_leap(t->year)) {
        dim = 29u;
    }
    if (t->day < 1u || t->day > dim) {
        return 0;
    }
    /* Hour 24 is not valid; a leap second (60) is not encodable either. */
    if (t->hour > 23u || t->minute > 59u || t->second > 59u) {
        return 0;
    }
    return 1;
}

int hype_rtc_decode(uint8_t sec, uint8_t min, uint8_t hour, uint8_t day, uint8_t month,
                    uint8_t year, uint8_t century, uint8_t status_b, hype_rtc_time_t *out) {
    int pm = 0;
    uint16_t full_year;

    if (out == 0) {
        return -1;
    }

    /* In 12-hour mode bit 7 of the hour register is PM. Strip it BEFORE any BCD
     * conversion -- 0x80 is not valid BCD and converting first would corrupt
     * the hour. */
    if ((status_b & HYPE_RTC_STATUS_B_24H) == 0u) {
        pm = (hour & 0x80u) != 0u;
        hour &= 0x7Fu;
    }

    if ((status_b & HYPE_RTC_STATUS_B_BINARY) == 0u) {
        sec = hype_rtc_bcd_to_bin(sec);
        min = hype_rtc_bcd_to_bin(min);
        hour = hype_rtc_bcd_to_bin(hour);
        day = hype_rtc_bcd_to_bin(day);
        month = hype_rtc_bcd_to_bin(month);
        year = hype_rtc_bcd_to_bin(year);
        if (century != 0u) {
            century = hype_rtc_bcd_to_bin(century);
        }
    }

    if ((status_b & HYPE_RTC_STATUS_B_24H) == 0u) {
        /* 12-hour: 12am is stored as 12 and means 0; 12pm is stored as 12 and
         * means 12; otherwise PM adds 12. */
        if (hour == 12u) {
            hour = pm ? 12u : 0u;
        } else if (pm) {
            hour = (uint8_t)(hour + 12u);
        }
    }

    if (century >= 19u && century <= 21u) {
        full_year = (uint16_t)((uint16_t)century * 100u + (uint16_t)year);
    } else {
        /* No usable century register: window a two-digit year into 2000..2099.
         * See the header for why this window is deliberate. */
        full_year = (uint16_t)(2000u + (uint16_t)year);
    }

    out->year = full_year;
    out->month = month;
    out->day = day;
    out->hour = hour;
    out->minute = min;
    out->second = sec;

    /* A dead or never-set RTC commonly reads as all zeroes. Rejecting it here is
     * the whole point: month 0 / day 0 must never reach a filesystem encoder,
     * because both FAT and exFAT treat those fields as 1-based and readers
     * render the result as garbage (which is exactly the bug this fixes). */
    return hype_rtc_time_valid(out) ? 0 : -1;
}

uint16_t hype_fat_encode_date(const hype_rtc_time_t *t) {
    if (!hype_rtc_time_valid(t)) {
        return 0u;
    }
    return (uint16_t)(((uint16_t)(t->year - 1980u) << 9) | ((uint16_t)t->month << 5) |
                      (uint16_t)t->day);
}

uint16_t hype_fat_encode_time(const hype_rtc_time_t *t) {
    if (!hype_rtc_time_valid(t)) {
        return 0u;
    }
    return (uint16_t)(((uint16_t)t->hour << 11) | ((uint16_t)t->minute << 5) |
                      (uint16_t)(t->second / 2u));
}

uint8_t hype_fat_encode_time_tenths(const hype_rtc_time_t *t) {
    if (!hype_rtc_time_valid(t)) {
        return 0u;
    }
    /* The time field holds second/2, losing the odd second. CrtTimeTenth carries
     * it back as 100 tenths. The RTC has no sub-second resolution, so the
     * fractional part is always 0. */
    return (uint8_t)((t->second & 1u) ? 100u : 0u);
}

uint8_t hype_exfat_encode_10ms(const hype_rtc_time_t *t) {
    if (!hype_rtc_time_valid(t)) {
        return 0u;
    }
    /* Same job as hype_fat_encode_time_tenths, in exFAT's 10ms unit. */
    return (uint8_t)((t->second & 1u) ? 100u : 0u);
}

static uint8_t month_days(uint16_t y, uint8_t m) {
    if (m == 2u && is_leap(y)) {
        return 29u;
    }
    return g_days_in_month[m - 1u];
}

void hype_rtc_advance(const hype_rtc_time_t *base, uint64_t seconds, hype_rtc_time_t *out) {
    uint64_t s;
    if (!hype_rtc_time_valid(base)) {
        out->year = 0;
        out->month = 0;
        out->day = 0;
        out->hour = 0;
        out->minute = 0;
        out->second = 0;
        return;
    }
    out->year = base->year;
    out->month = base->month;
    out->day = base->day;
    out->hour = base->hour;
    out->minute = base->minute;
    out->second = base->second;

    /* Whole days first (cheap division), then the sub-day remainder. */
    s = seconds % 86400u;
    out->second = (uint8_t)(out->second + s % 60u);
    s /= 60u;
    out->minute = (uint8_t)(out->minute + s % 60u);
    out->hour = (uint8_t)(out->hour + s / 60u);
    if (out->second >= 60u) {
        out->second -= 60u;
        out->minute++;
    }
    if (out->minute >= 60u) {
        out->minute -= 60u;
        out->hour++;
    }
    {
        uint64_t days = seconds / 86400u;
        if (out->hour >= 24u) {
            out->hour -= 24u;
            days++;
        }
        while (days > 0u) {
            uint8_t md = month_days(out->year, out->month);
            if (out->day < md) {
                uint64_t left = (uint64_t)(md - out->day);
                uint64_t step = (days < left) ? days : left;
                out->day = (uint8_t)(out->day + step);
                days -= step;
            } else {
                out->day = 1u;
                out->month++;
                if (out->month > 12u) {
                    out->month = 1u;
                    out->year++;
                }
                days--;
            }
        }
    }
}

uint32_t hype_rtc_to_unix(const hype_rtc_time_t *t) {
    uint32_t y, m, era, yoe, doy, doe;
    uint64_t days;

    if (!hype_rtc_time_valid(t)) {
        return 0u;
    }
    /* days_from_civil: shift so March is month 0 of a "computing year" that
     * starts on 1 March -- puts the messy Feb-29 leap day at the END of the
     * cycle, which is what makes the era/yoe division exact. */
    y = t->year;
    m = t->month;
    if (m <= 2u) {
        y--;
    }
    era = y / 400u;
    yoe = y - era * 400u; /* 0..399 */
    doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + t->day - 1u; /* 0..365 */
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;                   /* 0..146096 */
    days = (uint64_t)era * 146097u + doe - 719468u; /* shift to a 1970-01-01 epoch */

    return (uint32_t)(days * 86400u + (uint32_t)t->hour * 3600u + (uint32_t)t->minute * 60u +
                      t->second);
}

uint32_t hype_exfat_encode_timestamp(const hype_rtc_time_t *t) {
    if (!hype_rtc_time_valid(t)) {
        /* 1980-01-01 00:00:00 -- month and day are 1-based in exFAT, so zero is
         * out of spec and trips fsck. Matches HYPE_EXFAT_TIMESTAMP_EPOCH. */
        return 0x00210000u;
    }
    return ((uint32_t)(t->year - 1980u) << 25) | ((uint32_t)t->month << 21) |
           ((uint32_t)t->day << 16) | ((uint32_t)t->hour << 11) | ((uint32_t)t->minute << 5) |
           ((uint32_t)(t->second / 2u));
}
