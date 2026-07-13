#ifndef HYPE_ARCH_LAPIC_H
#define HYPE_ARCH_LAPIC_H

#include <stdint.h>

/*
 * Local APIC timer masking (M1-8). We use the legacy PIT for the
 * host's own timer tick, not the LAPIC timer -- but firmware (OVMF)
 * was observed, while validating M1-5, to leave its own LAPIC timer
 * armed and firing at vector 32 even after ExitBootServices(). Left
 * alone, that would collide with whatever we assign vector 32 to next
 * (the PIT's remapped IRQ0). Masking it explicitly avoids relying on
 * "firmware happened to leave it in a state that doesn't bite us."
 *
 * Unlike pic.c's port I/O, the LAPIC is memory-mapped -- a plain
 * pointer read/write, so this is fully unit-testable against an
 * ordinary buffer standing in for the real MMIO region. Production
 * code passes the real default base (HYPE_LAPIC_DEFAULT_BASE); nothing
 * here hardcodes it.
 */

#define HYPE_LAPIC_DEFAULT_BASE 0xFEE00000ULL
#define HYPE_LAPIC_LVT_TIMER_OFFSET 0x320
#define HYPE_LAPIC_LVT_MASKED (1u << 16)

/* Sets the mask bit in the LVT Timer register, leaving every other bit
 * (vector, timer mode, ...) untouched. `lapic_base` must point at the
 * 4KB LAPIC register window (real hardware: HYPE_LAPIC_DEFAULT_BASE,
 * identity-mapped; tests: any suitably sized buffer). */
void hype_lapic_mask_timer(volatile uint32_t *lapic_base);

#endif /* HYPE_ARCH_LAPIC_H */
