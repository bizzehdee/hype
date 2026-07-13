#include <stdio.h>
#include "../../arch/x86_64/svm/svm.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_efer_with_svme(void) {
    /* A realistic EFER for a running 64-bit kernel: SCE|LME|LMA, no
     * SVME yet. */
    uint64_t efer = 0x00000d01ULL;
    uint64_t result = hype_svm_efer_with_svme(efer);

    CHECK_HEX("SVME bit gets set", HYPE_EFER_SVME, result & HYPE_EFER_SVME);
    CHECK_HEX("existing bits (SCE|LME|LMA) are preserved", efer, result & ~HYPE_EFER_SVME);
}

static void test_efer_with_svme_idempotent(void) {
    uint64_t efer = HYPE_EFER_SVME | 0x500ULL;
    uint64_t result = hype_svm_efer_with_svme(efer);
    CHECK_HEX("already-set SVME stays set, nothing else changes", efer, result);
}

static void test_vcpu_enable_apic_accel(void) {
    hype_vmcb_t vmcb;

    hype_vmcb_build_realmode_guest(&vmcb, 0, 0, 0, 0);
    hype_svm_vcpu_enable_apic_accel(&vmcb);

    CHECK_HEX("AVIC enable bit set", HYPE_SVM_INT_CTL_AVIC_ENABLE,
              vmcb.control.vintr & HYPE_SVM_INT_CTL_AVIC_ENABLE);
    CHECK_HEX("apic_bar set to the real LAPIC MMIO base", 0xFEE00000ULL, vmcb.control.avic_apic_bar);

    int backing_nonzero = vmcb.control.avic_backing_page_ptr != 0;
    int logical_nonzero = vmcb.control.avic_logical_table_ptr != 0;
    int physical_nonzero = (vmcb.control.avic_physical_table_ptr & HYPE_SVM_AVIC_ADDR_MASK) != 0;
    CHECK_HEX("backing page pointer wired to a real static buffer", 1, backing_nonzero);
    CHECK_HEX("logical table pointer wired to a real static buffer", 1, logical_nonzero);
    CHECK_HEX("physical table pointer wired to a real static buffer", 1, physical_nonzero);

    int all_distinct = vmcb.control.avic_backing_page_ptr != vmcb.control.avic_logical_table_ptr &&
                        vmcb.control.avic_logical_table_ptr !=
                            (vmcb.control.avic_physical_table_ptr & HYPE_SVM_AVIC_ADDR_MASK) &&
                        vmcb.control.avic_backing_page_ptr !=
                            (vmcb.control.avic_physical_table_ptr & HYPE_SVM_AVIC_ADDR_MASK);
    CHECK_HEX("backing/logical/physical tables are distinct buffers", 1, all_distinct);

    int page_aligned = (vmcb.control.avic_backing_page_ptr & 0xFFFULL) == 0 &&
                        (vmcb.control.avic_logical_table_ptr & 0xFFFULL) == 0 &&
                        (vmcb.control.avic_physical_table_ptr & 0xFFFULL) == 0;
    CHECK_HEX("all table pointers are 4KB-aligned", 1, page_aligned);
}

int main(void) {
    test_efer_with_svme();
    test_efer_with_svme_idempotent();
    test_vcpu_enable_apic_accel();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
