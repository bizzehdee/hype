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

/*
 * INPUT-1: the real 8259 INTA-cycle operation an actual interrupt
 * controller performs on the CPU's behalf -- finds the
 * highest-priority (lowest-numbered, this project's own scope: no
 * rotating-priority modes modeled) IRQ that's both pending (IRR) and
 * unmasked (IMR clear), moves it from IRR to ISR (now "in service"
 * until the guest's own EOI, matching real hardware), and returns the
 * actual vector to deliver: `chip->irq_offset` (ICW2's own vector base
 * -- already tracked, just never consumed until now) plus the IRQ
 * number. Returns 1 and fills *out_vector if such an IRQ exists, 0
 * otherwise (nothing to deliver -- every pending IRQ is masked, or
 * there are none). Pure logic, no CPU/guest-memory access -- actually
 * injecting the returned vector into a guest is the exempt glue's job
 * (arch/x86_64/svm/svm_vcpu.c's hype_svm_vcpu_request_interrupt(),
 * INT-1/INT-2).
 */
int hype_pic_emu_acknowledge_highest_priority(hype_pic_emu_chip_t *chip, uint8_t *out_vector);

/* Raises a global (0-15) IRQ line across the master/slave pair: 0-7 go
 * on the master's IRR, 8-15 on the slave's IRR bit (global-8). The
 * slave's cascade into master IR2 is resolved at acknowledge time (see
 * hype_pic_emu_acknowledge), so this just sets the right IRR bit. No-op
 * if global_irq > 15. (M4-6d2: an AHCI controller whose PCI Interrupt
 * Line firmware routed to a slave line, e.g. 11, needs the slave path.) */
void hype_pic_emu_raise_global_irq(hype_pic_emu_t *pic, uint8_t global_irq);

/*
 * Cascade-aware INTA acknowledge across the master/slave pair. Resolves
 * the highest-priority pending+unmasked IRQ the way a real cascaded
 * 8259 pair does: the slave's INT feeds the master's IR2, so a slave IRQ
 * only reaches the CPU when master IR2 is also unmasked, and master
 * priorities interleave (IR0, IR1, then the slave's IR8..IR15 via IR2,
 * then IR3..IR7). On success moves the winning IRQ from IRR to ISR (and,
 * for a slave IRQ, also sets master ISR IR2 -- the guest EOIs both
 * chips), fills *out_vector with the delivering chip's irq_offset + its
 * local IRQ, and returns 1. Returns 0 if nothing is deliverable. Pure
 * logic; injecting the vector stays the exempt glue's job.
 */
int hype_pic_emu_acknowledge(hype_pic_emu_t *pic, uint8_t *out_vector);

/*
 * M4-6d4: priority-aware "is anything deliverable right now?" predicate for
 * the dispatch loop. Real 8259 fully-nested mode delivers the highest-
 * priority pending+unmasked IRQ whenever it OUTRANKS whatever is currently
 * in service -- a higher-priority line preempts a lower-priority one that's
 * mid-service (the CPU takes the nested interrupt once it can). The old loop
 * gated all delivery on "both ISRs completely clear", which serialized every
 * interrupt and let a lower-priority in-service line (e.g. an AHCI completion
 * IRQ on a slave line during a CD-read storm) block the highest-priority
 * timer tick IRQ0 -- starving the guest's scheduler tick. This returns 1 iff
 * there is a pending, unmasked, cascade-reachable IRQ whose priority is
 * strictly higher than the highest-priority IRQ currently in service (or
 * nothing is in service). Priority order matches the 8259: master IR0 (top),
 * IR1, then the slave's IR8..IR15 via the IR2 cascade, then master IR3..IR7.
 * Pure logic; the caller still injects through the IF/shadow-respecting
 * request path, so this only removes the artificial serialization -- it never
 * forces an interrupt the guest can't yet accept.
 */
int hype_pic_emu_has_deliverable(const hype_pic_emu_t *pic);

#endif /* HYPE_DEVICES_PIC_H */
