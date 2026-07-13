#include "vmcb.h"

uint16_t hype_vmcb_seg_attrib(uint8_t access, uint8_t flags) {
    return (uint16_t)access | (uint16_t)((flags & 0x0Fu) << 8);
}

/* Real-mode reset-state-style access rights (matches the values every
 * x86 CPU's own power-on/RESET state uses for these segments, per the
 * SDMs' documented reset state): code = present, DPL0, execute/read,
 * accessed; data = present, DPL0, read/write, accessed. Flags nibble 0
 * (no G/D-B/L/AVL) since these are 16-bit real-mode segments. */
#define REALMODE_CODE_ACCESS 0x9Bu
#define REALMODE_DATA_ACCESS 0x93u

static void set_realmode_seg(hype_vmcb_seg_t *seg, uint16_t selector, uint64_t base,
                              uint32_t limit, uint8_t access) {
    seg->selector = selector;
    seg->base = base;
    seg->limit = limit;
    seg->attrib = hype_vmcb_seg_attrib(access, 0);
}

void hype_vmcb_build_realmode_guest(hype_vmcb_t *vmcb, uint16_t entry_seg, uint64_t stack_phys) {
    unsigned char *bytes = (unsigned char *)vmcb;
    unsigned long long i;

    for (i = 0; i < sizeof(*vmcb); i++) {
        bytes[i] = 0;
    }

    /* Intercept HLT (the guest's only instruction, M2-7) and shutdown
     * (a triple fault -- no exception handling exists yet, so a
     * shutdown must come back to us rather than reset the machine). */
    vmcb->control.intercept_misc1 = HYPE_SVM_INTERCEPT_HLT | HYPE_SVM_INTERCEPT_SHUTDOWN;

    /* ASID 0 is reserved for the host; every guest needs a nonzero,
     * per-vCPU value eventually (M8's multi-VM concurrency), but a
     * single fixed value is correct for M2's single vCPU. */
    vmcb->control.guest_asid_tlb_ctl = 1;

    /* No nested paging yet (M3's job) -- guest-physical IS
     * host-physical directly, which is exactly what a flat real-mode
     * guest with no paging of its own needs. */
    vmcb->control.np_enable = 0;

    /* Reload all guest state on every VMRUN (no caching optimization
     * yet -- correctness first). */
    vmcb->control.vmcb_clean_bits = 0;

    set_realmode_seg(&vmcb->save.cs, entry_seg, (uint64_t)entry_seg * 16, 0xFFFF, REALMODE_CODE_ACCESS);
    set_realmode_seg(&vmcb->save.ds, 0, 0, 0xFFFF, REALMODE_DATA_ACCESS);
    set_realmode_seg(&vmcb->save.es, 0, 0, 0xFFFF, REALMODE_DATA_ACCESS);
    set_realmode_seg(&vmcb->save.ss, 0, 0, 0xFFFF, REALMODE_DATA_ACCESS);
    set_realmode_seg(&vmcb->save.fs, 0, 0, 0xFFFF, REALMODE_DATA_ACCESS);
    set_realmode_seg(&vmcb->save.gs, 0, 0, 0xFFFF, REALMODE_DATA_ACCESS);

    vmcb->save.gdtr.base = 0;
    vmcb->save.gdtr.limit = 0xFFFF;
    vmcb->save.idtr.base = 0;
    vmcb->save.idtr.limit = 0x3FF; /* real-mode IVT size, for completeness -- HLT is intercepted first */

    vmcb->save.cr0 = 0x00000010; /* ET only -- paging and protection off, matches real mode */
    vmcb->save.cr3 = 0;
    vmcb->save.cr4 = 0;
    vmcb->save.efer = 0; /* guest not in long mode */
    vmcb->save.rflags = 0x2; /* bit 1 is always-set/reserved; IF=0 to start */
    vmcb->save.rip = 0; /* CS.base above already encodes the entry physical address */
    vmcb->save.rsp = stack_phys;
    vmcb->save.rax = 0;
}
