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

/* IA32_VMX_CR0/CR4_FIXED0/1 (SDM Vol 3C, "VMXON" + App A.7/A.8): before
 * VMXON, CR0 and CR4 must have every bit set that is 1 in FIXED0 and
 * every bit clear that is 0 in FIXED1. Not applying these is a common
 * cause of a #GP from VMXON on real silicon (e.g. CR0.NE, bit 5, which
 * VMX requires but some environments leave clear). */
#define HYPE_MSR_IA32_VMX_CR0_FIXED0 0x486u
#define HYPE_MSR_IA32_VMX_CR0_FIXED1 0x487u
#define HYPE_MSR_IA32_VMX_CR4_FIXED0 0x488u
#define HYPE_MSR_IA32_VMX_CR4_FIXED1 0x489u

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

/* Forces a CR0/CR4 value to satisfy the VMX fixed-bit requirements:
 * every bit that is 1 in fixed0 must be set, every bit that is 0 in
 * fixed1 must be clear -> (cr | fixed0) & fixed1 (SDM Vol 3C, App A.7/
 * A.8). Applied before VMXON so a required bit like CR0.NE can't be the
 * cause of a #GP. Pure bit-manipulation. */
uint64_t hype_vmx_cr_with_fixed_bits(uint64_t cr, uint64_t fixed0, uint64_t fixed1);

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
