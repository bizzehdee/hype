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
    lapic->divide_accum = 0;
    lapic->lvt_timer_armed_seen = 0;
    lapic->timer_irq_pending = 0;
    lapic->timer_in_service = 0;
}

uint32_t hype_guest_lapic_divisor(uint32_t divide_config) {
    /* Divisor encoded in bits [3,1,0] (bit 2 reserved). */
    uint32_t d = ((divide_config & 0x8u) >> 1) | (divide_config & 0x3u);
    return (d == 0x7u) ? 1u : (1u << (d + 1u));
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
            /* M4-6b5 diag: record if the guest ever unmasked the timer LVT
             * (a real vector, mask bit clear) -- i.e. actually tried to use
             * the LAPIC timer as a clockevent, vs never touching it. */
            if ((value & HYPE_GUEST_LAPIC_LVT_MASKED) == 0) {
                lapic->lvt_timer_armed_seen = value;
            }
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

void hype_guest_lapic_advance(hype_guest_lapic_t *lapic, uint64_t ticks) {
    uint32_t divisor;
    uint64_t total;
    /* Timer disarmed (init_count == 0) or masked: nothing to do. */
    if (lapic->init_count == 0 || (lapic->lvt_timer & HYPE_GUEST_LAPIC_LVT_MASKED) != 0) {
        lapic->timer_irq_pending = 0;
        lapic->divide_accum = 0;
        return;
    }
    if (ticks == 0) {
        return;
    }

    /* M4-6b5: `ticks` is at the LAPIC timer's BASE input frequency (the FW-1
     * loop scales the real-elapsed time to a realistic bus-clock rate, not
     * PIT_HZ). The actual count decrements at base/divisor, per the guest's
     * Divide Configuration Register -- so a guest that sets divide-by-16 and
     * programs its counts accordingly sees the timer fire at the real time it
     * expects (the mismatch that made Linux calibrate an implausible frequency
     * and abandon the LAPIC timer for the 100 Hz PIT). The remainder carries
     * in divide_accum so no fractional counts are lost across calls. */
    divisor = hype_guest_lapic_divisor(lapic->divide_config);
    total = lapic->divide_accum + ticks;
    ticks = total / divisor;
    lapic->divide_accum = total - ticks * (uint64_t)divisor;
    if (ticks == 0) {
        return;
    }

    /* A one-shot already sitting at terminal count (current_count == 0)
     * has fired and must not fire again; a periodic never rests at 0
     * (it reloads on expiry), so a 0 here is always a spent one-shot. */
    if (lapic->current_count == 0) {
        return;
    }

    if (ticks >= (uint64_t)lapic->current_count) {
        /* Counter crossed terminal count -> the timer expired. */
        lapic->timer_irq_pending = 1;
        if ((lapic->lvt_timer & HYPE_GUEST_LAPIC_LVT_PERIODIC) != 0) {
            /* Reload from init_count, carrying the overshoot forward so
             * the periodic phase stays roughly aligned to real time. */
            uint64_t leftover = (ticks - (uint64_t)lapic->current_count) % (uint64_t)lapic->init_count;
            lapic->current_count = lapic->init_count - (uint32_t)leftover;
        } else {
            /* One-shot: fire once and stay at terminal count. */
            lapic->current_count = 0;
        }
    } else {
        lapic->current_count -= (uint32_t)ticks;
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
