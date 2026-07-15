#include "ps2_mouse.h"

void hype_ps2_mouse_reset(hype_ps2_mouse_t *mouse) {
    unsigned int i;

    for (i = 0; i < HYPE_PS2_MOUSE_QUEUE_SIZE; i++) {
        mouse->queue[i] = 0;
    }
    mouse->head = 0;
    mouse->count = 0;
    mouse->reporting_enabled = 0;
}

static void enqueue_byte(hype_ps2_mouse_t *mouse, uint8_t value) {
    unsigned int tail;

    if (mouse->count >= HYPE_PS2_MOUSE_QUEUE_SIZE) {
        return; /* full -- shouldn't happen given this project's own scope, stay safe regardless */
    }
    tail = (mouse->head + mouse->count) % HYPE_PS2_MOUSE_QUEUE_SIZE;
    mouse->queue[tail] = value;
    mouse->count++;
}

int hype_ps2_mouse_has_pending_byte(const hype_ps2_mouse_t *mouse) {
    return mouse->count > 0;
}

uint8_t hype_ps2_mouse_read_byte(hype_ps2_mouse_t *mouse) {
    uint8_t value;

    if (mouse->count == 0) {
        return 0;
    }
    value = mouse->queue[mouse->head];
    mouse->head = (mouse->head + 1) % HYPE_PS2_MOUSE_QUEUE_SIZE;
    mouse->count--;
    return value;
}

void hype_ps2_mouse_write_command(hype_ps2_mouse_t *mouse, uint8_t command) {
    switch (command) {
    case HYPE_PS2_MOUSE_CMD_RESET:
        enqueue_byte(mouse, HYPE_PS2_MOUSE_ACK);
        enqueue_byte(mouse, HYPE_PS2_MOUSE_SELF_TEST_PASSED);
        enqueue_byte(mouse, HYPE_PS2_MOUSE_DEVICE_ID_STANDARD);
        mouse->reporting_enabled = 0;
        break;
    case HYPE_PS2_MOUSE_CMD_SET_DEFAULTS:
        enqueue_byte(mouse, HYPE_PS2_MOUSE_ACK);
        mouse->reporting_enabled = 0;
        break;
    case HYPE_PS2_MOUSE_CMD_ENABLE_REPORTING:
        enqueue_byte(mouse, HYPE_PS2_MOUSE_ACK);
        mouse->reporting_enabled = 1;
        break;
    case HYPE_PS2_MOUSE_CMD_DISABLE_REPORTING:
        enqueue_byte(mouse, HYPE_PS2_MOUSE_ACK);
        mouse->reporting_enabled = 0;
        break;
    case HYPE_PS2_MOUSE_CMD_GET_DEVICE_ID:
        enqueue_byte(mouse, HYPE_PS2_MOUSE_ACK);
        enqueue_byte(mouse, HYPE_PS2_MOUSE_DEVICE_ID_STANDARD);
        break;
    default:
        /* Unrecognized command -- still ACKed generically, matching
         * devices/ps2_keyboard.h's own tolerance for commands this
         * stub doesn't implement more specifically. */
        enqueue_byte(mouse, HYPE_PS2_MOUSE_ACK);
        break;
    }
}

void hype_ps2_mouse_enqueue_movement(hype_ps2_mouse_t *mouse, uint8_t status, uint8_t dx, uint8_t dy) {
    if (!mouse->reporting_enabled) {
        return;
    }
    enqueue_byte(mouse, status);
    enqueue_byte(mouse, dx);
    enqueue_byte(mouse, dy);
}
