#ifndef HYPE_ARCH_SVM_H
#define HYPE_ARCH_SVM_H

#include <stdint.h>

#include "../cpu/vmm_ops.h"
#include "../cpu/mmio_decode.h"
#include "../../../devices/pic.h"
#include "../../../devices/pit.h"
#include "../../../devices/pflash.h"
#include "../../../devices/fw_cfg.h"
#include "../../../devices/ahci.h"
#include "../../../devices/atapi.h"
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

/*
 * Handles an NPF (M4-3) VM-exit against `pf`, an emulated CFI flash
 * device (devices/pflash.h) mapped starting at guest-physical address
 * `pf_base_phys` and previously marked not-present via
 * hype_npt_mark_not_present() (arch/x86_64/svm/npt.h) -- the only way
 * this project ever produces an NPF in the first place. Decodes
 * EXITINFO1/EXITINFO2 (hype_svm_decode_npf_info()) for the fault
 * direction/address, then decodes the faulting instruction by reading
 * its raw bytes directly out of guest memory at vmcb->save.rip (a
 * plain host pointer dereference -- this project's guest/NPT setup is
 * a flat identity map, so no translation is needed; AMD's Decode
 * Assist, the VMCB's num_bytes_fetched/guest_instruction_bytes fields,
 * was the original plan, but confirmed empirically NOT reliably
 * populated under nested SVM even when the host CPU's own CPUID
 * advertises the feature -- see M4-3's commit) via hype_mmio_decode()
 * to determine which register carries the value and how wide the
 * access is, then dispatches to hype_pflash_read()/hype_pflash_write().
 * A read's result is merged back into the destination register
 * (hype_mmio_merge_read_value()); a write's source register is
 * extracted the same width-aware way (hype_mmio_extract_write_value())
 * -- RAX is read/written via vmcb->save.rax (the one GPR VMRUN itself
 * manages); every other register (RSP excepted -- never a valid MMIO
 * accessor register) via this backend's own post-VMRUN GPR capture.
 * Advances the guest's RIP past the decoded instruction using the
 * decoder's own computed instr_len. Returns 0 if the fault was against
 * `pf`'s own range and the instruction was a recognized form, non-zero
 * otherwise (the caller's job to treat as fatal -- silently guessing
 * at an unrecognized MMIO access is not safe, matching this project's
 * IOIO handler's own fail-closed convention). Exempt from unit testing
 * -- reaches into the exempt VMCB fields this backend's real VMRUN
 * produces; hype_svm_decode_npf_info(), hype_mmio_decode(),
 * hype_mmio_merge_read_value(), hype_mmio_extract_write_value(), and
 * devices/pflash.h's own read/write are all already fully tested in
 * isolation.
 */
int hype_svm_vcpu_handle_npf(hype_vcpu_ctx_t *ctx, hype_pflash_t *pf, uint64_t pf_base_phys);

/*
 * Handles an IOIO VM-exit against `fw`, a fw_cfg device model
 * (devices/fw_cfg.h) -- how this project's own synthesized ACPI
 * content (devices/acpi.h/acpi_loader.h) reaches the guest's real,
 * vendored OVMF firmware (M4-4). Decodes EXITINFO1
 * (hype_svm_decode_ioio_info1()) and dispatches four ports: 0x510
 * (16-bit OUT, select) and 0x511 (8-bit IN, next byte) drive
 * hype_fw_cfg_select()/hype_fw_cfg_read_byte() directly from/into the
 * guest's RAX; 0x514 and 0x518 (32-bit OUT each) drive the DMA
 * interface (hype_fw_cfg_dma_addr_high()/_low()) -- the 0x518 write is
 * what triggers the actual transfer, per fw_cfg's own protocol, so
 * this function then reads the 16-byte access struct directly out of
 * guest memory at the address hype_fw_cfg_dma_addr_low() returns (a
 * plain host pointer dereference, same flat-identity-map reasoning as
 * hype_svm_vcpu_handle_npf()'s own instruction-byte fetch), decodes it
 * (hype_fw_cfg_dma_decode()), resolves its own `address` field into
 * another guest pointer for the actual data transfer, calls
 * hype_fw_cfg_dma_execute(), and writes the result back into the
 * access struct's Control field (big-endian, matching what OVMF's own
 * polling loop expects). Returns 0 for a recognized port, non-zero for
 * any other port or an IN issued against an OUT-only port (or vice
 * versa) -- the caller's job to treat as fatal, matching this
 * project's other IOIO/NPF handlers' fail-closed convention. Advances
 * the guest's RIP to EXITINFO2 on success, same "next-RIP-for-free"
 * convenience as hype_svm_vcpu_handle_ioio(). Exempt from unit testing
 * -- reaches into the exempt VMCB fields this backend's real VMRUN
 * produces and does its own raw guest-memory access; every
 * hype_fw_cfg_*() call this dispatches to is already fully tested in
 * isolation.
 */
int hype_svm_vcpu_handle_fw_cfg_ioio(hype_vcpu_ctx_t *ctx, hype_fw_cfg_t *fw);

/*
 * Handles an NPF (M4-5) VM-exit against `ahci`, a single-port AHCI HBA
 * model (devices/ahci.h) with one ATAPI CD-ROM device (`atapi`,
 * devices/atapi.h) attached, mapped starting at guest-physical address
 * `ahci_base_phys` and previously marked not-present via
 * hype_npt_mark_not_present() -- same MMIO-trap mechanism as
 * hype_svm_vcpu_handle_npf() (M4-3), reusing the same instruction
 * decode (hype_mmio_decode(), reading the faulting instruction
 * directly out of guest memory at RIP) since AHCI registers are
 * accessed via ordinary 32-bit MOV instructions just like pflash's.
 * On an MMIO write that lands on PxCI (Command Issue) and results in
 * bit 0 being set, additionally processes command slot 0 synchronously
 * (this project's own single-outstanding-command scope): walks the
 * guest's Command List/Command Table/PRDT (raw guest-memory reads, the
 * same flat-identity-map pointer dereferences every other exempt
 * handler here already relies on), extracts the 16-byte ATAPI CDB,
 * dispatches it via hype_atapi_execute_cdb(), copies the response into
 * the PRDT-described guest buffer(s) (or does nothing for a
 * data-less command like TEST UNIT READY), updates PxTFD and the
 * Received FIS's D2H Register FIS, and clears PxCI's bit 0 -- a
 * polling guest driver (this project's own validated pattern, matching
 * fw_cfg's DMA test and real UEFI AHCI drivers' typical early-boot
 * behavior before interrupts are set up) observes completion by
 * re-reading PxCI. Returns 0 for a recognized access, non-zero
 * otherwise (the caller's job to treat as fatal, matching this
 * project's other MMIO handlers' fail-closed convention). Exempt from
 * unit testing -- reaches into the exempt VMCB fields this backend's
 * real VMRUN produces and does its own raw guest-memory access; every
 * hype_ahci_*()/hype_atapi_*() call this dispatches to is already
 * fully tested in isolation.
 */
int hype_svm_vcpu_handle_ahci_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_atapi_t *atapi,
                                   uint64_t ahci_base_phys);

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
