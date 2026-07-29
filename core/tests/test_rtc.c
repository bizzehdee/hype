#include <stdio.h>
#include "../rtc.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK(desc, cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", (desc)); \
            failures++; \
        } \
    } while (0)

/* 24-hour, BCD -- the configuration essentially every PC RTC ships in. */
#define SB_BCD_24H HYPE_RTC_STATUS_B_24H
/* 24-hour, binary. */
#define SB_BIN_24H (HYPE_RTC_STATUS_B_24H | HYPE_RTC_STATUS_B_BINARY)
/* 12-hour, BCD. */
#define SB_BCD_12H 0u

static void test_bcd_to_bin(void) {
    CHECK_HEX("bcd 0x00", 0, hype_rtc_bcd_to_bin(0x00u));
    CHECK_HEX("bcd 0x09", 9, hype_rtc_bcd_to_bin(0x09u));
    CHECK_HEX("bcd 0x10", 10, hype_rtc_bcd_to_bin(0x10u));
    CHECK_HEX("bcd 0x59", 59, hype_rtc_bcd_to_bin(0x59u));
    CHECK_HEX("bcd 0x99", 99, hype_rtc_bcd_to_bin(0x99u));
}

static void test_decode_bcd_24h(void) {
    hype_rtc_time_t t;
    /* 2026-07-28 14:35:07, century register present. */
    CHECK_HEX("decode bcd/24h ok", 0,
              hype_rtc_decode(0x07u, 0x35u, 0x14u, 0x28u, 0x07u, 0x26u, 0x20u, SB_BCD_24H, &t));
    CHECK_HEX("year", 2026, t.year);
    CHECK_HEX("month", 7, t.month);
    CHECK_HEX("day", 28, t.day);
    CHECK_HEX("hour", 14, t.hour);
    CHECK_HEX("minute", 35, t.minute);
    CHECK_HEX("second", 7, t.second);
}

static void test_decode_binary_mode(void) {
    hype_rtc_time_t t;
    /* Same instant, binary registers -- values are NOT BCD here, so 20/7/26
     * are literal. A decoder that converted anyway would produce 14/7/26 -> 2014. */
    CHECK_HEX("decode binary ok", 0,
              hype_rtc_decode(7u, 35u, 14u, 28u, 7u, 26u, 20u, SB_BIN_24H, &t));
    CHECK_HEX("binary year", 2026, t.year);
    CHECK_HEX("binary hour", 14, t.hour);
    CHECK_HEX("binary second", 7, t.second);
}

/*
 * 12-hour mode. Bit 7 of the hour register is PM and must be stripped BEFORE BCD
 * conversion -- 0x80 is not valid BCD, so converting first corrupts the hour.
 * The 12am/12pm cases are the ones implementations get wrong.
 */
static void test_decode_12h(void) {
    hype_rtc_time_t t;

    /* 02:30 PM -> 14:30. Hour register = 0x02 | 0x80. */
    CHECK_HEX("12h 2pm ok", 0,
              hype_rtc_decode(0x00u, 0x30u, 0x82u, 0x01u, 0x06u, 0x26u, 0x20u, SB_BCD_12H, &t));
    CHECK_HEX("12h 2pm -> 14", 14, t.hour);

    /* 02:30 AM stays 02:30. */
    CHECK_HEX("12h 2am ok", 0,
              hype_rtc_decode(0x00u, 0x30u, 0x02u, 0x01u, 0x06u, 0x26u, 0x20u, SB_BCD_12H, &t));
    CHECK_HEX("12h 2am -> 2", 2, t.hour);

    /* 12am is stored as 12 and means hour 0 -- NOT 12, and not 24. */
    CHECK_HEX("12h 12am ok", 0,
              hype_rtc_decode(0x00u, 0x00u, 0x12u, 0x01u, 0x06u, 0x26u, 0x20u, SB_BCD_12H, &t));
    CHECK_HEX("12h 12am -> 0", 0, t.hour);

    /* 12pm is stored as 12 with PM set and stays 12 -- must not become 24. */
    CHECK_HEX("12h 12pm ok", 0,
              hype_rtc_decode(0x00u, 0x00u, 0x92u, 0x01u, 0x06u, 0x26u, 0x20u, SB_BCD_12H, &t));
    CHECK_HEX("12h 12pm -> 12", 12, t.hour);

    /* 11pm -> 23, the largest valid result. */
    CHECK_HEX("12h 11pm ok", 0,
              hype_rtc_decode(0x00u, 0x00u, 0x91u, 0x01u, 0x06u, 0x26u, 0x20u, SB_BCD_12H, &t));
    CHECK_HEX("12h 11pm -> 23", 23, t.hour);
}

static void test_decode_century(void) {
    hype_rtc_time_t t;
    /* Century register 0x19 (BCD 19) -> 1999. */
    CHECK_HEX("century 19 ok", 0,
              hype_rtc_decode(0x00u, 0x00u, 0x12u, 0x31u, 0x12u, 0x99u, 0x19u, SB_BCD_24H, &t));
    CHECK_HEX("century 19 -> 1999", 1999, t.year);
    /* No century register: a 2-digit year is windowed into 2000..2099. */
    CHECK_HEX("no century ok", 0,
              hype_rtc_decode(0x00u, 0x00u, 0x12u, 0x01u, 0x01u, 0x26u, 0x00u, SB_BCD_24H, &t));
    CHECK_HEX("no century -> 2026", 2026, t.year);
    /* A nonsense century is ignored rather than trusted. */
    CHECK_HEX("bogus century ok", 0,
              hype_rtc_decode(0x00u, 0x00u, 0x12u, 0x01u, 0x01u, 0x26u, 0x77u, SB_BCD_24H, &t));
    CHECK_HEX("bogus century -> windowed", 2026, t.year);
}

/*
 * The case that matters most: a dead or never-set RTC reads as all zeroes.
 * Month 0 / day 0 must be REJECTED, not passed to a filesystem encoder -- both
 * FAT and exFAT treat those fields as 1-based, and writing them is precisely
 * the bug this module exists to fix.
 */
static void test_decode_rejects_unset_clock(void) {
    hype_rtc_time_t t;
    CHECK("all-zero registers rejected",
          hype_rtc_decode(0, 0, 0, 0, 0, 0, 0, SB_BCD_24H, &t) != 0);
    CHECK("month 0 rejected",
          hype_rtc_decode(0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x26u, 0x20u, SB_BCD_24H, &t) != 0);
    CHECK("day 0 rejected",
          hype_rtc_decode(0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x26u, 0x20u, SB_BCD_24H, &t) != 0);
    CHECK("month 13 rejected",
          hype_rtc_decode(0x00u, 0x00u, 0x00u, 0x01u, 0x13u, 0x26u, 0x20u, SB_BCD_24H, &t) != 0);
    CHECK("hour 24 rejected",
          hype_rtc_decode(0x00u, 0x00u, 0x24u, 0x01u, 0x01u, 0x26u, 0x20u, SB_BCD_24H, &t) != 0);
    CHECK("second 60 rejected",
          hype_rtc_decode(0x60u, 0x00u, 0x00u, 0x01u, 0x01u, 0x26u, 0x20u, SB_BCD_24H, &t) != 0);
    CHECK("null out rejected", hype_rtc_decode(0, 0, 0, 1, 1, 0x26u, 0x20u, SB_BCD_24H, 0) != 0);
}

static void set(hype_rtc_time_t *t, uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi,
                uint8_t s) {
    t->year = y;
    t->month = mo;
    t->day = d;
    t->hour = h;
    t->minute = mi;
    t->second = s;
}

static void test_time_valid_bounds(void) {
    hype_rtc_time_t t;

    CHECK("null invalid", !hype_rtc_time_valid(0));

    /* 1980 is the epoch for both filesystems; 2107 is the last year FAT's 7-bit
     * year field can express (1980 + 127). */
    set(&t, 1979, 12, 31, 0, 0, 0);
    CHECK("1979 invalid", !hype_rtc_time_valid(&t));
    set(&t, 1980, 1, 1, 0, 0, 0);
    CHECK("1980-01-01 valid", hype_rtc_time_valid(&t));
    set(&t, 2107, 12, 31, 23, 59, 59);
    CHECK("2107 valid", hype_rtc_time_valid(&t));
    set(&t, 2108, 1, 1, 0, 0, 0);
    CHECK("2108 invalid", !hype_rtc_time_valid(&t));

    /* Per-month day limits. */
    set(&t, 2026, 4, 30, 0, 0, 0);
    CHECK("apr 30 valid", hype_rtc_time_valid(&t));
    set(&t, 2026, 4, 31, 0, 0, 0);
    CHECK("apr 31 invalid", !hype_rtc_time_valid(&t));

    /* Leap years: 2024 and 2000 have Feb 29; 2026 and 2100 do not. */
    set(&t, 2024, 2, 29, 0, 0, 0);
    CHECK("2024-02-29 valid (div by 4)", hype_rtc_time_valid(&t));
    set(&t, 2026, 2, 29, 0, 0, 0);
    CHECK("2026-02-29 invalid", !hype_rtc_time_valid(&t));
    set(&t, 2000, 2, 29, 0, 0, 0);
    CHECK("2000-02-29 valid (div by 400)", hype_rtc_time_valid(&t));
    set(&t, 2100, 2, 29, 0, 0, 0);
    CHECK("2100-02-29 invalid (div by 100, not 400)", !hype_rtc_time_valid(&t));
}

static void test_fat_encoding(void) {
    hype_rtc_time_t t;

    /* 2026-07-28 14:35:06 */
    set(&t, 2026, 7, 28, 14, 35, 6);
    /* date = (2026-1980)<<9 | 7<<5 | 28 = 46<<9 | 224 | 28 */
    CHECK_HEX("fat date", (46u << 9) | (7u << 5) | 28u, hype_fat_encode_date(&t));
    /* time = 14<<11 | 35<<5 | 3 */
    CHECK_HEX("fat time", (14u << 11) | (35u << 5) | 3u, hype_fat_encode_time(&t));
    CHECK_HEX("fat tenths even second", 0, hype_fat_encode_time_tenths(&t));

    /* An odd second is lost by the 2-second time field and carried in tenths. */
    set(&t, 2026, 7, 28, 14, 35, 7);
    CHECK_HEX("fat time odd second truncates", (14u << 11) | (35u << 5) | 3u,
              hype_fat_encode_time(&t));
    CHECK_HEX("fat tenths odd second", 100, hype_fat_encode_time_tenths(&t));

    /* The epoch encodes as the smallest legal date, not as zero. */
    set(&t, 1980, 1, 1, 0, 0, 0);
    CHECK_HEX("fat date epoch", (0u << 9) | (1u << 5) | 1u, hype_fat_encode_date(&t));
    CHECK_HEX("fat time midnight", 0, hype_fat_encode_time(&t));

    /* An invalid time yields 0 -- the same "unset" entry as before the RTC
     * existed, rather than a confidently wrong date. */
    set(&t, 1979, 1, 1, 0, 0, 0);
    CHECK_HEX("fat date invalid -> 0", 0, hype_fat_encode_date(&t));
    CHECK_HEX("fat time invalid -> 0", 0, hype_fat_encode_time(&t));
    CHECK_HEX("fat tenths invalid -> 0", 0, hype_fat_encode_time_tenths(&t));
    CHECK_HEX("fat date null -> 0", 0, hype_fat_encode_date(0));
    CHECK_HEX("fat time null -> 0", 0, hype_fat_encode_time(0));
    CHECK_HEX("fat tenths null -> 0", 0, hype_fat_encode_time_tenths(0));
}

static void test_exfat_encoding(void) {
    hype_rtc_time_t t;

    set(&t, 2026, 7, 28, 14, 35, 6);
    CHECK_HEX("exfat timestamp",
              ((uint32_t)(2026u - 1980u) << 25) | ((uint32_t)7u << 21) | ((uint32_t)28u << 16) |
                  ((uint32_t)14u << 11) | ((uint32_t)35u << 5) | 3u,
              hype_exfat_encode_timestamp(&t));

    /* 1980-01-01 00:00:00 must equal the epoch constant the writer used before. */
    set(&t, 1980, 1, 1, 0, 0, 0);
    CHECK_HEX("exfat epoch matches HYPE_EXFAT_TIMESTAMP_EPOCH", 0x00210000u,
              hype_exfat_encode_timestamp(&t));

    /*
     * The load-bearing difference from FAT: an invalid time must fall back to the
     * EPOCH, never to zero. exFAT's month and day fields are 1-based, so an
     * all-zero timestamp is out of spec and trips fsck.
     */
    set(&t, 1979, 1, 1, 0, 0, 0);
    CHECK_HEX("exfat invalid -> epoch, not zero", 0x00210000u, hype_exfat_encode_timestamp(&t));
    CHECK_HEX("exfat null -> epoch, not zero", 0x00210000u, hype_exfat_encode_timestamp(0));
    CHECK("exfat fallback is never zero", hype_exfat_encode_timestamp(0) != 0u);
}

/* A decoded RTC reading must survive round-tripping into both encodings. */
static void test_decode_then_encode(void) {
    hype_rtc_time_t t;
    CHECK_HEX("round-trip decode ok", 0,
              hype_rtc_decode(0x06u, 0x35u, 0x14u, 0x28u, 0x07u, 0x26u, 0x20u, SB_BCD_24H, &t));
    CHECK_HEX("round-trip fat date", (46u << 9) | (7u << 5) | 28u, hype_fat_encode_date(&t));
    CHECK("round-trip exfat nonzero", hype_exfat_encode_timestamp(&t) != 0u);
    CHECK("round-trip exfat not epoch", hype_exfat_encode_timestamp(&t) != 0x00210000u);
}


/* ---- #253: the 10ms increment and the TSC-driven advance ---- */

static void test_exfat_10ms(void) {
    hype_rtc_time_t t;
    t.year = 2026; t.month = 7; t.day = 29; t.hour = 10; t.minute = 0; t.second = 41;
    CHECK_HEX("odd second -> 100 x 10ms", 100u, hype_exfat_encode_10ms(&t));
    t.second = 40;
    CHECK_HEX("even second -> 0", 0u, hype_exfat_encode_10ms(&t));
    t.month = 0;
    CHECK_HEX("invalid time -> 0", 0u, hype_exfat_encode_10ms(&t));
}

static void check_time(const char *what, const hype_rtc_time_t *t, unsigned y, unsigned mo,
                       unsigned d, unsigned h, unsigned mi, unsigned se) {
    char desc[96];
    snprintf(desc, sizeof desc, "%s (y)", what); CHECK_HEX(desc, y, t->year);
    snprintf(desc, sizeof desc, "%s (mo)", what); CHECK_HEX(desc, mo, t->month);
    snprintf(desc, sizeof desc, "%s (d)", what); CHECK_HEX(desc, d, t->day);
    snprintf(desc, sizeof desc, "%s (h)", what); CHECK_HEX(desc, h, t->hour);
    snprintf(desc, sizeof desc, "%s (mi)", what); CHECK_HEX(desc, mi, t->minute);
    snprintf(desc, sizeof desc, "%s (s)", what); CHECK_HEX(desc, se, t->second);
}

static void test_advance(void) {
    hype_rtc_time_t base, out;
    base.year = 2026; base.month = 7; base.day = 29;
    base.hour = 10; base.minute = 58; base.second = 30;

    hype_rtc_advance(&base, 0u, &out);
    check_time("advance by 0", &out, 2026, 7, 29, 10, 58, 30);
    hype_rtc_advance(&base, 29u, &out);
    check_time("within the minute", &out, 2026, 7, 29, 10, 58, 59);
    hype_rtc_advance(&base, 30u, &out);
    check_time("minute rollover", &out, 2026, 7, 29, 10, 59, 0);
    hype_rtc_advance(&base, 90u + 3600u, &out);
    check_time("hour rollover", &out, 2026, 7, 29, 12, 0, 0);

    /* Midnight, month end, year end. */
    base.hour = 23; base.minute = 59; base.second = 59;
    hype_rtc_advance(&base, 1u, &out);
    check_time("midnight rollover", &out, 2026, 7, 30, 0, 0, 0);
    base.day = 31;
    hype_rtc_advance(&base, 1u, &out);
    check_time("month rollover", &out, 2026, 8, 1, 0, 0, 0);
    base.month = 12;
    hype_rtc_advance(&base, 1u, &out);
    check_time("year rollover", &out, 2027, 1, 1, 0, 0, 0);

    /* Leap handling: 2028-02-28 has a 29th; 2027 does not; 2100 is NOT a
     * leap year (divisible by 100, not by 400). */
    base.year = 2028; base.month = 2; base.day = 28;
    base.hour = 12; base.minute = 0; base.second = 0;
    hype_rtc_advance(&base, 86400u, &out);
    check_time("into Feb 29", &out, 2028, 2, 29, 12, 0, 0);
    hype_rtc_advance(&base, 2u * 86400u, &out);
    check_time("across Feb 29", &out, 2028, 3, 1, 12, 0, 0);
    base.year = 2027;
    hype_rtc_advance(&base, 86400u, &out);
    check_time("non-leap Feb 28 + 1d", &out, 2027, 3, 1, 12, 0, 0);
    base.year = 2100; /* century non-leap */
    hype_rtc_advance(&base, 86400u, &out);
    check_time("2100 is not a leap year", &out, 2100, 3, 1, 12, 0, 0);

    /* A long uptime: 400 days from mid-2026 crosses a leap boundary region. */
    base.year = 2026; base.month = 7; base.day = 29;
    base.hour = 10; base.minute = 0; base.second = 0;
    hype_rtc_advance(&base, 400ull * 86400u, &out);
    check_time("400 days later", &out, 2027, 9, 2, 10, 0, 0);

    /* An invalid base yields an invalid (all-zero) result, never a plausible
     * fake -- the encoders then fall back to their unset behaviour. */
    base.month = 0;
    hype_rtc_advance(&base, 5u, &out);
    check_time("invalid base zeroed", &out, 0, 0, 0, 0, 0, 0);
}

/* The 1980 epoch boundary and the FAT year ceiling, both ways. */
static void test_epoch_bounds(void) {
    hype_rtc_time_t t;
    t.year = 1980; t.month = 1; t.day = 1; t.hour = 0; t.minute = 0; t.second = 0;
    CHECK_HEX("exFAT epoch encodes as the epoch constant", 0x00210000u,
              hype_exfat_encode_timestamp(&t));
    CHECK_HEX("FAT epoch date", (0u << 9) | (1u << 5) | 1u, hype_fat_encode_date(&t));
    t.year = 1979; t.month = 12; t.day = 31;
    CHECK_HEX("pre-epoch is invalid (exFAT falls back to the epoch)", 0x00210000u,
              hype_exfat_encode_timestamp(&t));
    CHECK_HEX("pre-epoch FAT date is unset", 0u, hype_fat_encode_date(&t));
    t.year = 2107; t.month = 12; t.day = 31; t.hour = 23; t.minute = 59; t.second = 58;
    CHECK("ceiling year encodes", hype_fat_encode_date(&t) != 0u);
    CHECK_HEX("ceiling year field", 127u, (unsigned)(hype_fat_encode_date(&t) >> 9));
    t.year = 2108;
    CHECK_HEX("past the 7-bit year is unset", 0u, hype_fat_encode_date(&t));
}

int main(void) {
    test_bcd_to_bin();
    test_decode_bcd_24h();
    test_decode_binary_mode();
    test_decode_12h();
    test_decode_century();
    test_decode_rejects_unset_clock();
    test_time_valid_bounds();
    test_fat_encoding();
    test_exfat_encoding();
    test_decode_then_encode();
    test_exfat_10ms();
    test_advance();
    test_epoch_bounds();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
