#include "cpu_features.h"
#include "../../../core/strutil.h"

hype_cpu_vendor_t hype_cpu_vendor_from_string(uint32_t ebx, uint32_t ecx, uint32_t edx) {
    char vendor[13];
    unsigned int i;

    /* Concatenation order is EBX, EDX, ECX -- not register order --
     * per CPUID leaf 0's spec (verified against the well-known
     * "GenuineIntel"/"AuthenticAMD" byte layout: EBX="Genu", EDX="ineI",
     * ECX="ntel"). Byte-copied rather than pointer-cast to avoid a
     * strict-aliasing violation. */
    for (i = 0; i < 4; i++) {
        vendor[i] = (char)((ebx >> (8 * i)) & 0xFFu);
        vendor[4 + i] = (char)((edx >> (8 * i)) & 0xFFu);
        vendor[8 + i] = (char)((ecx >> (8 * i)) & 0xFFu);
    }
    vendor[12] = '\0';

    if (hype_streq(vendor, "GenuineIntel")) {
        return HYPE_CPU_VENDOR_INTEL;
    }
    if (hype_streq(vendor, "AuthenticAMD")) {
        return HYPE_CPU_VENDOR_AMD;
    }
    return HYPE_CPU_VENDOR_UNKNOWN;
}

int hype_cpu_has_vmx(uint32_t leaf1_ecx) {
    return (int)((leaf1_ecx >> 5) & 1u);
}

int hype_cpu_has_svm(uint32_t leaf80000001_ecx) {
    return (int)((leaf80000001_ecx >> 2) & 1u);
}

hype_vmm_kind_t hype_vmm_kind_select(hype_cpu_vendor_t vendor, int has_vmx, int has_svm) {
    if (vendor == HYPE_CPU_VENDOR_INTEL && has_vmx) {
        return HYPE_VMM_KIND_VMX;
    }
    if (vendor == HYPE_CPU_VENDOR_AMD && has_svm) {
        return HYPE_VMM_KIND_SVM;
    }
    return HYPE_VMM_KIND_NONE;
}

/* SVM PAUSE-filter support: CPUID Fn8000_000A_EDX bit 10 (PAUSEFILTER) --
 * the VMCB pause_filter_count that lets a spin loop be intercepted after a
 * burst of PAUSEs. Bit 12 (PFTHRESHOLD) additionally enables the
 * pause_filter_threshold window. Pure bit checks; the real CPUID read is the
 * exempt hw shim. */
int hype_cpu_has_pause_filter(uint32_t leaf8000000a_edx) {
    return (int)((leaf8000000a_edx >> 10) & 1u);
}

int hype_cpu_has_pause_threshold(uint32_t leaf8000000a_edx) {
    return (int)((leaf8000000a_edx >> 12) & 1u);
}
