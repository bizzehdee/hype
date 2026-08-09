#include "pic.h"
#include "ps2_host.h"

#define HYPE_PS2_HOST_PORT_DATA 0x60u

static hype_host_kbd_buffer_t g_host_kbd_buffer;

/*
 * #363: is the host keyboard interrupt still being SERVICED, and on which core?
 *
 * Two fixes to this ticket addressed the wrong layer -- rendering, then poll location -- because I
 * reasoned about where code runs instead of measuring where the interrupt lands. Polling is
 * non-blocking and does no I/O; it pops a buffer that only this ISR fills. So if the ISR stops
 * being entered, every core polls an empty buffer forever and the keyboard is dead machine-wide,
 * whichever core is doing the polling.
 *
 * The suspected mechanism is that a host IRQ arriving while an AP runs a guest is serviced ON that
 * AP, so a wedged AP leaves the 8259 without an EOI and no further keyboard interrupt is delivered
 * anywhere. That is a hypothesis. These counters test it: if `entries` stops advancing at the
 * moment the guest wedges, it is right and the fix is to route IRQ1 at the BSP; if it keeps
 * advancing, the fault is downstream and a routing change would be wasted work.
 *
 * `last_apic` names the core that most recently ran the ISR, which is the part that distinguishes
 * "serviced somewhere" from "serviced on the core that later died".
 */
static volatile unsigned long long g_kbd_isr_entries;
static volatile unsigned long long g_kbd_isr_eois;
static volatile unsigned int g_kbd_isr_last_apic;

static inline unsigned int kbd_this_apic(void) {
    return (*(volatile uint32_t *)(uintptr_t)0xFEE00020u) >> 24;
}

void hype_host_kbd_isr_stats(unsigned long long *entries, unsigned long long *eois,
                             unsigned int *last_apic) {
    if (entries != 0) *entries = g_kbd_isr_entries;
    if (eois != 0) *eois = g_kbd_isr_eois;
    if (last_apic != 0) *last_apic = g_kbd_isr_last_apic;
}

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
    g_kbd_isr_entries++;
    g_kbd_isr_last_apic = kbd_this_apic();
    hype_host_kbd_buffer_push(&g_host_kbd_buffer, scancode);
    hype_pic_send_eoi(HYPE_HOST_KBD_IRQ);
    g_kbd_isr_eois++;
}

int hype_host_kbd_poll_scancode(uint8_t *out_scancode) {
    return hype_host_kbd_buffer_pop(&g_host_kbd_buffer, out_scancode);
}

void hype_host_kbd_inject_scancode(uint8_t scancode) {
    /* Same buffer the ISR pushes into -- see the header for why USB HID joins the
     * queue rather than adding a parallel path. */
    hype_host_kbd_buffer_push(&g_host_kbd_buffer, scancode);
}
