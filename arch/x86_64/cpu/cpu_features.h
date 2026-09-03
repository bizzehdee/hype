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

/*
 * #802: is hype itself running as a guest?
 *
 * CPUID leaf 1 ECX bit 31 is the architectural "hypervisor present" bit -- reserved-zero on real
 * hardware, set by every hypervisor that means to be discoverable. This is NOT the same question
 * as hype_cpu_detect_vmm_kind(), which answers "VMX or SVM", i.e. which vendor's extension to
 * drive; a nested rig and bare metal answer that one identically.
 *
 * It is a hint, not a guarantee: a hypervisor may clear the bit to hide, and the failure mode
 * matters. A false "bare metal" costs correctness (a workaround that a nested rig needs gets
 * dropped); a false "nested" costs only performance. So callers must treat 0 as the assertive
 * answer and anything they cannot confirm as "nested" -- see hype_vmcb_set_rdtsc_intercept().
 */
int hype_cpu_hypervisor_present(uint32_t leaf1_ecx);

/*
 * Decodes CPUID leaf 0x40000000's EBX/ECX/EDX into the 12-character hypervisor signature
 * ("KVMKVMKVM\0\0\0", "Microsoft Hv", "VMwareVMware", ...). Same byte layout as leaf 0's vendor
 * string but in plain EBX/ECX/EDX order, which is the one difference worth having a separate
 * function for. Only meaningful when hype_cpu_hypervisor_present() said yes; `out` is left an
 * empty string for an all-zero leaf so a log line reads as "unnamed", not as garbage.
 */
void hype_cpu_hv_signature_decode(uint32_t ebx, uint32_t ecx, uint32_t edx, char out[13]);

typedef struct {
    int present;         /* leaf 1 ECX bit 31 */
    char signature[13];  /* leaf 0x40000000, "" when absent or unnamed */
} hype_cpu_hv_t;

/* Real CPUID probe for the two above. Exempt hw shim -- the decision logic is the pure pair. */
hype_cpu_hv_t hype_cpu_detect_hypervisor(void);

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

/*
 * #608: which bits of IA32_SPEC_CTRL (0x48) this real host CPU actually implements -- IBRS
 * (bit 0), STIBP (bit 1), SSBD (bit 2). Both vendors share this exact bit layout (AMD adopted
 * Intel's IA32_SPEC_CTRL shape for guest/OS compatibility), but each enumerates support through
 * its own CPUID leaf: Intel CPUID.(EAX=7,ECX=0):EDX bits 26 (IBRS_IBPB)/27 (STIBP)/31 (SSBD);
 * AMD CPUID.8000_0008H:EBX bits 14 (IBRS)/15 (STIBP)/24 (SSBD). A guest WRMSR to SPEC_CTRL must be
 * masked to exactly this set before being stored/applied -- accepting a bit the real hardware does
 * not implement would let a guest arm a control that silently does nothing, the same "advertised a
 * dead control" failure #269 already names for a different MSR.
 */
uint32_t hype_cpu_spec_ctrl_legal_mask(hype_cpu_vendor_t vendor, uint32_t leaf7_edx,
                                       uint32_t leaf80000008_ebx);

/* Real CPUID.(EAX=7,ECX=0):EDX read. Exempt hw shim. */
uint32_t hype_cpu_leaf7_edx(void);

/* Whether IA32_PRED_CMD (0x49) bit 0 (IBPB) is real on this host: Intel CPUID.(EAX=7,ECX=0):EDX
 * bit 26 (the same bit that gates IBRS -- Intel enumerates the pair together); AMD
 * CPUID.8000_0008H:EBX bit 12 (IBPB is its own, separate bit on AMD, unlike Intel). */
int hype_cpu_has_ibpb(hype_cpu_vendor_t vendor, uint32_t leaf7_edx, uint32_t leaf80000008_ebx);

/* Real CPUID.8000_0008H:EBX read, gated on the leaf existing (checked against CPUID.8000_0000H's
 * own reported max extended leaf). Exempt hw shim. */
uint32_t hype_cpu_leaf80000008_ebx(void);

/*
 * #604: NX/XD (No-Execute), CPUID.8000_0001H:EDX bit 20. Both vendors gate the SAME EFER.NXE
 * (MSR 0xC0000080) bit 11 behind this same extended-leaf bit -- Intel calls it XD, AMD calls it
 * NX, but the CPUID enumeration and the EFER bit it unlocks are identical, unlike SMEP/SPEC_CTRL
 * above where the two vendors enumerate through different leaves. A CPU old enough to lack it
 * just does not get the mitigation.
 */
int hype_cpu_has_nx(uint32_t leaf80000001_edx);

/* Real CPUID.8000_0001H:EDX read. Exempt hw shim. */
uint32_t hype_cpu_leaf80000001_edx(void);

#endif /* HYPE_ARCH_CPU_FEATURES_H */
