#include "ps2_host.h"

void hype_host_kbd_buffer_reset(hype_host_kbd_buffer_t *buf) {
    unsigned int i;

    for (i = 0; i < HYPE_HOST_KBD_BUFFER_SIZE; i++) {
        buf->buffer[i] = 0;
    }
    buf->head = 0;
    buf->count = 0;
}

void hype_host_kbd_buffer_push(hype_host_kbd_buffer_t *buf, uint8_t scancode) {
    unsigned int tail;

    if (buf->count >= HYPE_HOST_KBD_BUFFER_SIZE) {
        return; /* full -- drop rather than overwrite unread data or corrupt state */
    }
    tail = (buf->head + buf->count) % HYPE_HOST_KBD_BUFFER_SIZE;
    buf->buffer[tail] = scancode;
    buf->count++;
}

int hype_host_kbd_buffer_pop(hype_host_kbd_buffer_t *buf, uint8_t *out_scancode) {
    if (buf->count == 0) {
        return 0;
    }
    *out_scancode = buf->buffer[buf->head];
    buf->head = (buf->head + 1) % HYPE_HOST_KBD_BUFFER_SIZE;
    buf->count--;
    return 1;
}
