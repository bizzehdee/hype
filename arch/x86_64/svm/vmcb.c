#include "vmcb.h"

/* x86 architectural power-on/RESET default IA32_PAT: entries (PA0..PA7) =
 * WB(6), WT(4), UC-(7), UC(0), WB(6), WT(4), UC-(7), UC(0). Byte-packed
 * high->low (PA7..PA0) = 00 07 04 06 00 07 04 06. Guest default pages use PAT
 * index 0, which must be WB or all guest RAM is uncacheable under NPT. */
#define HYPE_SVM_PAT_POWERON_DEFAULT 0x0007040600070406ULL

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

    /* Intercept HLT (the guest's only instruction, M2-7), shutdown
     * (a triple fault -- no exception handling exists yet, so a
     * shutdown must come back to us rather than reset the machine),
     * CPUID/MSR (CPUMSR-1/CPUMSR-2), and IOIO (FW-1 -- a real firmware
     * guest does real port I/O; hype_svm_vcpu_create()'s own g_iopm
     * fill is what actually marks every port intercepted, same
     * "control bit only enables interception, the bitmap decides per-
     * port" split hype_svm_vcpu_create_long_mode() already documents)
     * -- guest-isolation baseline: without these, CPUID/RDMSR/WRMSR/
     * IN/OUT execute natively against the real host CPU/hardware. */
    vmcb->control.intercept_misc1 = HYPE_SVM_INTERCEPT_HLT | HYPE_SVM_INTERCEPT_SHUTDOWN |
                                     HYPE_SVM_INTERCEPT_CPUID | HYPE_SVM_INTERCEPT_MSR_PROT |
                                     HYPE_SVM_INTERCEPT_IOIO_PROT;
    vmcb->control.intercept_misc2 = HYPE_SVM_INTERCEPT_VMRUN;

    /* FW-1: intercept every guest exception vector (see
     * HYPE_SVM_EXITCODE_EXCEPTION_BASE's own comment) -- real firmware
     * catching its own unexpected fault and spinning forever looks
     * identical to a genuine hang unless we see the fault first. */
    vmcb->control.intercept_exceptions = 0xFFFFFFFFu;

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
    /* PERF-1: guest PAT = x86 architectural power-on default. Under nested
     * paging the VMCB's g_pat IS the guest's PAT. Leaving it 0 made all 8
     * entries UC, so every default (PAT-index-0) guest page was UNCACHEABLE
     * until the guest's own pat_init ran -- early-boot memory ops (kernel
     * decompress, initramfs, memset) executed at UC speed (the ~13s IF=0
     * stalls). This value puts WB at index 0 (PA0=WB WT UC- UC WB WT UC- UC),
     * so normal guest RAM is write-back from the first instruction. */
    vmcb->save.g_pat = HYPE_SVM_PAT_POWERON_DEFAULT;
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

/* Flat long-mode segment conventions: present, DPL0; code =
 * execute/read (0x9B), data = read/write (0x93). Flags nibble bits
 * are G|D/B|L|AVL (high to low) per hype_vmcb_seg_attrib(); CS gets
 * L=1 (bit 1 of the nibble) marking it a 64-bit code segment, plus
 * G=1 (bit 3) for a conventional 4GB limit; data segments get G=1
 * and D/B=1 (bit 2), the standard flat-32-style data segment
 * convention real 64-bit kernels themselves also set up (base/limit
 * are otherwise not meaningfully enforced by hardware in 64-bit
 * mode). */
#define LONGMODE_CODE_ACCESS 0x9Bu
#define LONGMODE_CODE_FLAGS 0xAu /* G=1, L=1 */
#define LONGMODE_DATA_ACCESS 0x93u
#define LONGMODE_DATA_FLAGS 0xCu /* G=1, D/B=1 */
#define LONGMODE_LIMIT 0xFFFFFFFFu

static void set_longmode_seg(hype_vmcb_seg_t *seg, uint8_t access, uint8_t flags) {
    seg->selector = 0;
    seg->base = 0;
    seg->limit = LONGMODE_LIMIT;
    seg->attrib = hype_vmcb_seg_attrib(access, flags);
}

void hype_vmcb_build_long_mode_guest(hype_vmcb_t *vmcb, uint64_t entry_rip, uint64_t guest_cr3,
                                      uint64_t rsp, uint64_t iopm_phys, uint64_t msrpm_phys) {
    unsigned char *bytes = (unsigned char *)vmcb;
    unsigned long long i;

    for (i = 0; i < sizeof(*vmcb); i++) {
        bytes[i] = 0;
    }

    /* CPUMSR-1/CPUMSR-2: CPUID/MSR intercepted here too, same
     * guest-isolation baseline reasoning as
     * hype_vmcb_build_realmode_guest() above. */
    vmcb->control.intercept_misc1 =
        HYPE_SVM_INTERCEPT_HLT | HYPE_SVM_INTERCEPT_SHUTDOWN | HYPE_SVM_INTERCEPT_IOIO_PROT |
        HYPE_SVM_INTERCEPT_CPUID | HYPE_SVM_INTERCEPT_MSR_PROT;
    vmcb->control.intercept_misc2 = HYPE_SVM_INTERCEPT_VMRUN;

    vmcb->control.iopm_base_pa = iopm_phys;
    vmcb->control.msrpm_base_pa = msrpm_phys;

    vmcb->control.guest_asid_tlb_ctl = 1;
    vmcb->control.np_enable = 0; /* caller opts in via hype_vmcb_enable_nested_paging() */
    vmcb->control.vmcb_clean_bits = 0;

    set_longmode_seg(&vmcb->save.cs, LONGMODE_CODE_ACCESS, LONGMODE_CODE_FLAGS);
    set_longmode_seg(&vmcb->save.ds, LONGMODE_DATA_ACCESS, LONGMODE_DATA_FLAGS);
    set_longmode_seg(&vmcb->save.es, LONGMODE_DATA_ACCESS, LONGMODE_DATA_FLAGS);
    set_longmode_seg(&vmcb->save.ss, LONGMODE_DATA_ACCESS, LONGMODE_DATA_FLAGS);
    set_longmode_seg(&vmcb->save.fs, LONGMODE_DATA_ACCESS, LONGMODE_DATA_FLAGS);
    set_longmode_seg(&vmcb->save.gs, LONGMODE_DATA_ACCESS, LONGMODE_DATA_FLAGS);

    vmcb->save.gdtr.base = 0;
    vmcb->save.gdtr.limit = 0xFFFF;
    vmcb->save.idtr.base = 0;
    vmcb->save.idtr.limit = 0xFFFF;

    vmcb->save.cr0 = 0x80000001u; /* PG | PE */
    vmcb->save.cr3 = guest_cr3;
    vmcb->save.cr4 = 0x00000020u; /* PAE */
    /* PERF-1: guest PAT = x86 power-on default (WB at index 0). See the same
     * assignment in hype_vmcb_build_realmode_guest for why 0 (all-UC) is wrong
     * under nested paging. */
    vmcb->save.g_pat = HYPE_SVM_PAT_POWERON_DEFAULT;
    /* EFER: SVME (VMRUN requires it, see HYPE_SVM_SAVE_EFER_SVME) |
     * LME (bit 8) | LMA (bit 10) -- VMRUN loads guest state directly
     * rather than running the real activation sequence, so LMA (which
     * hardware would normally set itself once PG+LME+PE all become
     * true together) must be set explicitly here. */
    vmcb->save.efer = HYPE_SVM_SAVE_EFER_SVME | (1ULL << 8) | (1ULL << 10);
    vmcb->save.rflags = 0x2;
    vmcb->save.rip = entry_rip;
    vmcb->save.rsp = rsp;
    vmcb->save.rax = 0;
}

void hype_svm_decode_ioio_info1(uint64_t exitinfo1, hype_svm_ioio_t *out) {
    out->is_in = (exitinfo1 & HYPE_SVM_IOIO_INFO1_TYPE_IN) != 0;
    out->port = (uint16_t)((exitinfo1 >> HYPE_SVM_IOIO_INFO1_PORT_SHIFT) & 0xFFFFu);
    if (exitinfo1 & HYPE_SVM_IOIO_INFO1_SIZE8) {
        out->size_bytes = 1;
    } else if (exitinfo1 & HYPE_SVM_IOIO_INFO1_SIZE16) {
        out->size_bytes = 2;
    } else {
        out->size_bytes = 4;
    }
    out->is_string = (exitinfo1 & HYPE_SVM_IOIO_INFO1_STR) != 0;
    out->is_rep = (exitinfo1 & HYPE_SVM_IOIO_INFO1_REP) != 0;
    if (exitinfo1 & HYPE_SVM_IOIO_INFO1_ADDR16) {
        out->addr_size_bytes = 2;
    } else if (exitinfo1 & HYPE_SVM_IOIO_INFO1_ADDR32) {
        out->addr_size_bytes = 4;
    } else {
        /* A64 bit, or none set (older encodings default to 32-bit protected
         * mode): treat as 64-bit only when A64 is explicitly flagged. */
        out->addr_size_bytes = (exitinfo1 & HYPE_SVM_IOIO_INFO1_ADDR64) ? 8 : 4;
    }
}

/* Mask for the low `addr_size_bytes` of an address/count register. */
static uint64_t addr_mask_for(uint8_t addr_size_bytes) {
    if (addr_size_bytes >= 8) {
        return ~0ULL;
    }
    return (1ULL << (addr_size_bytes * 8u)) - 1ULL;
}

/* Apply an x86 register-width update: a result computed in `addr_size_bytes`
 * width written back into a 64-bit GPR. 8/4-byte ops zero-extend (upper bits
 * cleared for 4-byte, full value for 8-byte); a 2-byte op updates only the
 * low 16 bits and preserves the rest, matching real 16-bit-addressing string
 * ops. */
static uint64_t apply_reg_width(uint64_t original, uint64_t result, uint8_t addr_size_bytes) {
    if (addr_size_bytes >= 8) {
        return result;
    }
    if (addr_size_bytes == 4) {
        return result & 0xFFFFFFFFULL;
    }
    return (original & ~0xFFFFULL) | (result & 0xFFFFULL);
}

int hype_svm_build_string_io_plan(const hype_svm_ioio_t *io, uint64_t index_reg, uint64_t count_reg,
                                   uint64_t seg_base, uint64_t rflags, hype_svm_string_io_plan_t *out) {
    uint64_t mask;
    uint64_t index;
    uint64_t count;
    uint64_t span;
    uint64_t raw_new_index;

    if (!io->is_string) {
        return -1;
    }

    mask = addr_mask_for(io->addr_size_bytes);
    index = index_reg & mask;
    count = io->is_rep ? (count_reg & mask) : 1ULL;

    out->unit_bytes = io->size_bytes;
    out->count = count;
    out->descending = (rflags & HYPE_RFLAGS_DF) != 0;
    out->start_gpa = seg_base + index;
    out->new_count_reg = io->is_rep ? apply_reg_width(count_reg, 0, io->addr_size_bytes) : count_reg;

    if (count == 0) {
        /* REP with (E)CX == 0: architecturally a no-op. No bytes move, index
         * unchanged; the caller still retires the instruction. */
        out->byte_count = 0;
        out->low_gpa = out->start_gpa;
        out->new_index_reg = index_reg;
        return 0;
    }

    /* count * unit_bytes, overflow-checked. */
    if (io->size_bytes != 0 && count > (~0ULL) / (uint64_t)io->size_bytes) {
        return -1;
    }
    out->byte_count = count * (uint64_t)io->size_bytes;

    /* span = distance from the first to the last unit's start = (count-1)*unit. */
    span = (count - 1ULL) * (uint64_t)io->size_bytes;

    if (out->descending) {
        /* Walk downward: lowest address is the last unit's. Guard underflow. */
        if (index < span) {
            return -1;
        }
        out->low_gpa = seg_base + (index - span);
        raw_new_index = index - count * (uint64_t)io->size_bytes;
    } else {
        /* Walk upward: lowest address is the first unit's. Guard overflow. */
        if (out->start_gpa > (~0ULL) - out->byte_count) {
            return -1;
        }
        out->low_gpa = out->start_gpa;
        raw_new_index = index + count * (uint64_t)io->size_bytes;
    }
    out->new_index_reg = apply_reg_width(index_reg, raw_new_index, io->addr_size_bytes);
    return 0;
}

void hype_svm_decode_npf_info(uint64_t exitinfo1, uint64_t exitinfo2, hype_svm_npf_t *out) {
    out->is_write = (exitinfo1 & HYPE_SVM_NPF_INFO1_WRITE) != 0;
    out->guest_phys_addr = exitinfo2;
}

uint64_t hype_svm_encode_eventinj_intr(uint8_t vector) {
    return HYPE_SVM_EVENTINJ_V | (HYPE_SVM_EVENTINJ_TYPE_INTR << HYPE_SVM_EVENTINJ_TYPE_SHIFT) |
           ((uint64_t)vector & HYPE_SVM_EVENTINJ_VECTOR_MASK);
}

int hype_svm_can_accept_interrupt(uint64_t rflags, uint64_t interrupt_shadow) {
    if ((interrupt_shadow & HYPE_SVM_INTERRUPT_SHADOW_ACTIVE) != 0) {
        return 0;
    }
    return (rflags & HYPE_RFLAGS_IF) != 0;
}

uint64_t hype_svm_arm_vintr_request(uint64_t vintr) {
    return (vintr & ~HYPE_SVM_VINTR_INJECTION_BITS_MASK) | HYPE_SVM_VINTR_V_IRQ | HYPE_SVM_VINTR_V_IGN_TPR;
}

uint64_t hype_svm_disarm_vintr_request(uint64_t vintr) {
    return vintr & ~HYPE_SVM_VINTR_INJECTION_BITS_MASK;
}

void hype_vmcb_enable_nested_paging(hype_vmcb_t *vmcb, uint64_t npt_root_phys) {
    vmcb->control.np_enable = 1;
    vmcb->control.n_cr3 = npt_root_phys;
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
