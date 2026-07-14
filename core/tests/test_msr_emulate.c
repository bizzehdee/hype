#include <stdio.h>
#include "../../arch/x86_64/cpu/msr_emulate.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_apic_base_read_allowed(void) {
    CHECK_HEX("APIC_BASE read", HYPE_MSR_ACTION_READ_APIC_BASE,
              hype_msr_decide(HYPE_MSR_NUMBER_APIC_BASE, 0));
}

static void test_apic_base_write_rejected(void) {
    CHECK_HEX("APIC_BASE write", HYPE_MSR_ACTION_REJECT, hype_msr_decide(HYPE_MSR_NUMBER_APIC_BASE, 1));
}

static void test_efer_read_and_write_allowed(void) {
    CHECK_HEX("EFER read", HYPE_MSR_ACTION_READWRITE_EFER, hype_msr_decide(0xC0000080u, 0));
    CHECK_HEX("EFER write", HYPE_MSR_ACTION_READWRITE_EFER, hype_msr_decide(0xC0000080u, 1));
}

static void test_tsc_read_allowed_write_rejected(void) {
    CHECK_HEX("TSC read", HYPE_MSR_ACTION_READ_TSC, hype_msr_decide(HYPE_MSR_NUMBER_TSC, 0));
    CHECK_HEX("TSC write", HYPE_MSR_ACTION_REJECT, hype_msr_decide(HYPE_MSR_NUMBER_TSC, 1));
}

static void test_unknown_msr_rejected_both_directions(void) {
    CHECK_HEX("unknown MSR read", HYPE_MSR_ACTION_REJECT, hype_msr_decide(0xDEADBEEFu, 0));
    CHECK_HEX("unknown MSR write", HYPE_MSR_ACTION_REJECT, hype_msr_decide(0xDEADBEEFu, 1));
}

static void test_apic_base_value(void) {
    uint64_t value = hype_msr_apic_base_value();

    CHECK_HEX("base address bits", 0xFEE00000ULL, value & 0xFFFFF000ULL);
    CHECK_HEX("global enable bit set", 1, (value & (1ULL << 11)) != 0);
    CHECK_HEX("BSP bit set", 1, (value & (1ULL << 8)) != 0);
    CHECK_HEX("x2APIC bit clear", 0, (value & (1ULL << 10)) != 0);
}

int main(void) {
    test_apic_base_read_allowed();
    test_apic_base_write_rejected();
    test_efer_read_and_write_allowed();
    test_tsc_read_allowed_write_rejected();
    test_unknown_msr_rejected_both_directions();
    test_apic_base_value();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
