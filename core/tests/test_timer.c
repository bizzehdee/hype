#include <stdio.h>
#include "../../arch/x86_64/cpu/timer.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_tick_increments(void) {
    uint64_t start = hype_timer_get_ticks();

    hype_timer_tick();
    CHECK_INT("one tick advances the count by 1", (long long)start + 1, hype_timer_get_ticks());

    hype_timer_tick();
    hype_timer_tick();
    CHECK_INT("three ticks total advance the count by 3", (long long)start + 3, hype_timer_get_ticks());
}

int main(void) {
    test_tick_increments();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
