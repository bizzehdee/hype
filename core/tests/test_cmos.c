#include <stdio.h>
#include "../../devices/cmos.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_reset_is_all_zero(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);
    CHECK_HEX("index starts at 0", 0, cmos.index);
    CHECK_HEX("register 0 starts at 0", 0, cmos.registers[0]);
    CHECK_HEX("register 0x34 starts at 0", 0, cmos.registers[HYPE_CMOS_REG_EXTMEM_LOW]);
}

static void test_index_write_masks_nmi_disable_bit(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);
    hype_cmos_index_write(&cmos, 0x80u | HYPE_CMOS_REG_EXTMEM_LOW);
    CHECK_HEX("bit 7 (NMI-disable) masked off the stored index", HYPE_CMOS_REG_EXTMEM_LOW, cmos.index);
}

static void test_data_read_write_roundtrip(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);
    hype_cmos_index_write(&cmos, 0x10u);
    hype_cmos_data_write(&cmos, 0xABu);
    CHECK_HEX("data written reads back", 0xABu, hype_cmos_data_read(&cmos));

    hype_cmos_index_write(&cmos, 0x11u);
    CHECK_HEX("a different register is independent", 0, hype_cmos_data_read(&cmos));
}

static void test_set_extended_memory_above_16mb(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);

    /* 0x1234 in 64KB units -- low byte 0x34, high byte 0x12. */
    hype_cmos_set_extended_memory_above_16mb(&cmos, 0x1234u);

    hype_cmos_index_write(&cmos, HYPE_CMOS_REG_EXTMEM_LOW);
    CHECK_HEX("extmem low byte", 0x34u, hype_cmos_data_read(&cmos));

    hype_cmos_index_write(&cmos, HYPE_CMOS_REG_EXTMEM_HIGH);
    CHECK_HEX("extmem high byte", 0x12u, hype_cmos_data_read(&cmos));
}

static void test_index_out_of_bounds_wraps_within_register_file(void) {
    hype_cmos_t cmos;
    hype_cmos_reset(&cmos);

    /* 0xFF & 0x7F = 0x7F, the last valid register -- never out of
     * bounds regardless of what's written to port 0x70. */
    hype_cmos_index_write(&cmos, 0xFFu);
    CHECK_HEX("index clamped to the last valid register", 0x7Fu, cmos.index);
    hype_cmos_data_write(&cmos, 0x42u);
    CHECK_HEX("last register readable", 0x42u, hype_cmos_data_read(&cmos));
}

/* --- #286: the RTC status registers and the seeded clock --- */

static void test_reset_leaves_a_legal_rtc_power_on_state(void) {
    /*
     * The bug this pins: every register reset to 0, so register D's VRT bit read back
     * clear. EDK2's RtcWaitToUpdate() returns EFI_DEVICE_ERROR whenever VRT is clear, so
     * every RTC read failed -- which a release OVMF ignores and a DEBUG build turns into
     * ASSERT_EFI_ERROR at PcRtcEntry.c:181 and dead-loops on.
     */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_D);
    CHECK_HEX("register D has VRT set", HYPE_CMOS_STATUS_D_RESET, hype_cmos_data_read(&c));
    CHECK_HEX("VRT bit specifically", 0x80u, hype_cmos_data_read(&c) & 0x80u);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_A);
    CHECK_HEX("register A UIP clear", 0u, hype_cmos_data_read(&c) & 0x80u);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_B);
    CHECK_HEX("register B 24-hour", HYPE_CMOS_STATUS_B_24HOUR,
              hype_cmos_data_read(&c) & HYPE_CMOS_STATUS_B_24HOUR);
    CHECK_HEX("register B BCD (DM clear)", 0u,
              hype_cmos_data_read(&c) & HYPE_CMOS_STATUS_B_BINARY);
    /* The memory-size registers are still zero -- reset must not invent those. */
    hype_cmos_index_write(&c, HYPE_CMOS_REG_EXTMEM_LOW);
    CHECK_HEX("extmem low still zero", 0u, hype_cmos_data_read(&c));
}

static void test_set_time_writes_bcd_by_default(void) {
    hype_cmos_t c;

    hype_cmos_reset(&c);
    CHECK_HEX("set_time accepted", 0u,
              (unsigned)hype_cmos_set_time(&c, 2026u, 8u, 4u, 7u, 21u, 23u));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_SECONDS);
    CHECK_HEX("seconds 23 -> 0x23", 0x23u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_MINUTES);
    CHECK_HEX("minutes 21 -> 0x21", 0x21u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_HOURS);
    CHECK_HEX("hours 7 -> 0x07", 0x07u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_DAY);
    CHECK_HEX("day 4 -> 0x04", 0x04u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_MONTH);
    CHECK_HEX("month 8 -> 0x08", 0x08u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_YEAR);
    CHECK_HEX("year 2026 -> 0x26", 0x26u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_CENTURY);
    CHECK_HEX("century 20 -> 0x20", 0x20u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_DAY_OF_WEEK);
    CHECK_HEX("day-of-week is never 0", 1u, hype_cmos_data_read(&c));
}

static void test_set_time_honours_the_binary_mode_bit(void) {
    /* A guest that sets DM before reading expects binary, and writing BCD there would
     * hand it a date like "0x26" read as 38. */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_B);
    hype_cmos_data_write(&c, HYPE_CMOS_STATUS_B_RESET | HYPE_CMOS_STATUS_B_BINARY);
    CHECK_HEX("set_time accepted", 0u,
              (unsigned)hype_cmos_set_time(&c, 2026u, 12u, 31u, 23u, 59u, 58u));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_SECONDS);
    CHECK_HEX("seconds binary", 58u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_MONTH);
    CHECK_HEX("month binary", 12u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_YEAR);
    CHECK_HEX("year binary", 26u, hype_cmos_data_read(&c));
}

static void test_set_time_refuses_an_invalid_date(void) {
    /*
     * Refused, not clamped: month 0 and day 0 are exactly what EDK2's
     * RtcTimeFieldsValid() rejects, so writing a corrected-looking date would reproduce
     * the failure this function exists to prevent, and silently.
     */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    CHECK_HEX("month 0 refused", (unsigned)-1,
              (unsigned)hype_cmos_set_time(&c, 2026u, 0u, 4u, 0u, 0u, 0u));
    CHECK_HEX("day 0 refused", (unsigned)-1,
              (unsigned)hype_cmos_set_time(&c, 2026u, 8u, 0u, 0u, 0u, 0u));
    CHECK_HEX("month 13 refused", (unsigned)-1,
              (unsigned)hype_cmos_set_time(&c, 2026u, 13u, 1u, 0u, 0u, 0u));
    CHECK_HEX("hour 24 refused", (unsigned)-1,
              (unsigned)hype_cmos_set_time(&c, 2026u, 8u, 4u, 24u, 0u, 0u));
    CHECK_HEX("second 60 refused", (unsigned)-1,
              (unsigned)hype_cmos_set_time(&c, 2026u, 8u, 4u, 0u, 0u, 60u));
    CHECK_HEX("year 1979 refused", (unsigned)-1,
              (unsigned)hype_cmos_set_time(&c, 1979u, 8u, 4u, 0u, 0u, 0u));
    CHECK_HEX("NULL refused", (unsigned)-1,
              (unsigned)hype_cmos_set_time(0, 2026u, 8u, 4u, 0u, 0u, 0u));
    /* A refusal leaves the registers alone rather than half-written. */
    hype_cmos_index_write(&c, HYPE_CMOS_REG_MONTH);
    CHECK_HEX("month untouched by a refusal", 0u, hype_cmos_data_read(&c));
}

static void test_register_d_vrt_survives_a_guest_write(void) {
    /*
     * The exact sequence EDK2's PcRtcInit() performs: write register D with
     * PcdInitialValueRtcRegisterD (0), then require VRT to still be set. On real hardware
     * VRT is read-only -- it reflects whether the CMOS battery kept the time valid. With a
     * plain read/write model the firmware destroyed the bit it was about to check,
     * RtcWaitToUpdate returned EFI_DEVICE_ERROR, and a DEBUG OVMF dead-looped at
     * PcRtcEntry.c:181.
     */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_D);
    hype_cmos_data_write(&c, 0x00u);
    CHECK_HEX("VRT survives a write of 0", 0x80u, hype_cmos_data_read(&c) & 0x80u);
    /* And a guest cannot set the reserved bits either. */
    hype_cmos_data_write(&c, 0xFFu);
    CHECK_HEX("reserved bits stay clear", HYPE_CMOS_STATUS_D_RESET, hype_cmos_data_read(&c));
}

static void test_register_a_uip_is_held_clear(void) {
    /* A guest that could set UIP would then poll forever for its own bit to clear. */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_A);
    hype_cmos_data_write(&c, 0xFFu);
    CHECK_HEX("UIP held clear", 0u, hype_cmos_data_read(&c) & 0x80u);
    CHECK_HEX("the writable divider/rate bits still take", 0x7Fu, hype_cmos_data_read(&c));
}

static void test_register_c_is_read_only(void) {
    /* Interrupt flags are set by the device, never by software -- a writable register C
     * would let a guest invent RTC interrupts for itself. */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_C);
    hype_cmos_data_write(&c, 0xF0u);
    CHECK_HEX("register C ignored the write", 0u, hype_cmos_data_read(&c));
}

static void test_ordinary_registers_are_still_writable(void) {
    /* The read-only handling must not have made the whole file read-only -- the
     * memory-size fallback registers this model exists for are plain storage. */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_EXTMEM_LOW);
    hype_cmos_data_write(&c, 0xA5u);
    CHECK_HEX("extmem low writable", 0xA5u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_B);
    hype_cmos_data_write(&c, 0x06u);
    CHECK_HEX("register B writable", 0x06u, hype_cmos_data_read(&c));
}

/* --- #304: the RTC must tick --- */

static unsigned int bcd(uint8_t v) { return (unsigned)((v >> 4) * 10u + (v & 0x0Fu)); }

static void test_advance_moves_the_clock(void) {
    /*
     * The defect: seeded once at VM setup and never updated, so every read returned the
     * same instant and a guest computing `now - start` got 0 for ever. FreeBSD's loader
     * menu never reached its countdown.
     */
    hype_cmos_t c;
    uint8_t v = 0;

    hype_cmos_reset(&c);
    CHECK_HEX("seed", 0u, (unsigned)hype_cmos_set_time(&c, 2026u, 8u, 4u, 10u, 30u, 0u));
    hype_cmos_advance_to(&c, 65u); /* +1m05s */
    hype_cmos_index_write(&c, HYPE_CMOS_REG_SECONDS);
    CHECK_HEX("seconds advanced to 5", 5u, bcd(hype_cmos_data_read(&c)));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_MINUTES);
    CHECK_HEX("minutes advanced to 31", 31u, bcd(hype_cmos_data_read(&c)));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_HOURS);
    CHECK_HEX("hours unchanged", 10u, bcd(hype_cmos_data_read(&c)));
    (void)v;
}

static void test_advance_rolls_across_midnight_and_month(void) {
    /* The carry is hype_rtc_advance()'s job, but a wrong hand-off here would show up as a
     * date that goes backwards -- which is worse than one that is merely late. */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    CHECK_HEX("seed 23:59:59 on the last day of a month", 0u,
              (unsigned)hype_cmos_set_time(&c, 2026u, 8u, 31u, 23u, 59u, 59u));
    hype_cmos_advance_to(&c, 1u);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_SECONDS);
    CHECK_HEX("seconds roll to 0", 0u, bcd(hype_cmos_data_read(&c)));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_HOURS);
    CHECK_HEX("hour rolls to 0", 0u, bcd(hype_cmos_data_read(&c)));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_DAY);
    CHECK_HEX("day rolls to 1", 1u, bcd(hype_cmos_data_read(&c)));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_MONTH);
    CHECK_HEX("month rolls to September", 9u, bcd(hype_cmos_data_read(&c)));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_YEAR);
    CHECK_HEX("year unchanged", 26u, bcd(hype_cmos_data_read(&c)));
}

static void test_advance_is_idempotent_for_one_elapsed_value(void) {
    /* The IOIO glue calls this on every CMOS access, so repeating the same elapsed value
     * must not creep the clock forward -- otherwise the time would depend on how many
     * registers a guest happened to read. */
    hype_cmos_t c;
    unsigned int first, second;

    hype_cmos_reset(&c);
    (void)hype_cmos_set_time(&c, 2026u, 8u, 4u, 10u, 30u, 0u);
    hype_cmos_advance_to(&c, 42u);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_SECONDS);
    first = bcd(hype_cmos_data_read(&c));
    hype_cmos_advance_to(&c, 42u);
    hype_cmos_advance_to(&c, 42u);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_SECONDS);
    second = bcd(hype_cmos_data_read(&c));
    CHECK_HEX("same elapsed -> same time", first, second);
    CHECK_HEX("and it is the expected value", 42u, second);
}

static void test_advance_without_a_seed_does_nothing(void) {
    /* A guest reading a clock hype never set should see the unchanging zeros it had
     * before, not an invented date that drifts. */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    hype_cmos_advance_to(&c, 10000u);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_MONTH);
    CHECK_HEX("month still zero", 0u, hype_cmos_data_read(&c));
    hype_cmos_advance_to(0, 10u); /* must not fault */
}

static void test_advance_honours_binary_mode(void) {
    hype_cmos_t c;

    hype_cmos_reset(&c);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_STATUS_B);
    hype_cmos_data_write(&c, HYPE_CMOS_STATUS_B_RESET | HYPE_CMOS_STATUS_B_BINARY);
    (void)hype_cmos_set_time(&c, 2026u, 8u, 4u, 10u, 30u, 0u);
    hype_cmos_advance_to(&c, 90u);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_SECONDS);
    CHECK_HEX("seconds binary 30", 30u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_MINUTES);
    CHECK_HEX("minutes binary 31", 31u, hype_cmos_data_read(&c));
}

static void test_advance_rolls_the_year_and_century(void) {
    /* A year carry also has to update the century register, which is a separate register
     * a guest may or may not read -- leaving it stale would put the guest a century out
     * on New Year's Eve. */
    hype_cmos_t c;

    hype_cmos_reset(&c);
    CHECK_HEX("seed 1999-12-31 23:59:59", 0u,
              (unsigned)hype_cmos_set_time(&c, 1999u, 12u, 31u, 23u, 59u, 59u));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_CENTURY);
    CHECK_HEX("century starts at 19", 0x19u, hype_cmos_data_read(&c));
    hype_cmos_advance_to(&c, 1u);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_YEAR);
    CHECK_HEX("year rolls to 00", 0u, bcd(hype_cmos_data_read(&c)));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_CENTURY);
    CHECK_HEX("century rolls to 20", 0x20u, hype_cmos_data_read(&c));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_MONTH);
    CHECK_HEX("month rolls to January", 1u, bcd(hype_cmos_data_read(&c)));
}

static void test_advance_of_zero_leaves_the_seed(void) {
    hype_cmos_t c;

    hype_cmos_reset(&c);
    (void)hype_cmos_set_time(&c, 2026u, 8u, 4u, 10u, 30u, 15u);
    hype_cmos_advance_to(&c, 0u);
    hype_cmos_index_write(&c, HYPE_CMOS_REG_SECONDS);
    CHECK_HEX("seconds unchanged", 15u, bcd(hype_cmos_data_read(&c)));
    hype_cmos_index_write(&c, HYPE_CMOS_REG_DAY);
    CHECK_HEX("day unchanged", 4u, bcd(hype_cmos_data_read(&c)));
}

int main(void) {
    test_advance_rolls_the_year_and_century();
    test_advance_of_zero_leaves_the_seed();
    test_advance_moves_the_clock();
    test_advance_rolls_across_midnight_and_month();
    test_advance_is_idempotent_for_one_elapsed_value();
    test_advance_without_a_seed_does_nothing();
    test_advance_honours_binary_mode();
    test_register_d_vrt_survives_a_guest_write();
    test_register_a_uip_is_held_clear();
    test_register_c_is_read_only();
    test_ordinary_registers_are_still_writable();
    test_reset_leaves_a_legal_rtc_power_on_state();
    test_set_time_writes_bcd_by_default();
    test_set_time_honours_the_binary_mode_bit();
    test_set_time_refuses_an_invalid_date();
    test_reset_is_all_zero();
    test_index_write_masks_nmi_disable_bit();
    test_data_read_write_roundtrip();
    test_set_extended_memory_above_16mb();
    test_index_out_of_bounds_wraps_within_register_file();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
