#include <stdio.h>
#include "../io_histogram.h"

static int failures = 0;

#define CHECK_UINT(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected %llu, got %llu\n", (desc), (unsigned long long)(expected), \
                   (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* A small port space keeps the tests readable; production uses 0x10000. */
#define N 16u

static void test_record_counts_and_range(void) {
    uint32_t counts[N] = {0};

    hype_io_hist_record(counts, N, 3);
    hype_io_hist_record(counts, N, 3);
    hype_io_hist_record(counts, N, 7);
    /* out of range: ignored, no OOB write */
    hype_io_hist_record(counts, N, 16);
    hype_io_hist_record(counts, N, 999);

    CHECK_UINT("port 3 counted twice", 2, counts[3]);
    CHECK_UINT("port 7 counted once", 1, counts[7]);
    CHECK_UINT("untouched port stays 0", 0, counts[5]);
    CHECK_UINT("total is 3 (out-of-range ignored)", 3, hype_io_hist_total(counts, N));
}

static void test_record_saturates(void) {
    uint32_t counts[N] = {0};
    counts[2] = 0xFFFFFFFFu;

    hype_io_hist_record(counts, N, 2); /* must not wrap to 0 */

    CHECK_UINT("saturated counter stays at max", 0xFFFFFFFFu, counts[2]);
}

static void test_top_orders_descending_with_tie_break(void) {
    uint32_t counts[N] = {0};
    hype_io_hist_entry_t out[4];
    unsigned n;

    counts[1] = 5;
    counts[9] = 10;
    counts[4] = 10; /* tie with port 9 -> lower port (4) wins the tie */
    counts[2] = 1;

    n = hype_io_hist_top(counts, N, out, 4);

    CHECK_UINT("found 4 nonzero ports", 4, n);
    CHECK_UINT("rank0 port (tie -> lower port first)", 4, out[0].port);
    CHECK_UINT("rank0 count", 10, out[0].count);
    CHECK_UINT("rank1 port (tie -> higher port second)", 9, out[1].port);
    CHECK_UINT("rank1 count", 10, out[1].count);
    CHECK_UINT("rank2 port", 1, out[2].port);
    CHECK_UINT("rank2 count", 5, out[2].count);
    CHECK_UINT("rank3 port", 2, out[3].port);
    CHECK_UINT("rank3 count", 1, out[3].count);
}

static void test_top_caps_at_max_out_and_keeps_the_biggest(void) {
    uint32_t counts[N] = {0};
    hype_io_hist_entry_t out[3];
    unsigned n;

    counts[0] = 1;
    counts[1] = 2;
    counts[2] = 3;
    counts[3] = 4;
    counts[4] = 5; /* 5 nonzero ports, only top 3 requested */

    n = hype_io_hist_top(counts, N, out, 3);

    CHECK_UINT("capped at max_out=3", 3, n);
    CHECK_UINT("biggest is port 4 (count 5)", 4, out[0].port);
    CHECK_UINT("second is port 3 (count 4)", 3, out[1].port);
    CHECK_UINT("third is port 2 (count 3)", 2, out[2].port);
    /* ports 0 (count 1) and 1 (count 2) were the two smallest -> correctly
     * dropped: the smallest count that survived into the top-3 is 3. */
    CHECK_UINT("smallest surviving count is 3", 3, out[2].count);
}

static void test_top_skips_zero_and_handles_empty(void) {
    uint32_t counts[N] = {0};
    hype_io_hist_entry_t out[4];

    /* all zero -> nothing returned */
    CHECK_UINT("empty histogram returns 0", 0, hype_io_hist_top(counts, N, out, 4));

    counts[6] = 42;
    CHECK_UINT("only nonzero port returned", 1, hype_io_hist_top(counts, N, out, 4));
    CHECK_UINT("that port is 6", 6, out[0].port);
    CHECK_UINT("that count is 42", 42, out[0].count);

    /* max_out == 0 is a no-op */
    CHECK_UINT("max_out=0 returns 0", 0, hype_io_hist_top(counts, N, out, 0));
}

int main(void) {
    test_record_counts_and_range();
    test_record_saturates();
    test_top_orders_descending_with_tie_break();
    test_top_caps_at_max_out_and_keeps_the_biggest();
    test_top_skips_zero_and_handles_empty();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
