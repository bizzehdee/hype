#ifndef HYPE_ARCH_PIC_H
#define HYPE_ARCH_PIC_H

#include <stdint.h>

/*
 * Legacy 8259 PIC (M1-8): remapped so its IRQ vectors don't collide
 * with the SDM's 0-31 exception range, then fully masked -- the only
 * line we unmask is the one thing we actually handle (the PIT's IRQ0).
 *
 * Fully exempt from unit testing per AGENTS.md, same as serial_hw.c:
 * this is a fixed sequence of outb/inb calls with no separable pure
 * logic (unlike e.g. serial.c's baud divisor, there's no per-call
 * arithmetic here worth pulling out -- the ICW byte values are fixed
 * constants dictated by the 8259's init protocol, not computed).
 */

/* Remaps the master/slave PICs' IRQ0-7/8-15 to the given vector bases
 * (master_base for IRQ0-7, master_base+8 for IRQ8-15 -- the SDM/8259
 * convention groups them contiguously), then masks every line. */
void hype_pic_remap_and_mask_all(uint8_t master_vector_base);

/* Unmasks a single IRQ line (0-15). */
void hype_pic_unmask_irq(uint8_t irq);

/* Sends End-Of-Interrupt for the given IRQ (0-15) -- needed on the
 * slave PIC too when irq >= 8, per the 8259's cascade wiring. */
void hype_pic_send_eoi(uint8_t irq);

/* Read the master/slave In-Service Register (OCW3). RT-2b: the spurious-IRQ
 * handler checks the top bit to tell a real IRQ7/IRQ15 (bit set -> EOI) from
 * a spurious one (bit clear -> no EOI) so a masked line firing spuriously on
 * real hardware never reaches hype_fatal(). */
uint8_t hype_pic_read_master_isr(void);
uint8_t hype_pic_read_slave_isr(void);

#endif /* HYPE_ARCH_PIC_H */
