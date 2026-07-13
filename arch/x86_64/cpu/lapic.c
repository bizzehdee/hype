#include "lapic.h"

void hype_lapic_mask_timer(volatile uint32_t *lapic_base) {
    volatile uint8_t *base_bytes = (volatile uint8_t *)lapic_base;
    volatile uint32_t *lvt_timer = (volatile uint32_t *)(base_bytes + HYPE_LAPIC_LVT_TIMER_OFFSET);

    *lvt_timer = *lvt_timer | HYPE_LAPIC_LVT_MASKED;
}
