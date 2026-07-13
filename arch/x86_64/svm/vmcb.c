#include "vmcb.h"

uint16_t hype_vmcb_seg_attrib(uint8_t access, uint8_t flags) {
    return (uint16_t)access | (uint16_t)((flags & 0x0Fu) << 8);
}

/* Real-mode reset-state-style access rights (matches the values every
 * x86 CPU's own power-on/RESET state uses for these segments, per the
 * SDMs' documented reset state): code = present, DPL0, execute/read,
 * accessed; data = present, DPL0, read/write, accessed. Flags nibble 0
 * (no G/D-B/L/AVL) and a real 64KB limit, exactly matching real
 * hardware's own reset-state segments -- unlike real silicon, though,
 * a VMCB segment's base is a directly-cached value the hypervisor
 * sets itself (never derived from the selector by hardware the way a
 * real segment load would), so nothing requires entry_phys below to
 * be reachable via a 16-bit segment*16 selector the way classic real
 * mode addressing would; RIP staying 0 (a single HLT, never advancing
 * past it) keeps every access safely within this 64KB limit
 * regardless of where CS.base itself points. */
#define REALMODE_CODE_ACCESS 0x9Bu
#define REALMODE_DATA_ACCESS 0x93u
#define REALMODE_LIMIT 0xFFFFu

static void set_realmode_seg(hype_vmcb_seg_t *seg, uint64_t base, uint8_t access) {
    seg->selector = 0; /* meaningless here -- base/limit are loaded directly, not derived from it */
    seg->base = base;
    seg->limit = REALMODE_LIMIT;
    seg->attrib = hype_vmcb_seg_attrib(access, 0);
}

void hype_vmcb_build_realmode_guest(hype_vmcb_t *vmcb, uint64_t entry_phys, uint64_t stack_phys,
                                     uint64_t iopm_phys, uint64_t msrpm_phys) {
    unsigned char *bytes = (unsigned char *)vmcb;
    unsigned long long i;

    for (i = 0; i < sizeof(*vmcb); i++) {
        bytes[i] = 0;
    }

    /* Intercept HLT (the guest's only instruction, M2-7) and shutdown
     * (a triple fault -- no exception handling exists yet, so a
     * shutdown must come back to us rather than reset the machine). */
    vmcb->control.intercept_misc1 = HYPE_SVM_INTERCEPT_HLT | HYPE_SVM_INTERCEPT_SHUTDOWN;
    vmcb->control.intercept_misc2 = HYPE_SVM_INTERCEPT_VMRUN;

    vmcb->control.iopm_base_pa = iopm_phys;
    vmcb->control.msrpm_base_pa = msrpm_phys;

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

    set_realmode_seg(&vmcb->save.cs, entry_phys, REALMODE_CODE_ACCESS);
    set_realmode_seg(&vmcb->save.ds, 0, REALMODE_DATA_ACCESS);
    set_realmode_seg(&vmcb->save.es, 0, REALMODE_DATA_ACCESS);
    /* SS.base = stack_phys, RSP = 0 -- same reasoning as CS/RIP above:
     * stack_phys can likewise be an arbitrary high address (wherever
     * the guest's stack buffer actually lives), which SS.base=0 +
     * RSP=stack_phys couldn't reach without exceeding the 64KB limit. */
    set_realmode_seg(&vmcb->save.ss, stack_phys, REALMODE_DATA_ACCESS);
    set_realmode_seg(&vmcb->save.fs, 0, REALMODE_DATA_ACCESS);
    set_realmode_seg(&vmcb->save.gs, 0, REALMODE_DATA_ACCESS);

    vmcb->save.gdtr.base = 0;
    vmcb->save.gdtr.limit = 0xFFFF;
    vmcb->save.idtr.base = 0;
    vmcb->save.idtr.limit = 0x3FF; /* real-mode IVT size, for completeness -- HLT is intercepted first */

    vmcb->save.cr0 = 0x00000010; /* ET only -- paging and protection off, matches real mode */
    vmcb->save.cr3 = 0;
    vmcb->save.cr4 = 0;
    /* EFER.SVME (bit 12) must be set in the *guest's* saved EFER or
     * VMRUN itself refuses the VMCB (a state-consistency check,
     * independent of whether the guest itself ever uses SVM) --
     * VMRUN reports this the same way as any other invalid-VMCB
     * condition: EXITCODE = HYPE_SVM_EXITCODE_INVALID, no VM-entry at
     * all. Every other EFER bit (LME/LMA/...) stays 0 -- guest not in
     * long mode. */
    vmcb->save.efer = HYPE_SVM_SAVE_EFER_SVME;
    vmcb->save.rflags = 0x2; /* bit 1 is always-set/reserved; IF=0 to start */
    vmcb->save.rip = 0; /* CS.base above already is the entry physical address */
    vmcb->save.rsp = 0; /* SS.base above already is the stack's physical address */
    vmcb->save.rax = 0;
}

void hype_vmcb_configure_avic(hype_vmcb_t *vmcb, uint64_t apic_bar_phys,
                               uint64_t backing_page_phys, uint64_t logical_table_phys,
                               uint64_t physical_table_phys, uint8_t max_physical_id) {
    vmcb->control.vintr |= HYPE_SVM_INT_CTL_AVIC_ENABLE;
    vmcb->control.avic_apic_bar = apic_bar_phys & HYPE_SVM_AVIC_ADDR_MASK;
    vmcb->control.avic_backing_page_ptr = backing_page_phys & HYPE_SVM_AVIC_ADDR_MASK;
    vmcb->control.avic_logical_table_ptr = logical_table_phys & HYPE_SVM_AVIC_ADDR_MASK;
    vmcb->control.avic_physical_table_ptr =
        (physical_table_phys & HYPE_SVM_AVIC_ADDR_MASK) | max_physical_id;
}
