#include <stdio.h>
#include <string.h>
#include "../stack_watermark.h"

static int failures = 0;

#define CHECK_U64(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected %llu, got %llu\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

enum { STACK = 16384 };

static void test_untouched_stack_reports_zero(void) {
    static uint8_t s[STACK];
    hype_stack_paint(s, sizeof(s));
    CHECK_U64("painted, never used", 0, hype_stack_used(s, sizeof(s)));
    CHECK_U64("no guard hit", 0, hype_stack_guard_hit(0, sizeof(s)));
}

/* Stacks grow down: a core that pushed 3000 bytes disturbed the top 3000 bytes only. */
static void test_depth_is_measured_from_the_top(void) {
    static uint8_t s[STACK];
    hype_stack_paint(s, sizeof(s));
    memset(s + STACK - 3000, 0x00, 3000);
    CHECK_U64("3000 bytes used", 3000, hype_stack_used(s, sizeof(s)));
    CHECK_U64("not in the guard", 0, hype_stack_guard_hit(3000, sizeof(s)));
}

/* The lowest disturbed byte sets the depth, even with paint-valued bytes above it. */
static void test_lowest_disturbed_byte_wins(void) {
    static uint8_t s[STACK];
    hype_stack_paint(s, sizeof(s));
    s[STACK - 1] = 0x11;
    s[STACK - 9000] = 0x22; /* a deep frame's single non-paint byte */
    CHECK_U64("deepest byte sets depth", 9000, hype_stack_used(s, sizeof(s)));
}

/*
 * #817 regression: fw_1_resolve_on_any_fs() had a 19,160-byte frame on a 16 KiB AP stack. The
 * core wrote all the way through its slot; the slot's guard must report it.
 */
static void test_overflow_reaches_the_guard(void) {
    static uint8_t s[STACK];
    hype_stack_paint(s, sizeof(s));
    memset(s, 0x00, sizeof(s));
    CHECK_U64("whole slot used", STACK, hype_stack_used(s, sizeof(s)));
    CHECK_U64("guard hit", 1, hype_stack_guard_hit(STACK, sizeof(s)));

    hype_stack_paint(s, sizeof(s));
    s[HYPE_STACK_GUARD_BYTES - 1u] = 0x00;
    CHECK_U64("one byte inside the guard", 1,
              hype_stack_guard_hit(hype_stack_used(s, sizeof(s)), sizeof(s)));
    hype_stack_paint(s, sizeof(s));
    s[HYPE_STACK_GUARD_BYTES] = 0x00;
    CHECK_U64("first byte above the guard is not a hit", 0,
              hype_stack_guard_hit(hype_stack_used(s, sizeof(s)), sizeof(s)));
}

static void test_degenerate_inputs(void) {
    uint8_t tiny[8];
    hype_stack_paint(0, 64);
    CHECK_U64("NULL stack used", 0, hype_stack_used(0, 64));
    hype_stack_paint(tiny, sizeof(tiny));
    CHECK_U64("guard-sized stack untouched", 0, hype_stack_guard_hit(0, sizeof(tiny)));
    tiny[7] = 0;
    CHECK_U64("guard-sized stack any use is a hit", 1,
              hype_stack_guard_hit(hype_stack_used(tiny, sizeof(tiny)), sizeof(tiny)));
}

int main(void) {
    test_untouched_stack_reports_zero();
    test_depth_is_measured_from_the_top();
    test_lowest_disturbed_byte_wins();
    test_overflow_reaches_the_guard();
    test_degenerate_inputs();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
