#ifndef HYPE_ARCH_VMX_VMCS_FIELDS_H
#define HYPE_ARCH_VMX_VMCS_FIELDS_H

/*
 * VMCS field encodings and VMX capability MSR numbers (M2-3), per the
 * Intel 64 and IA-32 Architectures Software Developer's Manual, Volume
 * 3C, Appendix B ("Field Encoding in VMCS") and Appendix A ("VMX
 * Capability Reporting Facility") -- fetched and read for this task
 * (order number 326019, revision 043, the most recent revision found
 * to still include these appendices directly; a newer combined-volume
 * edition had relocated them elsewhere and wasn't used as the source
 * here), not reconstructed from memory. UNVALIDATED regardless -- see
 * vmx.h.
 */

/* Capability MSRs (Appendix A). */
#define HYPE_MSR_IA32_VMX_PINBASED_CTLS 0x481u
#define HYPE_MSR_IA32_VMX_PROCBASED_CTLS 0x482u
#define HYPE_MSR_IA32_VMX_EXIT_CTLS 0x483u
#define HYPE_MSR_IA32_VMX_ENTRY_CTLS 0x484u
#define HYPE_MSR_IA32_VMX_CR0_FIXED0 0x486u
#define HYPE_MSR_IA32_VMX_CR0_FIXED1 0x487u
#define HYPE_MSR_IA32_VMX_CR4_FIXED0 0x488u
#define HYPE_MSR_IA32_VMX_CR4_FIXED1 0x489u
#define HYPE_MSR_IA32_VMX_PROCBASED_CTLS2 0x48Bu
#define HYPE_MSR_IA32_VMX_EPT_VPID_CAP 0x48Cu
#define HYPE_MSR_IA32_VMX_TRUE_PINBASED_CTLS 0x48Du
#define HYPE_MSR_IA32_VMX_TRUE_PROCBASED_CTLS 0x48Eu
#define HYPE_MSR_IA32_VMX_TRUE_EXIT_CTLS 0x48Fu
#define HYPE_MSR_IA32_VMX_TRUE_ENTRY_CTLS 0x490u

/* IA32_VMX_BASIC bit 55: if set, the TRUE_* capability MSRs above exist
 * and should be preferred over the non-TRUE ones. */
#define HYPE_VMX_BASIC_HAS_TRUE_CTLS (1ULL << 55)

/* Primary processor-based VM-execution control bits used here. */
#define HYPE_VMX_PROCBASED_ACTIVATE_SECONDARY_CONTROLS (1u << 31)
/*
 * TPR shadow (M2-4): lets the guest read/write CR8/TPR against a
 * virtual-APIC page instead of trapping, without needing EPT (unlike
 * "virtualize APIC accesses" below, this doesn't require EPT-mapped
 * memory -- the virtual-APIC page is referenced by a plain physical
 * address, same as the MSR/I/O bitmaps). This encoding and the
 * secondary-control bits below are from strong, well-corroborated
 * recall (they match every open-source VMX implementation, e.g.
 * Linux's arch/x86/include/asm/vmx.h) rather than re-verified against
 * the fetched Appendix B/Table 24-6 text specifically for this task,
 * unlike vmcs_fields.h's M2-3 batch above.
 */
#define HYPE_VMX_PROCBASED_USE_TPR_SHADOW (1u << 21)
/* HLT exiting (primary proc-based control, bit 7): a guest HLT causes a
 * VM-exit (reason 12) instead of parking the logical processor -- without
 * it a guest that halts (every test guest's terminal instruction, and any
 * real OS idle loop) would never return control to hype. */
#define HYPE_VMX_PROCBASED_HLT_EXITING (1u << 7)
/* Unconditional I/O exiting (primary proc-based control, bit 24): every guest
 * IN/OUT causes a VM-exit (reason 30) regardless of port -- without it (and
 * without I/O bitmaps) guest port I/O executes straight on real hardware
 * instead of trapping to hype's device models. Simpler than I/O bitmaps and
 * correct for this project (hype intercepts all guest I/O). */
#define HYPE_VMX_PROCBASED_UNCOND_IO_EXITING (1u << 24)
/* Interrupt-window exiting (primary proc-based control, bit 2): VM-exit (reason
 * 7) as soon as the guest can accept an external interrupt (RFLAGS.IF=1, no
 * interrupt/NMI shadow) -- the VMX analogue of SVM's VINTR window, used to
 * deliver a pending vector the guest wasn't ready for at request time. */
#define HYPE_VMX_PROCBASED_INTERRUPT_WINDOW_EXITING (1u << 2)
/* Secondary processor-based VM-execution control bits used here. */
/* "Unrestricted guest" (below) requires "enable EPT" to also be 1 --
 * Intel SDM: an unrestricted guest can run with paging disabled, and
 * EPT is what still gives every guest-physical access a real
 * translation/permission check in that state. M3-1 is what actually
 * builds and wires in real EPT tables (ept.c); before that, M2's
 * vcpu_create path never got to VMLAUNCH/VMRESUME anyway (see
 * vmx_ops.c), so this requirement went unmet without effect -- still
 * worth fixing now that EPT construction exists, since VMX's
 * unrestricted-guest configuration would otherwise be invalid on real
 * hardware regardless. */
#define HYPE_VMX_PROCBASED2_ENABLE_EPT (1u << 1)
#define HYPE_VMX_PROCBASED2_UNRESTRICTED_GUEST (1u << 7)
/*
 * "Enable INVPCID" (secondary control, bit 12) -- #251.
 *
 * Not an optimisation: with this control CLEAR, a guest executing INVPCID takes
 * #UD (SDM Vol 3C, "Changes to Instruction Behavior in VMX Non-Root Operation").
 * hype passes the host's CPUID leaf 7 through, so an Intel guest sees INVPCID
 * advertised and Linux uses it in native_flush_tlb_global() -- the very first
 * global TLB flush, from cache_cpu_init() during setup_arch(). The guest then
 * faulted before its console existed and parked in a hlt;jmp loop.
 *
 * SVM has no equivalent gate, which is why AMD never hit this: INVPCID simply
 * executes. Requested through adjust_controls() so it is dropped if unsupported,
 * in which case the CPUID bit ought to be masked instead.
 */
#define HYPE_VMX_PROCBASED2_ENABLE_INVPCID (1u << 12)
/*
 * "Enable RDTSCP" (secondary control, bit 3) -- #252/A2, and the exact same shape
 * as ENABLE_INVPCID above.
 *
 * With this control CLEAR, a guest executing RDTSCP takes #UD regardless of what
 * CPUID says (SDM Vol 3C, "Changes to Instruction Behavior in VMX Non-Root
 * Operation"). hype passes the host's extended CPUID through, so an Intel guest
 * sees RDTSCP advertised and Linux uses it in pvclock_clocksource_read_nowd() --
 * i.e. on EVERY clocksource read. Observed as a #UD storm at
 * pvclock_clocksource_read_nowd+0xe which left the guest parked in
 * queued_spin_lock_slowpath.
 *
 * SVM has no equivalent gate, which is why AMD never hit it. Requested through
 * adjust_controls() so it is dropped if unsupported, in which case the CPUID bit
 * (leaf 0x80000001 EDX bit 27) ought to be masked instead.
 */
#define HYPE_VMX_PROCBASED2_ENABLE_RDTSCP (1u << 3)
/*
 * "Enable VPID" (secondary control, bit 5) -- #273.
 *
 * This is a PERFORMANCE control, not a correctness one, and #273's original
 * premise ("TLB entries alias across VMs today") was wrong. SDM Vol 3C 28.3.3.1:
 * with this control CLEAR, every VM entry "invalidates linear mappings and
 * combined mappings associated with VPID 0000H (for all PCIDs)", and combined
 * mappings for VPID 0000H are invalidated "for all EP4TA values". So today's
 * VPID-0 configuration is safe by brute force -- each transition throws the
 * translations away, which is precisely why nothing has ever aliased. hype also
 * pins each vCPU to its own AP core, so two guests never share a TLB anyway.
 *
 * Setting the control REMOVES that automatic flush, which is the whole point
 * (hype exits often, and each exit currently costs a full TLB refill) but also
 * means staleness is no longer masked. Hence the paired requirements below: only
 * enable it when INVVPID single-context is available, and invalidate a VPID when
 * a pool slot is handed to a new guest.
 *
 * Guest-physical mappings are NOT VPID-tagged -- they are tagged by EP4TA and
 * governed by INVEPT. Per-VM EPT roots (#272) already keep those apart, which is
 * also why #274's isolation does not actually depend on this ticket.
 */
#define HYPE_VMX_PROCBASED2_ENABLE_VPID (1u << 5)

/*
 * "WBINVD exiting" (secondary control, bit 6) -- SDM Vol. 3C, Table 27-7: WBINVD and WBNOINVD
 * cause a VM exit only when this control is set.
 *
 * Without it a guest's WBINVD executes on real hardware and flushes the caches of the whole
 * machine -- hype's own, and every other VM's. That breaks the isolation rule outright: a guest
 * must not be able to degrade another. hype made it worse by advertising WBINVD as coherent in
 * the FADT (devices/acpi.c), which is an invitation for guests to use it.
 *
 * Suspected in #368, where a guest on the BSP's SMT sibling cost the other thread ~400x on
 * cached stores -- far more than ordinary SMT contention should. Repeated cache maintenance from
 * a sibling explains that magnitude where cache pressure alone does not. NOT established as a
 * machine-wide effect on unrelated cores; the evidence supports a shared-core mechanism only.
 */
#define HYPE_VMX_PROCBASED2_WBINVD_EXITING (1u << 6)
/* APIC-register virtualization / virtual-interrupt delivery (M2-4):
 * both operate on the virtual-APIC page directly, not through EPT, so
 * (unlike "virtualize APIC accesses", intentionally not used here)
 * they don't require EPT to be enabled. */
#define HYPE_VMX_PROCBASED2_APIC_REGISTER_VIRT (1u << 8)
#define HYPE_VMX_PROCBASED2_VIRTUAL_INTERRUPT_DELIVERY (1u << 9)
/* #599: "virtualize APIC accesses" (bit 0) IS now used, by the APICv path: guest
 * physical accesses to the page EPT-mapped onto the APIC-access page are either
 * satisfied from the virtual-APIC page (most register reads, when
 * APIC_REGISTER_VIRT is set), turned into trap-like APIC-write VM exits (reason
 * 56), or reported as APIC-access VM exits (reason 44) for the offsets the CPU
 * will not virtualize -- the timer's current count (390H) among them. */
#define HYPE_VMX_PROCBASED2_VIRTUALIZE_APIC_ACCESSES (1u << 0)

/* VM-entry control bits used here. IA32E_MODE_GUEST is deliberately
 * NOT set -- this project's minimal test guest starts in unpaged
 * real-address mode (via "unrestricted guest" above), not long mode. */
#define HYPE_VMX_ENTRY_IA32E_MODE_GUEST (1u << 9)
/* VM-entry control "load IA32_EFER" (bit 15): load guest EFER from the
 * GUEST_IA32_EFER VMCS field on entry, so a long-mode guest gets LME/LMA set
 * consistently with IA32E_MODE_GUEST + CR0.PG + CR4.PAE. */
#define HYPE_VMX_ENTRY_LOAD_IA32_EFER (1u << 15)

/* VM-exit control: "host address-space size" (bit 9). MUST be 1 whenever the
 * host runs in IA-32e (64-bit) mode -- which hype always does post-EBS -- or
 * VM-entry fails the control-field checks with VM-instruction-error 7. This is
 * the classic first-VMLAUNCH failure: adjust_controls(0, ...) only forces the
 * MSR's required-1 bits, and this bit is NOT one of them, so it must be
 * requested explicitly. */
#define HYPE_VMX_EXIT_HOST_ADDR_SPACE_SIZE (1u << 9)
/*
 * VM-exit control: "acknowledge interrupt on exit" (bit 15). #248.
 *
 * With this CLEAR, an external-interrupt VM exit leaves the interrupt PENDING and
 * unacknowledged at the LAPIC/PIC. Since a VM exit also forces host RFLAGS to
 * 0x2 (IF=0), nothing takes it, and re-entering the guest exits again on the very
 * same interrupt -- an unbounded storm (13.8M exits observed on Intel).
 *
 * With it SET, the CPU performs the interrupt acknowledge cycle itself and
 * reports the vector in VM_EXIT_INTR_INFO, so the interrupt is no longer pending
 * and hype dispatches it from that field rather than relying on its own IDT
 * firing during a hand-opened STI window.
 *
 * Like HOST_ADDR_SPACE_SIZE this is not a required-1 bit, so it must be requested
 * explicitly -- and requested through adjust_controls(), because a CPU (or an L0
 * hypervisor when nested) is permitted not to support it.
 */
#define HYPE_VMX_EXIT_ACK_INTR_ON_EXIT (1u << 15)
/*
 * VM-exit controls for IA32_EFER (#248). These pair with the entry-side
 * LOAD_IA32_EFER: once the guest runs on its OWN EFER, the host's must be put
 * back on exit and the guest's preserved.
 *
 * "load IA32_EFER" (bit 21) restores EFER from HOST_IA32_EFER on VM exit. Not
 * strictly required -- with "host address-space size" set, an exit forces
 * EFER.LME/LMA to 1 anyway, which is what a 64-bit host needs -- but the other
 * EFER bits (NXE, SCE) would otherwise keep the GUEST's values in host context.
 * NXE in particular changes how the host's own page tables are interpreted, so
 * restore it explicitly rather than relying on the two bits the exit fixes up.
 *
 * "save IA32_EFER" (bit 20) writes the guest's EFER back to GUEST_IA32_EFER on
 * exit, so a change the guest made through a path hype did not intercept is not
 * silently reverted by the next entry.
 */
#define HYPE_VMX_EXIT_SAVE_IA32_EFER (1u << 20)
#define HYPE_VMX_EXIT_LOAD_IA32_EFER (1u << 21)
/*
 * VM-entry / VM-exit MSR areas (#251 slice 2). Appendix B, 64-bit control fields.
 *
 * Needed for MSRs that have NO VMCS field and that the guest can change without
 * a VM exit. IA32_KERNEL_GS_BASE is the motivating case: SWAPGS exchanges it with
 * GS.base and does not exit, so a value hype captured at WRMSR time would be
 * stale the moment the guest swaps. The hardware areas avoid that -- it loads and
 * stores them itself around every transition.
 *
 * The guest area is used for BOTH entry-load and exit-store, deliberately: the
 * exit stores whatever the guest ended up with (including via SWAPGS) into the
 * same table the next entry loads from, so guest changes persist without hype
 * observing them. The host area is separate and exit-load-only.
 */
#define HYPE_VMCS_VM_EXIT_MSR_STORE_ADDR 0x2006u
#define HYPE_VMCS_VM_EXIT_MSR_LOAD_ADDR 0x2008u
#define HYPE_VMCS_VM_ENTRY_MSR_LOAD_ADDR 0x200Au
#define HYPE_VMCS_VM_EXIT_MSR_STORE_COUNT 0x400Eu
#define HYPE_VMCS_VM_EXIT_MSR_LOAD_COUNT 0x4010u
#define HYPE_VMCS_VM_ENTRY_MSR_LOAD_COUNT 0x4014u

/* MSRs carried in those areas. KERNEL_GS_BASE is the one that matters for a Linux
 * guest reaching userspace; the SYSCALL set is included because it has exactly the
 * same shape -- no VMCS field, changed by the guest, and supplied free by
 * vmload/vmsave on SVM -- so finding each one via its own fault would be waste. */
#define HYPE_MSR_IA32_KERNEL_GS_BASE 0xC0000102u
#define HYPE_MSR_IA32_STAR 0xC0000081u
#define HYPE_MSR_IA32_LSTAR 0xC0000082u
#define HYPE_MSR_IA32_CSTAR 0xC0000083u
#define HYPE_MSR_IA32_SFMASK 0xC0000084u
/*
 * #270: IA32_TSC_AUX. RDTSCP returns it in ECX, and Linux stores the CPU (and node)
 * number there for getcpu()/the vDSO. It became load-bearing only when ab93854
 * enabled the ENABLE_RDTSCP secondary control -- before that RDTSCP took #UD, so the
 * MSR was unreachable and its absence cost nothing.
 */
#define HYPE_MSR_IA32_TSC_AUX 0xC0000103u

/*
 * IA32_PAT (#251). SVM writes the guest's PAT into the VMCB's save.g_pat, which
 * VMRUN loads; VMX has a VMCS field plus load/save controls and hype used neither,
 * so a guest WRMSR to 0x277 was absorbed and its RDMSR answered 0. A guest that
 * reads PAT as 0 concludes every memory type is UC -- this project has already
 * paid for that once: PERF-1's ~5-minute guest boot was uncacheable guest RAM
 * from g_pat=0.
 */
#define HYPE_VMCS_GUEST_IA32_PAT 0x2804u
#define HYPE_VMCS_HOST_IA32_PAT 0x2C00u
#define HYPE_VMX_ENTRY_LOAD_IA32_PAT (1u << 14)
#define HYPE_VMX_EXIT_SAVE_IA32_PAT (1u << 18)
#define HYPE_VMX_EXIT_LOAD_IA32_PAT (1u << 19)
#define HYPE_MSR_IA32_PAT 0x277u
/* x86 reset value: PAT0..7 = WB,WT,UC-,UC,WB,WT,UC-,UC. Used as the guest's
 * starting PAT so memory is cacheable before the guest sets its own. */
#define HYPE_VMX_PAT_RESET_VALUE 0x0007040600070406ull

/* Host IA32_EFER (0x2C02, 64-bit host-state field), source for the above. */
#define HYPE_VMCS_HOST_IA32_EFER 0x2C02u
/* IA32_EFER's MSR number. Defined here rather than including the SVM header for
 * it -- the two backends share the architectural constant, not each other's
 * headers (svm.h has its own HYPE_MSR_EFER for the same register). */
#define HYPE_MSR_IA32_EFER 0xC0000080u
/* EFER bits hype has to keep consistent with CR0.PG for a mode transition. */
#define HYPE_VMX_EFER_LME (1ull << 8)
#define HYPE_VMX_EFER_LMA (1ull << 10)
/* CR0.PG. Host-owned (#248) so hype sees the guest enabling/disabling paging and
 * can keep EFER.LMA and the IA-32e-mode-guest entry control in step with it. */
#define HYPE_VMX_CR0_PG (1ull << 31)
/*
 * A usable flat data-segment AR byte (#251): P=1, DPL=0, S=1, type=3
 * (data, read/write, accessed), D/B=1, G=1 -- and crucially WITHOUT bit 16, the
 * "unusable" bit. hype's real-mode segment setup leaves FS/GS/SS unusable
 * (measured gs_ar=0x1c000), and an access through an unusable segment raises
 * #GP(0) no matter what its base holds.
 */
#define HYPE_VMX_AR_DATA_USABLE 0xC093u
/* The unusable bit itself, for reading such a field back. */
#define HYPE_VMX_AR_UNUSABLE (1u << 16)

/* 16-bit fields (Table B-2/B-3). */
/* 16-bit control field: the guest's VPID (#273). Must be non-zero whenever
 * ENABLE_VPID is set, or VM entry fails the control-field checks -- VPID 0000H
 * is reserved for VMX root operation (the host). */
#define HYPE_VMCS_VIRTUAL_PROCESSOR_ID 0x0000u
#define HYPE_VMCS_GUEST_ES_SELECTOR 0x0800u
#define HYPE_VMCS_GUEST_CS_SELECTOR 0x0802u
#define HYPE_VMCS_GUEST_SS_SELECTOR 0x0804u
#define HYPE_VMCS_GUEST_DS_SELECTOR 0x0806u
#define HYPE_VMCS_GUEST_FS_SELECTOR 0x0808u
#define HYPE_VMCS_GUEST_GS_SELECTOR 0x080Au
#define HYPE_VMCS_GUEST_LDTR_SELECTOR 0x080Cu
#define HYPE_VMCS_GUEST_TR_SELECTOR 0x080Eu
#define HYPE_VMCS_HOST_ES_SELECTOR 0x0C00u
#define HYPE_VMCS_HOST_CS_SELECTOR 0x0C02u
#define HYPE_VMCS_HOST_SS_SELECTOR 0x0C04u
#define HYPE_VMCS_HOST_DS_SELECTOR 0x0C06u
#define HYPE_VMCS_HOST_FS_SELECTOR 0x0C08u
#define HYPE_VMCS_HOST_GS_SELECTOR 0x0C0Au
#define HYPE_VMCS_HOST_TR_SELECTOR 0x0C0Cu

/* 64-bit fields (Table B-4/B-6). */
#define HYPE_VMCS_VIRTUAL_APIC_PAGE_ADDR 0x2012u /* full; +1 = high (M2-4) */
#define HYPE_VMCS_APIC_ACCESS_ADDR 0x2014u       /* #599: the APIC-access page */
#define HYPE_VMCS_EOI_EXIT_BITMAP0 0x201Cu       /* #599: vectors whose EOI exits */
#define HYPE_VMCS_EOI_EXIT_BITMAP1 0x201Eu
#define HYPE_VMCS_EOI_EXIT_BITMAP2 0x2020u
#define HYPE_VMCS_EOI_EXIT_BITMAP3 0x2022u
#define HYPE_VMCS_EPT_POINTER 0x201Au /* full; +1 = high (M3-1) */
#define HYPE_VMCS_VMCS_LINK_POINTER 0x2800u /* full; +1 = high */
#define HYPE_VMCS_GUEST_IA32_EFER 0x2806u    /* 64-bit guest-state field */

/* 32-bit control fields (Table B-8). */
#define HYPE_VMCS_PIN_BASED_VM_EXEC_CONTROL 0x4000u
#define HYPE_VMCS_CPU_BASED_VM_EXEC_CONTROL 0x4002u
#define HYPE_VMCS_EXCEPTION_BITMAP 0x4004u
#define HYPE_VMCS_VM_EXIT_CONTROLS 0x400Cu
#define HYPE_VMCS_VM_ENTRY_CONTROLS 0x4012u
#define HYPE_VMCS_TPR_THRESHOLD 0x401Cu /* M2-4 */
#define HYPE_VMCS_SECONDARY_VM_EXEC_CONTROL 0x401Eu

/* 32-bit read-only VM-exit info fields (Table B-9). */
#define HYPE_VMCS_VM_INSTRUCTION_ERROR 0x4400u
#define HYPE_VMCS_VM_EXIT_REASON 0x4402u

/* 32-bit guest-state fields (Table B-10). */
#define HYPE_VMCS_GUEST_ES_LIMIT 0x4800u
#define HYPE_VMCS_GUEST_CS_LIMIT 0x4802u
#define HYPE_VMCS_GUEST_SS_LIMIT 0x4804u
#define HYPE_VMCS_GUEST_DS_LIMIT 0x4806u
#define HYPE_VMCS_GUEST_FS_LIMIT 0x4808u
#define HYPE_VMCS_GUEST_GS_LIMIT 0x480Au
#define HYPE_VMCS_GUEST_LDTR_LIMIT 0x480Cu
#define HYPE_VMCS_GUEST_TR_LIMIT 0x480Eu
#define HYPE_VMCS_GUEST_GDTR_LIMIT 0x4810u
#define HYPE_VMCS_GUEST_IDTR_LIMIT 0x4812u
#define HYPE_VMCS_GUEST_ES_AR_BYTES 0x4814u
#define HYPE_VMCS_GUEST_CS_AR_BYTES 0x4816u
#define HYPE_VMCS_GUEST_SS_AR_BYTES 0x4818u
#define HYPE_VMCS_GUEST_DS_AR_BYTES 0x481Au
#define HYPE_VMCS_GUEST_FS_AR_BYTES 0x481Cu
#define HYPE_VMCS_GUEST_GS_AR_BYTES 0x481Eu
#define HYPE_VMCS_GUEST_LDTR_AR_BYTES 0x4820u
#define HYPE_VMCS_GUEST_TR_AR_BYTES 0x4822u
#define HYPE_VMCS_GUEST_INTERRUPTIBILITY_STATE 0x4824u
#define HYPE_VMCS_GUEST_ACTIVITY_STATE 0x4826u

/* 32-bit host-state field (Table B-11). */
#define HYPE_VMCS_HOST_IA32_SYSENTER_CS 0x4C00u

/* Natural-width control fields (Table B-12). */
#define HYPE_VMCS_CR0_GUEST_HOST_MASK 0x6000u
#define HYPE_VMCS_CR4_GUEST_HOST_MASK 0x6002u
#define HYPE_VMCS_CR0_READ_SHADOW 0x6004u
#define HYPE_VMCS_CR4_READ_SHADOW 0x6006u

/* Natural-width read-only fields (Table B-13). */
#define HYPE_VMCS_EXIT_QUALIFICATION 0x6400u
/* Guest-linear address (0x640A, natural-width read-only). Meaningful for an EPT
 * violation caused by a linear-address translation (as opposed to a page-table
 * walk of the guest's own tables). Pairs with GUEST_PHYSICAL_ADDRESS to tell
 * "the guest touched THIS address, which mapped to THAT GPA" from "the EPT walk
 * of the guest's page tables itself faulted" -- #248/#236. */
#define HYPE_VMCS_GUEST_LINEAR_ADDRESS 0x640Au
/* Guest-physical address (0x2400, 64-bit read-only): the faulting GPA reported
 * on an EPT violation -- the VMX analogue of SVM NPF's EXITINFO2. */
#define HYPE_VMCS_GUEST_PHYSICAL_ADDRESS 0x2400u

/* Natural-width guest-state fields (Table B-14). */
#define HYPE_VMCS_GUEST_CR0 0x6800u
#define HYPE_VMCS_GUEST_CR3 0x6802u
#define HYPE_VMCS_GUEST_CR4 0x6804u
#define HYPE_VMCS_GUEST_ES_BASE 0x6806u
#define HYPE_VMCS_GUEST_CS_BASE 0x6808u
#define HYPE_VMCS_GUEST_SS_BASE 0x680Au
#define HYPE_VMCS_GUEST_DS_BASE 0x680Cu
#define HYPE_VMCS_GUEST_FS_BASE 0x680Eu
#define HYPE_VMCS_GUEST_GS_BASE 0x6810u
#define HYPE_VMCS_GUEST_LDTR_BASE 0x6812u
#define HYPE_VMCS_GUEST_TR_BASE 0x6814u
#define HYPE_VMCS_GUEST_GDTR_BASE 0x6816u
#define HYPE_VMCS_GUEST_IDTR_BASE 0x6818u
#define HYPE_VMCS_GUEST_DR7 0x681Au
#define HYPE_VMCS_GUEST_RSP 0x681Cu
#define HYPE_VMCS_GUEST_RIP 0x681Eu
#define HYPE_VMCS_GUEST_RFLAGS 0x6820u

/* Natural-width host-state fields (Table B-15). */
#define HYPE_VMCS_HOST_CR0 0x6C00u
#define HYPE_VMCS_HOST_CR3 0x6C02u
#define HYPE_VMCS_HOST_CR4 0x6C04u
#define HYPE_VMCS_HOST_FS_BASE 0x6C06u
#define HYPE_VMCS_HOST_GS_BASE 0x6C08u
#define HYPE_VMCS_HOST_TR_BASE 0x6C0Au
#define HYPE_VMCS_HOST_GDTR_BASE 0x6C0Cu
#define HYPE_VMCS_HOST_IDTR_BASE 0x6C0Eu
#define HYPE_VMCS_HOST_RSP 0x6C14u
#define HYPE_VMCS_HOST_RIP 0x6C16u

/* VMX basic exit reasons (Appendix C) this project checks for. */
#define HYPE_VMX_EXIT_REASON_TRIPLE_FAULT 2u
#define HYPE_VMX_EXIT_REASON_INTERRUPT_WINDOW 7u
#define HYPE_VMX_EXIT_REASON_CPUID 10u
#define HYPE_VMX_EXIT_REASON_HLT 12u
#define HYPE_VMX_EXIT_REASON_VMCALL 18u
#define HYPE_VMX_EXIT_REASON_WBINVD 54u
/* #599 APICv exit reasons. APIC_ACCESS covers the offsets the CPU refuses to
 * virtualize (the live timer count chief among them) plus any non-register-
 * shaped access; VIRTUALIZED_EOI delivers the EOI'd vector (exit qualification
 * bits 7:0) for hype's IO-APIC level bookkeeping; APIC_WRITE is trap-like --
 * the value is already stored in the virtual-APIC page at the qualification
 * offset, and RIP has already advanced. */
#define HYPE_VMX_EXIT_REASON_APIC_ACCESS 44u
#define HYPE_VMX_EXIT_REASON_VIRTUALIZED_EOI 45u
#define HYPE_VMX_EXIT_REASON_APIC_WRITE 56u
#define HYPE_VMCS_GUEST_INTERRUPT_STATUS 0x0810u /* 16-bit: RVI (7:0) | SVI (15:8) */

/* Retires an intercepted guest WBINVD without flushing any real cache, and counts it. */
void hype_vmx_vcpu_handle_wbinvd(void);
void hype_vmx_wbinvd_stats(unsigned long long *count, uint64_t *last_rip);
#define HYPE_VMX_EXIT_REASON_RDMSR 31u
#define HYPE_VMX_EXIT_REASON_WRMSR 32u
#define HYPE_VMX_EXIT_REASON_IO_INSTRUCTION 30u
#define HYPE_VMX_EXIT_REASON_EPT_VIOLATION 48u
/*
 * Control-register access (SDM Vol 3C, Table C-1, "Control-register accesses").
 * Taken whenever the guest touches a CR bit the host owns via
 * CR0/CR4_GUEST_HOST_MASK. hype needs this because IA32_VMX_CR4_FIXED0 requires
 * CR4.VMXE in VMX operation while guest firmware, not knowing it is
 * virtualised, writes CR4 with VMXE clear -- letting that reach GUEST_CR4
 * raises #GP(0) (#248).
 *
 * EXIT_QUALIFICATION layout for a MOV to CR: bits 3:0 = CR number,
 * bits 5:4 = access type (0 = MOV to CR), bits 11:8 = the source GPR.
 */
#define HYPE_VMX_EXIT_REASON_CR_ACCESS 28u
/*
 * XSETBV (#251). UNCONDITIONAL: no VM-execution control gates it, so a guest
 * setting XCR0 always exits and the hypervisor must emulate it. SVM makes it an
 * optional intercept that hype does not take, so there it simply executes -- the
 * same vendor asymmetry as INVPCID, and it surfaced immediately after that fix
 * as "exited the initial dispatch loop (reason=0x37)".
 */
#define HYPE_VMX_EXIT_REASON_XSETBV 55u
#define HYPE_VMX_CR_ACCESS_CR_MASK 0x0Fu
#define HYPE_VMX_CR_ACCESS_TYPE_SHIFT 4u
#define HYPE_VMX_CR_ACCESS_TYPE_MASK 0x03u
#define HYPE_VMX_CR_ACCESS_TYPE_MOV_TO_CR 0u
#define HYPE_VMX_CR_ACCESS_GPR_SHIFT 8u
#define HYPE_VMX_CR_ACCESS_GPR_MASK 0x0Fu
/* Bits the host must keep set regardless of what the guest writes. CR4.VMXE is
 * required by IA32_VMX_CR4_FIXED0; CR0.NE likewise (unrestricted guest relaxes
 * only CR0.PE and CR0.PG, so NE stays mandatory in both modes). */
#define HYPE_VMX_CR4_VMXE 0x2000ull
#define HYPE_VMX_CR0_NE 0x0020ull
/*
 * VMX-4 (#236): the remaining exit reasons the FW-1 loop dispatches on.
 *
 * EXCEPTION_NMI (0) is where VMX and SVM differ most in shape: SVM encodes the
 * faulting vector INTO the exit code (EXITCODE_EXCEPTION_BASE + vector), while
 * VMX reports one reason for all of them and puts the vector in
 * VM_EXIT_INTR_INFO bits 7:0. So "was this a #PF" is one comparison on SVM and
 * two on VMX -- see vmm_reason_is_exception() in boot/main.c.
 *
 * SHUTDOWN has no exact VMX analogue; a guest triple fault is the equivalent
 * "the guest destroyed itself" exit, which is what the FW-1 loop treats
 * SHUTDOWN as.
 */
#define HYPE_VMX_EXIT_REASON_EXCEPTION_NMI 0u
#define HYPE_VMX_EXIT_REASON_EXTERNAL_INTERRUPT 1u
#define HYPE_VMX_EXIT_REASON_PAUSE 40u
/* VM-exit interruption-information (0x4404): bits 7:0 vector, 10:8 type,
 * bit 11 error-code-valid, bit 31 valid. The read side of VM_ENTRY_INTR_INFO. */
#define HYPE_VMCS_VM_EXIT_INTR_INFO 0x4404u
/* VM-exit interruption error code (0x4406), valid when bit 11 above is set. */
#define HYPE_VMCS_VM_EXIT_INTR_ERROR_CODE 0x4406u
/* VM-exit instruction length (0x440C): bytes of the instruction that caused
 * the exit, used to advance guest RIP past an emulated instruction. */
#define HYPE_VMCS_VM_EXIT_INSTRUCTION_LEN 0x440Cu
/* VM-entry interruption-information field (0x4016): stages an event for the
 * next VM-entry. Bits 7:0 vector, 10:8 type (0=external interrupt), bit 31
 * valid. The VMX analogue of SVM's EVENTINJ. */
#define HYPE_VMCS_VM_ENTRY_INTR_INFO_FIELD 0x4016u
/*
 * #315: IDT-vectoring information (0x4408) and its error code (0x440A) -- the VMX analogue of SVM's
 * EXITINTINFO, with the SAME bit layout AND the same type encodings (0=external interrupt, 2=NMI,
 * 3=hardware exception, 4=software interrupt). Set when the VM exit happened WHILE the guest was
 * delivering an event through its own IDT, which for an external interrupt means the vector is
 * already consumed and this field is the only surviving copy.
 *
 * Because the layout and encodings match, the SVM decision logic (hype_svm_decide_event_replay) is
 * reused verbatim rather than reimplemented -- the two backends diverging on this is exactly what
 * #315 asked to prevent, and a second copy of the reasoning is how they would.
 */
#define HYPE_VMCS_IDT_VECTORING_INFO_FIELD 0x4408u
#define HYPE_VMCS_IDT_VECTORING_ERROR_CODE 0x440Au
/*
 * VMX-4 (#236), all needed by the FW-1 live-guest path.
 *
 * VM-entry exception error code (0x4018) and VM-entry instruction length
 * (0x401A) accompany the field above when re-injecting a fault: unlike SVM's
 * EVENTINJ, which carries the error code inside the same 64-bit field, VMX
 * splits them across three fields. Instruction length matters only for
 * software exceptions/interrupts (type 4-6), but writing it is harmless
 * otherwise.
 */
#define HYPE_VMCS_VM_ENTRY_EXCEPTION_ERROR_CODE 0x4018u
#define HYPE_VMCS_VM_ENTRY_INSTRUCTION_LEN 0x401Au
/*
 * PLE (Pause-Loop Exiting) gap and window (0x4020/0x4022), the VMX analogue of
 * SVM's pause filter: PLE_GAP is the max TSC gap between two PAUSEs for them to
 * count as being in the same loop, PLE_WINDOW the max TSC window before a PAUSE
 * exits. Note the units differ from SVM's, which counts *PAUSE executions*
 * (count/threshold) rather than TSC ticks -- so the two backends cannot share a
 * number, and the shim converts rather than passing values through.
 */
#define HYPE_VMCS_PLE_GAP 0x4020u
#define HYPE_VMCS_PLE_WINDOW 0x4022u

/* Interruptibility-state bits (0x4824, Table 25-3): guest blocking that
 * prevents an interrupt being delivered right now. Bits 0/1 are the STI and
 * MOV-SS shadows -- SVM's single INTERRUPT_SHADOW covers the same ground. */
#define HYPE_VMX_INTERRUPTIBILITY_BLOCKING_BY_STI (1u << 0)
#define HYPE_VMX_INTERRUPTIBILITY_BLOCKING_BY_MOV_SS (1u << 1)

/* Guest activity state (0x4826, Table 25-4): 0=active, 1=HLT. Written to wake
 * a halted guest, which on SVM is instead implicit in resuming past the
 * intercepted HLT. */
#define HYPE_VMX_ACTIVITY_STATE_ACTIVE 0u
#define HYPE_VMX_ACTIVITY_STATE_HLT 1u

/* Pin-based control: exit on an external interrupt (the VMX analogue of SVM's
 * INTR intercept, which is how hype keeps host timekeeping alive while a guest
 * runs). */
#define HYPE_VMX_PINBASED_EXT_INTR_EXITING (1u << 0)

/* Primary control: exit on PAUSE. Used with the PLE fields above; the
 * secondary PAUSE-loop-exiting control (bit 10) is the one PLE_GAP/WINDOW
 * actually gate, so both are needed for a real pause filter. */
#define HYPE_VMX_PROCBASED_PAUSE_EXITING (1u << 30)
#define HYPE_VMX_PROCBASED2_PAUSE_LOOP_EXITING (1u << 10)

#endif /* HYPE_ARCH_VMX_VMCS_FIELDS_H */
