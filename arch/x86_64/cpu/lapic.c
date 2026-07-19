#include "lapic.h"

void hype_lapic_mask_timer(volatile uint32_t *lapic_base) {
    volatile uint8_t *base_bytes = (volatile uint8_t *)lapic_base;
    volatile uint32_t *lvt_timer = (volatile uint32_t *)(base_bytes + HYPE_LAPIC_LVT_TIMER_OFFSET);

    *lvt_timer = *lvt_timer | HYPE_LAPIC_LVT_MASKED;
}

/* ICR_LOW delivery-mode field (bits 10:8) + level=assert (bit 14). These are
 * the canonical Intel/AMD MP-init values: INIT-assert = 0x00004500,
 * SIPI = 0x00004600 | vector. */
#define HYPE_LAPIC_ICR_DELMODE_INIT (0x5u << 8)    /* 0b101 -> 0x500 */
#define HYPE_LAPIC_ICR_DELMODE_STARTUP (0x6u << 8) /* 0b110 -> 0x600 */
#define HYPE_LAPIC_ICR_LEVEL_ASSERT (1u << 14)

uint32_t hype_lapic_icr_high(uint8_t apic_id) {
    /* Physical destination mode: APIC id lives in bits 31:24. */
    return (uint32_t)apic_id << 24;
}

uint32_t hype_lapic_icr_low_init(void) {
    return HYPE_LAPIC_ICR_DELMODE_INIT | HYPE_LAPIC_ICR_LEVEL_ASSERT;
}

uint32_t hype_lapic_icr_low_sipi(uint8_t trampoline_page) {
    return HYPE_LAPIC_ICR_DELMODE_STARTUP | HYPE_LAPIC_ICR_LEVEL_ASSERT | (uint32_t)trampoline_page;
}

void hype_lapic_send_ipi(volatile uint32_t *lapic_base, uint32_t icr_high, uint32_t icr_low) {
    volatile uint8_t *base_bytes = (volatile uint8_t *)lapic_base;
    volatile uint32_t *icr_hi = (volatile uint32_t *)(base_bytes + HYPE_LAPIC_ICR_HIGH_OFFSET);
    volatile uint32_t *icr_lo = (volatile uint32_t *)(base_bytes + HYPE_LAPIC_ICR_LOW_OFFSET);

    /* Destination first, then the low write latches and fires the IPI. */
    *icr_hi = icr_high;
    *icr_lo = icr_low;

    /* Spin until the local APIC reports the IPI has been accepted/sent
     * (delivery-status returns to 0) before the caller issues the next one. */
    while (*icr_lo & HYPE_LAPIC_ICR_DELIVERY_STATUS) {
        __asm__ volatile("pause");
    }
}
