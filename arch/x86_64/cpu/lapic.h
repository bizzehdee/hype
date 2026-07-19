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
#define HYPE_LAPIC_LVT_TIMER_PERIODIC (1u << 17) /* LVT timer mode = periodic */

/* M8-0b-ii (inc 5): host-LAPIC registers hype programs on the AP so the guest
 * on the dedicated core gets a periodic forced VMEXIT (its timer-delivery
 * mechanism, replacing the BSP's PIT tick which the AP doesn't receive). */
#define HYPE_LAPIC_EOI_OFFSET 0xB0
#define HYPE_LAPIC_SVR_OFFSET 0xF0
#define HYPE_LAPIC_SVR_ENABLE (1u << 8) /* APIC software-enable */
#define HYPE_LAPIC_TIMER_INITIAL_OFFSET 0x380
#define HYPE_LAPIC_TIMER_CURRENT_OFFSET 0x390
#define HYPE_LAPIC_TIMER_DIVIDE_OFFSET 0x3E0
#define HYPE_LAPIC_TIMER_DIVIDE_16 0x3u /* DCR encoding for divide-by-16 */

/* Sets the mask bit in the LVT Timer register, leaving every other bit
 * (vector, timer mode, ...) untouched. `lapic_base` must point at the
 * 4KB LAPIC register window (real hardware: HYPE_LAPIC_DEFAULT_BASE,
 * identity-mapped; tests: any suitably sized buffer). */
void hype_lapic_mask_timer(volatile uint32_t *lapic_base);

/* LVT Timer register value for a periodic timer on `vector`: vector in the
 * low 8 bits, periodic mode bit set, unmasked. Pure -- unit tested. */
uint32_t hype_lapic_lvt_timer_periodic(uint8_t vector);

/*
 * M8-0b: Interrupt Command Register (ICR) IPI send, for bringing a second
 * application processor (AP) up post-ExitBootServices via the standard
 * INIT-SIPI-SIPI sequence (firmware MP services don't survive EBS, so hype
 * issues the IPIs itself). The ICR is a 64-bit register split across two
 * MMIO dwords: ICR_HIGH (0x310) holds the destination APIC ID (bits 31:24 in
 * physical-destination mode); writing ICR_LOW (0x300) latches and sends.
 *
 * The dword *encoders* are pure (no MMIO) so their exact bit layout is
 * unit-tested; only the write+wait sequence (hype_lapic_send_ipi) touches
 * real MMIO and is exempt.
 */
#define HYPE_LAPIC_ICR_LOW_OFFSET 0x300
#define HYPE_LAPIC_ICR_HIGH_OFFSET 0x310
#define HYPE_LAPIC_ICR_DELIVERY_STATUS (1u << 12) /* bit 12: 1 = send pending */

/* ICR_HIGH value targeting a single processor by physical APIC id. */
uint32_t hype_lapic_icr_high(uint8_t apic_id);

/* ICR_LOW for "INIT, assert, edge" -- resets the target AP to a wait-for-SIPI
 * state (delivery mode 0b101, level=assert). Standard value 0x00004500. */
uint32_t hype_lapic_icr_low_init(void);

/* ICR_LOW for a Startup IPI (delivery mode 0b110, assert). The 8-bit vector
 * is the trampoline's physical page number (entry address >> 12), so the AP
 * begins executing in real mode at (vector << 12). Standard 0x00004600|vec. */
uint32_t hype_lapic_icr_low_sipi(uint8_t trampoline_page);

/* Sends one IPI: writes ICR_HIGH (destination) then ICR_LOW (latches/sends),
 * then spins until the delivery-status bit clears. `lapic_base` is the 4KB
 * register window (HYPE_LAPIC_DEFAULT_BASE on real HW). Exempt glue. */
void hype_lapic_send_ipi(volatile uint32_t *lapic_base, uint32_t icr_high, uint32_t icr_low);

#endif /* HYPE_ARCH_LAPIC_H */
