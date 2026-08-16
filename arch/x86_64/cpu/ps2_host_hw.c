#include "pic.h"
#include "ps2_host.h"

#define HYPE_PS2_HOST_PORT_DATA 0x60u
/* #218: the i8042 status port. Bit 0 (OBF) means a byte is waiting in the data port; bit 5
 * distinguishes a mouse byte from a keyboard byte on a controller with a second channel. */
#define HYPE_PS2_HOST_PORT_STATUS 0x64u
#define HYPE_PS2_HOST_STATUS_OBF 0x01u
#define HYPE_PS2_HOST_STATUS_AUX 0x20u

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

/* #218: bytes taken by the polling fallback, so a hardware run can tell "the poll found
 * nothing" from "the poll never ran" -- the distinction that cost this bug a whole session. */
static uint64_t g_kbd_polled_bytes;

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

/*
 * #218: drain the i8042 output buffer by POLLING, with no dependency on IRQ1.
 *
 * The host keyboard was interrupt-only: hype_host_kbd_isr() pushed into the buffer and
 * hype_host_kbd_poll_scancode() popped from it. On the operator's laptop IRQ1 never fires --
 * a full real-hardware run measured isr_entries=0 eois=0 -- so that buffer stayed empty and
 * no key ever reached hype. USB HID was the workaround, but the same run's inventory found no
 * HID device at all (a mass-storage stick, a card reader, Bluetooth and a camera), so with
 * both paths dead the machine had no keyboard input whatsoever.
 *
 * Polling the status port needs no interrupt routing, no PIC unmask and no IOAPIC entry, which
 * is what makes it work where the ISR does not. Bytes are pushed into the SAME buffer the ISR
 * uses, so ordering and the existing consumer are unchanged, and a machine where IRQ1 DOES
 * work simply finds the buffer already filled.
 *
 * Bounded per call: a stuck controller that always reports OBF must not spin here forever --
 * this runs on the BSP's render/input loop, which also drives the dashboard.
 */
static void host_kbd_drain_polled(void) {
    unsigned guard;
    for (guard = 0; guard < 16u; guard++) {
        uint8_t st = inb(HYPE_PS2_HOST_PORT_STATUS);
        uint8_t data;
        if ((st & HYPE_PS2_HOST_STATUS_OBF) == 0u) {
            return; /* nothing waiting */
        }
        data = inb(HYPE_PS2_HOST_PORT_DATA);
        if ((st & HYPE_PS2_HOST_STATUS_AUX) != 0u) {
            continue; /* mouse byte: consumed so it cannot block the keyboard, then dropped */
        }
        g_kbd_polled_bytes++;
        hype_host_kbd_buffer_push(&g_host_kbd_buffer, data);
    }
}

int hype_host_kbd_poll_scancode(uint8_t *out_scancode) {
    if (hype_host_kbd_buffer_pop(&g_host_kbd_buffer, out_scancode)) {
        return 1;
    }
    host_kbd_drain_polled();
    return hype_host_kbd_buffer_pop(&g_host_kbd_buffer, out_scancode);
}

uint64_t hype_host_kbd_polled_bytes(void) { return g_kbd_polled_bytes; }

void hype_host_kbd_inject_scancode(uint8_t scancode) {
    /* Same buffer the ISR pushes into -- see the header for why USB HID joins the
     * queue rather than adding a parallel path. */
    hype_host_kbd_buffer_push(&g_host_kbd_buffer, scancode);
}
