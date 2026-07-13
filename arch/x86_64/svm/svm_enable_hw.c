#include "svm.h"

/* Host Save Area: a 4KB scratch page the CPU saves host state into on
 * every VMRUN and restores from on every #VMEXIT. We never read it
 * ourselves (that's what the VMCB's save-state area and our own
 * pre/post-VMRUN register save/restore are for) -- it just has to
 * exist and be pointed at. */
static uint8_t g_hsave_area[4096] __attribute__((aligned(4096)));

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/*
 * Exempt from unit testing per AGENTS.md: real RDMSR/WRMSR, nothing to
 * observe without a real CPU. hype_svm_efer_with_svme() in svm_bits.c
 * holds the only real logic (what the new EFER value should be) and is
 * fully tested.
 */
int hype_svm_enable(void) {
    uint64_t efer = rdmsr(HYPE_MSR_EFER);
    wrmsr(HYPE_MSR_EFER, hype_svm_efer_with_svme(efer));
    wrmsr(HYPE_MSR_VM_HSAVE_PA, (uint64_t)g_hsave_area);
    return 0;
}
