#include <stdio.h>
#include "../../devices/pit.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_reset_defaults(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);
    CHECK_HEX("channel 0 counter starts at 0", 0, pit.channels[0].counter);
    CHECK_HEX("channel 0 access mode defaults to lobyte/hibyte", 3, pit.channels[0].access_mode);
}

static void test_unrecognized_port_rejected(void) {
    hype_pit_emu_t pit;
    uint8_t value = 0;
    hype_pit_emu_reset(&pit);
    CHECK_HEX("write to an unrelated port is rejected", 1, hype_pit_emu_io_write(&pit, 0x44, 0) != 0);
    CHECK_HEX("read from an unrelated port is rejected", 1, hype_pit_emu_io_read(&pit, 0x44, &value) != 0);
    CHECK_HEX("the mode/command port itself is write-only, read rejected", 1,
              hype_pit_emu_io_read(&pit, 0x43, &value) != 0);
}

static void test_lobyte_hibyte_program_channel0_mode3(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    /* Command: channel 0, access lobyte/hibyte, mode 3 (square wave), binary. */
    hype_pit_emu_io_write(&pit, 0x43, 0x36);
    hype_pit_emu_io_write(&pit, 0x40, 0x34); /* lobyte of 1193 (~1kHz divisor) */

    CHECK_HEX("counter not yet reloaded after only the lobyte", 0, pit.channels[0].counter);

    hype_pit_emu_io_write(&pit, 0x40, 0x04); /* hibyte */

    CHECK_HEX("reload value assembled from lobyte+hibyte", 0x0434, pit.channels[0].reload);
    CHECK_HEX("counter takes effect once both bytes arrive", 0x0434, pit.channels[0].counter);
    CHECK_HEX("mode programmed as square wave (3)", 3, pit.channels[0].mode);
}

static void test_lobyte_only_access_mode(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x10); /* channel 0, lobyte only, mode 0 */
    hype_pit_emu_io_write(&pit, 0x40, 0x7B);

    CHECK_HEX("lobyte-only write reloads immediately", 0x7B, pit.channels[0].counter);
}

static void test_hibyte_only_access_mode(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x20); /* channel 0, hibyte only, mode 0 */
    hype_pit_emu_io_write(&pit, 0x40, 0x12);

    CHECK_HEX("hibyte-only write reloads immediately, shifted", 0x1200, pit.channels[0].counter);
}

static void test_read_lobyte_hibyte_order(void) {
    hype_pit_emu_t pit;
    uint8_t lo, hi;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x36);
    hype_pit_emu_io_write(&pit, 0x40, 0x34);
    hype_pit_emu_io_write(&pit, 0x40, 0x04);

    hype_pit_emu_io_read(&pit, 0x40, &lo);
    hype_pit_emu_io_read(&pit, 0x40, &hi);

    CHECK_HEX("first read returns lobyte", 0x34, lo);
    CHECK_HEX("second read returns hibyte", 0x04, hi);
}

static void test_latch_command_snapshots_counter(void) {
    hype_pit_emu_t pit;
    uint8_t lo, hi;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x36);
    hype_pit_emu_io_write(&pit, 0x40, 0x00);
    hype_pit_emu_io_write(&pit, 0x40, 0x10); /* reload = 0x1000 */

    hype_pit_emu_tick(&pit); /* counter decrements to 0x0FFF */
    hype_pit_emu_tick(&pit); /* counter decrements to 0x0FFE */

    /* Latch channel 0 (SC=00, RW=00 -> latch command). */
    hype_pit_emu_io_write(&pit, 0x43, 0x00);

    hype_pit_emu_tick(&pit); /* live counter keeps moving, but the latch is frozen */

    hype_pit_emu_io_read(&pit, 0x40, &lo);
    hype_pit_emu_io_read(&pit, 0x40, &hi);

    CHECK_HEX("latched lobyte reflects the counter at latch time, not later", 0xFE, lo);
    CHECK_HEX("latched hibyte reflects the counter at latch time, not later", 0x0F, hi);
}

static void test_tick_decrements_counter(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x34); /* channel 0, lobyte/hibyte, mode 2 */
    hype_pit_emu_io_write(&pit, 0x40, 0x05);
    hype_pit_emu_io_write(&pit, 0x40, 0x00); /* reload = 5 */

    hype_pit_emu_tick(&pit);
    hype_pit_emu_tick(&pit);

    CHECK_HEX("counter decrements by one per tick", 3, pit.channels[0].counter);
}

static void test_tick_auto_reloads_in_rate_generator_mode(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x34); /* mode 2: rate generator */
    hype_pit_emu_io_write(&pit, 0x40, 0x02);
    hype_pit_emu_io_write(&pit, 0x40, 0x00); /* reload = 2 */

    hype_pit_emu_tick(&pit); /* 2 -> 1 */
    hype_pit_emu_tick(&pit); /* 1 -> 0 */
    hype_pit_emu_tick(&pit); /* 0 -> auto-reload to 2 */

    CHECK_HEX("mode 2 auto-reloads from `reload` on reaching 0", 2, pit.channels[0].counter);
}

static void test_tick_auto_reloads_in_square_wave_mode(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x36); /* mode 3: square wave */
    hype_pit_emu_io_write(&pit, 0x40, 0x02);
    hype_pit_emu_io_write(&pit, 0x40, 0x00); /* reload = 2 */

    hype_pit_emu_tick(&pit); /* 2 -> 1 */
    hype_pit_emu_tick(&pit); /* 1 -> 0 */
    hype_pit_emu_tick(&pit); /* 0 -> auto-reload to 2 */

    CHECK_HEX("mode 3 (square wave) also auto-reloads on reaching 0", 2, pit.channels[0].counter);
}

static void test_channel1_is_independently_addressable(void) {
    hype_pit_emu_t pit;
    uint8_t value;
    hype_pit_emu_reset(&pit);

    /* Channel 1 (SC=01), lobyte only, mode 0. */
    hype_pit_emu_io_write(&pit, 0x43, 0x50);
    hype_pit_emu_io_write(&pit, 0x41, 0x2A);

    hype_pit_emu_io_read(&pit, 0x41, &value);

    CHECK_HEX("channel 1 programmed via its own data port", 0x2A, pit.channels[1].counter);
    CHECK_HEX("channel 1 read back via its own data port", 0x2A, value);
}

static void test_read_lobyte_only_access_mode(void) {
    hype_pit_emu_t pit;
    uint8_t value;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x10); /* channel 0, lobyte only, mode 0 */
    hype_pit_emu_io_write(&pit, 0x40, 0x55);

    hype_pit_emu_io_read(&pit, 0x40, &value);

    CHECK_HEX("lobyte-only access mode reads back just the low byte", 0x55, value);
}

static void test_read_hibyte_only_access_mode(void) {
    hype_pit_emu_t pit;
    uint8_t value;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x20); /* channel 0, hibyte only, mode 0 */
    hype_pit_emu_io_write(&pit, 0x40, 0x33);

    hype_pit_emu_io_read(&pit, 0x40, &value);

    CHECK_HEX("hibyte-only access mode reads back just the high byte", 0x33, value);
}

static void test_tick_stays_at_zero_in_mode0(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x30); /* mode 0: interrupt on terminal count */
    hype_pit_emu_io_write(&pit, 0x40, 0x01);
    hype_pit_emu_io_write(&pit, 0x40, 0x00); /* reload = 1 */

    hype_pit_emu_tick(&pit); /* 1 -> 0 */
    hype_pit_emu_tick(&pit); /* stays at 0 -- mode 0 doesn't auto-reload */

    CHECK_HEX("mode 0 stays at terminal count, no auto-reload", 0, pit.channels[0].counter);
}

static void test_channels_are_independent(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    hype_pit_emu_io_write(&pit, 0x43, 0x30); /* channel 0 */
    hype_pit_emu_io_write(&pit, 0x40, 0x0A);
    hype_pit_emu_io_write(&pit, 0x40, 0x00);

    hype_pit_emu_io_write(&pit, 0x43, 0xB0); /* channel 2 (SC=10), lobyte/hibyte, mode 0 */
    hype_pit_emu_io_write(&pit, 0x42, 0x05);
    hype_pit_emu_io_write(&pit, 0x42, 0x00);

    CHECK_HEX("channel 0 programmed independently", 0x0A, pit.channels[0].counter);
    CHECK_HEX("channel 2 programmed independently", 0x05, pit.channels[2].counter);
    CHECK_HEX("channel 1 untouched", 0, pit.channels[1].counter);
}

static void test_readback_command_is_ignored(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    /* SC=11 (0xC0) -- read-back command, not supported by this stub;
     * must not corrupt any channel's state. */
    hype_pit_emu_io_write(&pit, 0x43, 0xC0);

    CHECK_HEX("read-back command is a safe no-op", 0, pit.channels[0].counter);
    CHECK_HEX("read-back command leaves default access mode alone", 3, pit.channels[0].access_mode);
}

/* M4-6a: the port-0x61 / channel-2-OUT path a Linux PIT-based TSC/delay
 * calibration drives -- program ch2 mode 0, load a count, set the gate,
 * then poll port 0x61 bit 5 for OUT to go high at terminal count. */
static void test_port61_ch2_out_goes_high_at_terminal_count(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);

    CHECK_HEX("port61 resets to 0", 0, pit.port61);
    CHECK_HEX("ch2 OUT resets low", 0, hype_pit_emu_port61_read(&pit) & HYPE_PIT_PORT61_CH2_OUT);

    /* Control word 0xb0: channel 2, access lobyte/hibyte, mode 0. */
    hype_pit_emu_io_write(&pit, 0x43, 0xb0);
    CHECK_HEX("OUT low after programming ch2 mode 0", 0,
              hype_pit_emu_port61_read(&pit) & HYPE_PIT_PORT61_CH2_OUT);

    /* Load count = 3 (lobyte then hibyte). */
    hype_pit_emu_io_write(&pit, 0x42, 0x03);
    hype_pit_emu_io_write(&pit, 0x42, 0x00);

    /* Enable the gate (bit 0) and confirm it reads back. */
    hype_pit_emu_port61_write(&pit, HYPE_PIT_PORT61_CH2_GATE);
    CHECK_HEX("gate bit reads back", HYPE_PIT_PORT61_CH2_GATE,
              hype_pit_emu_port61_read(&pit) & HYPE_PIT_PORT61_CH2_GATE);

    /* Not yet at terminal count. */
    hype_pit_emu_tick(&pit); /* 3 -> 2 */
    hype_pit_emu_tick(&pit); /* 2 -> 1 */
    CHECK_HEX("OUT still low mid-count", 0,
              hype_pit_emu_port61_read(&pit) & HYPE_PIT_PORT61_CH2_OUT);
    hype_pit_emu_tick(&pit); /* 1 -> 0: terminal count */
    CHECK_HEX("OUT high at terminal count", HYPE_PIT_PORT61_CH2_OUT,
              hype_pit_emu_port61_read(&pit) & HYPE_PIT_PORT61_CH2_OUT);

    /* Reprogramming ch2 drives OUT low again (next calibration loop). */
    hype_pit_emu_io_write(&pit, 0x43, 0xb0);
    CHECK_HEX("OUT low again after reprogramming", 0,
              hype_pit_emu_port61_read(&pit) & HYPE_PIT_PORT61_CH2_OUT);
}

static void test_port61_refresh_toggles(void) {
    hype_pit_emu_t pit;
    uint8_t a, b;
    hype_pit_emu_reset(&pit);
    a = hype_pit_emu_port61_read(&pit) & HYPE_PIT_PORT61_REFRESH;
    b = hype_pit_emu_port61_read(&pit) & HYPE_PIT_PORT61_REFRESH;
    CHECK_HEX("refresh clock bit flips between consecutive reads", 1, a != b);
}

static void test_port61_write_masks_to_writable_bits(void) {
    hype_pit_emu_t pit;
    hype_pit_emu_reset(&pit);
    /* Writing all-ones stores only the writable low nibble; the OUT and
     * refresh bits are device-produced, not writable. */
    hype_pit_emu_port61_write(&pit, 0xFF);
    CHECK_HEX("only writable low nibble latched", HYPE_PIT_PORT61_WRITABLE, pit.port61);
}

int main(void) {
    test_reset_defaults();
    test_unrecognized_port_rejected();
    test_lobyte_hibyte_program_channel0_mode3();
    test_lobyte_only_access_mode();
    test_hibyte_only_access_mode();
    test_read_lobyte_hibyte_order();
    test_latch_command_snapshots_counter();
    test_tick_decrements_counter();
    test_tick_auto_reloads_in_rate_generator_mode();
    test_tick_auto_reloads_in_square_wave_mode();
    test_channel1_is_independently_addressable();
    test_read_lobyte_only_access_mode();
    test_read_hibyte_only_access_mode();
    test_tick_stays_at_zero_in_mode0();
    test_channels_are_independent();
    test_readback_command_is_ignored();
    test_port61_ch2_out_goes_high_at_terminal_count();
    test_port61_refresh_toggles();
    test_port61_write_masks_to_writable_bits();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
