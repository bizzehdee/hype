#include "ps2_keyboard.h"

void hype_ps2_kbd_reset(hype_ps2_kbd_t *kbd) {
    unsigned int i;
    kbd->out_head = 0;
    kbd->out_count = 0;
    kbd->config_byte = 0;
    kbd->awaiting_config_byte_write = 0;
    kbd->keyboard_port_enabled = 1;
    kbd->aux_port_enabled = 1;
    kbd->next_data_write_is_for_aux = 0;
    kbd->irq_edges = 0;
    __atomic_store_n(&kbd->scancodes_queued, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&kbd->scancodes_read, 0ull, __ATOMIC_RELAXED);
    __atomic_store_n(&kbd->scancodes_dropped, 0ull, __ATOMIC_RELAXED);
    for (i = 0; i < HYPE_PS2_KBD_FIFO_SIZE; i++) {
        kbd->out_fifo[i] = 0;
        kbd->out_is_scancode[i] = 0;
    }
}

/* Append one byte to the output FIFO (dropped if full -- never happens
 * in practice). */
static void push_output(hype_ps2_kbd_t *kbd, uint8_t value, int is_scancode) {
    unsigned int slot;
    if (kbd->out_count >= HYPE_PS2_KBD_FIFO_SIZE) {
        if (is_scancode) {
            __atomic_add_fetch(&kbd->scancodes_dropped, 1ull, __ATOMIC_RELAXED);
        }
        return;
    }
    slot = (kbd->out_head + kbd->out_count) % HYPE_PS2_KBD_FIFO_SIZE;
    kbd->out_fifo[slot] = value;
    kbd->out_is_scancode[slot] = (uint8_t)(is_scancode != 0);
    kbd->out_count++;
    if (kbd->out_count == 1u && kbd->irq_edges < HYPE_PS2_KBD_FIFO_SIZE) {
        kbd->irq_edges++; /* #389: this byte just became the readable head */
    }
    if (is_scancode) {
        __atomic_add_fetch(&kbd->scancodes_queued, 1ull, __ATOMIC_RELAXED);
    }
}

void hype_ps2_kbd_enqueue_scancode(hype_ps2_kbd_t *kbd, uint8_t scancode) {
    /*
     * A live typist outrunning the guest: keep the NEWEST byte, since a keystroke the
     * guest is too slow to collect is better lost than delivered minutes late. Callers
     * that must not lose a byte -- anything sending a multi-byte sequence, where losing
     * one produces a different keystroke -- use try_enqueue below instead.
     */
    unsigned int i;
    for (i = 0; i < kbd->out_count; i++) {
        unsigned int slot = (kbd->out_head + i) % HYPE_PS2_KBD_FIFO_SIZE;
        if (kbd->out_is_scancode[slot]) {
            __atomic_add_fetch(&kbd->scancodes_dropped, 1ull, __ATOMIC_RELAXED);
        }
    }
    kbd->out_head = 0;
    kbd->out_count = 0;
    push_output(kbd, scancode, 1);
}

int hype_ps2_kbd_try_enqueue_scancode(hype_ps2_kbd_t *kbd, uint8_t scancode) {
    if (kbd->out_count >= HYPE_PS2_KBD_FIFO_SIZE) {
        __atomic_add_fetch(&kbd->scancodes_dropped, 1ull, __ATOMIC_RELAXED);
        return 0;
    }
    push_output(kbd, scancode, 1);
    return 1;
}

static void stage_response(hype_ps2_kbd_t *kbd, uint8_t value) {
    push_output(kbd, value, 0);
}

int hype_ps2_kbd_io_read(hype_ps2_kbd_t *kbd, uint16_t port, uint8_t *out_value) {
    if (port == HYPE_PS2_PORT_DATA) {
        /* Pop the next queued byte; reading the data port clears OBF once
         * the FIFO drains (real firmware's poll loop depends on this). */
        if (kbd->out_count > 0) {
            if (kbd->out_is_scancode[kbd->out_head]) {
                __atomic_add_fetch(&kbd->scancodes_read, 1ull, __ATOMIC_RELAXED);
            }
            *out_value = kbd->out_fifo[kbd->out_head];
            kbd->out_head = (kbd->out_head + 1) % HYPE_PS2_KBD_FIFO_SIZE;
            kbd->out_count--;
            if (kbd->out_count > 0 && kbd->irq_edges < HYPE_PS2_KBD_FIFO_SIZE) {
                kbd->irq_edges++; /* #389: the next byte is now the readable head */
            }
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

int hype_ps2_kbd_guest_initialized(const hype_ps2_kbd_t *kbd) {
    return kbd->guest_wrote != 0;
}

int hype_ps2_kbd_io_write(hype_ps2_kbd_t *kbd, uint16_t port, uint8_t value) {
    kbd->guest_wrote = 1;
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
        } else if (kbd->awaiting_scancode_set_param) {
            /* #436: 0xF0 parameter. 0 = query -> ACK + current set (2);
             * 1/2/3 = select -> ACK alone. */
            kbd->awaiting_scancode_set_param = 0;
            stage_response(kbd, HYPE_PS2_KBD_ACK);
            if (value == 0u) {
                stage_response(kbd, 0x02u);
            }
        } else if (value == 0xF2u) {
            /* #436: READ ID. A bare ACK left OVMF's Ps2KeyboardDxe waiting for
             * the two ID bytes until its timeout -- it then declared the
             * keyboard absent and never installed its ConIn poll, so no host
             * key could ever reach a firmware prompt. Real MF2 keyboard ID. */
            stage_response(kbd, HYPE_PS2_KBD_ACK);
            stage_response(kbd, 0xABu);
            stage_response(kbd, 0x83u);
        } else if (value == 0xF0u) {
            kbd->awaiting_scancode_set_param = 1;
            stage_response(kbd, HYPE_PS2_KBD_ACK);
        } else if (value == 0xEEu) {
            /* Echo: answers 0xEE, not ACK. */
            stage_response(kbd, 0xEEu);
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

void hype_ps2_kbd_scancode_stats(const hype_ps2_kbd_t *kbd,
                                 unsigned long long *queued,
                                 unsigned long long *read,
                                 unsigned long long *dropped) {
    if (queued != 0) {
        *queued = __atomic_load_n(&kbd->scancodes_queued, __ATOMIC_RELAXED);
    }
    if (read != 0) {
        *read = __atomic_load_n(&kbd->scancodes_read, __ATOMIC_RELAXED);
    }
    if (dropped != 0) {
        *dropped = __atomic_load_n(&kbd->scancodes_dropped, __ATOMIC_RELAXED);
    }
}

int hype_ps2_kbd_take_irq(hype_ps2_kbd_t *kbd) {
    if (kbd->irq_edges == 0u) {
        return 0;
    }
    kbd->irq_edges--;
    return 1;
}
