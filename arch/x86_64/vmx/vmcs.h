#ifndef HYPE_ARCH_VMX_VMCS_H
#define HYPE_ARCH_VMX_VMCS_H

#include <stdint.h>

#include "../../../core/blk_backend.h"
#include "../../../core/guest_mem.h" /* VMX-4: hype_gpa_map_t (set_pvclock) */
#include "../../../devices/ahci.h"
#include "../../../devices/atapi.h"
#include "../../../devices/bochs_vbe.h"
#include "../../../devices/fw_cfg.h"
#include "../../../devices/pci.h"
#include "../../../devices/cmos.h"
#include "../../../devices/guest_lapic.h"
#include "../../../devices/guest_uart.h"
#include "../../../devices/ioapic.h"
#include "../../../devices/pflash.h"
#include "../../../devices/virtio_blk.h"
#include "../../../devices/pic.h"
#include "../../../devices/pit.h"
#include "../../../devices/ps2_keyboard.h"
#include "../../../devices/ps2_mouse.h"
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
 *
 * `vpid` (#273) tags the guest's TLB entries; 0 leaves VPID disabled, and so
 * does any CPU whose IA32_VMX_EPT_VPID_CAP cannot also invalidate it.
 */
int hype_vmx_vmcs_build_guest(uint64_t cs_base, uint64_t rip, uint64_t stack_phys,
                              uint64_t eptp, uint8_t *vmcs_region, uint16_t vpid);

/* Long-mode variant: flat 64-bit guest at linear entry_rip with paging root
 * guest_cr3 (the caller builds guest paging, as the SVM microtests do). */
int hype_vmx_vmcs_build_long_mode_guest(uint64_t entry_rip, uint64_t guest_cr3, uint64_t stack_phys,
                                        uint64_t eptp, uint8_t *vmcs_region, uint16_t vpid);

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
/* fw_cfg IOIO (DMA interface): select/data/DMA ports (0x510/0x511/0x514/0x518). */
int hype_vmx_vcpu_handle_fw_cfg_ioio(hype_vcpu_ctx_t *ctx, hype_fw_cfg_t *fw,
                                     const hype_gpa_map_t *dma_map);
/* PS/2 keyboard (+ mouse) IOIO on 0x60/0x64, and PIC-acknowledged IRQ delivery. */
int hype_vmx_vcpu_handle_ps2_kbd_ioio(hype_vcpu_ctx_t *ctx, hype_ps2_kbd_t *kbd);
int hype_vmx_vcpu_handle_ps2_ioio(hype_vcpu_ctx_t *ctx, hype_ps2_kbd_t *kbd, hype_ps2_mouse_t *mouse,
                                  int *out_kbd_wait);
void hype_vmx_vcpu_deliver_pic_irq(hype_vcpu_ctx_t *ctx, hype_pic_emu_chip_t *chip, uint8_t irq);
/* MMIO via EPT violation (reason 48): decode the faulting instruction at guest
 * RIP and dispatch to the emulated pflash at [pf_base_phys, ...). */
int hype_vmx_vcpu_handle_pflash_npf(hype_vcpu_ctx_t *ctx, hype_pflash_t *pf, uint64_t pf_base_phys);
/* MMIO via EPT violation to the PCI ECAM window: decode at RIP, dispatch to
 * hype_pci_config_read/write. */
int hype_vmx_vcpu_handle_pci_ecam_npf(hype_vcpu_ctx_t *ctx, hype_pci_t *pci,
                                      uint64_t ecam_base_phys);
/* MMIO via EPT violation to the AHCI HBA: decode at RIP, dispatch to the AHCI
 * register model, and run issued command slots (PxCI write) via the shared
 * process_ahci_command_slot(). */
int hype_vmx_vcpu_handle_ahci_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_atapi_t *atapi,
                                  uint64_t ahci_base_phys);
/* MMIO via EPT violation to a SATA disk behind AHCI, the Bochs VBE display, and
 * the virtio-blk BAR -- each decode-at-RIP then dispatch to its device model. */
int hype_vmx_vcpu_handle_ahci_disk_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci,
                                       hype_ata_disk_t *disk, uint64_t ahci_base_phys);

/*
 * #262 slice 3: remapped-guest variant. `dma_map` translates every guest-physical
 * address the command carries and `guest_insn_bytes` supplies already-fetched
 * instruction bytes, so nothing dereferences a guest RIP as a host pointer. Mirrors
 * hype_vmx_vcpu_handle_ahci_npf_map for the ATAPI controller, and the SVM twin.
 */
int hype_vmx_vcpu_handle_ahci_disk_npf_map(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci,
                                           hype_ata_disk_t *disk, uint64_t ahci_base_phys,
                                           const hype_gpa_map_t *dma_map,
                                           const uint8_t *guest_insn_bytes);
int hype_vmx_vcpu_handle_bochs_vbe_npf(hype_vcpu_ctx_t *ctx, hype_bochs_vbe_t *dev,
                                       uint64_t mmio_base_phys);
int hype_vmx_vcpu_handle_virtio_blk_npf(hype_vcpu_ctx_t *ctx, hype_virtio_blk_t *dev,
                                        const hype_blk_backend_t *be, uint64_t mmio_base_phys);
/* Guest interrupt delivery (VMX-2, INT-1/INT-2): set guest GDTR/IDTR, request a
 * vector (deferred via interrupt-window exiting), inject it when the window
 * fires (reason 7 -> handle_intr_window). Mirrors the SVM EVENTINJ/VINTR path. */
void hype_vmx_vcpu_set_gdt(hype_vcpu_ctx_t *ctx, uint64_t base, uint16_t limit);
void hype_vmx_vcpu_set_idt(hype_vcpu_ctx_t *ctx, uint64_t base, uint16_t limit);
void hype_vmx_vcpu_request_interrupt(hype_vcpu_ctx_t *ctx, uint8_t vector);
void hype_vmx_vcpu_handle_intr_window(hype_vcpu_ctx_t *ctx);

/* #271: release every vCPU slot. Mirrors hype_svm_vcpu_pool_reset() -- call between
 * the sequential battery and the concurrent guests, never while a vCPU is live. */
void hype_vmx_vcpu_pool_reset(void);

/* #248: the VMX half of the interrupt-delivery counters. Same field meanings as
 * hype_svm_vcpu_get_int_diag() so the INTDIAG log line reads identically on both
 * vendors; without these the line printed all-zeros on Intel regardless. */
void hype_vmx_vcpu_get_int_diag(unsigned long long *eventinj, unsigned long long *defer,
                                unsigned long long *window, unsigned long long *overwrite);
unsigned long long hype_vmx_vcpu_get_eventinj_collisions(void);

/*
 * VMX-1 smoke test: launches a self-contained 3-byte guest (CPUID; HLT) via
 * vcpu_create/vcpu_run and checks the CPUID->HLT VM-exit sequence. Returns 0
 * on the expected sequence, -1 otherwise. Validates the trampoline + VMCS +
 * EPT round trip on real VMX hardware, independent of the microtest ABI.
 */
int hype_vmx_smoke_test(void);

/*
 * VMX-4 (#236): vCPU state accessors the FW-1 live-guest loop needs. Each is
 * the counterpart of the identically-named hype_svm_vcpu_* function and is
 * reached through the vmm_* shims in boot/main.c; see vmcs_hw.c for the
 * VMCS-field mapping and for where VMX and SVM genuinely differ (pause-filter
 * units, exception error codes, activity state, absent Decode Assist).
 */
uint64_t hype_vmx_vcpu_get_cr3(hype_vcpu_ctx_t *ctx);
void hype_vmx_vcpu_set_rip(hype_vcpu_ctx_t *ctx, uint64_t rip);
const uint8_t *hype_vmx_vcpu_guest_insn_bytes(hype_vcpu_ctx_t *ctx, uint8_t *out_num);
void hype_vmx_vcpu_get_last_npf(hype_vcpu_ctx_t *ctx, hype_vmm_npf_t *out);
void hype_vmx_vcpu_peek_ioio(hype_vcpu_ctx_t *ctx, hype_vmm_ioio_t *out);
void hype_vmx_vcpu_handle_unknown_ioio(hype_vcpu_ctx_t *ctx, hype_vmm_ioio_t *out);
void hype_vmx_vcpu_set_exception_intercepts(hype_vcpu_ctx_t *ctx, uint32_t mask);
void hype_vmx_vcpu_enable_intr_intercept(hype_vcpu_ctx_t *ctx);
void hype_vmx_vcpu_enable_pause_filter(hype_vcpu_ctx_t *ctx, uint16_t count, uint16_t threshold);
void hype_vmx_vcpu_reinject_exception(hype_vcpu_ctx_t *ctx, uint8_t vector, int has_error_code,
                                      uint32_t error_code);
void hype_vmx_vcpu_wake_hlt(hype_vcpu_ctx_t *ctx);
void hype_vmx_vcpu_get_intr_state(hype_vcpu_ctx_t *ctx, hype_vmm_intr_state_t *out);
int hype_vmx_vcpu_deliver_pending_if_ready(hype_vcpu_ctx_t *ctx);
void hype_vmx_vcpu_set_pvclock(hype_vcpu_ctx_t *ctx, const hype_gpa_map_t *map, uint64_t tsc_hz);

/* VMX-4 (#236): FW-1 device adapters. Unlike the microtest handlers above,
 * the MMIO ones take the faulting instruction's bytes as a parameter -- FW-1
 * remaps guest RAM away from identity, so GUEST_RIP is not a host pointer
 * there and the caller resolves it via its own page-table walk. */
int hype_vmx_vcpu_handle_lapic_npf(hype_vcpu_ctx_t *ctx, hype_guest_lapic_t *lapic,
                                   uint64_t lapic_base_phys, const uint8_t *guest_insn_bytes);
int hype_vmx_vcpu_handle_ioapic_npf(hype_vcpu_ctx_t *ctx, hype_ioapic_t *ioapic,
                                    uint64_t ioapic_base_phys, const uint8_t *guest_insn_bytes);
int hype_vmx_vcpu_handle_ahci_npf_map(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_atapi_t *atapi,
                                      uint64_t ahci_base_phys, const hype_gpa_map_t *dma_map,
                                      const uint8_t *guest_insn_bytes);
int hype_vmx_vcpu_absorb_mmio_npf(hype_vcpu_ctx_t *ctx, const uint8_t *guest_insn_bytes);
int hype_vmx_vcpu_handle_uart_ioio(hype_vcpu_ctx_t *ctx, hype_guest_uart_t *uart, uint16_t base_port);
int hype_vmx_vcpu_handle_cmos_ioio(hype_vcpu_ctx_t *ctx, hype_cmos_t *cmos);
int hype_vmx_vcpu_handle_pm1_cnt_ioio(hype_vcpu_ctx_t *ctx, uint16_t port, uint16_t *value,
                                      int *slp_en);
int hype_vmx_vcpu_handle_pci_cf8_ioio(hype_vcpu_ctx_t *ctx, hype_pci_t *pci);
int hype_vmx_vcpu_handle_debug_port_ioio(hype_vcpu_ctx_t *ctx, uint16_t base_port,
                                         uint8_t *out_byte);
int hype_vmx_vcpu_handle_acpi_pm_timer_ioio(hype_vcpu_ctx_t *ctx);

int hype_vmx_vcpu_exit_exception_vector(hype_vcpu_ctx_t *ctx);
uint32_t hype_vmx_vcpu_exit_exception_error_code(hype_vcpu_ctx_t *ctx);

int hype_vmx_vcpu_handle_virtio_blk_npf_map(hype_vcpu_ctx_t *ctx, hype_virtio_blk_t *dev,
                                            const hype_blk_backend_t *be,
                                            const hype_gpa_map_t *dma_map, uint64_t mmio_base_phys,
                                            const uint8_t *guest_insn_bytes);
int hype_vmx_vcpu_handle_pci_ecam_npf_insn(hype_vcpu_ctx_t *ctx, hype_pci_t *pci,
                                           uint64_t ecam_base_phys,
                                           const uint8_t *guest_insn_bytes);

void hype_vmx_vcpu_reset_realmode(hype_vcpu_ctx_t *ctx, uint64_t guest_rip, uint64_t guest_rsp,
                                  uint64_t ept_root);

uint32_t hype_vmx_vcpu_get_msr_index(hype_vcpu_ctx_t *ctx);

/*
 * #236/#248 probe: everything needed to explain a guest #GP on MOV to CR0/CR4.
 *
 * In VMX non-root operation a guest write to CR0/CR4 that violates the fixed-bit
 * MSRs raises #GP(0). IA32_VMX_CR4_FIXED0 requires CR4.VMXE (bit 13) on this
 * hardware, but real firmware does not know it is virtualised and writes CR4
 * with VMXE clear -- so unless the host *owns* that bit via
 * CR4_GUEST_HOST_MASK (keeping VMXE set in GUEST_CR4 while CR4_READ_SHADOW
 * reports it clear to the guest), the write faults.
 *
 * `attempted` is the value the guest was loading (from the GPR the instruction
 * names), which is the field that makes the fault self-explanatory: compare it
 * against cr4_fixed0 and the mask.
 */
typedef struct {
    uint64_t attempted; /* value the guest was loading into the CR */
    uint64_t guest_cr0;
    uint64_t guest_cr4;
    uint64_t cr0_mask; /* CR0_GUEST_HOST_MASK: 1 = host owns the bit */
    uint64_t cr4_mask; /* CR4_GUEST_HOST_MASK */
    uint64_t cr0_shadow; /* CR0_READ_SHADOW: what the guest sees */
    uint64_t cr4_shadow; /* CR4_READ_SHADOW */
    uint64_t cr0_fixed0; /* bits that MUST be 1 in VMX operation */
    uint64_t cr0_fixed1; /* bits that MAY be 1 */
    uint64_t cr4_fixed0;
    uint64_t cr4_fixed1;
} hype_vmx_cr_diag_t;

/* `gpr` selects which of ctx->gprs[] supplied the value (0=RAX ... 15=R15), as
 * decoded from the faulting instruction's ModRM r/m field. */
void hype_vmx_vcpu_get_cr_diag(hype_vcpu_ctx_t *ctx, unsigned gpr, hype_vmx_cr_diag_t *out);

/*
 * #248: service a control-register access exit for a MOV to CR0/CR4, keeping the
 * bits the fixed-bit MSRs require set in GUEST_CR* while the read shadow reports
 * the guest's own value back to it. Advances guest RIP past the instruction.
 *
 * Returns 1 if the access was emulated, 0 if it was not (a CR number or access
 * type this does not model) -- in which case the caller must NOT treat the exit
 * as handled, because RIP has deliberately not been advanced.
 */
int hype_vmx_vcpu_handle_cr_access(hype_vcpu_ctx_t *ctx);

/*
 * #248/#236 probe: dump the raw VMCS state behind an EPT violation, so an
 * unexplained one can be told apart from a misattributed exit without guessing.
 *
 * Prints the exit reason and qualification as the CPU reported them, the faulting
 * GPA and guest-linear address, GUEST_RIP, the guest's paging registers, and the
 * VM-entry controls + EFER (which is where a real->protected->long mode
 * transition goes wrong if the VMCS is not kept in step with the guest's own
 * CR0.PG/CR4.PAE writes).
 */
void hype_vmx_vcpu_dump_ept_violation(hype_vcpu_ctx_t *ctx);

/* #251: dump the last few guest CPUIDs (leaf/subleaf and what hype answered).
 * Called when the guest is detected idle, which for the Intel guest means parked
 * in a hlt;jmp loop a few instructions past a CPUID enumeration. */
void hype_vmx_vcpu_dump_cpuid_ring(void);

/* #251: emulate a guest XSETBV (exit reason 55, unconditional on VMX). Returns 0
 * if emulated, -1 for a form this does not model -- in which case RIP has NOT
 * been advanced and the caller must not treat the exit as handled. */
int hype_vmx_vcpu_handle_xsetbv(hype_vcpu_ctx_t *ctx);

#endif /* HYPE_ARCH_VMX_VMCS_H */
