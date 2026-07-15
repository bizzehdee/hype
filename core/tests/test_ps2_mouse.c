#include <stdio.h>
#include "../../devices/ps2_mouse.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_reset_state(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);
    CHECK_HEX("no pending byte after reset", 0, hype_ps2_mouse_has_pending_byte(&mouse));
    CHECK_HEX("reporting disabled after reset", 0, mouse.reporting_enabled);
}

static void test_reset_command_queues_ack_selftest_and_id(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);

    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_RESET);

    CHECK_HEX("byte 1: ACK", HYPE_PS2_MOUSE_ACK, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("byte 2: self-test passed", HYPE_PS2_MOUSE_SELF_TEST_PASSED, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("byte 3: device ID", HYPE_PS2_MOUSE_DEVICE_ID_STANDARD, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("queue empty afterward", 0, hype_ps2_mouse_has_pending_byte(&mouse));
}

static void test_reset_command_disables_reporting(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);
    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_ENABLE_REPORTING);
    hype_ps2_mouse_read_byte(&mouse); /* consume the ACK */

    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_RESET);

    CHECK_HEX("reporting disabled by reset", 0, mouse.reporting_enabled);
}

static void test_enable_reporting(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);

    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_ENABLE_REPORTING);

    CHECK_HEX("ACK queued", HYPE_PS2_MOUSE_ACK, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("reporting now enabled", 1, mouse.reporting_enabled);
}

static void test_disable_reporting(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);
    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_ENABLE_REPORTING);
    hype_ps2_mouse_read_byte(&mouse);

    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_DISABLE_REPORTING);

    CHECK_HEX("ACK queued", HYPE_PS2_MOUSE_ACK, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("reporting now disabled", 0, mouse.reporting_enabled);
}

static void test_get_device_id(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);

    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_GET_DEVICE_ID);

    CHECK_HEX("ACK queued", HYPE_PS2_MOUSE_ACK, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("device ID queued", HYPE_PS2_MOUSE_DEVICE_ID_STANDARD, hype_ps2_mouse_read_byte(&mouse));
}

static void test_set_defaults(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);
    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_ENABLE_REPORTING);
    hype_ps2_mouse_read_byte(&mouse);

    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_SET_DEFAULTS);

    CHECK_HEX("ACK queued", HYPE_PS2_MOUSE_ACK, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("reporting disabled by set-defaults", 0, mouse.reporting_enabled);
}

static void test_unrecognized_command_generically_acked(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);

    hype_ps2_mouse_write_command(&mouse, 0xE8u); /* "set resolution" -- not specifically modeled */

    CHECK_HEX("generic ACK", HYPE_PS2_MOUSE_ACK, hype_ps2_mouse_read_byte(&mouse));
}

static void test_movement_dropped_when_reporting_disabled(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);

    hype_ps2_mouse_enqueue_movement(&mouse, 0x08u, 0x05u, 0xFBu);

    CHECK_HEX("movement dropped -- reporting was never enabled", 0, hype_ps2_mouse_has_pending_byte(&mouse));
}

static void test_movement_queued_when_reporting_enabled(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);
    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_ENABLE_REPORTING);
    hype_ps2_mouse_read_byte(&mouse); /* consume the ACK */

    hype_ps2_mouse_enqueue_movement(&mouse, 0x08u, 0x05u, 0xFBu);

    CHECK_HEX("status byte", 0x08u, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("dx", 0x05u, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("dy", 0xFBu, hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("queue empty afterward", 0, hype_ps2_mouse_has_pending_byte(&mouse));
}

static void test_read_byte_with_nothing_pending_returns_zero(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);
    CHECK_HEX("reading with nothing queued returns 0", 0, hype_ps2_mouse_read_byte(&mouse));
}

static void test_queue_overflow_drops_extra_bytes_safely(void) {
    hype_ps2_mouse_t mouse;
    unsigned int i;

    hype_ps2_mouse_reset(&mouse);
    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_ENABLE_REPORTING);
    hype_ps2_mouse_read_byte(&mouse); /* consume the ACK, leaving the queue empty */

    /* Queue far more than HYPE_PS2_MOUSE_QUEUE_SIZE bytes' worth of
     * movement packets without ever reading any back -- the queue
     * must saturate rather than corrupt itself or wrap over unread
     * data. */
    for (i = 0; i < HYPE_PS2_MOUSE_QUEUE_SIZE; i++) {
        hype_ps2_mouse_enqueue_movement(&mouse, 0x08u, 0x01u, 0x02u);
    }

    CHECK_HEX("still has a pending byte after saturating", 1, hype_ps2_mouse_has_pending_byte(&mouse));
    CHECK_HEX("the first byte queued is still readable first (FIFO intact)", 0x08u,
              hype_ps2_mouse_read_byte(&mouse));
}

static void test_queue_fifo_ordering_across_multiple_commands(void) {
    hype_ps2_mouse_t mouse;
    hype_ps2_mouse_reset(&mouse);

    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_GET_DEVICE_ID); /* queues ACK, ID */
    CHECK_HEX("first queued byte read first (ACK)", HYPE_PS2_MOUSE_ACK, hype_ps2_mouse_read_byte(&mouse));
    hype_ps2_mouse_write_command(&mouse, HYPE_PS2_MOUSE_CMD_ENABLE_REPORTING); /* queues another ACK */
    CHECK_HEX("second queued byte (ID from first command)", HYPE_PS2_MOUSE_DEVICE_ID_STANDARD,
              hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("third queued byte (ACK from second command)", HYPE_PS2_MOUSE_ACK,
              hype_ps2_mouse_read_byte(&mouse));
    CHECK_HEX("queue empty afterward", 0, hype_ps2_mouse_has_pending_byte(&mouse));
}

int main(void) {
    test_reset_state();
    test_reset_command_queues_ack_selftest_and_id();
    test_reset_command_disables_reporting();
    test_enable_reporting();
    test_disable_reporting();
    test_get_device_id();
    test_set_defaults();
    test_unrecognized_command_generically_acked();
    test_movement_dropped_when_reporting_disabled();
    test_movement_queued_when_reporting_enabled();
    test_read_byte_with_nothing_pending_returns_zero();
    test_queue_overflow_drops_extra_bytes_safely();
    test_queue_fifo_ordering_across_multiple_commands();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
