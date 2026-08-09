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

/*
 * #368: IA32_APERF / IA32_MPERF -- the architectural way to ask "what speed is this core
 * ACTUALLY running at". MPERF counts at the nominal frequency; APERF counts at the delivered
 * one. Their ratio across a window is the delivered fraction, and it is the one measurement that
 * separates "each memory access is genuinely slow" from "the core is being clocked down", which
 * chunk timing alone cannot do. Present on both Intel and AMD.
 */
void hype_perf_read_amperf(uint64_t *aperf, uint64_t *mperf) {
    if (aperf != 0) *aperf = rdmsr(0xE8u);
    if (mperf != 0) *mperf = rdmsr(0xE7u);
}

/* IA32_THERM_STATUS. Intel-only; caller must check the vendor, as for the SMI counter.
 * Bit 0 = thermal status now, bit 1 = log (sticky), bit 10 = power limit status. */
uint64_t hype_therm_status_unchecked(void) { return rdmsr(0x19Cu); }

/* CR0, to see whether cache-disable (bit 30) or not-write-through (bit 29) has been set on this
 * core -- the one thing that would make every access uniformly slow without any MSR changing. */
uint64_t hype_read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}
