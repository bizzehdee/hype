#include <assert.h>
#include <stdio.h>

#include "../../devices/hpet.h"

static void test_capabilities_describe_what_is_modelled(void) {
    uint64_t cap = hype_hpet_capabilities();

    /* The period must be the 100 ns this model actually counts in: a guest
     * divides femtoseconds by it to convert ticks to time, so a wrong value
     * here is a wrong clock everywhere. */
    assert((uint32_t)(cap >> 32) == HYPE_HPET_PERIOD_FS);
    /* NUM_TIM_CAP is "number of comparators minus one" -- it must match the
     * comparators that really exist, not a larger number. */
    assert(((cap >> 8) & 0x1Fu) == HYPE_HPET_NUM_TIMERS - 1u);
    assert((cap & (1ull << 13)) != 0); /* 64-bit counter */
    assert((cap & (1ull << 15)) != 0); /* legacy routing capable */
    assert((cap & 0xFFu) == 0x01u);    /* revision */
}

static void test_counter_only_runs_when_enabled(void) {
    hype_hpet_t h;
    hype_hpet_reset(&h);

    /* Disabled at reset: time passes and the counter does not move. */
    assert(hype_hpet_advance(&h, 1000) == 0);
    assert(hype_hpet_read(&h, HYPE_HPET_REG_MAIN_COUNTER, 8) == 0);

    hype_hpet_write(&h, HYPE_HPET_REG_CONFIG, 8, HYPE_HPET_CONFIG_ENABLE);
    (void)hype_hpet_advance(&h, 1000);
    assert(hype_hpet_read(&h, HYPE_HPET_REG_MAIN_COUNTER, 8) == 1000);
}

static void test_oneshot_comparator_fires_once(void) {
    hype_hpet_t h;
    uint32_t fired;
    hype_hpet_reset(&h);
    hype_hpet_write(&h, HYPE_HPET_REG_CONFIG, 8, HYPE_HPET_CONFIG_ENABLE);

    hype_hpet_write(&h, HYPE_HPET_REG_TIMER_BASE, 8, HYPE_HPET_TIMER_INT_ENABLE);
    hype_hpet_write(&h, HYPE_HPET_REG_TIMER_BASE + 8u, 8, 500);

    /* Not yet reached. */
    assert(hype_hpet_advance(&h, 499) == 0);
    /* A single step that spans the comparator still delivers -- hype samples
     * time at VM exits, so an expiry inside the step must not be lost. */
    fired = hype_hpet_advance(&h, 100);
    assert(fired == 0x1u);
    assert(hype_hpet_read(&h, HYPE_HPET_REG_INT_STATUS, 8) == 0x1u);
    /* One-shot: it does not fire again. */
    assert(hype_hpet_advance(&h, 10000) == 0);
}

static void test_interrupt_status_is_write_one_to_clear(void) {
    hype_hpet_t h;
    hype_hpet_reset(&h);
    hype_hpet_write(&h, HYPE_HPET_REG_CONFIG, 8, HYPE_HPET_CONFIG_ENABLE);
    hype_hpet_write(&h, HYPE_HPET_REG_TIMER_BASE, 8, HYPE_HPET_TIMER_INT_ENABLE);
    hype_hpet_write(&h, HYPE_HPET_REG_TIMER_BASE + 8u, 8, 10);
    (void)hype_hpet_advance(&h, 20);
    assert(hype_hpet_read(&h, HYPE_HPET_REG_INT_STATUS, 8) == 0x1u);

    hype_hpet_write(&h, HYPE_HPET_REG_INT_STATUS, 8, 0x1u);
    assert(hype_hpet_read(&h, HYPE_HPET_REG_INT_STATUS, 8) == 0);
}

static void test_periodic_comparator_rearms(void) {
    hype_hpet_t h;
    hype_hpet_reset(&h);
    hype_hpet_write(&h, HYPE_HPET_REG_CONFIG, 8, HYPE_HPET_CONFIG_ENABLE);
    hype_hpet_write(&h, HYPE_HPET_REG_TIMER_BASE, 8,
                    HYPE_HPET_TIMER_INT_ENABLE | HYPE_HPET_TIMER_PERIODIC |
                        HYPE_HPET_TIMER_VAL_SET);
    hype_hpet_write(&h, HYPE_HPET_REG_TIMER_BASE + 8u, 8, 100);

    assert(hype_hpet_advance(&h, 100) == 0x1u);
    hype_hpet_write(&h, HYPE_HPET_REG_INT_STATUS, 8, 0x1u);
    assert(hype_hpet_advance(&h, 100) == 0x1u);
    hype_hpet_write(&h, HYPE_HPET_REG_INT_STATUS, 8, 0x1u);
    /* A long step that covers several periods still leaves the comparator
     * ahead of the counter, so the timer keeps working afterwards. */
    assert(hype_hpet_advance(&h, 1000) == 0x1u);
    hype_hpet_write(&h, HYPE_HPET_REG_INT_STATUS, 8, 0x1u);
    assert(hype_hpet_advance(&h, 100) == 0x1u);
}

static void test_disabled_timer_does_not_interrupt(void) {
    hype_hpet_t h;
    hype_hpet_reset(&h);
    hype_hpet_write(&h, HYPE_HPET_REG_CONFIG, 8, HYPE_HPET_CONFIG_ENABLE);
    /* Interrupt bit clear: the comparator still matches, but no interrupt is
     * signalled and no status bit is latched. */
    hype_hpet_write(&h, HYPE_HPET_REG_TIMER_BASE + 8u, 8, 50);
    assert(hype_hpet_advance(&h, 100) == 0);
    assert(hype_hpet_read(&h, HYPE_HPET_REG_INT_STATUS, 8) == 0);
}

static void test_32bit_halves_address_the_right_word(void) {
    hype_hpet_t h;
    hype_hpet_reset(&h);
    hype_hpet_write(&h, HYPE_HPET_REG_CONFIG, 8, HYPE_HPET_CONFIG_ENABLE);
    (void)hype_hpet_advance(&h, 0x100000007ull);

    /* A 32-bit guest reads the counter in two halves; each must yield its own
     * word rather than the low word twice. */
    assert(hype_hpet_read(&h, HYPE_HPET_REG_MAIN_COUNTER, 4) == 7u);
    assert(hype_hpet_read(&h, HYPE_HPET_REG_MAIN_COUNTER + 4u, 4) == 1u);
}

static void test_capability_bits_survive_a_guest_write(void) {
    hype_hpet_t h;
    uint64_t conf;
    hype_hpet_reset(&h);

    /* A guest writing the timer configuration must not be able to clear the
     * read-only capability bits -- reporting a comparator as no longer
     * periodic-capable after a write would describe hardware that changed
     * shape underneath it. */
    hype_hpet_write(&h, HYPE_HPET_REG_TIMER_BASE, 8, HYPE_HPET_TIMER_INT_ENABLE);
    conf = hype_hpet_read(&h, HYPE_HPET_REG_TIMER_BASE, 8);
    assert((conf & HYPE_HPET_TIMER_PERIODIC_CAP) != 0);
    assert((conf & HYPE_HPET_TIMER_SIZE_CAP) != 0);
    assert((conf & HYPE_HPET_TIMER_INT_ENABLE) != 0);
}

static void test_unimplemented_offsets_read_zero(void) {
    hype_hpet_t h;
    hype_hpet_reset(&h);
    /* Not all-ones: a guest reads all-ones as a live register with every bit
     * set, which is how an absent device gets mistaken for a present one. */
    assert(hype_hpet_read(&h, 0x030u, 8) == 0);
    assert(hype_hpet_read(&h, 0x3F8u, 8) == 0);
}

int main(void) {
    test_capabilities_describe_what_is_modelled();
    test_counter_only_runs_when_enabled();
    test_oneshot_comparator_fires_once();
    test_interrupt_status_is_write_one_to_clear();
    test_periodic_comparator_rearms();
    test_disabled_timer_does_not_interrupt();
    test_32bit_halves_address_the_right_word();
    test_capability_bits_survive_a_guest_write();
    test_unimplemented_offsets_read_zero();
    printf("test_hpet: all tests passed\n");
    return 0;
}
