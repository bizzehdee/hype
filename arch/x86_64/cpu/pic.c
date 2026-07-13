#include "pic.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT_ICW4 0x11
#define ICW4_8086 0x01

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void hype_pic_remap_and_mask_all(uint8_t master_vector_base) {
    uint8_t slave_vector_base = (uint8_t)(master_vector_base + 8);

    outb(PIC1_CMD, ICW1_INIT_ICW4);
    outb(PIC2_CMD, ICW1_INIT_ICW4);
    outb(PIC1_DATA, master_vector_base);
    outb(PIC2_DATA, slave_vector_base);
    outb(PIC1_DATA, 0x04); /* slave attached on IRQ2 (bitmask notation) */
    outb(PIC2_DATA, 0x02); /* cascade identity 2 (slave's own notation) */
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void hype_pic_unmask_irq(uint8_t irq) {
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(irq < 8 ? irq : irq - 8);
    uint8_t mask = inb(port);

    mask = (uint8_t)(mask & ~(1u << bit));
    outb(port, mask);
}

void hype_pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}
