#include "pic.h"
#include "ps2_host.h"

#define HYPE_PS2_HOST_PORT_DATA 0x60u

static hype_host_kbd_buffer_t g_host_kbd_buffer;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void hype_host_kbd_init(void) {
    hype_host_kbd_buffer_reset(&g_host_kbd_buffer);
    hype_isr_register(HYPE_HOST_KBD_VECTOR, hype_host_kbd_isr);
    hype_pic_unmask_irq(HYPE_HOST_KBD_IRQ);
}

void hype_host_kbd_isr(const hype_isr_frame_t *frame) {
    uint8_t scancode = inb(HYPE_PS2_HOST_PORT_DATA);
    (void)frame;
    hype_host_kbd_buffer_push(&g_host_kbd_buffer, scancode);
    hype_pic_send_eoi(HYPE_HOST_KBD_IRQ);
}

int hype_host_kbd_poll_scancode(uint8_t *out_scancode) {
    return hype_host_kbd_buffer_pop(&g_host_kbd_buffer, out_scancode);
}
