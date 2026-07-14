#ifndef HYPE_ARCH_CPU_FEATURES_H
#define HYPE_ARCH_CPU_FEATURES_H

#include <stdint.h>

/*
 * CPU vendor/virtualization-extension detection (M2-1). Same split as
 * everywhere else: given already-fetched CPUID register values, the
 * decision logic is pure and tested here; the actual `cpuid` execution
 * is the thin, exempt hardware shim in cpu_features_hw.c.
 */

typedef enum {
    HYPE_CPU_VENDOR_UNKNOWN = 0,
    HYPE_CPU_VENDOR_INTEL,
    HYPE_CPU_VENDOR_AMD
} hype_cpu_vendor_t;

/* Decodes CPUID leaf 0's ebx/ecx/edx (the 12-character vendor string,
 * e.g. "GenuineIntel"/"AuthenticAMD") into a vendor enum. */
hype_cpu_vendor_t hype_cpu_vendor_from_string(uint32_t ebx, uint32_t ecx, uint32_t edx);

/* VMX support: CPUID leaf 1, ECX bit 5. */
int hype_cpu_has_vmx(uint32_t leaf1_ecx);

/* SVM support: CPUID leaf 0x80000001, ECX bit 2. */
int hype_cpu_has_svm(uint32_t leaf80000001_ecx);

typedef enum {
    HYPE_VMM_KIND_NONE = 0, /* neither VMX nor SVM available/usable */
    HYPE_VMM_KIND_VMX,
    HYPE_VMM_KIND_SVM
} hype_vmm_kind_t;

/*
 * Picks which backend to use given vendor + extension support: SVM only
 * makes sense on AMD, VMX only on Intel (matches each vendor's own
 * feature bit even if a vendor string was spoofed/corrupted -- both
 * conditions must agree). Pure decision logic.
 */
hype_vmm_kind_t hype_vmm_kind_select(hype_cpu_vendor_t vendor, int has_vmx, int has_svm);

/*
 * Runs the real CPUID leaves and returns which backend to use. Exempt
 * from unit testing -- see cpu_features_hw.c. All the actual decision
 * logic above (vendor_from_string/has_vmx/has_svm/kind_select) is
 * fully tested.
 */
hype_vmm_kind_t hype_cpu_detect_vmm_kind(void);

typedef struct {
    hype_cpu_vendor_t vendor;
    int has_vmx;
    int has_svm;
    hype_vmm_kind_t kind;
} hype_cpu_diag_t;

/*
 * Same real CPUID probe as hype_cpu_detect_vmm_kind(), but also
 * reports the raw vendor/feature-bit findings that went into the
 * decision -- real-hardware bring-up (AGENTS.md's mandatory real-
 * hardware validation pass) is where a machine turning out to be the
 * "wrong" vendor, or a feature bit unexpectedly absent (e.g. SVM
 * disabled in firmware setup), is the single most useful thing to see
 * in a serial log before anything else runs. Exempt from unit testing
 * for the same reason as hype_cpu_detect_vmm_kind() -- real CPUID,
 * no decision logic of its own beyond calling the already-tested pure
 * functions above.
 */
hype_cpu_diag_t hype_cpu_detect_vmm_kind_diag(void);

#endif /* HYPE_ARCH_CPU_FEATURES_H */
