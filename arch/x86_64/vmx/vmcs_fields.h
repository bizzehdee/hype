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
/* APIC-register virtualization / virtual-interrupt delivery (M2-4):
 * both operate on the virtual-APIC page directly, not through EPT, so
 * (unlike "virtualize APIC accesses", intentionally not used here)
 * they don't require EPT to be enabled. */
#define HYPE_VMX_PROCBASED2_APIC_REGISTER_VIRT (1u << 8)
#define HYPE_VMX_PROCBASED2_VIRTUAL_INTERRUPT_DELIVERY (1u << 9)

/* VM-entry control bits used here. IA32E_MODE_GUEST is deliberately
 * NOT set -- this project's minimal test guest starts in unpaged
 * real-address mode (via "unrestricted guest" above), not long mode. */
#define HYPE_VMX_ENTRY_IA32E_MODE_GUEST (1u << 9)

/* 16-bit fields (Table B-2/B-3). */
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
#define HYPE_VMCS_EPT_POINTER 0x201Au /* full; +1 = high (M3-1) */
#define HYPE_VMCS_VMCS_LINK_POINTER 0x2800u /* full; +1 = high */

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
#define HYPE_VMX_EXIT_REASON_HLT 12u

#endif /* HYPE_ARCH_VMX_VMCS_FIELDS_H */
