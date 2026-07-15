#ifndef HYPE_ARCH_SVM_VMCB_H
#define HYPE_ARCH_SVM_VMCB_H

#include <stdint.h>

/*
 * VMCB layout (M2-3), per the AMD64 Architecture Programmer's Manual
 * Volume 2: System Programming, Rev 3.39, Appendix B ("Layout of
 * VMCB"), Tables B-1 (control area) and B-2 (state-save area). Byte
 * offsets below are transcribed directly from that spec (fetched and
 * read for this task, not reconstructed from memory), since a wrong
 * offset here is a hardware-register-layout bug, not a logic bug --
 * the kind that's easy to get subtly wrong and hard to notice by
 * inspection. Fields this project doesn't use yet are still given
 * their real names/offsets where the spec text was unambiguous (later
 * milestones -- AVIC for M2-4, N_CR3 for M3 -- will need them); a few
 * fields near the end of the state-save area (SPEC_CTRL, LBR stack,
 * IBS) collapse into one reserved block since this project doesn't
 * touch them and the PDF-to-text extraction of that specific sub-range
 * was ambiguous, whereas the total *size* of the two areas (1024 bytes
 * control + 3072 bytes state-save = 4096 bytes total) is unambiguous
 * and is what a static_assert below actually checks.
 */

typedef struct {
    uint16_t selector;
    uint16_t attrib;
    uint32_t limit;
    uint64_t base; /* AMD only implements the low 32 bits for most of these */
} __attribute__((packed)) hype_vmcb_seg_t;

typedef struct {
    /* 0x000 */ uint32_t intercept_cr;        /* 15:0 = CR read, 31:16 = CR write */
    /* 0x004 */ uint32_t intercept_dr;        /* 15:0 = DR read, 31:16 = DR write */
    /* 0x008 */ uint32_t intercept_exceptions; /* bit per vector 0-31 */
    /* 0x00C */ uint32_t intercept_misc1;     /* INTR=0,NMI=1,SMI=2,INIT=3,VINTR=4,...,HLT=24,IOIO_PROT=27,MSR_PROT=28,...,SHUTDOWN=31 */
    /* 0x010 */ uint32_t intercept_misc2;     /* VMRUN=0,VMMCALL=1,...,EFER_WRITE=15,CR_WRITE_POST=31:16 */
    /* 0x014 */ uint32_t intercept_misc3;     /* INVLPGB/MCOMMIT/TLBSYNC */
    /* 0x018 */ uint8_t reserved1[0x03C - 0x018];
    /* 0x03C */ uint16_t pause_filter_threshold;
    /* 0x03E */ uint16_t pause_filter_count;
    /* 0x040 */ uint64_t iopm_base_pa;
    /* 0x048 */ uint64_t msrpm_base_pa;
    /* 0x050 */ uint64_t tsc_offset;
    /* 0x058 */ uint64_t guest_asid_tlb_ctl;  /* 31:0 = ASID, 39:32 = TLB_CONTROL */
    /* 0x060 */ uint64_t vintr;               /* V_TPR/V_IRQ/VGIF/V_NMI/V_INTR_PRIO/AVIC enable/... */
    /* 0x068 */ uint64_t interrupt_shadow;    /* bit0 = shadow, bit1 = guest RFLAGS.IF */
    /* 0x070 */ uint64_t exitcode;
    /* 0x078 */ uint64_t exitinfo1;
    /* 0x080 */ uint64_t exitinfo2;
    /* 0x088 */ uint64_t exitintinfo;
    /* 0x090 */ uint64_t np_enable;           /* bit0 = NP_ENABLE; SEV bits above it, unused here */
    /* 0x098 */ uint64_t avic_apic_bar;       /* 51:0 */
    /* 0x0A0 */ uint64_t ghcb_gpa;
    /* 0x0A8 */ uint64_t eventinj;
    /* 0x0B0 */ uint64_t n_cr3;
    /* 0x0B8 */ uint64_t lbr_virt_enable;
    /* 0x0C0 */ uint64_t vmcb_clean_bits;     /* 31:0 used; 0 = "nothing clean," always reload */
    /* 0x0C8 */ uint64_t nrip;
    /* 0x0D0 */ uint8_t num_bytes_fetched;
    /* 0x0D1 */ uint8_t guest_instruction_bytes[15];
    /* 0x0E0 */ uint64_t avic_backing_page_ptr;
    /* 0x0E8 */ uint8_t reserved2[8];
    /* 0x0F0 */ uint64_t avic_logical_table_ptr;
    /* 0x0F8 */ uint64_t avic_physical_table_ptr; /* includes AVIC_PHYSICAL_MAX_INDEX in bits 7:0 */
    /* 0x100 */ uint8_t reserved3[8];
    /* 0x108 */ uint64_t vmsa_ptr;
    /* 0x110 */ uint64_t vmgexit_rax;
    /* 0x118 */ uint64_t vmgexit_cpl;         /* only bits 7:0 used */
    /* 0x120 */ uint8_t reserved4[0x3E0 - 0x120];
    /* 0x3E0 */ uint8_t reserved_host_usage[0x400 - 0x3E0];
} __attribute__((packed)) hype_vmcb_control_t;

typedef struct {
    /* 0x000 */ hype_vmcb_seg_t es;
    /* 0x010 */ hype_vmcb_seg_t cs;
    /* 0x020 */ hype_vmcb_seg_t ss;
    /* 0x030 */ hype_vmcb_seg_t ds;
    /* 0x040 */ hype_vmcb_seg_t fs;
    /* 0x050 */ hype_vmcb_seg_t gs;
    /* 0x060 */ hype_vmcb_seg_t gdtr; /* selector/attrib reserved, only base/limit real */
    /* 0x070 */ hype_vmcb_seg_t ldtr;
    /* 0x080 */ hype_vmcb_seg_t idtr; /* selector/attrib reserved, only base/limit real */
    /* 0x090 */ hype_vmcb_seg_t tr;
    /* 0x0A0 */ uint8_t reserved1[0x0CB - 0x0A0];
    /* 0x0CB */ uint8_t cpl; /* forced to 0 in real mode, 3 in virtual-8086 mode */
    /* 0x0CC */ uint32_t reserved2;
    /* 0x0D0 */ uint64_t efer;
    /* 0x0D8 */ uint8_t reserved3[0x148 - 0x0D8];
    /* 0x148 */ uint64_t cr4;
    /* 0x150 */ uint64_t cr3;
    /* 0x158 */ uint64_t cr0;
    /* 0x160 */ uint64_t dr7;
    /* 0x168 */ uint64_t dr6;
    /* 0x170 */ uint64_t rflags;
    /* 0x178 */ uint64_t rip;
    /* 0x180 */ uint8_t reserved4[0x1D8 - 0x180];
    /* 0x1D8 */ uint64_t rsp;
    /* 0x1E0 */ uint64_t s_cet;
    /* 0x1E8 */ uint64_t ssp;
    /* 0x1F0 */ uint64_t isst_addr;
    /* 0x1F8 */ uint64_t rax;
    /* 0x200 */ uint64_t star;
    /* 0x208 */ uint64_t lstar;
    /* 0x210 */ uint64_t cstar;
    /* 0x218 */ uint64_t sfmask;
    /* 0x220 */ uint64_t kernel_gs_base;
    /* 0x228 */ uint64_t sysenter_cs;
    /* 0x230 */ uint64_t sysenter_esp;
    /* 0x238 */ uint64_t sysenter_eip;
    /* 0x240 */ uint64_t cr2;
    /* 0x248 */ uint8_t reserved5[0x268 - 0x248];
    /* 0x268 */ uint64_t g_pat; /* only used if nested paging enabled */
    /* 0x270 */ uint8_t reserved6[0xC00 - 0x270]; /* DBGCTL/LBR/SPEC_CTRL/IBS -- unused here */
} __attribute__((packed)) hype_vmcb_save_t;

typedef struct {
    hype_vmcb_control_t control;
    hype_vmcb_save_t save;
} __attribute__((packed)) hype_vmcb_t;

/* If either of these ever fires, a field/reserved-block size above no
 * longer matches the spec's byte offsets -- fix the struct, don't
 * suppress the assertion. */
_Static_assert(sizeof(hype_vmcb_control_t) == 0x400, "VMCB control area must be exactly 1024 bytes");
_Static_assert(sizeof(hype_vmcb_t) == 0x1000, "VMCB must be exactly one 4KB page");

/* Intercept bits this project actually sets (Table B-1). */
#define HYPE_SVM_INTERCEPT_HLT (1u << 24)
#define HYPE_SVM_INTERCEPT_SHUTDOWN (1u << 31)
/*
 * CPUMSR-1: intercept every guest CPUID (bit 18 of intercept_misc1's
 * "Intercept Vector 3" layout -- cross-referenced against the AMD SDM
 * and internally consistent with this file's own already-established
 * neighboring bits, all from the same real table: HLT=24, IOIO_PROT=27,
 * MSR_PROT=28, SHUTDOWN=31). Without this, CPUID executes natively
 * against the real host CPU with zero mediation -- a guest-isolation
 * gap (AGENTS.md) surfaced while scoping M4-6; see
 * arch/x86_64/cpu/cpuid_emulate.h for what the guest sees instead.
 */
#define HYPE_SVM_INTERCEPT_CPUID (1u << 18)
/*
 * CPUMSR-2: intercept every guest RDMSR/WRMSR (bit 28 of
 * intercept_misc1 -- this file's own comment already documented this
 * bit's position, just never defined/set it). Enabling this bit alone
 * isn't sufficient by itself -- VMRUN also always consults the MSR
 * permission bitmap (msrpm_base_pa) to decide whether any *specific*
 * MSR actually traps; this project fills that bitmap with 0xFF
 * (svm_vcpu.c, mirroring g_iopm's own existing "intercept everything"
 * pattern) so every MSR does. Without either the intercept bit or a
 * filled bitmap, every guest RDMSR/WRMSR reaches real hardware
 * unmediated -- the same class of guest-isolation gap
 * HYPE_SVM_INTERCEPT_CPUID fixed for CPUID.
 */
#define HYPE_SVM_INTERCEPT_MSR_PROT (1u << 28)
/* intercept_misc2 bit 0: intercept the guest ever executing VMRUN
 * itself. Required for two independent reasons: (1) under nested SVM
 * (this project's own dev/QEMU environment, and any real deployment
 * that itself runs virtualized), the L0 hypervisor's nested-SVM
 * emulation treats an L1 VMCB that doesn't intercept VMRUN as invalid
 * -- VMRUN fails outright with EXITCODE = HYPE_SVM_EXITCODE_INVALID,
 * confirmed the hard way, empirically, before this bit existed; (2)
 * even on bare-metal, a guest must never be able to execute a
 * privileged virtualization instruction itself -- this project's own
 * guest-isolation hard invariant (AGENTS.md) -- so this needs to be
 * set regardless of the nested-specific requirement above. */
#define HYPE_SVM_INTERCEPT_VMRUN (1u << 0)

/* save.efer bit 12: VMRUN requires this bit set in the *guest's* saved
 * EFER or it refuses the VMCB outright (EXITCODE =
 * HYPE_SVM_EXITCODE_INVALID, no VM-entry at all) -- a state-consistency
 * check independent of whether the guest itself ever uses SVM. Same
 * bit position/value as svm.h's HYPE_EFER_SVME (the *host's* EFER,
 * set by hype_svm_enable()); duplicated here rather than included
 * from svm.h to avoid vmcb.h depending on the header that already
 * includes it. */
#define HYPE_SVM_SAVE_EFER_SVME (1ULL << 12)

/*
 * AVIC (M2-4). int_ctl (the `vintr` field, offset 0x060) bit 31 =
 * AVIC Enable -- corroborated by every real-world SVM implementation
 * (e.g. Linux KVM's AVIC_ENABLE_MASK) rather than re-verified against
 * the AMD SDM PDF specifically for this task.
 *
 * AVIC requires nested paging (NP_ENABLE=1, M3-1) to actually
 * intercept and redirect guest local-APIC MMIO accesses to the
 * backing page -- real-world hypervisors only ever set AVIC_ENABLE
 * alongside NP_ENABLE=1, and this project follows that same rule:
 * hype_vmcb_configure_avic() is meant to be called on a VMCB that
 * also has NP_ENABLE=1. It is deliberately NOT called from
 * hype_vmcb_build_realmode_guest() (M2-3), whose flat unpaged M2-7
 * test guest has NP_ENABLE=0 -- enabling AVIC there would very likely
 * make VMRUN reject the VMCB as invalid on real hardware. Wiring this
 * in for real is M3+'s job, once nested paging exists.
 */
#define HYPE_SVM_INT_CTL_AVIC_ENABLE (1ULL << 31)

/* avic_apic_bar/avic_backing_page_ptr/avic_logical_table_ptr/
 * avic_physical_table_ptr all hold a page-aligned physical address in
 * bits 51:12; avic_physical_table_ptr additionally packs the highest
 * physical APIC ID covered by the table into bits 7:0. */
#define HYPE_SVM_AVIC_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* SVM #VMEXIT codes this project checks for (AMD SDM Appendix C). */
#define HYPE_SVM_EXITCODE_HLT 0x78ULL
#define HYPE_SVM_EXITCODE_SHUTDOWN 0x7FULL
#define HYPE_SVM_EXITCODE_IOIO 0x7BULL
#define HYPE_SVM_EXITCODE_CPUID 0x72ULL
#define HYPE_SVM_EXITCODE_MSR 0x7CULL
#define HYPE_SVM_EXITCODE_NPF 0x400ULL
#define HYPE_SVM_EXITCODE_INVALID 0xFFFFFFFFFFFFFFFFULL /* VMRUN itself failed (bad VMCB state) */

/* EXITCODE 0x40-0x5F: guest exception vectors 0-31 (EXITCODE = 0x40 +
 * vector), fired only for vectors marked in control.intercept_exceptions
 * (FW-1: without this, an unhandled guest fault -- e.g. #GP from an
 * unemulated instruction/access -- is delivered straight to the guest's
 * own IDT instead of exiting to us; real EDK2 firmware's own early
 * exception handlers frequently just spin forever (CpuDeadLoop-style)
 * rather than crash, which looks identical to a hang with no further
 * VMEXITs at all unless this is intercepted). */
#define HYPE_SVM_EXITCODE_EXCEPTION_BASE 0x40ULL

/* intercept_misc1 bit 27 (M3-5): trap every guest IN/OUT so this
 * project's device stubs (devices/pic.h, devices/pit.h) can emulate
 * them -- there is no passthrough port I/O in this project (guests
 * never get direct hardware access, AGENTS.md), so every port a guest
 * touches must be intercepted, not just a chosen subset. */
#define HYPE_SVM_INTERCEPT_IOIO_PROT (1u << 27)

/*
 * EXITINFO1's bitfield for an IOIO intercept (AMD SDM): bit 0 = TYPE
 * (0 = OUT, 1 = IN), bits 6:4 = operand size (bit4=8-bit, bit5=16-bit,
 * bit6=32-bit -- exactly one set), bits 31:16 = port number. EXITINFO2
 * holds the guest RIP to resume at (the instruction after the
 * IN/OUT), the same "next-RIP-for-free" convenience this project
 * already relies on for HLT.
 */
#define HYPE_SVM_IOIO_INFO1_TYPE_IN (1ULL << 0)
#define HYPE_SVM_IOIO_INFO1_SIZE8 (1ULL << 4)
#define HYPE_SVM_IOIO_INFO1_SIZE16 (1ULL << 5)
#define HYPE_SVM_IOIO_INFO1_SIZE32 (1ULL << 6)
#define HYPE_SVM_IOIO_INFO1_PORT_SHIFT 16

typedef struct {
    int is_in;
    uint16_t port;
    uint8_t size_bytes; /* 1, 2, or 4 */
} hype_svm_ioio_t;

/*
 * Decodes an IOIO intercept's EXITINFO1 value into direction/port/
 * operand size. Pure bit extraction, no CPU state touched.
 */
void hype_svm_decode_ioio_info1(uint64_t exitinfo1, hype_svm_ioio_t *out);

/*
 * NPF (#VMEXIT_NPF, M4-3) EXITINFO1 mirrors the standard x86 #PF
 * error-code bit layout: bit0 = the NPT entry was present (a
 * permissions violation) vs. not present at all (this project's own
 * MMIO-trap mechanism, hype_npt_mark_not_present(), only ever produces
 * the not-present case); bit1 = the access was a write. EXITINFO2 is
 * the faulting guest-*physical* address (already fully resolved by
 * hardware page-walking NPT -- unlike the faulting instruction's
 * memory operand encoding, this never needs decoding from raw
 * instruction bytes). No separate intercept-enable bit exists for NPF
 * the way HLT/SHUTDOWN/IOIO have one -- it fires automatically
 * whenever nested paging is enabled (NP_ENABLE=1,
 * hype_vmcb_enable_nested_paging()) and a guest access hits a
 * not-present/permission-violated NPT entry.
 */
#define HYPE_SVM_NPF_INFO1_PRESENT (1ULL << 0)
#define HYPE_SVM_NPF_INFO1_WRITE (1ULL << 1)

typedef struct {
    int is_write;
    uint64_t guest_phys_addr;
} hype_svm_npf_t;

/*
 * Decodes an NPF intercept's EXITINFO1/EXITINFO2 into write-vs-read
 * and the faulting guest-physical address. Pure bit extraction, no CPU
 * state touched.
 */
void hype_svm_decode_npf_info(uint64_t exitinfo1, uint64_t exitinfo2, hype_svm_npf_t *out);

/*
 * Packs a segment's access-rights byte and flags nibble into the
 * VMCB's compressed 16-bit `attrib` format (bits 7:0 = access rights
 * [P|DPL|S|Type], bits 11:8 = flags [G|D/B|L|AVL]) -- the same two
 * pieces gdt.c's hype_gdt_encode_entry() packs into adjacent bytes of
 * a real GDT descriptor, just laid out differently here. Pure
 * bit-packing, no CPU state touched.
 */
uint16_t hype_vmcb_seg_attrib(uint8_t access, uint8_t flags);

/*
 * Fills `vmcb` (zeroed first) for a minimal real-address-mode guest:
 * CR0=ET-only (paging/protection off, matching real mode), RIP=0 and
 * CS.base = entry_phys (so the guest starts executing at physical
 * address entry_phys directly), RSP=0 and SS.base = stack_phys
 * (same reasoning) -- NOT the classic entry_seg*16 convention (16-bit
 * segment selectors can only reach the first ~1MB+64K, but a
 * UEFI-loaded hypervisor's own static buffers can end up anywhere in
 * memory, wherever firmware's PE loader put them). This works because
 * a VMCB segment's base is a value the hypervisor sets directly, never
 * derived from the selector by hardware the way a real segment load
 * would -- so entry_phys/stack_phys can be any address, decoupled
 * from the selector entirely (left 0, meaningless here). CS/SS both
 * keep a real 64KB limit (0xFFFF) and RIP/RSP both stay 0 (this
 * guest's only instruction, a single HLT, never advances or touches
 * the stack), so nothing ever needs to exceed that limit despite
 * base potentially being a large address.
 * HLT and shutdown intercepted, ASID=1, no nested paging (M3's job).
 * iopm_phys/msrpm_phys must point at zeroed, page-aligned buffers of
 * at least 12KB/8KB respectively (the caller owns their allocation) --
 * VMRUN always consults the I/O and MSR permission bitmaps to decide
 * intercepts, for every guest, whether or not it ever executes I/O or
 * RDMSR/WRMSR/etc; leaving these fields at 0 (pointing nowhere valid)
 * is itself a state-consistency violation that makes VMRUN reject the
 * whole VMCB (EXITCODE = HYPE_SVM_EXITCODE_INVALID, no VM-entry at
 * all) -- confirmed the hard way, via QEMU/KVM, before this parameter
 * existed. All-zero bitmap contents mean "intercept nothing," correct
 * for this guest (its only I/O-adjacent behavior, HLT, is intercepted
 * separately above).
 * Pure struct-filling -- no CPU state touched, no UEFI dependency; the
 * actual VMRUN happens in svm_vcpu.c.
 */
void hype_vmcb_build_realmode_guest(hype_vmcb_t *vmcb, uint64_t entry_phys, uint64_t stack_phys,
                                     uint64_t iopm_phys, uint64_t msrpm_phys);

/*
 * Fills `vmcb` (zeroed first) for a minimal 64-bit long-mode guest,
 * matching the Linux/x86_64 boot protocol's documented 64-bit entry
 * state (M3-5, core/linux_boot.h): CR0.PE=1/PG=1, CR4.PAE=1,
 * EFER.LME=1 (plus LMA=1 and SVME, see HYPE_SVM_SAVE_EFER_SVME --
 * VMRUN loads whatever's in the VMCB directly, without walking
 * through the real activation sequence hardware normally requires, so
 * LMA must be set explicitly here rather than relying on hardware to
 * derive it), flat 4GB code/data segments with CS marked as a 64-bit
 * (long-mode) segment, RIP=entry_rip, RSP=rsp, CR3=guest_cr3 (the
 * guest's own identity page tables -- see arch/x86_64/cpu/paging.h's
 * builder, reused as-is since a ring-0-only guest CR3 needs no User/
 * Supervisor bit, unlike NPT). HLT, shutdown, VMRUN, and now IOIO are
 * all intercepted (see HYPE_SVM_INTERCEPT_IOIO_PROT -- this guest is
 * expected to actually execute IN/OUT, unlike the M2-7 real-mode
 * guest). iopm_phys/msrpm_phys have the same requirement as
 * hype_vmcb_build_realmode_guest(). Nested paging is NOT enabled here
 * -- call hype_vmcb_enable_nested_paging() afterward, same layering as
 * every other builder in this file.
 *
 * RSI (the zero page's guest-physical address -- the one register the
 * 64-bit boot protocol requires at entry) is deliberately NOT set
 * here: unlike RAX/RSP/RIP/RFLAGS, the VMCB save-state area has no
 * RSI field at all -- VMRUN only loads/saves a specific subset of
 * architectural state; every other general-purpose register (RSI
 * included) simply passes through whatever the *host* had in it at
 * the moment VMRUN executes. Setting the guest's initial RSI is
 * svm_vcpu.c's job (hype_svm_vcpu_set_rsi()), immediately before
 * VMRUN, not this function's.
 *
 * Pure struct-filling -- no CPU state touched, no UEFI dependency.
 */
void hype_vmcb_build_long_mode_guest(hype_vmcb_t *vmcb, uint64_t entry_rip, uint64_t guest_cr3,
                                      uint64_t rsp, uint64_t iopm_phys, uint64_t msrpm_phys);

/*
 * Enables nested paging (M3-1) on an already-built VMCB: sets
 * NP_ENABLE and points N_CR3 at an NPT root built by
 * hype_npt_build_identity() (arch/x86_64/svm/npt.h). Deliberately
 * separate from hype_vmcb_build_realmode_guest() (which always leaves
 * NP_ENABLE=0) so existing callers/tests of that function are
 * unaffected -- callers that want nested paging call this afterward,
 * the same layering hype_vmcb_configure_avic() already uses (and which
 * AVIC, per its own comment above, actually depends on this having
 * been called first). Pure struct mutation -- no CPU state touched.
 */
void hype_vmcb_enable_nested_paging(hype_vmcb_t *vmcb, uint64_t npt_root_phys);

/*
 * Enables AVIC on an already-built VMCB (see the requirement note
 * above hype_vmcb_configure_avic()'s bit definitions -- `vmcb` must
 * also have NP_ENABLE=1): sets int_ctl's AVIC-enable bit and wires in
 * the guest-visible APIC_BAR physical address plus this vCPU's
 * backing-page and this VM's logical/physical ID table physical
 * addresses (each must already be page-aligned; the caller owns
 * their allocation and lifetime). max_physical_id is the highest
 * physical APIC ID this VM's physical ID table covers (0 for a
 * single-vCPU VM, matching M2's scope). Pure struct mutation --
 * preserves every other int_ctl bit already set.
 */
void hype_vmcb_configure_avic(hype_vmcb_t *vmcb, uint64_t apic_bar_phys,
                               uint64_t backing_page_phys, uint64_t logical_table_phys,
                               uint64_t physical_table_phys, uint8_t max_physical_id);

#endif /* HYPE_ARCH_SVM_VMCB_H */
