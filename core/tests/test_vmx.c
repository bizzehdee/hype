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

static void test_adjust_controls(void) {
    /* allowed0 (bits 31:0) = must-be-1 mask; allowed1 (bits 63:32) =
     * may-be-1 mask. */
    uint64_t cap;
    uint32_t result;

    /* Bit 0 must be 1 (allowed0 bit 0 set), bit 1 must be 0 (allowed1
     * bit 1 clear) regardless of what's "desired". */
    cap = ((uint64_t)0xFFFFFFFDu << 32) | 0x00000001u;
    result = hype_vmx_adjust_controls(0, cap);
    CHECK_HEX("bit forced to 1 by allowed0 even when not desired", 1u, result & 1u);
    CHECK_HEX("bit forced to 0 by allowed1 even if desired", 0u, hype_vmx_adjust_controls(0xFFFFFFFFu, cap) & 2u);

    /* A bit that's optional (allowed0 clear, allowed1 set) follows the
     * caller's desired value. */
    cap = ((uint64_t)0x00000004u << 32) | 0x00000000u;
    CHECK_HEX("optional bit set when desired", 4u, hype_vmx_adjust_controls(0x4u, cap) & 4u);
    CHECK_HEX("optional bit clear when not desired", 0u, hype_vmx_adjust_controls(0x0u, cap) & 4u);
}

int main(void) {
    test_feature_control_allows_vmxon();
    test_feature_control_with_vmxon_enabled();
    test_cr4_with_vmxe();
    test_adjust_controls();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
