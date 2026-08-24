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

/* #370: see the header. Vendor-specific bit, and "unknown vendor" must mean no. */
/*
 * #370's lesson, applied to the MSR it was NOT applied to: IA32_THERM_STATUS (0x19C) is gated by
 * the digital thermal sensor bit, CPUID.6:EAX[0] -- not by the vendor, and not by whatever gate
 * MSR_SMI_COUNT happens to use. hype read it whenever the vendor was Intel, which #GP-panicked
 * the BSP on every boot of the Intel nested-VMX rig, where KVM emulates SMI_COUNT but not this.
 */
int hype_cpu_has_therm_status(hype_cpu_vendor_t vendor, uint32_t leaf6_eax) {
    if (vendor != HYPE_CPU_VENDOR_INTEL) return 0;
    return (int)(leaf6_eax & 1u);
}

int hype_cpu_has_eff_freq(hype_cpu_vendor_t vendor, uint32_t leaf6_ecx,
                          uint32_t leaf80000007_edx) {
    if (vendor == HYPE_CPU_VENDOR_INTEL) return (int)(leaf6_ecx & 1u);
    if (vendor == HYPE_CPU_VENDOR_AMD) return (int)((leaf80000007_edx >> 10) & 1u);
    return 0;
}

/* #604: SMEP, vendor-agnostic -- both vendors use the same bit at the same leaf. */
int hype_cpu_has_smep(uint32_t leaf7_ebx) {
    return (int)((leaf7_ebx >> 7) & 1u);
}

/* #608: see the header for the bit-position citations (both vendors share IA32_SPEC_CTRL's
 * layout; only the CPUID enumeration differs). Unknown vendor grants nothing -- same "the
 * default is do not offer it" rule as every other capability check on this page. */
uint32_t hype_cpu_spec_ctrl_legal_mask(hype_cpu_vendor_t vendor, uint32_t leaf7_edx,
                                       uint32_t leaf80000008_ebx) {
    uint32_t mask = 0;
    if (vendor == HYPE_CPU_VENDOR_INTEL) {
        if ((leaf7_edx >> 26) & 1u) mask |= (1u << 0); /* IBRS */
        if ((leaf7_edx >> 27) & 1u) mask |= (1u << 1); /* STIBP */
        if ((leaf7_edx >> 31) & 1u) mask |= (1u << 2); /* SSBD */
    } else if (vendor == HYPE_CPU_VENDOR_AMD) {
        if ((leaf80000008_ebx >> 14) & 1u) mask |= (1u << 0); /* IBRS */
        if ((leaf80000008_ebx >> 15) & 1u) mask |= (1u << 1); /* STIBP */
        if ((leaf80000008_ebx >> 24) & 1u) mask |= (1u << 2); /* SSBD */
    }
    return mask;
}

int hype_cpu_has_ibpb(hype_cpu_vendor_t vendor, uint32_t leaf7_edx, uint32_t leaf80000008_ebx) {
    if (vendor == HYPE_CPU_VENDOR_INTEL) return (int)((leaf7_edx >> 26) & 1u);
    if (vendor == HYPE_CPU_VENDOR_AMD) return (int)((leaf80000008_ebx >> 12) & 1u);
    return 0;
}

/* #604: NX/XD, vendor-agnostic -- see the header. */
int hype_cpu_has_nx(uint32_t leaf80000001_edx) {
    return (int)((leaf80000001_edx >> 20) & 1u);
}
