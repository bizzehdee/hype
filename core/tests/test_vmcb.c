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

static void test_decode_ioio_info1_non_string_defaults(void) {
    hype_svm_ioio_t io;
    /* A plain register IN (no STR/REP) still decodes with string flags clear. */
    uint64_t exitinfo1 = ((uint64_t)0x21u << HYPE_SVM_IOIO_INFO1_PORT_SHIFT) | HYPE_SVM_IOIO_INFO1_SIZE8 |
                          HYPE_SVM_IOIO_INFO1_TYPE_IN;

    hype_svm_decode_ioio_info1(exitinfo1, &io);

    CHECK_HEX("not string", 0, io.is_string);
    CHECK_HEX("not rep", 0, io.is_rep);
    CHECK_HEX("addr size defaults to 4", 4, io.addr_size_bytes);
}

static void test_decode_ioio_info1_rep_insb_a64(void) {
    hype_svm_ioio_t io;
    /* `rep insb` on port 0x511, 64-bit address size: the fw_cfg-probe case. */
    uint64_t exitinfo1 = ((uint64_t)0x511u << HYPE_SVM_IOIO_INFO1_PORT_SHIFT) | HYPE_SVM_IOIO_INFO1_TYPE_IN |
                          HYPE_SVM_IOIO_INFO1_STR | HYPE_SVM_IOIO_INFO1_REP | HYPE_SVM_IOIO_INFO1_SIZE8 |
                          HYPE_SVM_IOIO_INFO1_ADDR64;

    hype_svm_decode_ioio_info1(exitinfo1, &io);

    CHECK_HEX("in", 1, io.is_in);
    CHECK_HEX("port", 0x511, io.port);
    CHECK_HEX("unit 1 byte", 1, io.size_bytes);
    CHECK_HEX("string", 1, io.is_string);
    CHECK_HEX("rep", 1, io.is_rep);
    CHECK_HEX("addr size 8", 8, io.addr_size_bytes);
}

static void test_decode_ioio_info1_addr16(void) {
    hype_svm_ioio_t io;
    hype_svm_decode_ioio_info1(HYPE_SVM_IOIO_INFO1_ADDR16 | HYPE_SVM_IOIO_INFO1_SIZE8, &io);
    CHECK_HEX("addr size 2", 2, io.addr_size_bytes);
}

/* --- SVM-STRIO: hype_svm_build_string_io_plan --- */

static void test_string_plan_non_string_rejected(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    io.is_string = 0;
    CHECK_HEX("non-string -> -1", (uint64_t)-1, (uint64_t)hype_svm_build_string_io_plan(&io, 0, 0, 0, 0, &plan));
}

static void test_string_plan_rep_insb_ascending(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    int rc;
    io.is_in = 1;
    io.is_string = 1;
    io.is_rep = 1;
    io.size_bytes = 1;
    io.addr_size_bytes = 8;
    /* rep insb, RDI=0x8000, RCX=4, DF=0. */
    rc = hype_svm_build_string_io_plan(&io, 0x8000, 4, 0, 0, &plan);
    CHECK_HEX("rc", 0, rc);
    CHECK_HEX("count", 4, plan.count);
    CHECK_HEX("unit", 1, plan.unit_bytes);
    CHECK_HEX("byte_count", 4, plan.byte_count);
    CHECK_HEX("start", 0x8000, plan.start_gpa);
    CHECK_HEX("low", 0x8000, plan.low_gpa);
    CHECK_HEX("ascending", 0, plan.descending);
    CHECK_HEX("new RDI", 0x8004, plan.new_index_reg);
    CHECK_HEX("new RCX", 0, plan.new_count_reg);
}

static void test_string_plan_no_rep_single_unit(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    io.is_string = 1;
    io.is_rep = 0; /* a plain INS transfers exactly one unit; RCX untouched */
    io.size_bytes = 2;
    io.addr_size_bytes = 8;
    hype_svm_build_string_io_plan(&io, 0x1000, 0xDEAD, 0, 0, &plan);
    CHECK_HEX("count 1", 1, plan.count);
    CHECK_HEX("byte_count 2", 2, plan.byte_count);
    CHECK_HEX("new index +2", 0x1002, plan.new_index_reg);
    CHECK_HEX("RCX preserved", 0xDEAD, plan.new_count_reg);
}

static void test_string_plan_descending(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    io.is_string = 1;
    io.is_rep = 1;
    io.size_bytes = 4;
    io.addr_size_bytes = 8;
    /* DF=1, RDI=0x9000, RCX=3, unit 4: units at 0x9000,0x8FFC,0x8FF8. */
    hype_svm_build_string_io_plan(&io, 0x9000, 3, 0, HYPE_RFLAGS_DF, &plan);
    CHECK_HEX("descending", 1, plan.descending);
    CHECK_HEX("count", 3, plan.count);
    CHECK_HEX("byte_count", 12, plan.byte_count);
    CHECK_HEX("start", 0x9000, plan.start_gpa);
    CHECK_HEX("low = last unit", 0x8FF8, plan.low_gpa);
    CHECK_HEX("new index 0x9000-12", 0x8FF4, plan.new_index_reg);
}

static void test_string_plan_rep_zero_count_noop(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    io.is_string = 1;
    io.is_rep = 1;
    io.size_bytes = 1;
    io.addr_size_bytes = 8;
    hype_svm_build_string_io_plan(&io, 0x2000, 0, 0, 0, &plan);
    CHECK_HEX("count 0", 0, plan.count);
    CHECK_HEX("byte_count 0", 0, plan.byte_count);
    CHECK_HEX("index unchanged", 0x2000, plan.new_index_reg);
}

static void test_string_plan_addr32_zero_extends(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    io.is_string = 1;
    io.is_rep = 1;
    io.size_bytes = 1;
    io.addr_size_bytes = 4;
    /* 32-bit address size: only low 32 bits of the index register are used,
     * and the result zero-extends (upper 32 bits cleared). */
    hype_svm_build_string_io_plan(&io, 0xFFFFFFFF00001000ULL, 0x10, 0, 0, &plan);
    CHECK_HEX("start uses low 32", 0x1000, plan.start_gpa);
    CHECK_HEX("new index zero-extended", 0x1010, plan.new_index_reg);
}

static void test_string_plan_addr16_preserves_upper(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    io.is_string = 1;
    io.is_rep = 1;
    io.size_bytes = 1;
    io.addr_size_bytes = 2;
    /* 16-bit: only DI (low 16) updates; upper bits of the 64-bit reg preserved. */
    hype_svm_build_string_io_plan(&io, 0xAAAA0010ULL, 4, 0, 0, &plan);
    CHECK_HEX("start uses low 16", 0x10, plan.start_gpa);
    CHECK_HEX("new index preserves upper, updates low16", 0xAAAA0014ULL, plan.new_index_reg);
}

static void test_string_plan_seg_base_added(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    io.is_string = 1;
    io.is_rep = 0;
    io.size_bytes = 1;
    io.addr_size_bytes = 8;
    /* seg base is added to the index to form the linear address. */
    hype_svm_build_string_io_plan(&io, 0x100, 0, 0x40000, 0, &plan);
    CHECK_HEX("start = base + index", 0x40100, plan.start_gpa);
}

static void test_string_plan_ascending_overflow_rejected(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    io.is_string = 1;
    io.is_rep = 1;
    io.size_bytes = 1;
    io.addr_size_bytes = 8;
    /* start near the top of the address space + a big count overflows. */
    CHECK_HEX("overflow -> -1", (uint64_t)-1,
              (uint64_t)hype_svm_build_string_io_plan(&io, 0xFFFFFFFFFFFFFFF0ULL, 0x100, 0, 0, &plan));
}

static void test_string_plan_descending_underflow_rejected(void) {
    hype_svm_ioio_t io = {0};
    hype_svm_string_io_plan_t plan;
    io.is_string = 1;
    io.is_rep = 1;
    io.size_bytes = 4;
    io.addr_size_bytes = 8;
    /* DF=1 with index below (count-1)*unit underflows the low address. */
    CHECK_HEX("underflow -> -1", (uint64_t)-1,
              (uint64_t)hype_svm_build_string_io_plan(&io, 0x4, 0x100, 0, HYPE_RFLAGS_DF, &plan));
}

static void test_decode_npf_info_write(void) {
    hype_svm_npf_t npf;
    uint64_t exitinfo1 = HYPE_SVM_NPF_INFO1_WRITE;
    uint64_t exitinfo2 = 0xFEEDC000ULL;

    hype_svm_decode_npf_info(exitinfo1, exitinfo2, &npf);

    CHECK_HEX("decoded as write", 1, npf.is_write);
    CHECK_HEX("decoded guest-physical fault address", exitinfo2, npf.guest_phys_addr);
}

static void test_decode_npf_info_read(void) {
    hype_svm_npf_t npf;
    uint64_t exitinfo1 = HYPE_SVM_NPF_INFO1_PRESENT; /* write bit clear -- a read */
    uint64_t exitinfo2 = 0x1000ULL;

    hype_svm_decode_npf_info(exitinfo1, exitinfo2, &npf);

    CHECK_HEX("decoded as read", 0, npf.is_write);
    CHECK_HEX("decoded guest-physical fault address", exitinfo2, npf.guest_phys_addr);
}

static void test_encode_eventinj_intr(void) {
    uint64_t value = hype_svm_encode_eventinj_intr(0x31u);

    CHECK_HEX("V bit set", 1, (value & HYPE_SVM_EVENTINJ_V) != 0);
    CHECK_HEX("TYPE is TYPE_INTR (0)", 0, (value & HYPE_SVM_EVENTINJ_TYPE_MASK) >> HYPE_SVM_EVENTINJ_TYPE_SHIFT);
    CHECK_HEX("EV bit clear (no error code)", 0, (value & HYPE_SVM_EVENTINJ_EV) != 0);
    CHECK_HEX("vector", 0x31u, value & HYPE_SVM_EVENTINJ_VECTOR_MASK);
    CHECK_HEX("errorcode bits clear", 0, value >> HYPE_SVM_EVENTINJ_ERRORCODE_SHIFT);
}

static void test_encode_eventinj_intr_vector_masked_to_8_bits(void) {
    /* vector is already a uint8_t parameter, so the widest value that
     * can ever reach the function is 0xFF -- confirm it round-trips
     * through the VECTOR_MASK unchanged rather than being clipped
     * further. */
    uint64_t value = hype_svm_encode_eventinj_intr((uint8_t)0xFFu);
    CHECK_HEX("full-width vector preserved", 0xFFu, value & HYPE_SVM_EVENTINJ_VECTOR_MASK);
}

static void test_can_accept_interrupt_if_set_no_shadow(void) {
    CHECK_HEX("IF=1, no shadow -> can accept", 1, hype_svm_can_accept_interrupt(HYPE_RFLAGS_IF, 0));
}

static void test_can_accept_interrupt_if_clear(void) {
    CHECK_HEX("IF=0 -> cannot accept", 0, hype_svm_can_accept_interrupt(0, 0));
}

static void test_can_accept_interrupt_in_shadow(void) {
    CHECK_HEX("IF=1 but in interrupt shadow -> cannot accept", 0,
              hype_svm_can_accept_interrupt(HYPE_RFLAGS_IF, HYPE_SVM_INTERRUPT_SHADOW_ACTIVE));
}

static void test_can_accept_interrupt_if_clear_and_in_shadow(void) {
    CHECK_HEX("neither condition met -> cannot accept", 0,
              hype_svm_can_accept_interrupt(0, HYPE_SVM_INTERRUPT_SHADOW_ACTIVE));
}

static void test_arm_vintr_request_sets_bits_preserves_others(void) {
    uint64_t armed = hype_svm_arm_vintr_request(HYPE_SVM_INT_CTL_AVIC_ENABLE);

    CHECK_HEX("V_IRQ set", 1, (armed & HYPE_SVM_VINTR_V_IRQ) != 0);
    CHECK_HEX("V_IGN_TPR set", 1, (armed & HYPE_SVM_VINTR_V_IGN_TPR) != 0);
    CHECK_HEX("unrelated bit (AVIC enable) preserved", 1, (armed & HYPE_SVM_INT_CTL_AVIC_ENABLE) != 0);
}

static void test_arm_vintr_request_idempotent_over_stale_priority_bits(void) {
    /* A previous, now-irrelevant V_INTR_PRIO value must not survive a
     * fresh arm -- the mask clears the whole injection-bits group
     * before re-setting it. */
    uint64_t stale = 0x5ULL << HYPE_SVM_VINTR_V_INTR_PRIO_SHIFT;
    uint64_t armed = hype_svm_arm_vintr_request(stale);
    CHECK_HEX("stale priority bits cleared", 0, armed & HYPE_SVM_VINTR_V_INTR_PRIO_MASK);
}

static void test_disarm_vintr_request_clears_bits_preserves_others(void) {
    uint64_t armed = hype_svm_arm_vintr_request(HYPE_SVM_INT_CTL_AVIC_ENABLE);
    uint64_t disarmed = hype_svm_disarm_vintr_request(armed);

    CHECK_HEX("V_IRQ clear", 0, disarmed & HYPE_SVM_VINTR_V_IRQ);
    CHECK_HEX("V_IGN_TPR clear", 0, disarmed & HYPE_SVM_VINTR_V_IGN_TPR);
    CHECK_HEX("V_INTR_PRIO clear", 0, disarmed & HYPE_SVM_VINTR_V_INTR_PRIO_MASK);
    CHECK_HEX("unrelated bit (AVIC enable) preserved", 1, (disarmed & HYPE_SVM_INT_CTL_AVIC_ENABLE) != 0);
}

static void test_irr_set_any_highest_clear(void) {
    uint32_t irr[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_HEX("empty IRR: any=0", 0, hype_svm_irr_any(irr));
    CHECK_HEX("empty IRR: highest=-1", (unsigned long long)(long long)-1,
              (unsigned long long)(long long)hype_svm_irr_highest(irr));

    hype_svm_irr_set(irr, 0x20u); /* vector 32 -> word 1, bit 0 */
    CHECK_HEX("set 0x20: word index", 1, (irr[1] != 0));
    CHECK_HEX("set 0x20: bit", 0x1u, irr[1]);
    CHECK_HEX("any after one set", 1, hype_svm_irr_any(irr));
    CHECK_HEX("highest is 0x20", 0x20u, hype_svm_irr_highest(irr));

    /* x86 delivers the highest-priority (highest-numbered) pending vector. */
    hype_svm_irr_set(irr, 0xECu); /* the LAPIC timer vector seen in boots */
    CHECK_HEX("highest is now 0xEC", 0xECu, hype_svm_irr_highest(irr));
    hype_svm_irr_set(irr, 0x30u);
    CHECK_HEX("highest still 0xEC (0x30 lower)", 0xECu, hype_svm_irr_highest(irr));

    /* Clearing the top exposes the next-highest -- nothing was lost when they
     * collided (the whole point of the bitmap vs a single overwrite slot). */
    hype_svm_irr_clear(irr, 0xECu);
    CHECK_HEX("after clearing 0xEC, highest is 0x30", 0x30u, hype_svm_irr_highest(irr));
    hype_svm_irr_clear(irr, 0x30u);
    CHECK_HEX("after clearing 0x30, highest is 0x20", 0x20u, hype_svm_irr_highest(irr));
    hype_svm_irr_clear(irr, 0x20u);
    CHECK_HEX("empty again after clearing all", 0, hype_svm_irr_any(irr));
}

static void test_irr_boundary_vectors(void) {
    uint32_t irr[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    hype_svm_irr_set(irr, 0u);    /* lowest vector -> word 0 bit 0 */
    hype_svm_irr_set(irr, 255u);  /* highest vector -> word 7 bit 31 */
    CHECK_HEX("vector 0 stored", 0x1u, irr[0]);
    CHECK_HEX("vector 255 stored", 0x80000000u, irr[7]);
    CHECK_HEX("highest of {0,255} is 255", 255u, hype_svm_irr_highest(irr));
    hype_svm_irr_clear(irr, 255u);
    CHECK_HEX("highest of {0} is 0", 0u, hype_svm_irr_highest(irr));
    CHECK_HEX("still any (vector 0 set)", 1, hype_svm_irr_any(irr));
}

static void test_acpi_pm_timer_scale(void) {
    /* At a 3.4 GHz host TSC, the divisor is 3400000000/3579545 = 949, so the
     * PM timer advances ~3.58 MHz. A 1-second-worth TSC (3.4e9) should map to
     * ~3.58e6 PM ticks (mod 2^24). */
    uint64_t tsc_hz = 3400000000ULL;
    uint64_t div = tsc_hz / 3579545ULL; /* 949 */
    CHECK_HEX("divisor sanity", 949, div);
    CHECK_HEX("tsc=0 -> 0", 0, hype_acpi_pm_timer_scale(0, tsc_hz));
    CHECK_HEX("one divisor tick -> 1", 1, hype_acpi_pm_timer_scale(div, tsc_hz));
    CHECK_HEX("scaled value masks to 24 bits",
              (uint32_t)((tsc_hz / div) & 0x00FFFFFFu), hype_acpi_pm_timer_scale(tsc_hz, tsc_hz));
    /* monotonic: a larger TSC never yields a smaller pre-wrap value */
    if (hype_acpi_pm_timer_scale(1000u * div, tsc_hz) <= hype_acpi_pm_timer_scale(500u * div, tsc_hz)) {
        printf("FAIL: pm timer not monotonic pre-wrap\n");
        failures++;
    }
    /* Unknown host rate (0 / below the PM rate): fall back to the raw masked TSC. */
    CHECK_HEX("tsc_hz=0 falls back to raw masked TSC", (uint32_t)(0x01234567u & 0x00FFFFFFu),
              hype_acpi_pm_timer_scale(0x01234567u, 0));
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
    test_decode_ioio_info1_non_string_defaults();
    test_decode_ioio_info1_rep_insb_a64();
    test_decode_ioio_info1_addr16();
    test_string_plan_non_string_rejected();
    test_string_plan_rep_insb_ascending();
    test_string_plan_no_rep_single_unit();
    test_string_plan_descending();
    test_string_plan_rep_zero_count_noop();
    test_string_plan_addr32_zero_extends();
    test_string_plan_addr16_preserves_upper();
    test_string_plan_seg_base_added();
    test_string_plan_ascending_overflow_rejected();
    test_string_plan_descending_underflow_rejected();
    test_decode_npf_info_write();
    test_decode_npf_info_read();
    test_configure_avic();
    test_configure_avic_masks_low_bits_and_sets_max_index();
    test_encode_eventinj_intr();
    test_encode_eventinj_intr_vector_masked_to_8_bits();
    test_can_accept_interrupt_if_set_no_shadow();
    test_can_accept_interrupt_if_clear();
    test_can_accept_interrupt_in_shadow();
    test_can_accept_interrupt_if_clear_and_in_shadow();
    test_arm_vintr_request_sets_bits_preserves_others();
    test_arm_vintr_request_idempotent_over_stale_priority_bits();
    test_disarm_vintr_request_clears_bits_preserves_others();
    test_irr_set_any_highest_clear();
    test_irr_boundary_vectors();
    test_acpi_pm_timer_scale();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
