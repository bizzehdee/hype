#include "guest_lapic.h"

void hype_guest_lapic_reset(hype_guest_lapic_t *lapic) {
    unsigned int i;
    lapic->svr = 0x000000FFu; /* xAPIC reset: all-ones low byte, APIC software-disabled (bit 8 = 0) */
    lapic->lvt_timer = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->lvt_lint0 = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->lvt_lint1 = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->lvt_thermal = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->lvt_pmc = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->lvt_error = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->lvt_cmci = HYPE_GUEST_LAPIC_LVT_MASKED;
    lapic->esr = 0;
    lapic->dfr = 0xFFFFFFFFu;
    lapic->ldr = 0;
    lapic->icr_low = 0;
    lapic->icr_high = 0;
    for (i = 0; i < 8u; i++) {
        lapic->self_ipi_pending[i] = 0;
        lapic->isr[i] = 0;
    }
    lapic->tpr = 0;
    lapic->self_ipi_count = 0;
    lapic->divide_config = 0;
    lapic->init_count = 0;
    lapic->current_count = 0;
    lapic->tick_accum = 0;
    lapic->divide_accum = 0;
    lapic->lvt_timer_armed_seen = 0;
    lapic->timer_irq_pending = 0;
    lapic->timer_in_service = 0;
    lapic->eoi_count = 0;
    lapic->apic_id = 0; /* SMP-3: the BSP's ID; APs are set explicitly after reset */
    /* SMP-4: no IPI in flight across a reset. */
    lapic->ipi_out_valid = 0;
    lapic->ipi_out_dropped = 0;
    lapic->ipi_out_count = 0;
}

int hype_guest_lapic_take_ipi(hype_guest_lapic_t *lapic, hype_guest_lapic_ipi_t *out) {
    if (lapic == 0 || !lapic->ipi_out_valid) {
        return 0;
    }
    if (out != 0) {
        /* Field by field: whole-struct assignment emits a memcpy, which does not link on the
         * freestanding UEFI target. */
        out->delivery_mode = lapic->ipi_out.delivery_mode;
        out->vector = lapic->ipi_out.vector;
        out->dest_apic_id = lapic->ipi_out.dest_apic_id;
        out->shorthand = lapic->ipi_out.shorthand;
        out->logical = lapic->ipi_out.logical;
        out->level_assert = lapic->ipi_out.level_assert;
    }
    lapic->ipi_out_valid = 0;
    return 1;
}

/* SMP-4 (#188): latch an IPI for the VM layer to route. See hype_guest_lapic_take_ipi. */
static void lapic_post_ipi(hype_guest_lapic_t *lapic, uint32_t icr_low, uint32_t dest) {
    if (lapic->ipi_out_valid) {
        lapic->ipi_out_dropped++;
    }
    lapic->ipi_out.delivery_mode =
        (icr_low & HYPE_GUEST_LAPIC_ICR_DELMODE_MASK) >> HYPE_GUEST_LAPIC_ICR_DELMODE_SHIFT;
    lapic->ipi_out.vector = icr_low & HYPE_GUEST_LAPIC_ICR_VECTOR_MASK;
    lapic->ipi_out.dest_apic_id = dest;
    lapic->ipi_out.shorthand = icr_low & HYPE_GUEST_LAPIC_ICR_SHORTHAND_MASK;
    lapic->ipi_out.logical = (icr_low & HYPE_GUEST_LAPIC_ICR_DESTMODE_LOGICAL) != 0u;
    lapic->ipi_out.level_assert = (icr_low & HYPE_GUEST_LAPIC_ICR_LEVEL_ASSERT) != 0u;
    lapic->ipi_out_valid = 1;
    lapic->ipi_out_count++;
}

void hype_guest_lapic_set_apic_id(hype_guest_lapic_t *lapic, uint32_t apic_id) {
    lapic->apic_id = apic_id;
}

void hype_guest_lapic_accept_vector(hype_guest_lapic_t *lapic, uint8_t vector) {
    lapic->isr[vector >> 5] |= 1u << (vector & 31u);
}

int hype_guest_lapic_isr_highest(const hype_guest_lapic_t *lapic) {
    unsigned int word = 8u;

    /* Scan from the top: the highest set bit anywhere is the highest-priority vector. */
    while (word-- > 0u) {
        uint32_t bits = lapic->isr[word];
        unsigned int bit = 32u;
        if (bits == 0u) {
            continue;
        }
        while (bit-- > 0u) {
            if ((bits & (1u << bit)) != 0u) {
                return (int)(word * 32u + bit);
            }
        }
    }
    return -1;
}

/* Clears the highest set ISR bit -- the vector the guest is EOIing. No-op when none is set. */
static void hype_guest_lapic_clear_highest_isr(hype_guest_lapic_t *lapic) {
    int v = hype_guest_lapic_isr_highest(lapic);
    if (v >= 0) {
        lapic->isr[(unsigned int)v >> 5] &= ~(1u << ((unsigned int)v & 31u));
    }
}

uint32_t hype_guest_lapic_ppr(const hype_guest_lapic_t *lapic) {
    int isrv = hype_guest_lapic_isr_highest(lapic);
    uint32_t tpr_class = lapic->tpr & 0xF0u;
    uint32_t isrv_class = (isrv < 0) ? 0u : ((uint32_t)isrv & 0xF0u);

    /*
     * Intel SDM Vol 3, "Task and Processor Priorities": the sub-class bits survive only when
     * TPR's priority class strictly dominates the in-service one -- otherwise the in-service
     * vector sets the class and the sub-class reads 0.
     */
    if (tpr_class >= isrv_class) {
        return lapic->tpr & 0xFFu;
    }
    return isrv_class;
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
    /*
     * #311: the ISR block is a range of eight dwords at 16-byte spacing, so it cannot be a
     * switch label. An offset inside the range that is not 16-byte aligned is not a register
     * at all and falls through to the benign 0 below, same as any other unmodelled offset.
     */
    if (offset >= HYPE_GUEST_LAPIC_REG_ISR_BASE && offset <= HYPE_GUEST_LAPIC_REG_ISR_LAST &&
        (offset & 0xFu) == 0u) {
        *out = lapic->isr[(offset - HYPE_GUEST_LAPIC_REG_ISR_BASE) >> 4];
        return 0;
    }

    switch (offset) {
        case HYPE_GUEST_LAPIC_REG_ID:
            /* SMP-3 (#187): the architectural xAPIC ID register carries the ID in [31:24].
             * Was hardcoded 0, which every vCPU of a multi-vCPU guest would have read. */
            *out = (lapic->apic_id & 0xFFu) << 24;
            return 0;
        case HYPE_GUEST_LAPIC_REG_TPR:
            *out = lapic->tpr;
            return 0;
        case HYPE_GUEST_LAPIC_REG_PPR:
            *out = hype_guest_lapic_ppr(lapic);
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
        case HYPE_GUEST_LAPIC_REG_ICR_LOW:
            /* Delivery status (bit 12) always reads idle: sends complete
             * synchronously in this model, so no send is ever "pending". */
            *out = lapic->icr_low & ~HYPE_GUEST_LAPIC_ICR_DELIVERY_STATUS;
            return 0;
        case HYPE_GUEST_LAPIC_REG_ICR_HIGH:
            *out = lapic->icr_high;
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
        case HYPE_GUEST_LAPIC_REG_LVT_THERMAL:
            *out = lapic->lvt_thermal;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_PMC:
            *out = lapic->lvt_pmc;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_ERROR:
            *out = lapic->lvt_error;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_CMCI:
            *out = lapic->lvt_cmci;
            return 0;
        case HYPE_GUEST_LAPIC_REG_ESR:
            /* No error sources are modelled; the latched value is always 0. */
            *out = lapic->esr;
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
        case HYPE_GUEST_LAPIC_REG_ICR_HIGH:
            lapic->icr_high = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_ICR_LOW: {
            /*
             * Writing ICR_LOW latches and sends.
             *
             * Two destinations, decided independently: SELF, which is pended straight into
             * this LAPIC's own self-IPI set (the #103 path -- kernels >= 6.16 need the LAPIC
             * self-IPI for SRCU's irq_work), and OTHERS, which is latched for the VM layer
             * because this model knows nothing about its siblings.
             *
             * SMP-4 (#188): "others" used to be unconditionally dropped -- correct while a VM
             * had one vCPU, and the reason AP bring-up could not even be expressed. INIT and
             * STARTUP are always outbound (a CPU never INITs itself), and a fixed IPI can be
             * aimed at either or both.
             */
            uint32_t delmode =
                (value & HYPE_GUEST_LAPIC_ICR_DELMODE_MASK) >> HYPE_GUEST_LAPIC_ICR_DELMODE_SHIFT;
            uint32_t shorthand = value & HYPE_GUEST_LAPIC_ICR_SHORTHAND_MASK;
            uint32_t dest = lapic->icr_high >> 24;
            uint32_t self_id = lapic->apic_id & 0xFFu;
            int to_self = 0;
            int to_others = 0;

            lapic->icr_low = value;

            switch (shorthand) {
                case HYPE_GUEST_LAPIC_ICR_SHORTHAND_SELF:
                    to_self = 1;
                    break;
                case HYPE_GUEST_LAPIC_ICR_SHORTHAND_ALL_INCL:
                    to_self = 1;
                    to_others = 1;
                    break;
                case HYPE_GUEST_LAPIC_ICR_SHORTHAND_ALL_EXCL:
                    to_others = 1;
                    break;
                default: /* no shorthand: the destination field decides */
                    if ((value & HYPE_GUEST_LAPIC_ICR_DESTMODE_LOGICAL) != 0) {
                        to_self = (dest & (lapic->ldr >> 24)) != 0;
                        /* A logical mask may name others too; the VM layer resolves it
                         * against every vCPU's LDR, which is knowledge this model lacks. */
                        to_others = 1;
                    } else {
                        to_self = (dest == self_id) || (dest == 0xFFu);
                        to_others = (dest != self_id) || (dest == 0xFFu);
                    }
                    break;
            }

            /* Only FIXED delivery is self-deliverable here, and vectors 0-15 are illegal for
             * it -- dropped rather than pended into the IRR. INIT/SIPI/NMI to self are not
             * modelled: nothing in this project's guests does it. */
            if (to_self && delmode == HYPE_GUEST_LAPIC_ICR_DELMODE_FIXED) {
                uint32_t vector = value & HYPE_GUEST_LAPIC_ICR_VECTOR_MASK;
                if (vector >= 16u) {
                    lapic->self_ipi_pending[vector >> 5] |= 1u << (vector & 31u);
                    lapic->self_ipi_count++;
                }
            }
            if (to_others || delmode == HYPE_GUEST_LAPIC_ICR_DELMODE_INIT ||
                delmode == HYPE_GUEST_LAPIC_ICR_DELMODE_STARTUP) {
                lapic_post_ipi(lapic, value, dest);
            }
            return 0;
        }
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
        case HYPE_GUEST_LAPIC_REG_LVT_THERMAL:
            lapic->lvt_thermal = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_PMC:
            lapic->lvt_pmc = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_ERROR:
            lapic->lvt_error = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_LVT_CMCI:
            lapic->lvt_cmci = value;
            return 0;
        case HYPE_GUEST_LAPIC_REG_ESR:
            /* A write latches the (empty) internal error status into the
             * visible register; both are always 0 here. */
            lapic->esr = 0;
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
            /* End-of-interrupt: the guest's ISR has finished. Clear the timer
             * in-service (so the next expiry can be delivered) and bump the EOI
             * counter so the FW-1 loop can drop a level line's IO-APIC
             * Remote-IRR (real hardware broadcasts this EOI to the IO-APIC). */
            lapic->timer_in_service = 0;
            /* #311: and retire the highest-priority in-service vector, so nested delivery
             * unwinds as a stack in the same LIFO order the guest EOIs in. */
            hype_guest_lapic_clear_highest_isr(lapic);
            lapic->eoi_count++;
            return 0;
        case HYPE_GUEST_LAPIC_REG_TPR:
            lapic->tpr = value & 0xFFu;
            return 0;
        case HYPE_GUEST_LAPIC_REG_ID:
        case HYPE_GUEST_LAPIC_REG_VERSION:
        case HYPE_GUEST_LAPIC_REG_PPR:
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
    /* Timer disarmed (init_count == 0): nothing counts. NOTE: a MASKED timer is
     * NOT disarmed -- on real hardware the count register keeps decrementing
     * whenever init_count != 0, and the LVT mask bit only suppresses the
     * *interrupt* on expiry, never the counting. Freezing the counter while
     * masked broke Linux's LAPIC-timer calibration, which programs the timer,
     * masks the LVT, and reads current_count to measure the rate: it saw a
     * stuck counter, so an ACPI-mode guest (which uses the LAPIC timer as its
     * clockevent) could never establish a working timer and hung in early boot
     * waiting for the first tick. The mask is honored below, at IRQ time only. */
    if (lapic->init_count == 0) {
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
        /* Counter crossed terminal count -> the timer expired. The mask bit
         * gates only interrupt DELIVERY: a masked timer still expires/reloads
         * (so the counter Linux reads keeps moving during calibration) but
         * raises no IRQ. */
        if ((lapic->lvt_timer & HYPE_GUEST_LAPIC_LVT_MASKED) == 0) {
            lapic->timer_irq_pending = 1;
        }
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

int hype_guest_lapic_recover_in_service(hype_guest_lapic_t *lapic) {
    if (lapic->timer_in_service) {
        lapic->timer_in_service = 0;
        return 1;
    }
    return 0;
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

/*
 * SMP-6 (#190): pend a vector on ANOTHER vCPU's LAPIC, to be injected by that vCPU's own
 * dispatch loop.
 *
 * This exists because the cross-vCPU FIXED IPI path used to call vmm_request_interrupt()
 * against the target's context -- i.e. the SENDING core wrote the TARGET's VMCB while the
 * target core could be inside VMRUN. Hardware writes back the VMCB control area on exit, so
 * that injection could simply be lost. The INIT/SIPI path parks the target first for exactly
 * this reason; the FIXED path did not.
 *
 * Measured consequence: FreeBSD's BSP spun in smp_targeted_tlb_shootdown_native waiting for an
 * acknowledgement from an AP that was sitting idle in cpu_idle_acpi, having never seen the
 * shootdown IPI. 26 IPIs sent, none dropped by the outbound slot, and still no progress.
 *
 * Posting a bit into the target's pending set is a plain atomic OR on shared memory, and the
 * target drains it on its own core, into its own VMCB.
 */
void hype_guest_lapic_post_vector(hype_guest_lapic_t *lapic, uint8_t vector) {
    (void)__atomic_fetch_or(&lapic->self_ipi_pending[vector >> 5],
                            1u << (vector & 31u), __ATOMIC_RELEASE);
    (void)__atomic_fetch_add(&lapic->self_ipi_count, 1ull, __ATOMIC_RELAXED);
}

int hype_guest_lapic_take_self_ipi(hype_guest_lapic_t *lapic, uint8_t *vector_out) {
    unsigned int word;
    for (word = 0; word < 8u; word++) {
        uint32_t bits = lapic->self_ipi_pending[word];
        unsigned int bit;
        if (bits == 0) {
            continue;
        }
        for (bit = 0; bit < 32u; bit++) {
            if ((bits & (1u << bit)) != 0) {
                /* SMP-6: cleared atomically. Another vCPU's core may be OR-ing a bit into this
                 * same word via hype_guest_lapic_post_vector() at this instant; a
                 * read-modify-write would drop that IPI. */
                (void)__atomic_fetch_and(&lapic->self_ipi_pending[word],
                                         ~(1u << bit), __ATOMIC_ACQUIRE);
                *vector_out = (uint8_t)(word * 32u + bit);
                return 1;
            }
        }
    }
    return 0;
}
