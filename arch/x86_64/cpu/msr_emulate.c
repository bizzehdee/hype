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
        if (msr_number == HYPE_MSR_NUMBER_HV_TIME_REF_COUNT) {
            return is_write ? HYPE_MSR_ACTION_REJECT
                            : HYPE_MSR_ACTION_READ_HV_TIME_REF_COUNT;
        }
    }
    return HYPE_MSR_ACTION_REJECT;
}

uint64_t hype_msr_apic_base_value(void) {
    return HYPE_LAPIC_DEFAULT_BASE | (1ULL << 11) | (1ULL << 8);
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
