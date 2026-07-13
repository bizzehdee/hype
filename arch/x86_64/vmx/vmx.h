#ifndef HYPE_ARCH_VMX_H
#define HYPE_ARCH_VMX_H

#include <stdint.h>

#include "../cpu/vmm_ops.h"

/*
 * Intel VT-x/VMX backend (M2, plan.md §4/§9).
 *
 * UNVALIDATED: this project's dev environment is AMD-only hardware
 * (confirmed while building M2-1 -- CPUID reports no VMX, and there's
 * no way to even emulate real VMX semantics without it). Every line in
 * this backend is written directly from the Intel SDM (Vol 3, chapters
 * 23-25) and cross-checked carefully, but has never been executed, not
 * even once, not even under software emulation. Do not treat any of it
 * as tested until M2-8's real Intel hardware validation gate actually
 * runs it -- that gate is not optional for this backend the way it is
 * (as a second confirmation) for the already-QEMU-validated SVM side.
 */

#define HYPE_MSR_IA32_FEATURE_CONTROL 0x3Au
#define HYPE_FEATURE_CONTROL_LOCK (1ULL << 0)
#define HYPE_FEATURE_CONTROL_VMX_OUTSIDE_SMX (1ULL << 2)
#define HYPE_MSR_IA32_VMX_BASIC 0x480u

#define HYPE_CR4_VMXE (1ULL << 13)

/* Whether IA32_FEATURE_CONTROL's current value permits VMXON: either
 * it isn't locked yet (we can configure and lock it ourselves), or it's
 * locked with VMX-outside-SMX already enabled. Locked-and-disabled
 * means firmware turned VMX off and there's nothing software can do
 * about it. Pure bit logic -- see vmx_bits.c. */
int hype_vmx_feature_control_allows_vmxon(uint64_t feature_control);

/* Given the current IA32_FEATURE_CONTROL value, returns it with the
 * lock bit and VMX-outside-SMX bit set, leaving every other bit
 * untouched. Pure bit-manipulation. */
uint64_t hype_vmx_feature_control_with_vmxon_enabled(uint64_t feature_control);

/* Given the current CR4 value, returns it with VMXE (bit 13) set,
 * leaving every other bit untouched. Pure bit-manipulation. */
uint64_t hype_vmx_cr4_with_vmxe(uint64_t old_cr4);

/*
 * Adjusts a desired 32-bit VM-execution/entry/exit control value
 * against one of the VMX capability MSRs (IA32_VMX_(TRUE_)PINBASED_CTLS/
 * PROCBASED_CTLS/PROCBASED_CTLS2/EXIT_CTLS/ENTRY_CTLS): bits 31:0 of the
 * MSR are the "allowed-0" settings (a bit set there means that control
 * bit is *not* allowed to be 0, i.e. must be 1); bits 63:32 are the
 * "allowed-1" settings (a bit *clear* there means that control bit is
 * not allowed to be 1, i.e. must be 0). The standard algorithm (Intel
 * SDM Vol 3C, Appendix A.3.1 and everywhere else a capability MSR is
 * consulted): OR in whatever must be 1, then AND against whatever may
 * be 1. Pure bit-manipulation, no CPU state touched -- the actual RDMSR
 * is the caller's job (vmcs_hw.c).
 */
uint32_t hype_vmx_adjust_controls(uint32_t desired, uint64_t capability_msr);

/*
 * Enables VMX on the calling physical CPU: unlocks/enables
 * IA32_FEATURE_CONTROL if needed, sets CR4.VMXE, writes the VMCS
 * revision ID into a VMXON region, and executes VMXON. Returns 0 on
 * success, non-zero if FEATURE_CONTROL is locked with VMX disabled or
 * VMXON itself fails. Exempt from unit testing per AGENTS.md -- real
 * MSR/CR4/VMXON, nothing to observe without a real CPU (and see the
 * UNVALIDATED note above for why that matters more here than usual).
 * hype_vmx_feature_control_allows_vmxon()/
 * hype_vmx_feature_control_with_vmxon_enabled()/hype_vmx_cr4_with_vmxe()
 * above hold the only real logic and are fully tested.
 */
int hype_vmx_enable(void);

extern const hype_vmm_ops_t hype_vmx_ops;

#endif /* HYPE_ARCH_VMX_H */
