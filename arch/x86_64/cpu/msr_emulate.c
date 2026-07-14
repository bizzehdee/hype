#include "msr_emulate.h"

#include "lapic.h"

/* AMD SDM: EFER's MSR number, 0xC0000080 -- duplicated here rather
 * than included from arch/x86_64/svm/svm.h, same reasoning as vmcb.h's
 * own HYPE_SVM_SAVE_EFER_SVME (avoid a header depending on the one
 * that already includes it). */
#define HYPE_MSR_NUMBER_EFER 0xC0000080u

hype_msr_action_t hype_msr_decide(uint32_t msr_number, int is_write) {
    if (msr_number == HYPE_MSR_NUMBER_APIC_BASE) {
        return is_write ? HYPE_MSR_ACTION_REJECT : HYPE_MSR_ACTION_READ_APIC_BASE;
    }
    if (msr_number == HYPE_MSR_NUMBER_EFER) {
        return HYPE_MSR_ACTION_READWRITE_EFER;
    }
    if (msr_number == HYPE_MSR_NUMBER_TSC) {
        return is_write ? HYPE_MSR_ACTION_REJECT : HYPE_MSR_ACTION_READ_TSC;
    }
    return HYPE_MSR_ACTION_REJECT;
}

uint64_t hype_msr_apic_base_value(void) {
    return HYPE_LAPIC_DEFAULT_BASE | (1ULL << 11) | (1ULL << 8);
}
