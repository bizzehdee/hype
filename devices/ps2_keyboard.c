#include "ps2_keyboard.h"

void hype_ps2_kbd_reset(hype_ps2_kbd_t *kbd) {
    kbd->out_head = 0;
    kbd->out_count = 0;
    kbd->config_byte = 0;
    kbd->awaiting_config_byte_write = 0;
    kbd->keyboard_port_enabled = 1;
    kbd->aux_port_enabled = 1;
    kbd->next_data_write_is_for_aux = 0;
}

/* Append one byte to the output FIFO (dropped if full -- never happens
 * in practice). */
static void push_output(hype_ps2_kbd_t *kbd, uint8_t value) {
    if (kbd->out_count >= HYPE_PS2_KBD_FIFO_SIZE) {
        return;
    }
    kbd->out_fifo[(kbd->out_head + kbd->out_count) % HYPE_PS2_KBD_FIFO_SIZE] = value;
    kbd->out_count++;
}

void hype_ps2_kbd_enqueue_scancode(hype_ps2_kbd_t *kbd, uint8_t scancode) {
    /* A key event overwrites any unread previous byte -- this project's
     * own single-pending-scancode scope (a real controller FIFOs them,
     * but nothing here sends scancodes faster than the guest reads). */
    kbd->out_head = 0;
    kbd->out_count = 0;
    push_output(kbd, scancode);
}

static void stage_response(hype_ps2_kbd_t *kbd, uint8_t value) {
    push_output(kbd, value);
}

int hype_ps2_kbd_io_read(hype_ps2_kbd_t *kbd, uint16_t port, uint8_t *out_value) {
    if (port == HYPE_PS2_PORT_DATA) {
        /* Pop the next queued byte; reading the data port clears OBF once
         * the FIFO drains (real firmware's poll loop depends on this). */
        if (kbd->out_count > 0) {
            *out_value = kbd->out_fifo[kbd->out_head];
            kbd->out_head = (kbd->out_head + 1) % HYPE_PS2_KBD_FIFO_SIZE;
            kbd->out_count--;
        } else {
            *out_value = 0;
        }
        return 0;
    }
    if (port == HYPE_PS2_PORT_STATUS_COMMAND) {
        /* Only SYSTEM_FLAG + (OBF when a byte waits). Deliberately never
         * sets the transmit-timeout bit (0x20): OVMF's Ps2KeyboardDxe
         * read gate requires (bit5|bit0)==bit0, so a set bit5 would make
         * it ignore a valid byte. */
        uint8_t status = HYPE_PS2_STATUS_SYSTEM_FLAG;
        if (kbd->out_count > 0) {
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
        case HYPE_PS2_CMD_DISABLE_AUX_PORT:
            kbd->aux_port_enabled = 0;
            break;
        case HYPE_PS2_CMD_ENABLE_AUX_PORT:
            kbd->aux_port_enabled = 1;
            break;
        case HYPE_PS2_CMD_TEST_AUX_PORT:
            stage_response(kbd, HYPE_PS2_AUX_TEST_PASSED);
            break;
        case HYPE_PS2_CMD_WRITE_TO_AUX:
            kbd->next_data_write_is_for_aux = 1;
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
        } else if (value == HYPE_PS2_KBD_CMD_RESET) {
            /* Keyboard reset: real firmware (OVMF's Ps2KeyboardDxe) waits
             * for ACK (0xFA) THEN BAT-complete (0xAA). Staging only the
             * ACK made it burn a ~1s poll timeout waiting for the BAT
             * byte -- the bulk of FW-1's PS/2 init spin. */
            stage_response(kbd, HYPE_PS2_KBD_ACK);
            stage_response(kbd, HYPE_PS2_KBD_BAT_OK);
        } else {
            /* Any other command byte sent to the keyboard device itself
             * (not the controller) -- generically ACKed, which is all
             * the init sequence's set-scancode/enable/LED commands wait
             * for. Never reached for an AUX-targeted write -- the exempt
             * glue routes those to devices/ps2_mouse.h instead, per
             * hype_ps2_kbd_take_aux_data_write()'s own comment. */
            stage_response(kbd, HYPE_PS2_KBD_ACK);
        }
        return 0;
    }

    return -1;
}

int hype_ps2_kbd_has_pending_byte(const hype_ps2_kbd_t *kbd) {
    return kbd->out_count > 0;
}

int hype_ps2_kbd_take_aux_data_write(hype_ps2_kbd_t *kbd) {
    int was_set = kbd->next_data_write_is_for_aux;
    kbd->next_data_write_is_for_aux = 0;
    return was_set;
}
