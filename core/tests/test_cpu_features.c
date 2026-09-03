#include <stdio.h>
#include <string.h>
#include "../../arch/x86_64/cpu/cpu_features.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_vendor_from_string(void) {
    /* "GenuineIntel": EBX="Genu", EDX="ineI", ECX="ntel" (note the
     * EBX,EDX,ECX concatenation order -- not register order). */
    CHECK_INT("GenuineIntel decodes as Intel", (int)HYPE_CPU_VENDOR_INTEL,
              (int)hype_cpu_vendor_from_string(0x756e6547u, 0x6c65746eu, 0x49656e69u));

    /* "AuthenticAMD": EBX="Auth", EDX="enti", ECX="cAMD". */
    CHECK_INT("AuthenticAMD decodes as AMD", (int)HYPE_CPU_VENDOR_AMD,
              (int)hype_cpu_vendor_from_string(0x68747541u, 0x444d4163u, 0x69746e65u));

    CHECK_INT("garbage bytes decode as unknown", (int)HYPE_CPU_VENDOR_UNKNOWN,
              (int)hype_cpu_vendor_from_string(0x11111111u, 0x22222222u, 0x33333333u));
}

static void test_has_vmx(void) {
    CHECK_INT("bit 5 set means VMX present", 1, hype_cpu_has_vmx(1u << 5));
    CHECK_INT("bit 5 clear means VMX absent", 0, hype_cpu_has_vmx(0xFFFFFFDFu));
    CHECK_INT("no bits set means VMX absent", 0, hype_cpu_has_vmx(0));
}

static void test_has_svm(void) {
    CHECK_INT("bit 2 set means SVM present", 1, hype_cpu_has_svm(1u << 2));
    CHECK_INT("bit 2 clear means SVM absent", 0, hype_cpu_has_svm(0xFFFFFFFBu));
    CHECK_INT("no bits set means SVM absent", 0, hype_cpu_has_svm(0));
}

static void test_kind_select(void) {
    CHECK_INT("Intel + VMX selects VMX", (int)HYPE_VMM_KIND_VMX,
              (int)hype_vmm_kind_select(HYPE_CPU_VENDOR_INTEL, 1, 0));
    CHECK_INT("AMD + SVM selects SVM", (int)HYPE_VMM_KIND_SVM,
              (int)hype_vmm_kind_select(HYPE_CPU_VENDOR_AMD, 0, 1));
    CHECK_INT("Intel without VMX selects none", (int)HYPE_VMM_KIND_NONE,
              (int)hype_vmm_kind_select(HYPE_CPU_VENDOR_INTEL, 0, 0));
    CHECK_INT("AMD without SVM selects none", (int)HYPE_VMM_KIND_NONE,
              (int)hype_vmm_kind_select(HYPE_CPU_VENDOR_AMD, 0, 0));
    CHECK_INT("unknown vendor selects none even if a feature bit is set",
              (int)HYPE_VMM_KIND_NONE, (int)hype_vmm_kind_select(HYPE_CPU_VENDOR_UNKNOWN, 1, 1));
    CHECK_INT("Intel vendor with only SVM bit (spoofed/corrupt) selects none",
              (int)HYPE_VMM_KIND_NONE, (int)hype_vmm_kind_select(HYPE_CPU_VENDOR_INTEL, 0, 1));
}

static void test_has_pause_filter(void) {
    CHECK_INT("PAUSEFILTER bit 10 set", 1, hype_cpu_has_pause_filter(1u << 10));
    CHECK_INT("PAUSEFILTER bit 10 clear", 0, hype_cpu_has_pause_filter(0));
    CHECK_INT("PAUSEFILTER ignores other bits", 0, hype_cpu_has_pause_filter(~(1u << 10)));
    CHECK_INT("PFTHRESHOLD bit 12 set", 1, hype_cpu_has_pause_threshold(1u << 12));
    CHECK_INT("PFTHRESHOLD bit 12 clear", 0, hype_cpu_has_pause_threshold(0));
    CHECK_INT("PFTHRESHOLD ignores other bits", 0, hype_cpu_has_pause_threshold(~(1u << 12)));
}

/*
 * #370: reading IA32_APERF/MPERF without this check raised #GP under KVM and panicked the BSP
 * inside a diagnostic. Each vendor advertises the pair in its own leaf, so the wrong vendor's bit
 * must NOT authorise the read -- that is the case that faulted.
 */
static void test_has_eff_freq(void) {
    CHECK_INT("Intel CPUID.6:ECX bit 0 set", 1,
              hype_cpu_has_eff_freq(HYPE_CPU_VENDOR_INTEL, 1u, 0u));
    CHECK_INT("Intel CPUID.6:ECX bit 0 clear", 0,
              hype_cpu_has_eff_freq(HYPE_CPU_VENDOR_INTEL, ~1u, ~0u));
    CHECK_INT("AMD CPUID.8000_0007:EDX bit 10 set", 1,
              hype_cpu_has_eff_freq(HYPE_CPU_VENDOR_AMD, 0u, 1u << 10));
    CHECK_INT("AMD CPUID.8000_0007:EDX bit 10 clear", 0,
              hype_cpu_has_eff_freq(HYPE_CPU_VENDOR_AMD, ~0u, ~(1u << 10)));
    CHECK_INT("AMD does not read Intel's bit", 0,
              hype_cpu_has_eff_freq(HYPE_CPU_VENDOR_AMD, 1u, 0u));
    CHECK_INT("Intel does not read AMD's bit", 0,
              hype_cpu_has_eff_freq(HYPE_CPU_VENDOR_INTEL, 0u, 1u << 10));
    /* The default must be "do not read": on an unrecognised vendor the cost of guessing wrong is
     * a #GP with no handler, not a missing log line. */
    CHECK_INT("unknown vendor refuses even with both bits set", 0,
              hype_cpu_has_eff_freq(HYPE_CPU_VENDOR_UNKNOWN, ~0u, ~0u));
}

/* #393 follow-up: IA32_THERM_STATUS is gated by CPUID.6:EAX[0], not by the vendor alone.
 * Reading it on a machine without the digital thermal sensor raises #GP, which post-EBS is a
 * dead BSP -- observed on every boot of the Intel nested rig. */
static void test_therm_status_requires_the_dts_bit(void) {
    CHECK_INT("Intel with DTS is readable", 1,
              hype_cpu_has_therm_status(HYPE_CPU_VENDOR_INTEL, 1u));
    CHECK_INT("Intel without DTS is NOT readable", 0,
              hype_cpu_has_therm_status(HYPE_CPU_VENDOR_INTEL, 0u));
    CHECK_INT("a missing leaf 6 reads as zero and refuses", 0,
              hype_cpu_has_therm_status(HYPE_CPU_VENDOR_INTEL, 0u));
    CHECK_INT("AMD never claims it", 0,
              hype_cpu_has_therm_status(HYPE_CPU_VENDOR_AMD, 1u));
    CHECK_INT("an unknown vendor never claims it", 0,
              hype_cpu_has_therm_status(HYPE_CPU_VENDOR_UNKNOWN, 1u));
}


/* #604: SMEP, CPUID.(EAX=7,ECX=0):EBX bit 7. Vendor-agnostic, unlike the pair above. */
static void test_has_smep(void) {
    CHECK_INT("bit 7 set", 1, hype_cpu_has_smep(1u << 7));
    CHECK_INT("bit 7 clear", 0, hype_cpu_has_smep(~(1u << 7)));
    CHECK_INT("a missing leaf 7 reads as zero and refuses", 0, hype_cpu_has_smep(0u));
    CHECK_INT("an unrelated bit does not false-positive", 0, hype_cpu_has_smep(1u << 6));
}

/* #608: IA32_SPEC_CTRL's legal-bit mask, per vendor's own CPUID enumeration. */
static void test_spec_ctrl_legal_mask(void) {
    CHECK_INT("Intel: all three bits set", 0x7,
              (int)hype_cpu_spec_ctrl_legal_mask(HYPE_CPU_VENDOR_INTEL,
                                                 (1u << 26) | (1u << 27) | (1u << 31), 0u));
    CHECK_INT("Intel: none set", 0x0,
              (int)hype_cpu_spec_ctrl_legal_mask(HYPE_CPU_VENDOR_INTEL, 0u, 0u));
    CHECK_INT("Intel: IBRS only", 0x1,
              (int)hype_cpu_spec_ctrl_legal_mask(HYPE_CPU_VENDOR_INTEL, 1u << 26, 0u));
    CHECK_INT("AMD: all three bits set", 0x7,
              (int)hype_cpu_spec_ctrl_legal_mask(HYPE_CPU_VENDOR_AMD, 0u,
                                                 (1u << 14) | (1u << 15) | (1u << 24)));
    CHECK_INT("AMD: STIBP only", 0x2,
              (int)hype_cpu_spec_ctrl_legal_mask(HYPE_CPU_VENDOR_AMD, 0u, 1u << 15));
    CHECK_INT("AMD does not read Intel's bits", 0x0,
              (int)hype_cpu_spec_ctrl_legal_mask(HYPE_CPU_VENDOR_AMD,
                                                 (1u << 26) | (1u << 27) | (1u << 31), 0u));
    CHECK_INT("unknown vendor grants nothing", 0x0,
              (int)hype_cpu_spec_ctrl_legal_mask(HYPE_CPU_VENDOR_UNKNOWN, ~0u, ~0u));
}

static void test_has_ibpb(void) {
    CHECK_INT("Intel bit 26 set", 1, hype_cpu_has_ibpb(HYPE_CPU_VENDOR_INTEL, 1u << 26, 0u));
    CHECK_INT("Intel bit 26 clear", 0, hype_cpu_has_ibpb(HYPE_CPU_VENDOR_INTEL, 0u, ~0u));
    CHECK_INT("AMD bit 12 set", 1, hype_cpu_has_ibpb(HYPE_CPU_VENDOR_AMD, 0u, 1u << 12));
    CHECK_INT("AMD bit 12 clear", 0, hype_cpu_has_ibpb(HYPE_CPU_VENDOR_AMD, ~0u, 0u));
    CHECK_INT("unknown vendor never claims it", 0,
              hype_cpu_has_ibpb(HYPE_CPU_VENDOR_UNKNOWN, ~0u, ~0u));
}

/* #802 */
static void test_hypervisor_present(void) {
    CHECK_INT("leaf1 ECX bit 31 set means nested", 1, hype_cpu_hypervisor_present(1u << 31));
    CHECK_INT("bit 31 clear means bare metal", 0, hype_cpu_hypervisor_present(~(1u << 31)));
    CHECK_INT("no other bit is mistaken for it", 0, hype_cpu_hypervisor_present(1u << 30));
    CHECK_INT("a zero leaf is bare metal", 0, hype_cpu_hypervisor_present(0u));
}

/* #802: leaf 0x40000000 is EBX,ECX,EDX order -- NOT leaf 0's EBX,EDX,ECX. A test that used the
 * wrong order would still pass on a palindromic string, so use a real signature. */
static void test_hv_signature_decode(void) {
    char out[13];

    /* "KVMKVMKVM" then three NULs: EBX="KVMK", ECX="VMKV", EDX="M" + three NULs. */
    hype_cpu_hv_signature_decode(0x4b4d564bu, 0x564b4d56u, 0x0000004du, out);
    CHECK_INT("KVMKVMKVM decodes", 0, strcmp(out, "KVMKVMKVM"));

    /* "Microsoft Hv": EBX="Micr", ECX="osof", EDX="t Hv". */
    hype_cpu_hv_signature_decode(0x7263694du, 0x666f736fu, 0x76482074u, out);
    CHECK_INT("Microsoft Hv decodes", 0, strcmp(out, "Microsoft Hv"));

    /* An all-zero leaf must read as an empty string, not as 12 NULs printed by a %s. */
    hype_cpu_hv_signature_decode(0u, 0u, 0u, out);
    CHECK_INT("empty leaf is an empty string", 0, strcmp(out, ""));

    /* Always NUL-terminated even when all 12 bytes are used. */
    hype_cpu_hv_signature_decode(~0u, ~0u, ~0u, out);
    CHECK_INT("terminated at 12 characters", 12, (int)strlen(out));
}

int main(void) {
    test_hypervisor_present();
    test_hv_signature_decode();
    test_therm_status_requires_the_dts_bit();
    test_has_smep();
    test_spec_ctrl_legal_mask();
    test_has_ibpb();
    test_vendor_from_string();
    test_has_vmx();
    test_has_svm();
    test_kind_select();
    test_has_pause_filter();
    test_has_eff_freq();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
