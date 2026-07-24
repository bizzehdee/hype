#ifndef HYPE_ARCH_VMX_VMCS_H
#define HYPE_ARCH_VMX_VMCS_H

#include <stdint.h>

#include "../../../devices/pci.h"
#include "../../../devices/pflash.h"
#include "../../../devices/pic.h"
#include "../../../devices/pit.h"
#include "../cpu/vmm_ops.h"
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
 * Allocates a 4KB-aligned VMCS region and builds a launchable VMCS (M2-8) for
 * a single real-mode-like guest entering at cs_base + rip, stack stack_phys,
 * with EPT pointer eptp -- "unrestricted guest" + "enable EPT" (per plan.md
 * §4) so the guest can run with paging/protection disabled. Host state is
 * captured from whatever's current when this runs (this project's own
 * GDT/IDT/CR0/CR3/CR4, per M1-2/M1-3); HOST_RIP/HOST_RSP are placeholders that
 * hype_vmx_vcpu_run()'s trampoline overrides on every VM-entry.
 *
 * Returns 0 on success (VMCLEAR/VMPTRLD and every VMWRITE succeeded),
 * non-zero otherwise. Exempt from unit testing per AGENTS.md -- real
 * VMCLEAR/VMPTRLD/VMWRITE, nothing to observe without a real CPU.
 * hype_vmx_adjust_controls() in vmx_bits.c holds the only real logic
 * (capability negotiation) and is fully tested.
 */
int hype_vmx_vmcs_build_guest(uint64_t cs_base, uint64_t rip, uint64_t stack_phys, uint64_t eptp);

/* Long-mode variant: flat 64-bit guest at linear entry_rip with paging root
 * guest_cr3 (the caller builds guest paging, as the SVM microtests do). */
int hype_vmx_vmcs_build_long_mode_guest(uint64_t entry_rip, uint64_t guest_cr3, uint64_t stack_phys,
                                        uint64_t eptp);

/* Assembles an EPT pointer (WB, 4-level) from a PML4 physical address. */
uint64_t hype_vmx_make_eptp(uint64_t pml4_phys);

/* Punch a 2MB MMIO hole in the internal identity EPT (call after
 * vcpu_create_long_mode) so a guest access to `gpa` causes an EPT violation. */
void hype_vmx_ept_mark_mmio_hole(uint64_t gpa);

/*
 * VMX vcpu_create/vcpu_run (M2-8, VMX-1) -- the hype_vmm_ops_t hooks. create
 * builds an identity EPT + launchable VMCS for a real-mode guest at guest_rip
 * (stack guest_rsp) and returns the vCPU context; run enters via the
 * VMLAUNCH/VMRESUME trampoline (vmx_run.S) and fills *info from the VMCS on
 * exit. Exempt from unit testing (real VMX instructions). See vmcs_hw.c.
 */
hype_vcpu_ctx_t *hype_vmx_vcpu_create(uint64_t guest_rip, uint64_t guest_rsp,
                                      uint64_t ept_or_npt_root);
/* Long-mode vCPU create (VMX mirror of hype_svm_vcpu_create_long_mode), used by
 * the M2-M4-5 microtests: flat 64-bit guest at entry_rip with guest_cr3. */
hype_vcpu_ctx_t *hype_vmx_vcpu_create_long_mode(uint64_t entry_rip, uint64_t guest_cr3,
                                                uint64_t guest_rsp, uint64_t ept_or_npt_root);
int hype_vmx_vcpu_run(hype_vcpu_ctx_t *ctx, hype_vmexit_info_t *info);

/* VMX exit handlers (VMX-2), mirrors of the SVM ones: emulate CPUID / MSR
 * against the guest GPRs in ctx (+ the VMCS for guest EFER) and advance guest
 * RIP. handle_msr's is_write distinguishes WRMSR (exit reason 32) from RDMSR
 * (31); returns 0 if handled, -1 to reject. */
void hype_vmx_vcpu_handle_cpuid(hype_vcpu_ctx_t *ctx);
int hype_vmx_vcpu_handle_msr(hype_vcpu_ctx_t *ctx, int is_write);
/* set_rsi seeds guest RSI before entry (Linux zero-page ptr, m3-5). handle_ioio
 * emulates a port-I/O exit (reason 30) against the PIC/PIT models. */
void hype_vmx_vcpu_set_rsi(hype_vcpu_ctx_t *ctx, uint64_t rsi);
int hype_vmx_vcpu_handle_ioio(hype_vcpu_ctx_t *ctx, hype_pic_emu_t *pic, hype_pit_emu_t *pit);
/* MMIO via EPT violation (reason 48): decode the faulting instruction at guest
 * RIP and dispatch to the emulated pflash at [pf_base_phys, ...). */
int hype_vmx_vcpu_handle_pflash_npf(hype_vcpu_ctx_t *ctx, hype_pflash_t *pf, uint64_t pf_base_phys);
/* MMIO via EPT violation to the PCI ECAM window: decode at RIP, dispatch to
 * hype_pci_config_read/write. */
int hype_vmx_vcpu_handle_pci_ecam_npf(hype_vcpu_ctx_t *ctx, hype_pci_t *pci,
                                      uint64_t ecam_base_phys);

/*
 * VMX-1 smoke test: launches a self-contained 3-byte guest (CPUID; HLT) via
 * vcpu_create/vcpu_run and checks the CPUID->HLT VM-exit sequence. Returns 0
 * on the expected sequence, -1 otherwise. Validates the trampoline + VMCS +
 * EPT round trip on real VMX hardware, independent of the microtest ABI.
 */
int hype_vmx_smoke_test(void);

#endif /* HYPE_ARCH_VMX_VMCS_H */
