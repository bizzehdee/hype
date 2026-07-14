#include <stdio.h>
#include "../../arch/x86_64/svm/vmcb.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_struct_sizes(void) {
    /* Enforced at compile time too (_Static_assert in vmcb.h) -- this
     * just makes the same fact visible as a normal test result. */
    CHECK_HEX("control area is exactly 1024 bytes", 0x400, sizeof(hype_vmcb_control_t));
    CHECK_HEX("VMCB is exactly one 4KB page", 0x1000, sizeof(hype_vmcb_t));
    CHECK_HEX("state-save area fills the rest of the page", 0xC00, sizeof(hype_vmcb_save_t));
}

static void test_field_offsets(void) {
    hype_vmcb_t vmcb;
    unsigned char *base = (unsigned char *)&vmcb;

    /* Spot-check a handful of offsets against the AMD SDM tables
     * directly, rather than only trusting the struct's own field
     * ordering to imply correctness. */
    CHECK_HEX("intercept_misc1 at 0x00C", (unsigned long long)(base + 0x00C),
              (unsigned long long)&vmcb.control.intercept_misc1);
    CHECK_HEX("guest_asid_tlb_ctl at 0x058", (unsigned long long)(base + 0x058),
              (unsigned long long)&vmcb.control.guest_asid_tlb_ctl);
    CHECK_HEX("exitcode at 0x070", (unsigned long long)(base + 0x070),
              (unsigned long long)&vmcb.control.exitcode);
    CHECK_HEX("np_enable at 0x090", (unsigned long long)(base + 0x090),
              (unsigned long long)&vmcb.control.np_enable);
    CHECK_HEX("n_cr3 at 0x0B0", (unsigned long long)(base + 0x0B0),
              (unsigned long long)&vmcb.control.n_cr3);
    CHECK_HEX("state-save area starts at 0x400", (unsigned long long)(base + 0x400),
              (unsigned long long)&vmcb.save);
    CHECK_HEX("cs at 0x400+0x010", (unsigned long long)(base + 0x400 + 0x010),
              (unsigned long long)&vmcb.save.cs);
    CHECK_HEX("cpl at 0x400+0x0CB", (unsigned long long)(base + 0x400 + 0x0CB),
              (unsigned long long)&vmcb.save.cpl);
    CHECK_HEX("efer at 0x400+0x0D0", (unsigned long long)(base + 0x400 + 0x0D0),
              (unsigned long long)&vmcb.save.efer);
    CHECK_HEX("cr4 at 0x400+0x148", (unsigned long long)(base + 0x400 + 0x148),
              (unsigned long long)&vmcb.save.cr4);
    CHECK_HEX("rip at 0x400+0x178", (unsigned long long)(base + 0x400 + 0x178),
              (unsigned long long)&vmcb.save.rip);
    CHECK_HEX("rsp at 0x400+0x1D8", (unsigned long long)(base + 0x400 + 0x1D8),
              (unsigned long long)&vmcb.save.rsp);
    CHECK_HEX("rax at 0x400+0x1F8", (unsigned long long)(base + 0x400 + 0x1F8),
              (unsigned long long)&vmcb.save.rax);
    CHECK_HEX("cr2 at 0x400+0x240", (unsigned long long)(base + 0x400 + 0x240),
              (unsigned long long)&vmcb.save.cr2);
}

static void test_seg_attrib(void) {
    /* access=0x9B, flags=0xA -> attrib = 0x0A9B (access in low byte,
     * flags nibble in bits 11:8). */
    CHECK_HEX("attrib packs access in low byte, flags in bits 11:8", 0x0A9B,
              hype_vmcb_seg_attrib(0x9B, 0xA));
    CHECK_HEX("flags nibble is masked to 4 bits", 0x0F93, hype_vmcb_seg_attrib(0x93, 0xFF));
}

static void test_build_realmode_guest(void) {
    hype_vmcb_t vmcb;

    /* entry_phys/stack_phys deliberately far above the classic 1MB
     * real-mode reach, e.g. wherever a UEFI PE loader actually placed
     * our own static buffers. */
    hype_vmcb_build_realmode_guest(&vmcb, 0x1e1e6000ULL, 0x1e200000ULL, 0x200000ULL, 0x300000ULL);

    CHECK_HEX("HLT is intercepted", HYPE_SVM_INTERCEPT_HLT,
              vmcb.control.intercept_misc1 & HYPE_SVM_INTERCEPT_HLT);
    CHECK_HEX("shutdown is intercepted", HYPE_SVM_INTERCEPT_SHUTDOWN,
              vmcb.control.intercept_misc1 & HYPE_SVM_INTERCEPT_SHUTDOWN);
    CHECK_HEX("VMRUN is intercepted", HYPE_SVM_INTERCEPT_VMRUN,
              vmcb.control.intercept_misc2 & HYPE_SVM_INTERCEPT_VMRUN);
    CHECK_HEX("ASID is nonzero", 1, vmcb.control.guest_asid_tlb_ctl);
    CHECK_HEX("nested paging disabled", 0, vmcb.control.np_enable);

    CHECK_HEX("CS base is entry_phys directly", 0x1e1e6000ULL, vmcb.save.cs.base);
    CHECK_HEX("CS limit is a real 64KB", 0xFFFFu, vmcb.save.cs.limit);
    CHECK_HEX("DS base is 0", 0, vmcb.save.ds.base);
    CHECK_HEX("DS limit is a real 64KB", 0xFFFFu, vmcb.save.ds.limit);
    CHECK_HEX("SS base is stack_phys directly", 0x1e200000ULL, vmcb.save.ss.base);
    CHECK_HEX("SS limit is a real 64KB", 0xFFFFu, vmcb.save.ss.limit);
    CHECK_HEX("RIP is 0 (entry point is CS.base)", 0, vmcb.save.rip);
    CHECK_HEX("RSP is 0 (stack pointer is SS.base)", 0, vmcb.save.rsp);
    CHECK_HEX("CR0 is ET-only (paging/protection off)", 0x10, vmcb.save.cr0);
    CHECK_HEX("EFER has only SVME set (required by VMRUN, guest not in long mode)",
              HYPE_SVM_SAVE_EFER_SVME, vmcb.save.efer);
    CHECK_HEX("IOPM base wired through", 0x200000ULL, vmcb.control.iopm_base_pa);
    CHECK_HEX("MSRPM base wired through", 0x300000ULL, vmcb.control.msrpm_base_pa);
}

static void test_build_realmode_guest_zeroes_first(void) {
    hype_vmcb_t vmcb;
    unsigned char *bytes = (unsigned char *)&vmcb;
    unsigned long long i;

    for (i = 0; i < sizeof(vmcb); i++) {
        bytes[i] = 0xAA;
    }

    hype_vmcb_build_realmode_guest(&vmcb, 0, 0, 0, 0);

    /* A field this function never explicitly sets (host usage region)
     * should end up zeroed by the initial clear, not left as the
     * 0xAA sentinel -- confirms the whole struct is actually zeroed
     * first, not just the fields the function happens to assign. */
    CHECK_HEX("untouched reserved region is zeroed, not left dirty", 0,
              vmcb.control.reserved_host_usage[0]);
}

static void test_enable_nested_paging(void) {
    hype_vmcb_t vmcb;

    hype_vmcb_build_realmode_guest(&vmcb, 0, 0, 0, 0);
    CHECK_HEX("nested paging starts disabled", 0, vmcb.control.np_enable);

    hype_vmcb_enable_nested_paging(&vmcb, 0x400000ULL);

    CHECK_HEX("NP_ENABLE set", 1, vmcb.control.np_enable);
    CHECK_HEX("N_CR3 set to the given NPT root", 0x400000ULL, vmcb.control.n_cr3);
}

static void test_configure_avic(void) {
    hype_vmcb_t vmcb;

    hype_vmcb_build_realmode_guest(&vmcb, 0, 0, 0, 0);
    /* Some other int_ctl bit already set (e.g. V_IGN_TPR, bit 20) --
     * confirms configure_avic ORs its bit in rather than clobbering
     * whatever else int_ctl already held. */
    vmcb.control.vintr = (1ULL << 20);

    hype_vmcb_configure_avic(&vmcb, 0xFEE00000ULL, 0x100000ULL, 0x101000ULL, 0x102000ULL, 0);

    CHECK_HEX("AVIC enable bit set", HYPE_SVM_INT_CTL_AVIC_ENABLE,
              vmcb.control.vintr & HYPE_SVM_INT_CTL_AVIC_ENABLE);
    CHECK_HEX("pre-existing int_ctl bit preserved", (1ULL << 20), vmcb.control.vintr & (1ULL << 20));
    CHECK_HEX("apic_bar page-aligned address preserved", 0xFEE00000ULL, vmcb.control.avic_apic_bar);
    CHECK_HEX("backing page pointer set", 0x100000ULL, vmcb.control.avic_backing_page_ptr);
    CHECK_HEX("logical table pointer set", 0x101000ULL, vmcb.control.avic_logical_table_ptr);
    CHECK_HEX("physical table pointer + max index", 0x102000ULL,
              vmcb.control.avic_physical_table_ptr);
}

static void test_configure_avic_masks_low_bits_and_sets_max_index(void) {
    hype_vmcb_t vmcb;

    hype_vmcb_build_realmode_guest(&vmcb, 0, 0, 0, 0);
    /* A non-page-aligned address (low 12 bits set) must be masked off,
     * and max_physical_id packed into avic_physical_table_ptr's low
     * byte alongside the (masked) address. */
    hype_vmcb_configure_avic(&vmcb, 0xFEE00ABCULL, 0x100ABCULL, 0x101ABCULL, 0x102000ULL, 0x07u);

    CHECK_HEX("apic_bar low bits masked off", 0xFEE00000ULL, vmcb.control.avic_apic_bar);
    CHECK_HEX("backing page low bits masked off", 0x100000ULL, vmcb.control.avic_backing_page_ptr);
    CHECK_HEX("logical table low bits masked off", 0x101000ULL, vmcb.control.avic_logical_table_ptr);
    CHECK_HEX("physical table address + max index 7", 0x102007ULL,
              vmcb.control.avic_physical_table_ptr);
}

static void test_build_long_mode_guest(void) {
    hype_vmcb_t vmcb;

    hype_vmcb_build_long_mode_guest(&vmcb, 0x100200ULL, 0x200000ULL, 0x300000ULL, 0x400000ULL,
                                     0x500000ULL);

    CHECK_HEX("HLT is intercepted", HYPE_SVM_INTERCEPT_HLT,
              vmcb.control.intercept_misc1 & HYPE_SVM_INTERCEPT_HLT);
    CHECK_HEX("shutdown is intercepted", HYPE_SVM_INTERCEPT_SHUTDOWN,
              vmcb.control.intercept_misc1 & HYPE_SVM_INTERCEPT_SHUTDOWN);
    CHECK_HEX("IOIO is intercepted (unlike the real-mode guest)", HYPE_SVM_INTERCEPT_IOIO_PROT,
              vmcb.control.intercept_misc1 & HYPE_SVM_INTERCEPT_IOIO_PROT);
    CHECK_HEX("VMRUN is intercepted", HYPE_SVM_INTERCEPT_VMRUN,
              vmcb.control.intercept_misc2 & HYPE_SVM_INTERCEPT_VMRUN);
    CHECK_HEX("nested paging starts disabled (caller opts in)", 0, vmcb.control.np_enable);
    CHECK_HEX("IOPM base wired through", 0x400000ULL, vmcb.control.iopm_base_pa);
    CHECK_HEX("MSRPM base wired through", 0x500000ULL, vmcb.control.msrpm_base_pa);

    CHECK_HEX("RIP is the given 64-bit entry point", 0x100200ULL, vmcb.save.rip);
    CHECK_HEX("RSP is the given stack address", 0x300000ULL, vmcb.save.rsp);
    CHECK_HEX("CR3 is the given guest page table root", 0x200000ULL, vmcb.save.cr3);
    CHECK_HEX("CR0 has PE and PG set", 0x80000001ULL, vmcb.save.cr0);
    CHECK_HEX("CR4 has PAE set", 0x20ULL, vmcb.save.cr4);
    CHECK_HEX("EFER has SVME, LME, and LMA set", (1ULL << 12) | (1ULL << 8) | (1ULL << 10),
              vmcb.save.efer);

    CHECK_HEX("CS is marked as a 64-bit long-mode segment", 1,
              (vmcb.save.cs.attrib & (1u << 9)) != 0); /* flags nibble bit 1 (L) at attrib bit 9 */
}

static void test_decode_ioio_info1_out(void) {
    hype_svm_ioio_t io;
    /* port 0x21 (bits 31:16), 8-bit size (bit 4), OUT (bit 0 clear). */
    uint64_t exitinfo1 = ((uint64_t)0x21u << HYPE_SVM_IOIO_INFO1_PORT_SHIFT) | HYPE_SVM_IOIO_INFO1_SIZE8;

    hype_svm_decode_ioio_info1(exitinfo1, &io);

    CHECK_HEX("decoded as OUT", 0, io.is_in);
    CHECK_HEX("decoded port", 0x21, io.port);
    CHECK_HEX("decoded size", 1, io.size_bytes);
}

static void test_decode_ioio_info1_in_16bit(void) {
    hype_svm_ioio_t io;
    uint64_t exitinfo1 = ((uint64_t)0x3F8u << HYPE_SVM_IOIO_INFO1_PORT_SHIFT) | HYPE_SVM_IOIO_INFO1_SIZE16 |
                          HYPE_SVM_IOIO_INFO1_TYPE_IN;

    hype_svm_decode_ioio_info1(exitinfo1, &io);

    CHECK_HEX("decoded as IN", 1, io.is_in);
    CHECK_HEX("decoded port beyond the 8-bit imm range", 0x3F8, io.port);
    CHECK_HEX("decoded size", 2, io.size_bytes);
}

static void test_decode_ioio_info1_32bit(void) {
    hype_svm_ioio_t io;
    uint64_t exitinfo1 = HYPE_SVM_IOIO_INFO1_SIZE32;

    hype_svm_decode_ioio_info1(exitinfo1, &io);

    CHECK_HEX("decoded size", 4, io.size_bytes);
}

int main(void) {
    test_struct_sizes();
    test_field_offsets();
    test_seg_attrib();
    test_build_realmode_guest();
    test_build_realmode_guest_zeroes_first();
    test_enable_nested_paging();
    test_build_long_mode_guest();
    test_decode_ioio_info1_out();
    test_decode_ioio_info1_in_16bit();
    test_decode_ioio_info1_32bit();
    test_configure_avic();
    test_configure_avic_masks_low_bits_and_sets_max_index();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
