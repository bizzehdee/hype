#include <stdio.h>
#include "../../arch/x86_64/cpu/lapic.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Room for the LVT Timer register (offset 0x320) plus slack. */
static uint32_t g_fake_lapic[0x400 / 4];

static uint32_t *lvt_timer(void) {
    return (uint32_t *)((unsigned char *)g_fake_lapic + HYPE_LAPIC_LVT_TIMER_OFFSET);
}

static void test_mask_timer_sets_only_the_mask_bit(void) {
    *lvt_timer() = 0x000000EC; /* vector 0xEC, some mode bits, unmasked */

    hype_lapic_mask_timer(g_fake_lapic);

    CHECK_HEX("mask bit set", HYPE_LAPIC_LVT_MASKED, *lvt_timer() & HYPE_LAPIC_LVT_MASKED);
    CHECK_HEX("other bits (vector/mode) preserved", 0xEC, *lvt_timer() & 0xFFu);
}

static void test_mask_timer_idempotent(void) {
    *lvt_timer() = HYPE_LAPIC_LVT_MASKED | 0x30;

    hype_lapic_mask_timer(g_fake_lapic);

    CHECK_HEX("already-masked register stays masked", HYPE_LAPIC_LVT_MASKED,
              *lvt_timer() & HYPE_LAPIC_LVT_MASKED);
    CHECK_HEX("other bits untouched when already masked", 0x30, *lvt_timer() & 0xFFu);
}

static void test_mask_timer_does_not_touch_neighboring_registers(void) {
    uint32_t *before = (uint32_t *)((unsigned char *)g_fake_lapic + HYPE_LAPIC_LVT_TIMER_OFFSET - 4);
    uint32_t *after = (uint32_t *)((unsigned char *)g_fake_lapic + HYPE_LAPIC_LVT_TIMER_OFFSET + 4);

    *before = 0xAAAAAAAAu;
    *after = 0x55555555u;
    *lvt_timer() = 0;

    hype_lapic_mask_timer(g_fake_lapic);

    CHECK_HEX("register before LVT Timer untouched", 0xAAAAAAAAu, *before);
    CHECK_HEX("register after LVT Timer untouched", 0x55555555u, *after);
}

int main(void) {
    test_mask_timer_sets_only_the_mask_bit();
    test_mask_timer_idempotent();
    test_mask_timer_does_not_touch_neighboring_registers();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
