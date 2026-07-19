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

    hype_cpuid_emulate(0, 0, &real, 0, &out);

    CHECK_HEX("max basic leaf", 0x16, out.eax);
    CHECK_HEX("ebx \"Auth\"", 0x68747541u, out.ebx);
    CHECK_HEX("edx \"enti\"", 0x69746e65u, out.edx);
    CHECK_HEX("ecx \"cAMD\"", 0x444d4163u, out.ecx);
}

static void test_leaf1_forces_hypervisor_bit_and_clears_mtrr(void) {
    hype_cpuid_result_t real = {0x00A00F11u, 0x12345678u, 0x7FFAFBFFu, 0xFFFAFBFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(1, 0, &real, 0, &out);

    CHECK_HEX("eax passthrough", real.eax, out.eax);
    CHECK_HEX("ebx passthrough", real.ebx, out.ebx);
    CHECK_HEX("hypervisor-present bit forced set", 1, (out.ecx & (1u << 31)) != 0);
    CHECK_HEX("TSC_DEADLINE bit forced clear", 0, (out.ecx & (1u << 24)) != 0);
    CHECK_HEX("XSAVE bit forced clear", 0, (out.ecx & (1u << 26)) != 0);
    CHECK_HEX("OSXSAVE bit forced clear", 0, (out.ecx & (1u << 27)) != 0);
    /* ecx = real | hypervisor-present, minus TSC_DEADLINE + XSAVE + OSXSAVE. */
    CHECK_HEX("ecx otherwise passthrough",
              (real.ecx | (1u << 31)) & ~((1u << 24) | (1u << 26) | (1u << 27)), out.ecx);
    CHECK_HEX("MTRR bit forced clear", 0, (out.edx & (1u << 12)) != 0);
    CHECK_HEX("edx otherwise passthrough", real.edx & ~(1u << 12), out.edx);
}

static void test_leaf15_tsc_frequency_when_published(void) {
    /* tsc_khz = 2_994_000 -> ECX = crystal Hz = 2_994_000_000, ratio 1/1,
     * so Linux computes tsc_khz = (ECX/1000)*EBX/EAX = 2_994_000. */
    hype_cpuid_result_t real = {0xAAAAu, 0xBBBBu, 0xCCCCu, 0xDDDDu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x15u, 0, &real, 2994000u, &out);

    CHECK_HEX("leaf 0x15 EAX = ratio denominator 1", 1u, out.eax);
    CHECK_HEX("leaf 0x15 EBX = ratio numerator 1", 1u, out.ebx);
    CHECK_HEX("leaf 0x15 ECX = crystal Hz", 2994000000u, out.ecx);
    CHECK_HEX("leaf 0x15 EDX = 0", 0u, out.edx);
}

static void test_leaf15_all_zero_when_unpublished(void) {
    hype_cpuid_result_t real = {0xAAAAu, 0xBBBBu, 0xCCCCu, 0xDDDDu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x15u, 0, &real, 0u, &out);

    CHECK_HEX("leaf 0x15 EAX zero", 0u, out.eax);
    CHECK_HEX("leaf 0x15 EBX zero", 0u, out.ebx);
    CHECK_HEX("leaf 0x15 ECX zero", 0u, out.ecx);
    CHECK_HEX("leaf 0x15 EDX zero", 0u, out.edx);
}

static void test_leaf16_base_frequency_mhz(void) {
    hype_cpuid_result_t real = {0xAAAAu, 0xBBBBu, 0xCCCCu, 0xDDDDu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x16u, 0, &real, 2994000u, &out);

    CHECK_HEX("leaf 0x16 EAX = base MHz (kHz/1000)", 2994u, out.eax);
    CHECK_HEX("leaf 0x16 EBX zero", 0u, out.ebx);
    CHECK_HEX("leaf 0x16 ECX zero", 0u, out.ecx);
    CHECK_HEX("leaf 0x16 EDX zero", 0u, out.edx);

    hype_cpuid_emulate(0x16u, 0, &real, 0u, &out);
    CHECK_HEX("leaf 0x16 EAX zero when unpublished", 0u, out.eax);
}

static void test_leaf1_mtrr_already_clear_is_idempotent(void) {
    hype_cpuid_result_t real = {0, 0, 0, 0xFFFFEFFFu}; /* MTRR bit already 0 */
    hype_cpuid_result_t out;

    hype_cpuid_emulate(1, 0, &real, 0, &out);

    CHECK_HEX("edx unchanged when MTRR bit already clear", real.edx, out.edx);
}

static void test_leaf_extended_max_vendor_string(void) {
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000000u, 0, &real, 0, &out);

    CHECK_HEX("max extended leaf", 0x80000008u, out.eax);
    CHECK_HEX("ebx \"Auth\"", 0x68747541u, out.ebx);
    CHECK_HEX("edx \"enti\"", 0x69746e65u, out.edx);
    CHECK_HEX("ecx \"cAMD\"", 0x444d4163u, out.ecx);
}

static void test_leaf_ext1_clears_svm_bit(void) {
    hype_cpuid_result_t real = {0x00800F11u, 0, 0xFFFFFFFFu, 0x2C100800u};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000001u, 0, &real, 0, &out);

    CHECK_HEX("eax passthrough", real.eax, out.eax);
    CHECK_HEX("edx passthrough (NX/LM bits)", real.edx, out.edx);
    CHECK_HEX("SVM bit forced clear", 0, (out.ecx & (1u << 2)) != 0);
    CHECK_HEX("ecx otherwise passthrough", real.ecx & ~(1u << 2), out.ecx);
    CHECK_HEX("ebx is zero", 0, out.ebx);
}

static void test_leaf_ext1_svm_already_clear_is_idempotent(void) {
    hype_cpuid_result_t real = {0, 0, 0xFFFFFFFBu, 0}; /* SVM bit already 0 */
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000001u, 0, &real, 0, &out);

    CHECK_HEX("ecx unchanged when SVM bit already clear", real.ecx, out.ecx);
}

static void test_leaf_ext8_address_sizes_passthrough(void) {
    hype_cpuid_result_t real = {0x00003028u, 0x00000000u, 0x00000000u, 0x00000000u};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000008u, 0, &real, 0, &out);

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

    hype_cpuid_emulate(6u, 0, &real, 0, &out);

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

    hype_cpuid_emulate(0x80000007u, 0, &real, 0, &out);

    CHECK_HEX("eax zeroed", 0u, out.eax);
    CHECK_HEX("ebx zeroed", 0u, out.ebx);
    CHECK_HEX("ecx zeroed", 0u, out.ecx);
    CHECK_HEX("edx = Invariant TSC (bit 8) only", (1u << 8), out.edx);
}

static void test_hypervisor_leaf_signature(void) {
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x40000000u, 0, &real, 0, &out);

    CHECK_HEX("max hypervisor leaf", 0x40000000u, out.eax);
    CHECK_HEX("ebx \"Hype\"", 0x65707948u, out.ebx);
    CHECK_HEX("ecx \"Hype\"", 0x65707948u, out.ecx);
    CHECK_HEX("edx \"Hype\"", 0x65707948u, out.edx);
}

static void test_unhandled_leaf_returns_all_zero(void) {
    hype_cpuid_result_t real = {0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(2, 0, &real, 0, &out); /* cache descriptors -- not implemented */

    CHECK_HEX("eax", 0, out.eax);
    CHECK_HEX("ebx", 0, out.ebx);
    CHECK_HEX("ecx", 0, out.ecx);
    CHECK_HEX("edx", 0, out.edx);
}

static void test_unhandled_extended_leaf_returns_all_zero(void) {
    hype_cpuid_result_t real = {1, 2, 3, 4};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000004u, 0, &real, 0, &out); /* brand string -- not implemented */

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
    test_leaf6_advertises_arat_only();
    test_leaf15_tsc_frequency_when_published();
    test_leaf15_all_zero_when_unpublished();
    test_leaf16_base_frequency_mhz();
    test_leaf_ext7_advertises_invariant_tsc_only();
    test_leaf_ext8_address_sizes_passthrough();
    test_hypervisor_leaf_signature();
    test_unhandled_leaf_returns_all_zero();
    test_unhandled_extended_leaf_returns_all_zero();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
