#include <stdio.h>
#include "../../arch/x86_64/cpu/pit.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_divisor_for_frequency(void) {
    CHECK_INT("1000Hz -> divisor 1193", 1193, hype_pit_divisor_for_frequency(1000));
    CHECK_INT("100Hz -> divisor 11931", 11931, hype_pit_divisor_for_frequency(100));
    CHECK_INT("0Hz is invalid", 0, hype_pit_divisor_for_frequency(0));
    CHECK_INT("18Hz needs a divisor > 65535, invalid", 0, hype_pit_divisor_for_frequency(18));
    CHECK_INT("frequency faster than the PIT's clock rounds to 0, invalid",
              0, hype_pit_divisor_for_frequency(2000000));
}

int main(void) {
    test_divisor_for_frequency();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
