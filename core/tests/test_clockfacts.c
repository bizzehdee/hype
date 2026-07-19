#include <stdio.h>
#include <string.h>
#include "../clockfacts.h"

static int failures = 0;

#define CHECK(desc, cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", (desc)); \
            failures++; \
        } \
    } while (0)

static void test_reset_empties(void) {
    hype_clockfacts_t cf;
    cf.len = 999;
    cf.buf[0] = 'x';
    hype_clockfacts_reset(&cf);
    CHECK("reset zeroes len", cf.len == 0);
    CHECK("reset NUL-terminates", cf.buf[0] == '\0');
}

static void test_captures_lpj_line(void) {
    hype_clockfacts_t cf;
    hype_clockfacts_reset(&cf);
    int r = hype_clockfacts_observe(&cf,
        "Calibrating delay loop (skipped), value calculated using timer frequency.. 8000.00 BogoMIPS (lpj=16000000)");
    CHECK("lpj line matched", r == 1);
    CHECK("lpj text present", strstr(cf.buf, "lpj=16000000") != NULL);
}

static void test_captures_clocksource_switch(void) {
    hype_clockfacts_t cf;
    hype_clockfacts_reset(&cf);
    int r = hype_clockfacts_observe(&cf, "clocksource: Switched to clocksource tsc");
    CHECK("clocksource switch matched", r == 1);
    CHECK("clocksource text present", strstr(cf.buf, "tsc") != NULL);
}

static void test_captures_tsc_unstable_and_detected(void) {
    hype_clockfacts_t cf;
    hype_clockfacts_reset(&cf);
    CHECK("Marking TSC unstable matched",
          hype_clockfacts_observe(&cf, "clocksource: timekeeping watchdog: Marking TSC unstable") == 1);
    CHECK("tsc: Detected matched",
          hype_clockfacts_observe(&cf, "tsc: Detected 2994.000 MHz processor") == 1);
    CHECK("Refined jiffies matched",
          hype_clockfacts_observe(&cf, "clocksource: Refined jiffies-based generic timekeeping") == 1);
}

static void test_non_clock_line_ignored(void) {
    hype_clockfacts_t cf;
    hype_clockfacts_reset(&cf);
    int r = hype_clockfacts_observe(&cf, "random: crng init done");
    CHECK("unrelated line not matched", r == 0);
    CHECK("buffer stays empty", cf.len == 0);
}

static void test_duplicate_line_ignored(void) {
    hype_clockfacts_t cf;
    hype_clockfacts_reset(&cf);
    CHECK("first capture", hype_clockfacts_observe(&cf, "clocksource: Switched to clocksource tsc") == 1);
    unsigned int len_after_first = cf.len;
    CHECK("duplicate returns 0", hype_clockfacts_observe(&cf, "clocksource: Switched to clocksource tsc") == 0);
    CHECK("duplicate does not grow buffer", cf.len == len_after_first);
}

static void test_multiple_lines_joined(void) {
    hype_clockfacts_t cf;
    hype_clockfacts_reset(&cf);
    hype_clockfacts_observe(&cf, "8000.00 BogoMIPS (lpj=16000000)");
    hype_clockfacts_observe(&cf, "clocksource: Switched to clocksource tsc");
    CHECK("both present", strstr(cf.buf, "lpj=16000000") && strstr(cf.buf, "Switched to clocksource"));
    CHECK("joined by separator", strstr(cf.buf, " | ") != NULL);
}

static void test_empty_line_ignored(void) {
    hype_clockfacts_t cf;
    hype_clockfacts_reset(&cf);
    /* empty string can't match any (non-empty) key */
    CHECK("empty line returns 0", hype_clockfacts_observe(&cf, "") == 0);
}

static void test_truncates_never_overflows(void) {
    hype_clockfacts_t cf;
    hype_clockfacts_reset(&cf);
    /* Build a long lpj line that exceeds the buffer capacity. */
    char big[HYPE_CLOCKFACTS_CAP + 200];
    int p = 0;
    p += sprintf(big + p, "lpj=");
    while (p < (int)sizeof(big) - 1) {
        big[p++] = '9';
    }
    big[p] = '\0';
    int r = hype_clockfacts_observe(&cf, big);
    CHECK("oversized line still captured (truncated)", r == 1);
    CHECK("buffer never exceeds cap", cf.len < HYPE_CLOCKFACTS_CAP);
    CHECK("buffer NUL-terminated", cf.buf[cf.len] == '\0');
}

static void test_no_room_for_second_entry(void) {
    hype_clockfacts_t cf;
    hype_clockfacts_reset(&cf);
    /* Fill the buffer with one big truncated entry, then a second must be
     * rejected (not even the separator fits / no avail). */
    char big[HYPE_CLOCKFACTS_CAP + 50];
    int p = sprintf(big, "lpj=");
    while (p < (int)sizeof(big) - 1) {
        big[p++] = '7';
    }
    big[p] = '\0';
    hype_clockfacts_observe(&cf, big);
    int r = hype_clockfacts_observe(&cf, "clocksource: Switched to clocksource tsc");
    CHECK("second entry rejected when full", r == 0);
    CHECK("buffer still within cap", cf.len < HYPE_CLOCKFACTS_CAP);
}

int main(void) {
    test_reset_empties();
    test_captures_lpj_line();
    test_captures_clocksource_switch();
    test_captures_tsc_unstable_and_detected();
    test_non_clock_line_ignored();
    test_duplicate_line_ignored();
    test_multiple_lines_joined();
    test_empty_line_ignored();
    test_truncates_never_overflows();
    test_no_room_for_second_entry();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
