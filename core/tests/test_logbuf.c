#include <stdio.h>
#include <string.h>

#include "../logbuf.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual)                                                    \
    do {                                                                                     \
        long long e_ = (long long)(expected), a_ = (long long)(actual);                      \
        if (e_ != a_) {                                                                      \
            printf("FAIL: %s (expected %lld, got %lld)\n", (desc), e_, a_);                  \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static void test_append_accumulates_in_order(void) {
    hype_logbuf_reset();
    hype_logbuf_append("hello ");
    hype_logbuf_append("world\n");
    CHECK_INT("len is sum of appends", 12, hype_logbuf_len());
    CHECK_INT("not truncated", 0, hype_logbuf_truncated());
    CHECK_INT("content matches", 0,
              memcmp(hype_logbuf_data(), "hello world\n", 12));
}

static void test_reset_clears(void) {
    hype_logbuf_append("stuff");
    hype_logbuf_reset();
    CHECK_INT("len 0 after reset", 0, hype_logbuf_len());
    CHECK_INT("not truncated after reset", 0, hype_logbuf_truncated());
}

static void test_null_append_is_noop(void) {
    hype_logbuf_reset();
    hype_logbuf_append(0);
    CHECK_INT("NULL append leaves len 0", 0, hype_logbuf_len());
}

static void test_truncates_at_capacity(void) {
    /* Fill exactly to capacity, then one more byte must be dropped and
     * latch the truncated flag; the retained content stays intact. */
    static char big[HYPE_LOGBUF_CAPACITY + 16];
    unsigned int i;
    hype_logbuf_reset();
    for (i = 0; i < sizeof(big) - 1; i++) {
        big[i] = 'A';
    }
    big[sizeof(big) - 1] = '\0';
    hype_logbuf_append(big);
    CHECK_INT("len capped at capacity", (long long)HYPE_LOGBUF_CAPACITY, hype_logbuf_len());
    CHECK_INT("truncated flag latched", 1, hype_logbuf_truncated());
    CHECK_INT("first retained byte intact", 'A', hype_logbuf_data()[0]);
    CHECK_INT("last retained byte intact", 'A', hype_logbuf_data()[HYPE_LOGBUF_CAPACITY - 1]);
    /* A further append stays dropped. */
    hype_logbuf_append("x");
    CHECK_INT("still capped after extra append", (long long)HYPE_LOGBUF_CAPACITY, hype_logbuf_len());
}

int main(void) {
    test_append_accumulates_in_order();
    test_reset_clears();
    test_null_append_is_noop();
    test_truncates_at_capacity();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
