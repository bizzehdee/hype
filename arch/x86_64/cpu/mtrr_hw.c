#include "../../../core/mtrr.h"

/*
 * #368: reads this core's MTRR and PAT MSRs. Exempt from unit testing (rdmsr), like
 * paging_load.c -- all the logic that can be tested lives in core/mtrr.c, which takes these
 * values as parameters.
 */

#define MSR_IA32_MTRRCAP 0xFEu
#define MSR_IA32_MTRR_PHYSBASE0 0x200u
#define MSR_IA32_MTRR_DEF_TYPE 0x2FFu
#define MSR_IA32_PAT 0x277u

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

uint64_t hype_mtrr_read_pat(void) { return rdmsr(MSR_IA32_PAT); }

uint64_t hype_mtrr_read_def_type(void) { return rdmsr(MSR_IA32_MTRR_DEF_TYPE); }

/*
 * Fills `out` with this core's variable-range MTRRs and returns how many were read. The count
 * comes from IA32_MTRRCAP bits 7:0 (VCNT) rather than being assumed: reading a PHYSBASE the CPU
 * does not implement is a #GP, which post-EBS is a triple fault, not a diagnostic.
 */
unsigned int hype_mtrr_read_var(hype_mtrr_var_t *out, unsigned int max) {
    unsigned int vcnt = (unsigned int)(rdmsr(MSR_IA32_MTRRCAP) & 0xFFu);
    unsigned int i;
    if (out == 0 || max == 0u) return 0u;
    if (vcnt > HYPE_MTRR_MAX_VAR) vcnt = HYPE_MTRR_MAX_VAR;
    if (vcnt > max) vcnt = max;
    for (i = 0; i < vcnt; i++) {
        out[i].base = rdmsr(MSR_IA32_MTRR_PHYSBASE0 + i * 2u);
        out[i].mask = rdmsr(MSR_IA32_MTRR_PHYSBASE0 + i * 2u + 1u);
    }
    return vcnt;
}

/*
 * MSR_SMI_COUNT. Intel-only: reading it on another vendor is a #GP, which after
 * ExitBootServices is a triple fault rather than a diagnostic, so the CALLER must check the
 * vendor first -- this function cannot, and deliberately does not try.
 */
uint64_t hype_smi_count_unchecked(void) { return rdmsr(0x34u); }
