#include <stdio.h>
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

int main(void) {
    test_vendor_from_string();
    test_has_vmx();
    test_has_svm();
    test_kind_select();
    test_has_pause_filter();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
