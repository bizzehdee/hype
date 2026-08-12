#include <stdio.h>
#include "../../devices/ps2_keyboard.h"

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
    hype_ps2_kbd_t kbd;
    uint8_t value;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, &value);
    CHECK_HEX("OBF clear after reset, system flag set", HYPE_PS2_STATUS_SYSTEM_FLAG, value);
}

static void test_unrecognized_port_rejected(void) {
    hype_ps2_kbd_t kbd;
    uint8_t value = 0;

    hype_ps2_kbd_reset(&kbd);
    CHECK_HEX("read from an unrelated port is rejected", 1,
              hype_ps2_kbd_io_read(&kbd, 0x61u, &value) != 0);
    CHECK_HEX("write to an unrelated port is rejected", 1, hype_ps2_kbd_io_write(&kbd, 0x61u, 0) != 0);
}

static void test_enqueue_scancode_sets_obf_and_reads_back(void) {
    hype_ps2_kbd_t kbd;
    uint8_t status = 0, data = 0;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_enqueue_scancode(&kbd, 0x1Eu); /* Set 1 make code for 'A' */

    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, &status);
    CHECK_HEX("OBF set after enqueue", 1, (status & HYPE_PS2_STATUS_OUTPUT_FULL) != 0);

    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("scancode read back", 0x1Eu, data);

    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, &status);
    CHECK_HEX("OBF clears after the data read", 0, status & HYPE_PS2_STATUS_OUTPUT_FULL);
}

static void test_enqueue_overwrites_unread_byte(void) {
    hype_ps2_kbd_t kbd;
    uint8_t data = 0;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_enqueue_scancode(&kbd, 0x1Eu);
    hype_ps2_kbd_enqueue_scancode(&kbd, 0x9Eu); /* break code for the same key, arrives before read */

    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("most recent scancode wins", 0x9Eu, data);
}

static void test_read_config_byte(void) {
    hype_ps2_kbd_t kbd;
    uint8_t data = 0;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_READ_CONFIG_BYTE);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("config byte starts at 0", 0, data);
}

static void test_write_config_byte_roundtrip(void) {
    hype_ps2_kbd_t kbd;
    uint8_t data = 0;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_WRITE_CONFIG_BYTE);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_DATA, 0x61u); /* enable IRQ1 + system flag bits, e.g. */

    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_READ_CONFIG_BYTE);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("config byte written then read back", 0x61u, data);
}

static void test_data_write_without_pending_config_command_is_a_keyboard_command(void) {
    hype_ps2_kbd_t kbd;
    uint8_t data = 0;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_DATA, 0xF4u); /* "enable scanning" keyboard command */

    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("keyboard command generically ACKed", HYPE_PS2_KBD_ACK, data);
}

static void test_self_test(void) {
    hype_ps2_kbd_t kbd;
    uint8_t data = 0;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_SELF_TEST);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("self-test passed response", HYPE_PS2_SELF_TEST_PASSED, data);
}

static void test_interface_test(void) {
    hype_ps2_kbd_t kbd;
    uint8_t data = 0;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_INTERFACE_TEST);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("interface test passed response", HYPE_PS2_INTERFACE_TEST_PASSED, data);
}

static void test_disable_then_enable_keyboard_port(void) {
    hype_ps2_kbd_t kbd;

    hype_ps2_kbd_reset(&kbd);
    CHECK_HEX("keyboard port enabled after reset", 1, kbd.keyboard_port_enabled);

    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_DISABLE_KEYBOARD_PORT);
    CHECK_HEX("keyboard port disabled", 0, kbd.keyboard_port_enabled);

    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_ENABLE_KEYBOARD_PORT);
    CHECK_HEX("keyboard port re-enabled", 1, kbd.keyboard_port_enabled);
}

static void test_unrecognized_controller_command_is_ignored(void) {
    hype_ps2_kbd_t kbd;
    uint8_t status = 0;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, 0x00u); /* not a recognized command */

    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, &status);
    CHECK_HEX("no response staged for an unrecognized command", 0, status & HYPE_PS2_STATUS_OUTPUT_FULL);
}

static void test_disable_then_enable_aux_port(void) {
    hype_ps2_kbd_t kbd;

    hype_ps2_kbd_reset(&kbd);
    CHECK_HEX("aux port enabled after reset", 1, kbd.aux_port_enabled);

    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_DISABLE_AUX_PORT);
    CHECK_HEX("aux port disabled", 0, kbd.aux_port_enabled);

    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_ENABLE_AUX_PORT);
    CHECK_HEX("aux port re-enabled", 1, kbd.aux_port_enabled);
}

static void test_aux_port_test(void) {
    hype_ps2_kbd_t kbd;
    uint8_t data = 0;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_TEST_AUX_PORT);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("aux port test passed response", HYPE_PS2_AUX_TEST_PASSED, data);
}

static void test_write_to_aux_sets_one_shot_flag(void) {
    hype_ps2_kbd_t kbd;

    hype_ps2_kbd_reset(&kbd);
    CHECK_HEX("flag clear before 0xD4", 0, hype_ps2_kbd_take_aux_data_write(&kbd));

    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, HYPE_PS2_CMD_WRITE_TO_AUX);
    CHECK_HEX("flag set after 0xD4", 1, hype_ps2_kbd_take_aux_data_write(&kbd));
    CHECK_HEX("flag consumed -- clear on the very next check", 0, hype_ps2_kbd_take_aux_data_write(&kbd));
}

static void test_has_pending_byte(void) {
    hype_ps2_kbd_t kbd;

    hype_ps2_kbd_reset(&kbd);
    CHECK_HEX("nothing pending after reset", 0, hype_ps2_kbd_has_pending_byte(&kbd));

    hype_ps2_kbd_enqueue_scancode(&kbd, 0x1Eu);
    CHECK_HEX("pending after enqueue", 1, hype_ps2_kbd_has_pending_byte(&kbd));
}

/* FW-1f: keyboard reset (0xFF via 0x60) must return ACK (0xFA) THEN
 * BAT-complete (0xAA) as two separately-readable bytes -- OVMF's
 * Ps2KeyboardDxe waits for both. Exercises the output FIFO. */
static void test_keyboard_reset_returns_ack_then_bat(void) {
    hype_ps2_kbd_t kbd;
    uint8_t data, status;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_DATA, HYPE_PS2_KBD_CMD_RESET); /* 0xFF */

    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, &status);
    CHECK_HEX("OBF set after reset command", HYPE_PS2_STATUS_OUTPUT_FULL,
              status & HYPE_PS2_STATUS_OUTPUT_FULL);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("first reset response byte is ACK", HYPE_PS2_KBD_ACK, data);

    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, &status);
    CHECK_HEX("OBF still set for the second byte", HYPE_PS2_STATUS_OUTPUT_FULL,
              status & HYPE_PS2_STATUS_OUTPUT_FULL);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &data);
    CHECK_HEX("second reset response byte is BAT-complete", HYPE_PS2_KBD_BAT_OK, data);

    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, &status);
    CHECK_HEX("OBF clears after both bytes read", 0, status & HYPE_PS2_STATUS_OUTPUT_FULL);
}

/* Status must never set the transmit-timeout bit (0x20) alongside OBF --
 * OVMF's read gate requires (bit5|bit0)==bit0. */
static void test_status_has_no_transmit_timeout_bit(void) {
    hype_ps2_kbd_t kbd;
    uint8_t status;
    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_enqueue_scancode(&kbd, 0x1Cu);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, &status);
    CHECK_HEX("transmit-timeout bit (0x20) never set", 0, status & 0x20u);
}

static void test_try_enqueue_appends_a_whole_sequence(void) {
    /* #301: the four scancodes of a shifted character must ALL survive. The clobbering
     * enqueue below left only the last one, so `sendkey A` delivered a bare shift
     * release -- a script that typed a different string than it asked for. */
    hype_ps2_kbd_t kbd;
    const uint8_t seq[4] = {0x2A, 0x1E, 0x9E, 0xAA}; /* shift, a, a-break, shift-break */
    unsigned int i;
    uint8_t got;

    hype_ps2_kbd_reset(&kbd);
    for (i = 0; i < 4u; i++) {
        CHECK_HEX("sequence byte accepted", 1,
                  hype_ps2_kbd_try_enqueue_scancode(&kbd, seq[i]));
    }
    for (i = 0; i < 4u; i++) {
        CHECK_HEX("read", 0, hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &got));
        CHECK_HEX("byte in order", seq[i], got);
    }
}

static void test_try_enqueue_refuses_when_full_instead_of_dropping(void) {
    /* Refusing is what lets the caller WAIT. Dropping is the bug. */
    hype_ps2_kbd_t kbd;
    unsigned int i;
    uint8_t got;

    hype_ps2_kbd_reset(&kbd);
    for (i = 0; i < HYPE_PS2_KBD_FIFO_SIZE; i++) {
        CHECK_HEX("fills up to capacity", 1,
                  hype_ps2_kbd_try_enqueue_scancode(&kbd, (uint8_t)(0x10u + i)));
    }
    CHECK_HEX("refuses past capacity", 0, hype_ps2_kbd_try_enqueue_scancode(&kbd, 0xFF));
    /* Nothing already queued was disturbed by the refusal. */
    CHECK_HEX("read", 0, hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &got));
    CHECK_HEX("oldest byte still first", 0x10u, got);
    /* And a drained slot makes room again. */
    CHECK_HEX("accepts after a drain", 1, hype_ps2_kbd_try_enqueue_scancode(&kbd, 0xFF));
}

static void test_clobbering_enqueue_is_unchanged(void) {
    /* The live-typist path keeps its behaviour: newest wins. Pinned so the #301 fix
     * cannot quietly change what a real keystroke does. */
    hype_ps2_kbd_t kbd;
    uint8_t got;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_enqueue_scancode(&kbd, 0x1E);
    hype_ps2_kbd_enqueue_scancode(&kbd, 0x30);
    CHECK_HEX("read", 0, hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &got));
    CHECK_HEX("newest keystroke wins", 0x30u, got);
    /* And nothing else is queued: the discarded first keystroke is gone, not pending. */
    CHECK_HEX("read", 0, hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_STATUS_COMMAND, &got));
    CHECK_HEX("OBF clear -- no second byte", 0, got & HYPE_PS2_STATUS_OUTPUT_FULL);
}

static void test_scancode_stats_distinguish_guest_reads(void) {
    hype_ps2_kbd_t kbd;
    unsigned long long queued = 99, read = 99, dropped = 99;
    uint8_t got;

    hype_ps2_kbd_reset(&kbd);
    hype_ps2_kbd_scancode_stats(&kbd, &queued, &read, &dropped);
    CHECK_HEX("stats reset queued", 0, queued);
    CHECK_HEX("stats reset read", 0, read);
    CHECK_HEX("stats reset dropped", 0, dropped);

    hype_ps2_kbd_enqueue_scancode(&kbd, 0x1Eu);
    hype_ps2_kbd_enqueue_scancode(&kbd, 0x9Eu);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &got);
    hype_ps2_kbd_io_write(&kbd, HYPE_PS2_PORT_DATA, 0xF4u);
    hype_ps2_kbd_io_read(&kbd, HYPE_PS2_PORT_DATA, &got);
    hype_ps2_kbd_scancode_stats(&kbd, &queued, &read, &dropped);
    CHECK_HEX("two scancodes queued", 2, queued);
    CHECK_HEX("one scancode read", 1, read);
    CHECK_HEX("overwritten scancode dropped", 1, dropped);

    /* A full append reports the refusal as a scancode drop. */
    while (hype_ps2_kbd_try_enqueue_scancode(&kbd, 0x20u)) { }
    CHECK_HEX("full append refused", 0, hype_ps2_kbd_try_enqueue_scancode(&kbd, 0x21u));
    hype_ps2_kbd_scancode_stats(&kbd, 0, 0, &dropped);
    CHECK_HEX("full refusals counted", 3, dropped);
}

int main(void) {
    test_try_enqueue_appends_a_whole_sequence();
    test_try_enqueue_refuses_when_full_instead_of_dropping();
    test_clobbering_enqueue_is_unchanged();
    test_scancode_stats_distinguish_guest_reads();
    test_reset_state();
    test_unrecognized_port_rejected();
    test_enqueue_scancode_sets_obf_and_reads_back();
    test_enqueue_overwrites_unread_byte();
    test_keyboard_reset_returns_ack_then_bat();
    test_status_has_no_transmit_timeout_bit();
    test_read_config_byte();
    test_write_config_byte_roundtrip();
    test_data_write_without_pending_config_command_is_a_keyboard_command();
    test_self_test();
    test_interface_test();
    test_disable_then_enable_keyboard_port();
    test_unrecognized_controller_command_is_ignored();
    test_disable_then_enable_aux_port();
    test_aux_port_test();
    test_write_to_aux_sets_one_shot_flag();
    test_has_pending_byte();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
