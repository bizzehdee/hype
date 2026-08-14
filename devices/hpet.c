#include "hpet.h"

/* Vendor ID reported in the capabilities register. 0x8086 is what every real
 * and emulated HPET reports; a guest that special-cases it must see the same
 * value it would on the hardware this block claims to be. */
#define HPET_VENDOR_ID 0x8086u
#define HPET_REVISION 0x01u

uint64_t hype_hpet_capabilities(void) {
    uint64_t cap = 0;

    cap |= (uint64_t)HPET_REVISION;                       /* REV_ID */
    cap |= (uint64_t)(HYPE_HPET_NUM_TIMERS - 1u) << 8;    /* NUM_TIM_CAP */
    cap |= 1ull << 13;                                    /* COUNT_SIZE_CAP: 64-bit counter */
    cap |= 1ull << 15;                                    /* LEG_RT_CAP: legacy routing supported */
    cap |= (uint64_t)HPET_VENDOR_ID << 16;                /* VENDOR_ID */
    cap |= (uint64_t)HYPE_HPET_PERIOD_FS << 32;           /* COUNTER_CLK_PERIOD */
    return cap;
}

void hype_hpet_reset(hype_hpet_t *hpet) {
    unsigned i;

    hpet->config = 0;
    hpet->int_status = 0;
    hpet->counter = 0;
    hpet->offset = 0;
    hpet->last_absolute = 0;
    for (i = 0; i < HYPE_HPET_NUM_TIMERS; i++) {
        /* Each comparator reports what it can do even before a guest writes
         * it: periodic capable, 64-bit wide. Nothing is enabled at reset. */
        hpet->timers[i].config = HYPE_HPET_TIMER_PERIODIC_CAP | HYPE_HPET_TIMER_SIZE_CAP |
                                 (HYPE_HPET_TIMER_ROUTE_CAP_MASK
                                  << HYPE_HPET_TIMER_ROUTE_CAP_SHIFT);
        hpet->timers[i].comparator = 0xFFFFFFFFFFFFFFFFull;
        hpet->timers[i].period = 0;
    }
}

uint32_t hype_hpet_sync(hype_hpet_t *hpet, uint64_t absolute_ticks) {
    uint64_t before;
    uint64_t now;
    uint32_t fired = 0;
    unsigned i;

    /* Track absolute time even while the counter is disabled, so enabling it
     * does not make the clock appear to jump backwards or leap forwards. */
    hpet->last_absolute = absolute_ticks;
    if ((hpet->config & HYPE_HPET_CONFIG_ENABLE) == 0u) {
        return 0;
    }
    now = absolute_ticks + (uint64_t)hpet->offset;
    if (now == hpet->counter) {
        return 0;
    }
    before = hpet->counter;
    hpet->counter = now;

    for (i = 0; i < HYPE_HPET_NUM_TIMERS; i++) {
        hype_hpet_timer_t *t = &hpet->timers[i];
        uint64_t cmp = t->comparator;

        /*
         * A match is "the counter reached the comparator during this step".
         * Comparing against the interval rather than for equality is what
         * makes the model correct when a step covers many ticks -- the guest
         * is entitled to the interrupt it would have had on real hardware
         * even though hype only samples time at VM exits.
         */
        if (cmp <= before || cmp > hpet->counter) {
            continue;
        }
        if ((t->config & HYPE_HPET_TIMER_INT_ENABLE) != 0u) {
            hpet->int_status |= (1ull << i);
            fired |= (1u << i);
        }
        if ((t->config & HYPE_HPET_TIMER_PERIODIC) != 0u && t->period != 0u) {
            /* Periodic comparators re-arm themselves by their period, and must
             * end up ahead of the counter even if a single step spanned
             * several periods. */
            do {
                t->comparator += t->period;
            } while (t->comparator <= hpet->counter);
        }
    }
    return fired;
}

static uint64_t timer_index_for(uint32_t offset, uint32_t *out_reg) {
    uint32_t rel = offset - HYPE_HPET_REG_TIMER_BASE;
    *out_reg = rel % HYPE_HPET_REG_TIMER_STRIDE;
    return rel / HYPE_HPET_REG_TIMER_STRIDE;
}

static uint64_t slice(uint64_t full, uint32_t offset, unsigned size) {
    /* A 4-byte access reads the half of the 64-bit register it addresses. */
    if (size == 4u && (offset & 4u) != 0u) {
        return (full >> 32) & 0xFFFFFFFFull;
    }
    if (size == 4u) {
        return full & 0xFFFFFFFFull;
    }
    return full;
}

static uint64_t merge(uint64_t old, uint64_t value, uint32_t offset, unsigned size) {
    if (size == 4u && (offset & 4u) != 0u) {
        return (old & 0xFFFFFFFFull) | ((value & 0xFFFFFFFFull) << 32);
    }
    if (size == 4u) {
        return (old & 0xFFFFFFFF00000000ull) | (value & 0xFFFFFFFFull);
    }
    return value;
}

uint64_t hype_hpet_read(const hype_hpet_t *hpet, uint32_t offset, unsigned size) {
    uint32_t base = offset & ~7u;

    if (base == HYPE_HPET_REG_CAP_ID) {
        return slice(hype_hpet_capabilities(), offset, size);
    }
    if (base == HYPE_HPET_REG_CONFIG) {
        return slice(hpet->config, offset, size);
    }
    if (base == HYPE_HPET_REG_INT_STATUS) {
        return slice(hpet->int_status, offset, size);
    }
    if (base == HYPE_HPET_REG_MAIN_COUNTER) {
        return slice(hpet->counter, offset, size);
    }
    if (base >= HYPE_HPET_REG_TIMER_BASE) {
        uint32_t reg;
        uint64_t idx = timer_index_for(base, &reg);
        if (idx < HYPE_HPET_NUM_TIMERS) {
            if (reg == 0u) {
                return slice(hpet->timers[idx].config, offset, size);
            }
            if (reg == 8u) {
                return slice(hpet->timers[idx].comparator, offset, size);
            }
        }
    }
    /* Unimplemented offsets read as zero, which is what a register that does
     * not exist in this block reports -- not all-ones, which a guest would
     * read as a live register full of set bits. */
    return 0;
}

void hype_hpet_write(hype_hpet_t *hpet, uint32_t offset, unsigned size, uint64_t value) {
    uint32_t base = offset & ~7u;

    if (base == HYPE_HPET_REG_CAP_ID) {
        return; /* read-only */
    }
    if (base == HYPE_HPET_REG_CONFIG) {
        hpet->config = merge(hpet->config, value, offset, size) &
                       (HYPE_HPET_CONFIG_ENABLE | HYPE_HPET_CONFIG_LEGACY_ROUTE);
        return;
    }
    if (base == HYPE_HPET_REG_INT_STATUS) {
        /* Write-1-to-clear. */
        hpet->int_status &= ~merge(0, value, offset, size);
        return;
    }
    if (base == HYPE_HPET_REG_MAIN_COUNTER) {
        /* A write rebases the reported counter; the underlying clock keeps
         * running from wherever real time has reached. */
        hpet->counter = merge(hpet->counter, value, offset, size);
        hpet->offset = (int64_t)(hpet->counter - hpet->last_absolute);
        return;
    }
    if (base >= HYPE_HPET_REG_TIMER_BASE) {
        uint32_t reg;
        uint64_t idx = timer_index_for(base, &reg);
        if (idx >= HYPE_HPET_NUM_TIMERS) {
            return;
        }
        if (reg == 0u) {
            hype_hpet_timer_t *t = &hpet->timers[idx];
            uint64_t w = merge(t->config, value, offset, size);
            /* The capability bits are read-only; everything a guest may set is
             * kept. VAL_SET is a one-shot request, not stored state. */
            t->config = (w & ~(HYPE_HPET_TIMER_PERIODIC_CAP | HYPE_HPET_TIMER_SIZE_CAP |
                               HYPE_HPET_TIMER_VAL_SET |
                               (HYPE_HPET_TIMER_ROUTE_CAP_MASK
                                << HYPE_HPET_TIMER_ROUTE_CAP_SHIFT))) |
                        HYPE_HPET_TIMER_PERIODIC_CAP | HYPE_HPET_TIMER_SIZE_CAP |
                        (HYPE_HPET_TIMER_ROUTE_CAP_MASK << HYPE_HPET_TIMER_ROUTE_CAP_SHIFT);
            if ((w & HYPE_HPET_TIMER_VAL_SET) != 0u) {
                t->period = 0; /* the next comparator write supplies the period */
                t->config |= HYPE_HPET_TIMER_VAL_SET;
            }
            return;
        }
        if (reg == 8u) {
            hype_hpet_timer_t *t = &hpet->timers[idx];
            uint64_t w = merge(t->comparator, value, offset, size);
            if ((t->config & HYPE_HPET_TIMER_PERIODIC) != 0u &&
                (t->config & HYPE_HPET_TIMER_VAL_SET) != 0u) {
                /* Periodic arming: the value written is the period, and the
                 * first expiry is one period from now. */
                t->period = w;
                t->comparator = hpet->counter + w;
                t->config &= ~HYPE_HPET_TIMER_VAL_SET;
            } else {
                t->comparator = w;
            }
            return;
        }
    }
}
