#include "ps2_keyboard.h"

void hype_ps2_kbd_reset(hype_ps2_kbd_t *kbd) {
    kbd->data_buffer = 0;
    kbd->output_buffer_full = 0;
    kbd->config_byte = 0;
    kbd->awaiting_config_byte_write = 0;
    kbd->keyboard_port_enabled = 1;
}

void hype_ps2_kbd_enqueue_scancode(hype_ps2_kbd_t *kbd, uint8_t scancode) {
    kbd->data_buffer = scancode;
    kbd->output_buffer_full = 1;
}

static void stage_response(hype_ps2_kbd_t *kbd, uint8_t value) {
    kbd->data_buffer = value;
    kbd->output_buffer_full = 1;
}

int hype_ps2_kbd_io_read(hype_ps2_kbd_t *kbd, uint16_t port, uint8_t *out_value) {
    if (port == HYPE_PS2_PORT_DATA) {
        *out_value = kbd->data_buffer;
        kbd->output_buffer_full = 0;
        return 0;
    }
    if (port == HYPE_PS2_PORT_STATUS_COMMAND) {
        uint8_t status = HYPE_PS2_STATUS_SYSTEM_FLAG;
        if (kbd->output_buffer_full) {
            status |= HYPE_PS2_STATUS_OUTPUT_FULL;
        }
        *out_value = status;
        return 0;
    }
    return -1;
}

int hype_ps2_kbd_io_write(hype_ps2_kbd_t *kbd, uint16_t port, uint8_t value) {
    if (port == HYPE_PS2_PORT_STATUS_COMMAND) {
        switch (value) {
        case HYPE_PS2_CMD_READ_CONFIG_BYTE:
            stage_response(kbd, kbd->config_byte);
            break;
        case HYPE_PS2_CMD_WRITE_CONFIG_BYTE:
            kbd->awaiting_config_byte_write = 1;
            break;
        case HYPE_PS2_CMD_DISABLE_KEYBOARD_PORT:
            kbd->keyboard_port_enabled = 0;
            break;
        case HYPE_PS2_CMD_ENABLE_KEYBOARD_PORT:
            kbd->keyboard_port_enabled = 1;
            break;
        case HYPE_PS2_CMD_SELF_TEST:
            stage_response(kbd, HYPE_PS2_SELF_TEST_PASSED);
            break;
        case HYPE_PS2_CMD_INTERFACE_TEST:
            stage_response(kbd, HYPE_PS2_INTERFACE_TEST_PASSED);
            break;
        default:
            /* Unrecognized controller command -- silently ignored,
             * matching real hardware's own tolerance of commands a
             * given controller revision doesn't implement. */
            break;
        }
        return 0;
    }

    if (port == HYPE_PS2_PORT_DATA) {
        if (kbd->awaiting_config_byte_write) {
            kbd->config_byte = value;
            kbd->awaiting_config_byte_write = 0;
        } else {
            /* A command byte sent to the keyboard device itself (not
             * the controller) -- generically ACKed, see this file's
             * own header comment for why nothing more specific is
             * modeled yet. */
            stage_response(kbd, HYPE_PS2_KBD_ACK);
        }
        return 0;
    }

    return -1;
}
