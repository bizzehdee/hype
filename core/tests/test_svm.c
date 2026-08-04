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

/* #316 --------------------------------------------------------------------------------- */

static void test_guest_efer_write_forces_svme(void) {
    /*
     * THE bug. OpenBSD 7.9's kernel long-mode re-entry rebuilds EFER from scratch --
     * `rdmsr; mov %eax,%ebx; xor %eax,%eax; or $0x101,%eax; ...; wrmsr` -- so the value it
     * writes has SVME clear. Storing that verbatim made the NEXT VMRUN fail its first
     * consistency check ("EFER.SVME is zero", APM SS15.5.1) and took hype down with it.
     */
    uint64_t out = 0;

    CHECK_HEX("OpenBSD's rebuilt EFER is accepted", 0,
              hype_svm_guest_efer_write(HYPE_EFER_SVME, HYPE_EFER_SCE | HYPE_EFER_LME, 0, 0, &out));
    CHECK_HEX("SVME is forced back on", HYPE_EFER_SVME, out & HYPE_EFER_SVME);
    CHECK_HEX("the guest's own bits survive", HYPE_EFER_SCE | HYPE_EFER_LME,
              out & (HYPE_EFER_SCE | HYPE_EFER_LME));
}

static void test_guest_efer_write_rejects_mbz_bits(void) {
    /*
     * Bits 9, 16, 19 and 63:22 are MBZ. A real WRMSR raises #GP; letting one through would
     * fail VMRUN's "Any MBZ bit of EFER is set" check, i.e. kill the hypervisor rather than
     * the guest. Each MBZ region is checked separately so a wrong mask cannot pass by luck.
     */
    uint64_t out = 0;

    CHECK_HEX("bit 9 refused", -1, hype_svm_guest_efer_write(0, 1ULL << 9, 0, 0, &out));
    CHECK_HEX("bit 16 refused", -1, hype_svm_guest_efer_write(0, 1ULL << 16, 0, 0, &out));
    CHECK_HEX("bit 19 refused", -1, hype_svm_guest_efer_write(0, 1ULL << 19, 0, 0, &out));
    CHECK_HEX("bit 22 refused", -1, hype_svm_guest_efer_write(0, 1ULL << 22, 0, 0, &out));
    CHECK_HEX("bit 63 refused", -1, hype_svm_guest_efer_write(0, 1ULL << 63, 0, 0, &out));
    /* The defined bits next to those boundaries must NOT be refused. */
    CHECK_HEX("bit 8 (LME) allowed", 0, hype_svm_guest_efer_write(0, 1ULL << 8, 0, 0, &out));
    CHECK_HEX("bit 15 (TCE) allowed", 0, hype_svm_guest_efer_write(0, 1ULL << 15, 0, 0, &out));
    CHECK_HEX("bit 17 (MCOMMIT) allowed", 0, hype_svm_guest_efer_write(0, 1ULL << 17, 0, 0, &out));
    CHECK_HEX("bit 18 (INTWB) allowed", 0, hype_svm_guest_efer_write(0, 1ULL << 18, 0, 0, &out));
    CHECK_HEX("bit 20 (UAIE) allowed", 0, hype_svm_guest_efer_write(0, 1ULL << 20, 0, 0, &out));
    CHECK_HEX("bit 21 (AIBRSE) allowed", 0, hype_svm_guest_efer_write(0, 1ULL << 21, 0, 0, &out));
}

static void test_guest_efer_write_preserves_lma(void) {
    /*
     * APM SS3.1.7: LMA is hardware-owned -- "the value of this bit must be preserved [...] An
     * attempt to write a value that differs from the state determined by hardware results in a
     * #GP fault." A guest that reads-modifies-writes correctly passes; one that invents
     * long-mode state it has not earned by enabling paging is faulted.
     */
    uint64_t out = 0;

    CHECK_HEX("clearing a set LMA is refused", -1,
              hype_svm_guest_efer_write(HYPE_EFER_SVME | HYPE_EFER_LMA, HYPE_EFER_LME, 0, 0, &out));
    CHECK_HEX("inventing LMA is refused", -1,
              hype_svm_guest_efer_write(HYPE_EFER_SVME, HYPE_EFER_LME | HYPE_EFER_LMA, 0, 0, &out));
    CHECK_HEX("preserving a set LMA is allowed", 0,
              hype_svm_guest_efer_write(HYPE_EFER_SVME | HYPE_EFER_LMA,
                                        HYPE_EFER_LME | HYPE_EFER_LMA, 0, 0, &out));
    CHECK_HEX("preserved LMA is stored", HYPE_EFER_LMA, out & HYPE_EFER_LMA);
}

static void test_guest_efer_write_rejects_illegal_long_mode_combinations(void) {
    /*
     * APM SS15.5.1's illegal-state list: "EFER.LME and CR0.PG are both set and CR4.PAE is
     * zero" and "...and CR0.PE is zero". Both are VMRUN-fatal, so they must become the guest's
     * #GP instead. Software sets LME first and CR0.PG second, which is why enabling LME while
     * PG is already on is refused outright.
     */
    uint64_t out = 0;
    const uint64_t pg_pe = HYPE_CR0_PG | HYPE_CR0_PE;

    CHECK_HEX("setting LME while paging is already on is refused", -1,
              hype_svm_guest_efer_write(HYPE_EFER_SVME, HYPE_EFER_LME, pg_pe, HYPE_CR4_PAE, &out));
    /* LME already set (so not a fresh enable), PG on, but PAE off -> still illegal. */
    CHECK_HEX("LME + PG with PAE clear is refused", -1,
              hype_svm_guest_efer_write(HYPE_EFER_SVME | HYPE_EFER_LME, HYPE_EFER_LME, pg_pe, 0,
                                        &out));
    /* LME already set, PG on, PAE on, but PE clear -> illegal. */
    CHECK_HEX("LME + PG with PE clear is refused", -1,
              hype_svm_guest_efer_write(HYPE_EFER_SVME | HYPE_EFER_LME, HYPE_EFER_LME,
                                        HYPE_CR0_PG, HYPE_CR4_PAE, &out));
    /* The legal steady state a 64-bit guest actually runs in must be accepted. */
    CHECK_HEX("LME + PG + PAE + PE is accepted", 0,
              hype_svm_guest_efer_write(HYPE_EFER_SVME | HYPE_EFER_LME | HYPE_EFER_LMA,
                                        HYPE_EFER_LME | HYPE_EFER_LMA | HYPE_EFER_SCE, pg_pe,
                                        HYPE_CR4_PAE, &out));
}

static void test_guest_efer_write_matches_the_openbsd_sequence(void) {
    /*
     * The exact sequence from the guest that found this, in order: efiboot hands off with LME
     * set but paging and PAE off; the kernel sets CR4.PAE, then writes SCE|LME|NXE (SVME gone,
     * LMA correctly preserved as 0), then sets CR3 and CR0.PG|PE. Every step must be accepted,
     * and the stored value must keep SVME throughout -- otherwise the entry after the wrmsr
     * dies.
     */
    uint64_t out = 0;
    uint64_t efer = HYPE_EFER_SVME | HYPE_EFER_LME; /* what hype's builder left, PG/PAE clear */

    CHECK_HEX("the kernel's EFER rebuild is accepted", 0,
              hype_svm_guest_efer_write(efer, HYPE_EFER_SCE | HYPE_EFER_LME | HYPE_EFER_NXE, 0,
                                        HYPE_CR4_PAE, &out));
    CHECK_HEX("stored value would satisfy VMRUN", HYPE_EFER_SVME, out & HYPE_EFER_SVME);
    CHECK_HEX("LMA still clear before paging is enabled", 0, out & HYPE_EFER_LMA);
    CHECK_HEX("NXE took", HYPE_EFER_NXE, out & HYPE_EFER_NXE);
}

static void test_guest_efer_write_drops_raz_bits(void) {
    /* Bits 7:1 are RAZ, not MBZ -- dropped rather than faulted. */
    uint64_t out = 0;

    CHECK_HEX("a write to the RAZ bits is not a fault", 0,
              hype_svm_guest_efer_write(HYPE_EFER_SVME, HYPE_EFER_RAZ | HYPE_EFER_SCE, 0, 0, &out));
    CHECK_HEX("RAZ bits read back as zero", 0, out & HYPE_EFER_RAZ);
    CHECK_HEX("SCE next door is untouched", HYPE_EFER_SCE, out & HYPE_EFER_SCE);
}

static void test_guest_efer_read_hides_svme(void) {
    /*
     * The guest never asked for SVME; hype forces it in because VMRUN demands it. Reporting it
     * back would contradict hype's own CPUID, which clears the SVM feature bit.
     */
    uint64_t stored = HYPE_EFER_SVME | HYPE_EFER_LME | HYPE_EFER_LMA | HYPE_EFER_SCE;

    CHECK_HEX("SVME is hidden from the guest", 0, hype_svm_guest_efer_read(stored) & HYPE_EFER_SVME);
    CHECK_HEX("every other bit is reported as stored", HYPE_EFER_LME | HYPE_EFER_LMA | HYPE_EFER_SCE,
              hype_svm_guest_efer_read(stored));
}

static void test_guest_efer_write_then_read_round_trips(void) {
    /*
     * A guest that reads EFER, ORs a bit and writes it back must not lose anything -- the
     * read hides SVME and the write puts it back, so the pair has to compose.
     */
    uint64_t out = 0;
    uint64_t stored = HYPE_EFER_SVME | HYPE_EFER_LME;
    uint64_t seen = hype_svm_guest_efer_read(stored);

    CHECK_HEX("read-modify-write is accepted", 0,
              hype_svm_guest_efer_write(stored, seen | HYPE_EFER_SCE, 0, 0, &out));
    CHECK_HEX("the added bit took", HYPE_EFER_SCE, out & HYPE_EFER_SCE);
    CHECK_HEX("SVME survived the round trip", HYPE_EFER_SVME, out & HYPE_EFER_SVME);
    CHECK_HEX("LME survived the round trip", HYPE_EFER_LME, out & HYPE_EFER_LME);
}

static void test_guest_efer_write_leaves_out_untouched_on_a_fault(void) {
    /* A refused write must not have partially updated the caller's value. */
    uint64_t out = 0xA5A5A5A5A5A5A5A5ULL;

    CHECK_HEX("refused", -1, hype_svm_guest_efer_write(0, 1ULL << 63, 0, 0, &out));
    CHECK_HEX("out is untouched on a fault", 0xA5A5A5A5A5A5A5A5ULL, out);
}

int main(void) {
    test_guest_efer_write_forces_svme();
    test_guest_efer_write_rejects_mbz_bits();
    test_guest_efer_write_preserves_lma();
    test_guest_efer_write_rejects_illegal_long_mode_combinations();
    test_guest_efer_write_matches_the_openbsd_sequence();
    test_guest_efer_write_drops_raz_bits();
    test_guest_efer_read_hides_svme();
    test_guest_efer_write_then_read_round_trips();
    test_guest_efer_write_leaves_out_untouched_on_a_fault();
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
