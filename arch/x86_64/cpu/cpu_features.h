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

/* SVM PAUSE-filter support: CPUID Fn8000_000A_EDX bit 10 (PAUSEFILTER),
 * bit 12 (PFTHRESHOLD). Pure bit checks. */
int hype_cpu_has_pause_filter(uint32_t leaf8000000a_edx);
int hype_cpu_has_pause_threshold(uint32_t leaf8000000a_edx);

/* Real CPUID Fn8000_000A_EDX read (SVM feature bitmap). Exempt hw shim. */
uint32_t hype_cpu_svm_feature_edx(void);

/*
 * #370: is IA32_APERF/IA32_MPERF (MSR 0xE8/0xE7) readable on this core?
 *
 * These are NOT unconditionally present, and #368 read them as if they were. Under a hypervisor
 * that does not implement them -- KVM, i.e. the entire QEMU dev rig -- rdmsr raises #GP, and
 * post-ExitBootServices that is a panic, not a diagnostic. It killed the BSP inside the #368
 * framebuffer probe on every run that had a GOP, which is what #370 was actually about.
 *
 * Each vendor advertises the pair through its own bit, so both are needed:
 *   Intel  CPUID.06H:ECX bit 0        -- effective-frequency interface
 *   AMD    CPUID.8000_0007H:EDX bit 10 -- EffFreqRO
 * An unknown vendor gets 0: the wrong answer here is a triple fault, so the default must be "do
 * not read". Pure bit checks; the leaf reads are the exempt hw shims below.
 */
int hype_cpu_has_eff_freq(hype_cpu_vendor_t vendor, uint32_t leaf6_ecx, uint32_t leaf80000007_edx);

/* Is IA32_THERM_STATUS (0x19C) readable: CPUID.6:EAX[0], the digital thermal sensor. Reading it
 * without this raises #GP, which post-EBS is a panic -- see the note in cpu_features.c. */
int hype_cpu_has_therm_status(hype_cpu_vendor_t vendor, uint32_t leaf6_eax);

/*
 * Real CPUID.06H:ECX / CPUID.8000_0007H:EDX reads. Exempt hw shims.
 *
 * Each returns 0 when the CPU does not implement that leaf at all -- reading past the reported
 * maximum leaf returns another leaf's contents, and mistaking those for a feature bitmap is how a
 * capability check ends up authorising the very #GP it exists to prevent.
 */
uint32_t hype_cpu_leaf6_eax(void);
uint32_t hype_cpu_leaf6_ecx(void);
uint32_t hype_cpu_leaf80000007_edx(void);

/*
 * #604: SMEP (Supervisor Mode Execution Prevention), CPUID.(EAX=7,ECX=0):EBX bit 7. Present on
 * both vendors since roughly 2013 (Intel Ivy Bridge / AMD Excavator onward); a CPU too old to have
 * it just does not get the mitigation, same "the default when absent is off" shape as every other
 * capability check on this page. Pure bit check; the leaf read is the exempt hw shim below.
 */
int hype_cpu_has_smep(uint32_t leaf7_ebx);

/* Real CPUID.(EAX=7,ECX=0):EBX read, gated on the leaf existing first (same #370 discipline as
 * leaf 6/0x80000007 above -- reading past the reported max leaf returns another leaf's contents).
 * Exempt hw shim. */
uint32_t hype_cpu_leaf7_ebx(void);

#endif /* HYPE_ARCH_CPU_FEATURES_H */
