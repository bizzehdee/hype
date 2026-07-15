#include "guest_lapic.h"

void hype_guest_lapic_reset(hype_guest_lapic_t *lapic) {
    lapic->svr = 0x000000FFu; /* xAPIC reset: all-ones low byte, APIC software-disabled (bit 8 = 0) */
    lapic->lvt_timer = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->lvt_lint0 = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->lvt_lint1 = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->dfr = 0xFFFFFFFFu;
    lapic->ldr = 0;
    lapic->divide_config = 0;
    lapic->init_count = 0;
    lapic->current_count = 0;
    lapic->tick_accum = 0;
    lapic->timer_irq_pending = 0;
    lapic->timer_in_service = 0;
}

int hype_guest_lapic_read(hype_guest_lapic_t *lapic, uint32_t offset, unsigned int size, uint32_t *out) {
    if (size != 4u) {
        return -1;
    }
    switch (offset) {
        case HYPE_GUEST_LAPIC_REG_ID:
            *out = 0; /* single vCPU -> APIC ID 0 */
            return 0;
        case HYPE_GUEST_LAPIC_REG_VERSION:
            *out = HYPE_GUEST_LAPIC_VERSION_VALUE;
            return 0;
        case HYPE_GUEST_LAPIC_REG_SVR:
            *out = lapic->svr;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LDR:
            *out = lapic->ldr;
            return 0;
        case HYPE_GUEST_LAPIC_REG_DFR:
            *out = lapic->dfr;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_TIMER:
            *out = lapic->lvt_timer;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_LINT0:
            *out = lapic->lvt_lint0;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_LINT1:
            *out = lapic->lvt_lint1;
            return 0;
        case HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT:
            *out = lapic->init_count;
            return 0;
        case HYPE_GUEST_LAPIC_REG_TIMER_CURRENT_COUNT:
            *out = lapic->current_count;
            return 0;
        case HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG:
            *out = lapic->divide_config;
            return 0;
        default:
            /* EOI and any other register in the window read as 0 -- a
             * benign default, matching the MMIO models elsewhere here. */
            *out = 0;
            return 0;
    }
}

int hype_guest_lapic_write(hype_guest_lapic_t *lapic, uint32_t offset, unsigned int size, uint32_t value) {
    if (size != 4u) {
        return -1;
    }
    switch (offset) {
        case HYPE_GUEST_LAPIC_REG_SVR:
            lapic->svr = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LDR:
            lapic->ldr = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_DFR:
            lapic->dfr = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_TIMER:
            lapic->lvt_timer = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_LINT0:
            lapic->lvt_lint0 = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_LINT1:
            lapic->lvt_lint1 = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_TIMER_INIT_COUNT:
            /* Writing the initial count (re)arms the timer, per the SDM. */
            lapic->init_count = value;
            lapic->current_count = value;
            lapic->tick_accum = 0;
            return 0;
        case HYPE_GUEST_LAPIC_REG_TIMER_DIVIDE_CONFIG:
            lapic->divide_config = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_EOI:
            /* End-of-interrupt: the guest's timer ISR has finished;
             * clear in-service so the next expiry can be delivered. */
            lapic->timer_in_service = 0;
            return 0;
        case HYPE_GUEST_LAPIC_REG_ID:
        case HYPE_GUEST_LAPIC_REG_VERSION:
            /* Read-only -- ignore writes. */
            return 0;
        default:
            /* Unmodeled register in the window -- ignore, benign. */
            return 0;
    }
}

void hype_guest_lapic_tick(hype_guest_lapic_t *lapic) {
    /* Timer disarmed (init_count == 0) or masked: nothing to do, and
     * make sure nothing is left pending from a previous armed period. */
    if (lapic->init_count == 0 || (lapic->lvt_timer & HYPE_GUEST_LAPIC_LVT_MASKED) != 0) {
        lapic->timer_irq_pending = 0;
        return;
    }

    /* Keep TIMER_CURRENT_COUNT visibly moving for guest calibration/
     * delay loops: step it down toward 0 across HYPE_GUEST_LAPIC_TICK_EXITS
     * exits, reloading from init_count each synthetic period. */
    {
        uint32_t step = lapic->init_count / HYPE_GUEST_LAPIC_TICK_EXITS;
        if (step == 0) {
            step = 1;
        }
        if (lapic->current_count > step) {
            lapic->current_count -= step;
        } else {
            lapic->current_count = lapic->init_count;
        }
    }

    lapic->tick_accum++;
    if (lapic->tick_accum >= HYPE_GUEST_LAPIC_TICK_EXITS) {
        lapic->tick_accum = 0;
        lapic->timer_irq_pending = 1;
    }
}

int hype_guest_lapic_take_timer_irq(hype_guest_lapic_t *lapic, uint8_t *vector_out) {
    if (!lapic->timer_irq_pending || lapic->timer_in_service) {
        return 0;
    }
    lapic->timer_irq_pending = 0;
    lapic->timer_in_service = 1;
    *vector_out = (uint8_t)(lapic->lvt_timer & HYPE_GUEST_LAPIC_LVT_VECTOR_MASK);
    return 1;
}
