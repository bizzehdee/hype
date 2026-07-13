#include <stdio.h>
#include "../../arch/x86_64/vmx/vmx.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_feature_control_allows_vmxon(void) {
    CHECK_INT("unlocked (0) allows VMXON -- we can configure it ourselves",
              1, hype_vmx_feature_control_allows_vmxon(0));
    CHECK_INT("locked with VMX-outside-SMX enabled allows VMXON", 1,
              hype_vmx_feature_control_allows_vmxon(HYPE_FEATURE_CONTROL_LOCK |
                                                      HYPE_FEATURE_CONTROL_VMX_OUTSIDE_SMX));
    CHECK_INT("locked without VMX-outside-SMX forbids VMXON", 0,
              hype_vmx_feature_control_allows_vmxon(HYPE_FEATURE_CONTROL_LOCK));
}

static void test_feature_control_with_vmxon_enabled(void) {
    uint64_t result = hype_vmx_feature_control_with_vmxon_enabled(0x10ULL);
    CHECK_HEX("lock bit set", HYPE_FEATURE_CONTROL_LOCK, result & HYPE_FEATURE_CONTROL_LOCK);
    CHECK_HEX("VMX-outside-SMX bit set", HYPE_FEATURE_CONTROL_VMX_OUTSIDE_SMX,
              result & HYPE_FEATURE_CONTROL_VMX_OUTSIDE_SMX);
    CHECK_HEX("other bits preserved", 0x10ULL,
              result & ~(HYPE_FEATURE_CONTROL_LOCK | HYPE_FEATURE_CONTROL_VMX_OUTSIDE_SMX));
}

static void test_cr4_with_vmxe(void) {
    uint64_t cr4 = 0x20ULL; /* PAE (bit 5) -- unrelated to VMXE (bit 13) */
    uint64_t result = hype_vmx_cr4_with_vmxe(cr4);
    CHECK_HEX("VMXE bit set", HYPE_CR4_VMXE, result & HYPE_CR4_VMXE);
    CHECK_HEX("existing bits preserved", cr4, result & ~HYPE_CR4_VMXE);
}

int main(void) {
    test_feature_control_allows_vmxon();
    test_feature_control_with_vmxon_enabled();
    test_cr4_with_vmxe();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
