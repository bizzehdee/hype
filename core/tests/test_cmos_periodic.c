#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../devices/cmos.h"

static void sel(hype_cmos_t *c, uint8_t reg, uint8_t val) {
    hype_cmos_index_write(c, reg);
    hype_cmos_data_write(c, val);
}

static void test_rate_table_matches_the_hardware(void) {
    hype_cmos_t c;
    memset(&c, 0, sizeof c);
    sel(&c, HYPE_CMOS_REG_STATUS_A, 0x20);      /* rate 0 */
    assert(hype_cmos_periodic_hz(&c) == 0);
    sel(&c, HYPE_CMOS_REG_STATUS_A, 0x26);      /* rate 6 -> 1024 Hz, the conventional value */
    assert(hype_cmos_periodic_hz(&c) == 1024);
    sel(&c, HYPE_CMOS_REG_STATUS_A, 0x2F);      /* rate 15 -> 2 Hz */
    assert(hype_cmos_periodic_hz(&c) == 2);
    sel(&c, HYPE_CMOS_REG_STATUS_A, 0x21);      /* rate 1 aliases 256 Hz */
    assert(hype_cmos_periodic_hz(&c) == 256);
}

static void test_no_interrupt_until_the_guest_enables_it(void) {
    hype_cmos_t c;
    memset(&c, 0, sizeof c);
    sel(&c, HYPE_CMOS_REG_STATUS_A, 0x26);       /* 1024 Hz selected */
    /* PIE clear: a rate alone must not interrupt -- that would invent an
     * interrupt the guest never asked for. */
    assert(hype_cmos_advance(&c, 1000000000ull) == 0);
    sel(&c, HYPE_CMOS_REG_STATUS_B, HYPE_CMOS_STATUS_B_PIE);
    assert(hype_cmos_advance(&c, 1000000ull) == 1); /* 1ms > 1/1024s */
}

static void test_flag_is_a_level_cleared_by_reading_register_c(void) {
    hype_cmos_t c;
    uint8_t v;
    memset(&c, 0, sizeof c);
    sel(&c, HYPE_CMOS_REG_STATUS_A, 0x26);
    sel(&c, HYPE_CMOS_REG_STATUS_B, HYPE_CMOS_STATUS_B_PIE);
    assert(hype_cmos_advance(&c, 1000000ull) == 1);

    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_C);
    v = hype_cmos_data_read(&c);
    assert((v & HYPE_CMOS_STATUS_C_PF) != 0);
    assert((v & HYPE_CMOS_STATUS_C_IRQF) != 0);
    assert(c.registers[HYPE_CMOS_REG_STATUS_C] == 0);

    /* The read acknowledged the level, so a later elapsed period may assert a
     * fresh IRQ. */
    assert(hype_cmos_advance(&c, 1000000ull) == 1);
}

static void test_disabling_the_rate_stops_the_clock_cleanly(void) {
    hype_cmos_t c;
    memset(&c, 0, sizeof c);
    sel(&c, HYPE_CMOS_REG_STATUS_A, 0x26);
    sel(&c, HYPE_CMOS_REG_STATUS_B, HYPE_CMOS_STATUS_B_PIE);
    (void)hype_cmos_advance(&c, 500000ull);      /* part-way to the next tick */
    sel(&c, HYPE_CMOS_REG_STATUS_B, 0);          /* guest disables PIE */
    assert(hype_cmos_advance(&c, 10000000ull) == 0);
    /* Re-enabling starts a fresh period rather than firing immediately off a
     * stale accumulator. */
    sel(&c, HYPE_CMOS_REG_STATUS_B, HYPE_CMOS_STATUS_B_PIE);
    assert(hype_cmos_advance(&c, 100ull) == 0);
}


static void test_backlog_periods_are_owed_not_dropped(void) {
    hype_cmos_t c;
    uint8_t v;
    unsigned delivered = 0, i;
    memset(&c, 0, sizeof c);
    sel(&c, HYPE_CMOS_REG_STATUS_A, 0x26);       /* 1024 Hz */
    sel(&c, HYPE_CMOS_REG_STATUS_B, HYPE_CMOS_STATUS_B_PIE);
    /* #94: 10 periods elapse in ONE coarse step (a busy vCPU loop). Windows
     * adds the period to InterruptTime per interrupt RECEIVED, so all 10 must
     * arrive -- the old code delivered one and dropped nine, and the guest's
     * relative clock fell measurably behind its wall clock. */
    assert(hype_cmos_advance(&c, 10u * 976563ull) == 1);
    delivered = 1;
    for (i = 0; i < 16u; i++) {
        hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_C);
        v = hype_cmos_data_read(&c); /* ack */
        (void)v;
        if (hype_cmos_advance(&c, 0ull) == 1) {
            delivered++;
        }
    }
    assert(delivered == 10);
}

static void test_backlog_is_capped_at_one_second(void) {
    hype_cmos_t c;
    memset(&c, 0, sizeof c);
    sel(&c, HYPE_CMOS_REG_STATUS_A, 0x26);       /* 1024 Hz */
    sel(&c, HYPE_CMOS_REG_STATUS_B, HYPE_CMOS_STATUS_B_PIE);
    /* An hour-long stall must not owe an hour of interrupts: the burst is
     * bounded to one second's worth, like QEMU's reinjection cap. */
    assert(hype_cmos_advance(&c, 3600ull * 1000000000ull) == 1);
    assert(c.periodic_owed <= 1024u);
}

int main(void) {
    test_rate_table_matches_the_hardware();
    test_no_interrupt_until_the_guest_enables_it();
    test_flag_is_a_level_cleared_by_reading_register_c();
    test_disabling_the_rate_stops_the_clock_cleanly();
    test_backlog_periods_are_owed_not_dropped();
    test_backlog_is_capped_at_one_second();
    printf("test_cmos_periodic: all tests passed\n");
    return 0;
}
