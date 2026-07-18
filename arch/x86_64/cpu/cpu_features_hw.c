#include "cpu_features.h"

/*
 * Runs the real `cpuid` instruction. Exempt from unit testing per
 * AGENTS.md -- the actual decision logic (vendor string decode, VMX/SVM
 * bit checks, backend selection) lives in the tested functions in
 * cpu_features.c; this is deliberately just the three leaf reads.
 */
static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

hype_cpu_diag_t hype_cpu_detect_vmm_kind_diag(void) {
    uint32_t a, b, c, d;
    hype_cpu_diag_t diag;

    cpuid(0, &a, &b, &c, &d);
    diag.vendor = hype_cpu_vendor_from_string(b, c, d);

    cpuid(1, &a, &b, &c, &d);
    diag.has_vmx = hype_cpu_has_vmx(c);

    cpuid(0x80000001, &a, &b, &c, &d);
    diag.has_svm = hype_cpu_has_svm(c);

    diag.kind = hype_vmm_kind_select(diag.vendor, diag.has_vmx, diag.has_svm);
    return diag;
}

hype_vmm_kind_t hype_cpu_detect_vmm_kind(void) {
    return hype_cpu_detect_vmm_kind_diag().kind;
}

/* Reads CPUID Fn8000_000A (SVM revision + feature identification) EDX --
 * the SVM feature bitmap (PAUSEFILTER=10, PFTHRESHOLD=12, ...). Exempt hw
 * shim; the pure bit checks are hype_cpu_has_pause_filter/threshold(). */
uint32_t hype_cpu_svm_feature_edx(void) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    cpuid(0x8000000Au, &a, &b, &c, &d);
    return d;
}
