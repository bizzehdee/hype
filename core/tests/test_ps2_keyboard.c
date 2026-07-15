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

int main(void) {
    test_reset_state();
    test_unrecognized_port_rejected();
    test_enqueue_scancode_sets_obf_and_reads_back();
    test_enqueue_overwrites_unread_byte();
    test_read_config_byte();
    test_write_config_byte_roundtrip();
    test_data_write_without_pending_config_command_is_a_keyboard_command();
    test_self_test();
    test_interface_test();
    test_disable_then_enable_keyboard_port();
    test_unrecognized_controller_command_is_ignored();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
