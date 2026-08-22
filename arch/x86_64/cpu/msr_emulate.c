#include "msr_emulate.h"

#include "lapic.h"

/* AMD SDM: EFER's MSR number, 0xC0000080 -- duplicated here rather
 * than included from arch/x86_64/svm/svm.h, same reasoning as vmcb.h's
 * own HYPE_SVM_SAVE_EFER_SVME (avoid a header depending on the one
 * that already includes it). */
#define HYPE_MSR_NUMBER_EFER 0xC0000080u

hype_msr_action_t hype_msr_decide(uint32_t msr_number, int is_write) {
    return hype_msr_decide_ex(msr_number, is_write, 0);
}

hype_msr_action_t hype_msr_decide_ex(uint32_t msr_number, int is_write, int hv_enabled) {
    if (msr_number == HYPE_MSR_NUMBER_APIC_BASE) {
        return is_write ? HYPE_MSR_ACTION_REJECT : HYPE_MSR_ACTION_READ_APIC_BASE;
    }
    if (msr_number == HYPE_MSR_NUMBER_EFER) {
        return HYPE_MSR_ACTION_READWRITE_EFER;
    }
    if (msr_number == HYPE_MSR_NUMBER_TSC) {
        return is_write ? HYPE_MSR_ACTION_REJECT : HYPE_MSR_ACTION_READ_TSC;
    }
    /* #251: read/write BOTH ways. Unlike APIC_BASE and TSC these are genuinely
     * writable guest state -- rejecting the write is what left a 64-bit guest
     * with no usable GS base. */
    if (msr_number == HYPE_MSR_NUMBER_FS_BASE) {
        return HYPE_MSR_ACTION_READWRITE_FS_BASE;
    }
    if (msr_number == HYPE_MSR_NUMBER_GS_BASE) {
        return HYPE_MSR_ACTION_READWRITE_GS_BASE;
    }
    if (hv_enabled) {
        if (msr_number == HYPE_MSR_NUMBER_HV_GUEST_OS_ID) {
            return HYPE_MSR_ACTION_READWRITE_HV_GUEST_OS_ID;
        }
        if (msr_number == HYPE_MSR_NUMBER_HV_HYPERCALL) {
            return HYPE_MSR_ACTION_READWRITE_HV_HYPERCALL;
        }
        if (msr_number == HYPE_MSR_NUMBER_HV_VP_INDEX) {
            return is_write ? HYPE_MSR_ACTION_REJECT : HYPE_MSR_ACTION_READ_HV_VP_INDEX;
        }
        if (msr_number == HYPE_MSR_NUMBER_HV_REFERENCE_TSC) {
            return HYPE_MSR_ACTION_READWRITE_HV_REFERENCE_TSC;
        }
        if (msr_number == HYPE_MSR_NUMBER_HV_TSC_FREQUENCY) {
            return is_write ? HYPE_MSR_ACTION_REJECT : HYPE_MSR_ACTION_READ_HV_TSC_FREQUENCY;
        }
        if (msr_number == HYPE_MSR_NUMBER_HV_APIC_FREQUENCY) {
            return is_write ? HYPE_MSR_ACTION_REJECT : HYPE_MSR_ACTION_READ_HV_APIC_FREQUENCY;
        }
        if (msr_number == HYPE_MSR_NUMBER_HV_TIME_REF_COUNT) {
            return is_write ? HYPE_MSR_ACTION_REJECT
                            : HYPE_MSR_ACTION_READ_HV_TIME_REF_COUNT;
        }
    }
    return HYPE_MSR_ACTION_REJECT;
}

uint64_t hype_msr_apic_base_value(int is_bsp) {
    return hype_msr_apic_base_value_mode(is_bsp, HYPE_APIC_MODE_XAPIC);
}

uint64_t hype_msr_apic_base_value_mode(int is_bsp, int apic_mode) {
    /* bit 11 = global enable (clear only when fully disabled), bit 10 = EXTD
     * (x2APIC), bit 8 = BSP (vCPU 0 only -- see the header). */
    uint64_t value = HYPE_LAPIC_DEFAULT_BASE;
    if (apic_mode != HYPE_APIC_MODE_DISABLED) {
        value |= (1ULL << 11);
    }
    if (apic_mode == HYPE_APIC_MODE_X2APIC) {
        value |= (1ULL << 10);
    }
    if (is_bsp) {
        value |= (1ULL << 8);
    }
    return value;
}

int hype_msr_is_x2apic_range(uint32_t msr_number) {
    return msr_number >= HYPE_MSR_X2APIC_RANGE_BASE && msr_number <= HYPE_MSR_X2APIC_RANGE_LAST;
}

int hype_apic_base_mode_transition(int current, int want_en, int want_extd, int *next_out) {
    int requested;

    if (!want_en) {
        if (want_extd) {
            return -1; /* EN=0,EXTD=1 names no state -- always #GP */
        }
        requested = HYPE_APIC_MODE_DISABLED;
    } else {
        requested = want_extd ? HYPE_APIC_MODE_X2APIC : HYPE_APIC_MODE_XAPIC;
    }

    if (requested == current) {
        *next_out = requested; /* re-writing the current mode is a legal no-op */
        return 0;
    }
    if (current == HYPE_APIC_MODE_X2APIC && requested == HYPE_APIC_MODE_XAPIC) {
        return -1; /* the one illegal transition: must go through Disabled first */
    }
    *next_out = requested;
    return 0;
}

uint64_t hype_msr_hv_ref_count_from_tsc(uint64_t tsc_delta, uint64_t tsc_khz) {
    uint64_t ms, rem;

    if (tsc_khz == 0u) {
        return 0u;
    }
    ms = tsc_delta / tsc_khz;
    rem = tsc_delta % tsc_khz;
    return (ms * 10000u) + ((rem * 10000u) / tsc_khz);
}
