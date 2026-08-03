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

static void test_asid_for_slot(void) {
    /* ASID 0 belongs to the HOST. A guest handed 0 would share the host's TLB tag,
     * which is the worst possible version of #244 rather than a fix for it. */
    CHECK_HEX("slot 0 -> ASID 1, never 0", 1, hype_svm_asid_for_slot(0u, 16u));
    CHECK_HEX("slot 1 -> ASID 2", 2, hype_svm_asid_for_slot(1u, 16u));
    CHECK_HEX("distinct slots give distinct ASIDs", 0,
              hype_svm_asid_for_slot(0u, 16u) == hype_svm_asid_for_slot(1u, 16u));

    /* Usable guest ASIDs are 1..NASID-1, so NASID=16 tops out at 15. Handing the CPU
     * an out-of-range ASID fails VMRUN outright instead of degrading. */
    CHECK_HEX("clamped to NASID-1", 15, hype_svm_asid_for_slot(14u, 16u));
    CHECK_HEX("slot past the range clamps, not wraps to 0", 15,
              hype_svm_asid_for_slot(99u, 16u));

    /* A CPU with only the host ASID cannot run a guest safely; report 0 so the
     * caller refuses rather than silently sharing tag 0. */
    CHECK_HEX("NASID=1 -> no usable guest ASID", 0, hype_svm_asid_for_slot(0u, 1u));
    CHECK_HEX("NASID=0 -> no usable guest ASID", 0, hype_svm_asid_for_slot(0u, 0u));
    /* Smallest CPU that can host one guest. */
    CHECK_HEX("NASID=2 -> exactly one usable ASID", 1, hype_svm_asid_for_slot(0u, 2u));
    CHECK_HEX("NASID=2, slot 1 clamps onto it", 1, hype_svm_asid_for_slot(1u, 2u));
}

static void test_nasid_from_cpuid(void) {
    /* Fn8000_000A EBX is NASID whole, not a bitfield -- a mask here would silently
     * shrink the pool on a CPU with many ASIDs. */
    CHECK_HEX("EBX passes through", 32768, hype_svm_nasid_from_cpuid_ebx(32768u));
    CHECK_HEX("zero passes through", 0, hype_svm_nasid_from_cpuid_ebx(0u));
}

int main(void) {
    test_efer_with_svme();
    test_efer_with_svme_idempotent();
    test_vcpu_enable_apic_accel();

    test_asid_for_slot();
    test_nasid_from_cpuid();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
