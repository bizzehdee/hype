#ifndef HYPE_DEVICES_PIC_H
#define HYPE_DEVICES_PIC_H

#include <stdint.h>

/*
 * Guest-visible 8259 PIC emulation (M3-4). NOT arch/x86_64/cpu/pic.c,
 * which reprograms the HOST's own real PIC for this hypervisor's own
 * timer IRQ -- this is a from-scratch register-level model that a
 * guest's port I/O (0x20/0x21 master, 0xA0/0xA1 slave) gets routed to
 * once an SVM IOIO intercept exists to route it (M3-5's job), so a
 * guest's own PIC initialization sequence (remap+mask, something
 * every real x86 OS does early in boot) completes correctly instead
 * of hanging or reading back garbage.
 *
 * "Stub" scope: correct ICW1-4 initialization sequencing, OCW1 mask
 * read/write, OCW2 EOI (non-specific and specific), and OCW3 IRR/ISR
 * read-select -- exactly what a guest's own init/mask/EOI code paths
 * touch. No real interrupt injection back into the guest exists yet
 * (a separate, later feature); hype_pic_emu_raise_irq() below just
 * sets an IRR bit directly (matching real 8259 semantics: IRR
 * reflects a pending request regardless of masking) for whenever that
 * wiring lands, and for testing OCW3's read-select behavior now.
 */

typedef struct {
    uint8_t imr;             /* Interrupt Mask Register (OCW1) */
    uint8_t irq_offset;      /* vector base programmed via ICW2 */
    uint8_t init_state;      /* 0 = normal operation; 1/2/3 = expecting ICW2/ICW3/ICW4 next */
    uint8_t expect_icw4;     /* ICW1 bit 0 (IC4) */
    uint8_t is_cascade;      /* ICW1 bit 1 (SNGL) inverted -- 1 = cascade mode (ICW3 expected) */
    uint8_t read_isr_select; /* OCW3 bit 0 (RIS), only meaningful with OCW3 bit 1 (RR) set */
    uint8_t irr;             /* Interrupt Request Register */
    uint8_t isr;             /* In-Service Register */
} hype_pic_emu_chip_t;

typedef struct {
    hype_pic_emu_chip_t master; /* ports 0x20 (command)/0x21 (data) */
    hype_pic_emu_chip_t slave;  /* ports 0xA0 (command)/0xA1 (data) */
} hype_pic_emu_t;

/* Resets both chips to their post-power-on state (fully masked,
 * uninitialized) -- call on every (re)start, same as every other
 * guest-visible state in this project. */
void hype_pic_emu_reset(hype_pic_emu_t *pic);

/*
 * Handles a guest OUT to one of the four PIC ports. Returns 0 if
 * `port` is one of the four this device owns (and so was handled),
 * non-zero otherwise.
 */
int hype_pic_emu_io_write(hype_pic_emu_t *pic, uint16_t port, uint8_t value);

/*
 * Handles a guest IN from one of the four PIC ports, filling
 * *out_value. Returns 0 if `port` is recognized, non-zero otherwise.
 */
int hype_pic_emu_io_read(hype_pic_emu_t *pic, uint16_t port, uint8_t *out_value);

/* Sets IRR bit `irq` (0-7) on `chip` -- a pending request, independent
 * of IMR (matching real 8259 semantics; masking only affects whether
 * it's ever signaled to the CPU, not IRR's own value). No-op if irq >
 * 7. */
void hype_pic_emu_raise_irq(hype_pic_emu_chip_t *chip, uint8_t irq);

#endif /* HYPE_DEVICES_PIC_H */
