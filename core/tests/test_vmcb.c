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

    hype_vmcb_build_realmode_guest(&vmcb, 0x1000, 0x9000);

    CHECK_HEX("HLT is intercepted", HYPE_SVM_INTERCEPT_HLT,
              vmcb.control.intercept_misc1 & HYPE_SVM_INTERCEPT_HLT);
    CHECK_HEX("shutdown is intercepted", HYPE_SVM_INTERCEPT_SHUTDOWN,
              vmcb.control.intercept_misc1 & HYPE_SVM_INTERCEPT_SHUTDOWN);
    CHECK_HEX("ASID is nonzero", 1, vmcb.control.guest_asid_tlb_ctl);
    CHECK_HEX("nested paging disabled", 0, vmcb.control.np_enable);

    CHECK_HEX("CS selector matches entry_seg", 0x1000, vmcb.save.cs.selector);
    CHECK_HEX("CS base is entry_seg*16", 0x10000, vmcb.save.cs.base);
    CHECK_HEX("CS limit is 64K", 0xFFFF, vmcb.save.cs.limit);
    CHECK_HEX("RIP is 0 (entry point is CS.base)", 0, vmcb.save.rip);
    CHECK_HEX("RSP is the given stack address", 0x9000, vmcb.save.rsp);
    CHECK_HEX("CR0 is ET-only (paging/protection off)", 0x10, vmcb.save.cr0);
    CHECK_HEX("EFER is 0 (not in long mode)", 0, vmcb.save.efer);
}

static void test_build_realmode_guest_zeroes_first(void) {
    hype_vmcb_t vmcb;
    unsigned char *bytes = (unsigned char *)&vmcb;
    unsigned long long i;

    for (i = 0; i < sizeof(vmcb); i++) {
        bytes[i] = 0xAA;
    }

    hype_vmcb_build_realmode_guest(&vmcb, 0, 0);

    /* A field this function never explicitly sets (host usage region)
     * should end up zeroed by the initial clear, not left as the
     * 0xAA sentinel -- confirms the whole struct is actually zeroed
     * first, not just the fields the function happens to assign. */
    CHECK_HEX("untouched reserved region is zeroed, not left dirty", 0,
              vmcb.control.reserved_host_usage[0]);
}

int main(void) {
    test_struct_sizes();
    test_field_offsets();
    test_seg_attrib();
    test_build_realmode_guest();
    test_build_realmode_guest_zeroes_first();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
