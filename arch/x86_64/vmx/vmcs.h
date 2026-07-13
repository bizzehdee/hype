#ifndef HYPE_ARCH_VMX_VMCS_H
#define HYPE_ARCH_VMX_VMCS_H

#include <stdint.h>

#include "vmcs_fields.h"

/*
 * UNVALIDATED (see vmx.h) -- and this specific file carries one more
 * layer of uncertainty on top of that: VMWRITE/VMREAD's AT&T-syntax
 * operand order was derived by reasoning from Intel's documented
 * Intel-syntax operand order (VMWRITE field, value -- field is the
 * dest-position operand, value is the src-position operand; AT&T
 * reverses that to `vmwrite value, field`) rather than confirmed by
 * assembling and disassembling against a documented opcode/ModRM
 * table, because there was no VMX hardware available in this project's
 * dev environment to cross-check against by actually executing it (see
 * vmcs_hw.c's comment at the vmwrite() helper for the full reasoning).
 * If M2-8's real Intel hardware validation shows VMWRITE calls
 * behaving as if the operands were swapped, that comment is the first
 * place to look.
 *
 * Allocates a 4KB-aligned VMCS region and builds a minimal VMCS for a
 * single real-mode-like guest starting at entry_seg:0 (same convention
 * as hype_vmcb_build_realmode_guest()) with stack stack_phys, using
 * "unrestricted guest" (a required VM-execution control per plan.md §4)
 * so the guest can run with paging/protection disabled. Host state is
 * captured from whatever's current when this runs (this project's own
 * GDT/IDT/CR0/CR3/CR4, per M1-2/M1-3) with a placeholder HOST_RIP that
 * just halts -- the real VM-exit entry point doesn't exist until M2-5's
 * dispatch loop lands, and wiring it in properly is that milestone's
 * job, not this one's.
 *
 * Returns 0 on success (VMCLEAR/VMPTRLD and every VMWRITE succeeded),
 * non-zero otherwise. Exempt from unit testing per AGENTS.md -- real
 * VMCLEAR/VMPTRLD/VMWRITE, nothing to observe without a real CPU.
 * hype_vmx_adjust_controls() in vmx_bits.c holds the only real logic
 * (capability negotiation) and is fully tested.
 */
int hype_vmx_vmcs_build_realmode_guest(uint16_t entry_seg, uint64_t stack_phys);

#endif /* HYPE_ARCH_VMX_VMCS_H */
