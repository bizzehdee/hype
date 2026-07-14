#ifndef HYPE_ARCH_SVM_H
#define HYPE_ARCH_SVM_H

#include <stdint.h>

#include "../cpu/vmm_ops.h"
#include "../../../devices/pic.h"
#include "../../../devices/pit.h"
#include "vmcb.h"

/*
 * AMD-V/SVM backend (M2, plan.md §4/§9). Validated in this project's
 * dev environment: nested SVM is genuinely available here (confirmed
 * via a standalone CPUID probe under `-enable-kvm -cpu host` before
 * writing any of this -- see M2-1's commit), so this backend gets real
 * QEMU validation, not just careful SDM cross-referencing.
 */

#define HYPE_MSR_EFER 0xC0000080u
#define HYPE_EFER_SVME (1ULL << 12)
#define HYPE_MSR_VM_HSAVE_PA 0xC0010117u

/* Given the current EFER value, returns it with SVME (bit 12) set,
 * leaving every other bit (LME/LMA/NXE, ...) untouched. Pure
 * bit-manipulation -- the actual RDMSR/WRMSR round trip is the exempt
 * hardware shim in svm_enable_hw.c. */
uint64_t hype_svm_efer_with_svme(uint64_t old_efer);

/*
 * Enables SVM on the calling physical CPU: sets EFER.SVME and points
 * VM_HSAVE_PA at a host save area. Returns 0 on success. Exempt from
 * unit testing per AGENTS.md -- real RDMSR/WRMSR, nothing to observe
 * without a real CPU; hype_svm_efer_with_svme() above holds the only
 * real logic and is fully tested.
 */
int hype_svm_enable(void);

/*
 * Enables AVIC on `vmcb` (M2-4) using this project's own statically-
 * reserved AVIC backing/logical/physical ID table pages and the
 * platform's real LAPIC MMIO base as the guest-visible APIC_BAR
 * (single-vCPU scope for now -- see vmcb.h's HYPE_SVM_INT_CTL_AVIC_ENABLE
 * comment for why `vmcb` must already have NP_ENABLE=1, and why this is
 * NOT called from hype_vmcb_build_realmode_guest()). Pure struct
 * mutation over fixed static-buffer addresses -- no privileged
 * instructions, so unlike most of this backend's "enable" code, this
 * is fully unit-testable.
 */
void hype_svm_vcpu_enable_apic_accel(hype_vmcb_t *vmcb);

/*
 * M2-7: creates this backend's (single, static -- M2's scope is one
 * vCPU; M8 is where real multi-vCPU allocation happens) vCPU context,
 * building a flat real-address-mode guest
 * (hype_vmcb_build_realmode_guest()) whose CS.base/SS.base point
 * directly at guest_rip/guest_rsp (RIP/RSP=0) -- guest_rip/guest_rsp
 * can be any address, unlike the classic entry_seg*16 real-mode
 * convention (see vmcb.h), which matters because a UEFI-loaded
 * hypervisor's own static buffers can end up anywhere firmware's PE
 * loader put them, nowhere near the first 1MB.
 * ept_or_npt_root (M3-1): 0 means no nested paging (M2's original
 * scope, still supported); a nonzero value is treated as an NPT root
 * physical address built by hype_npt_build_identity()
 * (arch/x86_64/svm/npt.h) and wired in via
 * hype_vmcb_enable_nested_paging(). Exempt from unit testing -- thin
 * wrapper around hype_vmcb_build_realmode_guest()/
 * hype_vmcb_enable_nested_paging() (both already tested) with no logic
 * of its own beyond the zero-check.
 */
hype_vcpu_ctx_t *hype_svm_vcpu_create(uint64_t guest_rip, uint64_t guest_rsp, uint64_t ept_or_npt_root);

/*
 * M3-5: creates this backend's (same single, static instance as
 * hype_svm_vcpu_create() -- calling one after the other simply
 * replaces the running test guest, which is fine, only one ever runs
 * at a time) vCPU context for a 64-bit long-mode guest matching the
 * Linux boot protocol (hype_vmcb_build_long_mode_guest()): RIP=
 * entry_rip, RSP=rsp, CR3=guest_cr3 (the guest's own identity page
 * tables, built by the caller via arch/x86_64/cpu/paging.h -- reused
 * directly, not NPT). npt_root has the same 0-means-disabled
 * convention as hype_svm_vcpu_create(). Exempt from unit testing --
 * thin wrapper around already-tested builders.
 */
hype_vcpu_ctx_t *hype_svm_vcpu_create_long_mode(uint64_t entry_rip, uint64_t guest_cr3, uint64_t rsp,
                                                 uint64_t npt_root);

/*
 * Sets the value RSI will hold at this vCPU's next VM-entry (M3-5) --
 * see vmcb.h's hype_vmcb_build_long_mode_guest() comment for why this
 * isn't a VMCB field. The Linux boot protocol requires RSI to hold the
 * zero page's guest-physical address at 64-bit entry. Exempt from unit
 * testing -- trivial state mutation feeding directly into the exempt
 * hype_svm_vcpu_run()'s inline asm, nothing meaningful to observe
 * without executing VMRUN.
 */
void hype_svm_vcpu_set_rsi(hype_vcpu_ctx_t *ctx, uint64_t rsi);

/*
 * Handles an IOIO (M3-5) VM-exit: decodes EXITINFO1
 * (hype_svm_decode_ioio_info1()), routes the port to `pic` (0x20/0x21/
 * 0xA0/0xA1) or `pit` (0x40-0x43), reads/writes the emulated device
 * accordingly (patching the low byte of the guest's RAX for an IN),
 * and advances the guest's RIP to EXITINFO2 (the instruction after the
 * IN/OUT) on success. Returns 0 if the port was recognized and
 * handled, non-zero for any other port (the caller's job to treat as
 * fatal -- no guest is ever allowed direct hardware access, AGENTS.md,
 * so an unrecognized port is not silently ignored). Exempt from unit
 * testing -- reaches into the exempt VMCB fields this backend's real
 * VMRUN produces; hype_svm_decode_ioio_info1() and every
 * hype_pic_emu_io_*()/hype_pit_emu_io_*() call this dispatches to are
 * already fully tested in isolation.
 */
int hype_svm_vcpu_handle_ioio(hype_vcpu_ctx_t *ctx, hype_pic_emu_t *pic, hype_pit_emu_t *pit);

/* Adapts hype_svm_vcpu_enable_apic_accel() to the hype_vmm_ops_t
 * vcpu_enable_apic_accel signature. */
void hype_svm_vcpu_enable_apic_accel_ops(hype_vcpu_ctx_t *ctx);

/*
 * Runs the vCPU until the next #VMEXIT: CLGI (blocks host interrupt
 * recognition across the transition -- the standard convention every
 * real SVM hypervisor follows, so a host interrupt can never land
 * mid-transition) / VMLOAD (loads FS/GS/TR/LDTR hidden state and the
 * SYSCALL/SYSENTER MSRs, which VMRUN itself does not restore) / VMRUN
 * / VMSAVE (saves them back) / STGI. Returns 0 on a normal exit,
 * non-zero if VMRUN itself reports an invalid VMCB
 * (HYPE_SVM_EXITCODE_INVALID). Exempt from unit testing per AGENTS.md
 * -- real privileged instructions, nothing to observe without a real
 * CPU; and unlike the rest of this backend's exempt code, this is the
 * one piece that gets real QEMU/KVM validation (M2-1's confirmed
 * nested-SVM probe) rather than SDM-reading alone.
 */
int hype_svm_vcpu_run(hype_vcpu_ctx_t *ctx, hype_vmexit_info_t *info);

extern const hype_vmm_ops_t hype_svm_ops;

#endif /* HYPE_ARCH_SVM_H */
