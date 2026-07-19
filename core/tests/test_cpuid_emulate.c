#include <stdio.h>
#include "../../arch/x86_64/cpu/cpuid_emulate.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_leaf0_vendor_string(void) {
    hype_cpuid_result_t real = {0, 0, 0, 0}; /* unused for leaf 0 */
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0, 0, &real, &out);

    CHECK_HEX("max basic leaf", 1, out.eax);
    CHECK_HEX("ebx \"Auth\"", 0x68747541u, out.ebx);
    CHECK_HEX("edx \"enti\"", 0x69746e65u, out.edx);
    CHECK_HEX("ecx \"cAMD\"", 0x444d4163u, out.ecx);
}

static void test_leaf1_forces_hypervisor_bit_and_clears_mtrr(void) {
    hype_cpuid_result_t real = {0x00A00F11u, 0x12345678u, 0x7FFAFBFFu, 0xFFFAFBFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(1, 0, &real, &out);

    CHECK_HEX("eax passthrough", real.eax, out.eax);
    CHECK_HEX("ebx passthrough", real.ebx, out.ebx);
    CHECK_HEX("hypervisor-present bit forced set", 1, (out.ecx & (1u << 31)) != 0);
    CHECK_HEX("TSC_DEADLINE bit forced clear", 0, (out.ecx & (1u << 24)) != 0);
    /* ecx = real | hypervisor-present, minus the TSC_DEADLINE bit. */
    CHECK_HEX("ecx otherwise passthrough", (real.ecx | (1u << 31)) & ~(1u << 24), out.ecx);
    CHECK_HEX("MTRR bit forced clear", 0, (out.edx & (1u << 12)) != 0);
    CHECK_HEX("edx otherwise passthrough", real.edx & ~(1u << 12), out.edx);
}

static void test_leaf1_mtrr_already_clear_is_idempotent(void) {
    hype_cpuid_result_t real = {0, 0, 0, 0xFFFFEFFFu}; /* MTRR bit already 0 */
    hype_cpuid_result_t out;

    hype_cpuid_emulate(1, 0, &real, &out);

    CHECK_HEX("edx unchanged when MTRR bit already clear", real.edx, out.edx);
}

static void test_leaf_extended_max_vendor_string(void) {
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000000u, 0, &real, &out);

    CHECK_HEX("max extended leaf", 0x80000008u, out.eax);
    CHECK_HEX("ebx \"Auth\"", 0x68747541u, out.ebx);
    CHECK_HEX("edx \"enti\"", 0x69746e65u, out.edx);
    CHECK_HEX("ecx \"cAMD\"", 0x444d4163u, out.ecx);
}

static void test_leaf_ext1_clears_svm_bit(void) {
    hype_cpuid_result_t real = {0x00800F11u, 0, 0xFFFFFFFFu, 0x2C100800u};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000001u, 0, &real, &out);

    CHECK_HEX("eax passthrough", real.eax, out.eax);
    CHECK_HEX("edx passthrough (NX/LM bits)", real.edx, out.edx);
    CHECK_HEX("SVM bit forced clear", 0, (out.ecx & (1u << 2)) != 0);
    CHECK_HEX("ecx otherwise passthrough", real.ecx & ~(1u << 2), out.ecx);
    CHECK_HEX("ebx is zero", 0, out.ebx);
}

static void test_leaf_ext1_svm_already_clear_is_idempotent(void) {
    hype_cpuid_result_t real = {0, 0, 0xFFFFFFFBu, 0}; /* SVM bit already 0 */
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000001u, 0, &real, &out);

    CHECK_HEX("ecx unchanged when SVM bit already clear", real.ecx, out.ecx);
}

static void test_leaf_ext8_address_sizes_passthrough(void) {
    hype_cpuid_result_t real = {0x00003028u, 0x00000000u, 0x00000000u, 0x00000000u};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000008u, 0, &real, &out);

    CHECK_HEX("eax passthrough (phys/linear address widths)", real.eax, out.eax);
    CHECK_HEX("ebx passthrough", real.ebx, out.ebx);
    CHECK_HEX("ecx passthrough", real.ecx, out.ecx);
    CHECK_HEX("edx passthrough", real.edx, out.edx);
}

static void test_leaf6_advertises_arat_only(void) {
    /* Real hardware reports thermal/power bits here; hype advertises only
     * ARAT (EAX bit 2) so Linux trusts the LAPIC timer in idle instead of
     * falling back to the 100 Hz PIT broadcast. */
    hype_cpuid_result_t real = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(6u, 0, &real, &out);

    CHECK_HEX("eax = ARAT (bit 2) only", (1u << 2), out.eax);
    CHECK_HEX("ebx zeroed", 0u, out.ebx);
    CHECK_HEX("ecx zeroed", 0u, out.ecx);
    CHECK_HEX("edx zeroed", 0u, out.edx);
}

static void test_leaf_ext7_advertises_invariant_tsc_only(void) {
    /* Real hardware would report power-management bits here; hype ignores
     * them and advertises only Invariant TSC (EDX bit 8) so the guest keeps
     * its passthrough TSC instead of marking it unstable. */
    hype_cpuid_result_t real = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000007u, 0, &real, &out);

    CHECK_HEX("eax zeroed", 0u, out.eax);
    CHECK_HEX("ebx zeroed", 0u, out.ebx);
    CHECK_HEX("ecx zeroed", 0u, out.ecx);
    CHECK_HEX("edx = Invariant TSC (bit 8) only", (1u << 8), out.edx);
}

static void test_hypervisor_signature_is_kvm(void) {
    /* PVCLOCK: present the KVM identity so a Linux/BSD guest enables kvmclock
     * (bypasses the guest's failing TSC calibration). "KVMKVMKVM\0\0\0". */
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x40000000u, 0, &real, &out);

    CHECK_HEX("max hypervisor leaf = 0x40000001", 0x40000001u, out.eax);
    CHECK_HEX("ebx \"KVMK\"", 0x4b4d564bu, out.ebx);
    CHECK_HEX("ecx \"VMKV\"", 0x564b4d56u, out.ecx);
    CHECK_HEX("edx \"M\\0\\0\\0\"", 0x0000004du, out.edx);
}

static void test_kvm_features_leaf_advertises_only_pvclock(void) {
    hype_cpuid_result_t real = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x40000001u, 0, &real, &out);

    /* Only CLOCKSOURCE (bit 0), CLOCKSOURCE2 (bit 3), TSC_STABLE (bit 24). */
    CHECK_HEX("kvm features eax", (1u << 0) | (1u << 3) | (1u << 24), out.eax);
    CHECK_HEX("kvm features ebx zeroed", 0u, out.ebx);
    CHECK_HEX("kvm features ecx zeroed", 0u, out.ecx);
    CHECK_HEX("kvm features edx zeroed", 0u, out.edx);
    CHECK_HEX("async-PF NOT advertised (bit 2)", 0, (out.eax & (1u << 2)) != 0);
    CHECK_HEX("steal-time NOT advertised (bit 5)", 0, (out.eax & (1u << 5)) != 0);
}

static void test_unhandled_leaf_returns_all_zero(void) {
    hype_cpuid_result_t real = {0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(2, 0, &real, &out); /* cache descriptors -- not implemented */

    CHECK_HEX("eax", 0, out.eax);
    CHECK_HEX("ebx", 0, out.ebx);
    CHECK_HEX("ecx", 0, out.ecx);
    CHECK_HEX("edx", 0, out.edx);
}

static void test_unhandled_extended_leaf_returns_all_zero(void) {
    hype_cpuid_result_t real = {1, 2, 3, 4};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000004u, 0, &real, &out); /* brand string -- not implemented */

    CHECK_HEX("eax", 0, out.eax);
    CHECK_HEX("ebx", 0, out.ebx);
    CHECK_HEX("ecx", 0, out.ecx);
    CHECK_HEX("edx", 0, out.edx);
}

int main(void) {
    test_leaf0_vendor_string();
    test_leaf1_forces_hypervisor_bit_and_clears_mtrr();
    test_leaf1_mtrr_already_clear_is_idempotent();
    test_leaf_extended_max_vendor_string();
    test_leaf_ext1_clears_svm_bit();
    test_leaf_ext1_svm_already_clear_is_idempotent();
    test_leaf_ext8_address_sizes_passthrough();
    test_leaf6_advertises_arat_only();
    test_leaf_ext7_advertises_invariant_tsc_only();
    test_hypervisor_signature_is_kvm();
    test_kvm_features_leaf_advertises_only_pvclock();
    test_unhandled_leaf_returns_all_zero();
    test_unhandled_extended_leaf_returns_all_zero();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
