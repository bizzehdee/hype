#ifndef HYPE_ARCH_TIMER_H
#define HYPE_ARCH_TIMER_H

#include <stdint.h>

#include "isr.h"

/* PIC remap base chosen for M1-8: IRQ0 (the PIT) lands on vector 32,
 * matching the convention observed from OVMF's own LAPIC timer while
 * validating M1-5 (see lapic.h) -- keeps this project's vector map the
 * same one every OS-dev PIC-remap tutorial uses. */
#define HYPE_TIMER_VECTOR 32
#define HYPE_TIMER_IRQ 0

/* Monotonically increasing tick count, incremented once per PIT
 * interrupt. Pure state, fully testable. */
uint64_t hype_timer_get_ticks(void);
void hype_timer_tick(void);

/*
 * The registered ISR handler for HYPE_TIMER_VECTOR (see
 * hype_isr_register() in isr.h) -- increments the tick count and sends
 * the PIC an EOI, then returns normally (isr_stubs.S resumes whatever
 * was interrupted). Exempt from unit testing: it calls hype_pic_send_eoi()
 * (real outb, see pic.h), so there's nothing to observe without a real
 * device. hype_timer_tick() above holds the only real logic and is
 * fully tested.
 */
void hype_timer_isr(const hype_isr_frame_t *frame);

#endif /* HYPE_ARCH_TIMER_H */
