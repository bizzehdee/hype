#include <stdio.h>
#include "../guest_ram.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), \
                   (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_zeroes_entire_range(void) {
    uint8_t buf[64];
    int i;

    for (i = 0; i < 64; i++) {
        buf[i] = 0xAA;
    }

    hype_guest_ram_zero(buf, sizeof(buf));

    int all_zero = 1;
    for (i = 0; i < 64; i++) {
        if (buf[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    CHECK_INT("entire buffer zeroed", 1, all_zero);
}

static void test_zero_size_is_a_no_op(void) {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    hype_guest_ram_zero(buf, 0);

    CHECK_INT("zero-size call touches nothing", 1, buf[0]);
    CHECK_INT("zero-size call touches nothing (last byte)", 8, buf[7]);
}

static void test_only_zeroes_the_given_size(void) {
    uint8_t buf[16];
    int i;

    for (i = 0; i < 16; i++) {
        buf[i] = 0xAA;
    }

    hype_guest_ram_zero(buf, 8);

    int first_half_zero = 1;
    for (i = 0; i < 8; i++) {
        if (buf[i] != 0) {
            first_half_zero = 0;
        }
    }
    int second_half_untouched = 1;
    for (i = 8; i < 16; i++) {
        if (buf[i] != 0xAA) {
            second_half_untouched = 0;
        }
    }

    CHECK_INT("first half (within size) zeroed", 1, first_half_zero);
    CHECK_INT("second half (beyond size) left untouched", 1, second_half_untouched);
}

int main(void) {
    test_zeroes_entire_range();
    test_zero_size_is_a_no_op();
    test_only_zeroes_the_given_size();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
