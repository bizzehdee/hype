#ifndef HYPE_ARCH_SVM_H
#define HYPE_ARCH_SVM_H

#include <stdint.h>

#include "../cpu/vmm_ops.h"
#include "../cpu/mmio_decode.h"
#include "../cpu/cpuid_emulate.h"
#include "../cpu/msr_emulate.h"
#include "../../../devices/pic.h"
#include "../../../devices/pit.h"
#include "../../../devices/pflash.h"
#include "../../../devices/fw_cfg.h"
#include "../../../devices/ahci.h"
#include "../../../devices/atapi.h"
#include "../../../devices/pci.h"
#include "../../../devices/cmos.h"
#include "../../../devices/ps2_keyboard.h"
#include "../../../devices/ps2_mouse.h"
#include "../../../devices/bochs_vbe.h"
#include "../../../devices/virtio_blk.h"
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

/* VM_CR (AMD SDM Vol 2, 15.31): bit 4 (SVMDIS) can be set by firmware
 * to lock SVM off independently of the "SVM enabled" BIOS toggle the
 * user sees -- if set, the EFER.SVME WRMSR in hype_svm_enable() below
 * takes a #GP. Read-only diagnostic for real-hardware bring-up
 * (svm_enable_hw.c prints it before touching EFER); not otherwise
 * acted on here. */
#define HYPE_MSR_VM_CR 0xC0010114u
#define HYPE_MSR_VM_CR_SVMDIS (1ULL << 4)

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
 * INT-1/INT-2: overrides this vCPU's IDTR/GDTR (hype_vmcb_build_long_mode_guest()'s
 * own default of base=0/limit=0xFFFF has no real descriptor table
 * behind it -- fine for every long-mode test guest so far, none of
 * which ever took a real hardware-validated segment/gate reload, but
 * genuine interrupt injection does exactly that: hardware loads the
 * IDT gate from guest memory at IDTR.base, then validates/loads CS
 * from a real descriptor at GDTR.base -- both must point at
 * host-pre-populated, real descriptor tables for injection to actually
 * succeed rather than fault). Exempt from unit testing -- trivial
 * state mutation, nothing meaningful to observe without executing
 * VMRUN, same reasoning as hype_svm_vcpu_set_rsi().
 */
void hype_svm_vcpu_set_idt(hype_vcpu_ctx_t *ctx, uint64_t base, uint16_t limit);
void hype_svm_vcpu_set_gdt(hype_vcpu_ctx_t *ctx, uint64_t base, uint16_t limit);

/*
 * INT-1/INT-2: overrides this vCPU's CS/SS *selector* fields
 * (hype_vmcb_build_long_mode_guest()'s own default of 0 for every
 * segment -- fine for VMRUN's own state load, which sets segment
 * attributes directly from the VMCB without any descriptor-table
 * validation, but a real hardware interrupt/exception delivery pushes
 * the CPU's *current* CS (and, in 64-bit mode, always SS too --
 * unlike 32-bit mode, x86-64 IRETQ's own stack frame always includes
 * RSP/SS regardless of any privilege-level change) onto the stack,
 * and IRETQ later pops and *genuinely reloads* both from those exact
 * values -- reloading the null selector (0) is architecturally invalid
 * for CS/SS and raises #GP, confirmed the hard way: this project's own
 * first real interrupt-delivery test triple-faulted (SHUTDOWN) right
 * at its own IRETQ before this existed). Must match real, present
 * descriptors in whatever hype_svm_vcpu_set_gdt() pointed GDTR at.
 * Exempt from unit testing, same reasoning as hype_svm_vcpu_set_idt()/
 * _set_gdt().
 */
void hype_svm_vcpu_set_cs_ss_selectors(hype_vcpu_ctx_t *ctx, uint16_t cs_selector, uint16_t ss_selector);

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
 * Handles a CPUID (CPUMSR-1) VM-exit: reads the guest's requested
 * leaf/subleaf from RAX/RCX, executes the real `cpuid` instruction for
 * that same leaf/subleaf (a plain hardware read -- not guest-visible
 * until hype_cpuid_emulate() has curated it), calls
 * hype_cpuid_emulate() (arch/x86_64/cpu/cpuid_emulate.h) to decide what
 * the guest should actually see, writes the result into RAX/RBX/RCX/
 * RDX (zero-extending each to the full 64-bit register, matching real
 * CPUID's own architectural behavior in 64-bit mode), and advances RIP
 * by 2 (CPUID's own fixed instruction length -- no ModRM/operand
 * decoding needed, unlike hype_svm_vcpu_handle_npf()'s MMIO path).
 * Always succeeds -- there is no "unrecognized CPUID leaf" failure
 * mode, hype_cpuid_emulate() has a safe all-zero fallback for
 * anything it doesn't explicitly handle. Exempt from unit testing --
 * reaches into the exempt VMCB/GPR fields this backend's real VMRUN
 * produces and executes a real CPUID instruction; hype_cpuid_emulate()
 * itself is already fully tested in isolation.
 */
void hype_svm_vcpu_handle_cpuid(hype_vcpu_ctx_t *ctx);

/*
 * FW-1 real-hardware/real-firmware debugging: decodes and returns the
 * most recent NPF's direction/faulting-guest-physical-address
 * (hype_svm_decode_npf_info(), arch/x86_64/svm/vmcb.h) without
 * dispatching to any specific device model -- for a guest this
 * project doesn't yet have a full device model for (real, unmodified
 * OVMF), knowing exactly *which* guest-physical address an
 * unhandled NPF targeted is the actual diagnostic that matters, the
 * same log-driven iteration this project has used for every other
 * real-hardware-facing surprise. Exempt from unit testing -- reaches
 * into the exempt VMCB fields this backend's real VMRUN produces;
 * hype_svm_decode_npf_info() itself is already fully tested in
 * isolation.
 */
void hype_svm_vcpu_get_last_npf(hype_vcpu_ctx_t *ctx, hype_svm_npf_t *out);

/*
 * FW-1: decodes an IOIO VM-exit this project has no specific device
 * model for (real, unmodified OVMF probes far more ports than
 * hype_svm_vcpu_handle_ioio()'s PIC/PIT allow-list covers) and gives it
 * a safe, generic default response instead of treating it as fatal --
 * an IN reads back all-1s (this project's established "absent device"
 * convention, matching devices/pci.h's own unbacked-config-space reads),
 * an OUT is silently dropped -- then advances RIP via EXITINFO2, the
 * same "next-RIP-for-free" convenience hype_svm_vcpu_handle_ioio()
 * itself already relies on. Returns the decoded port/direction/size via
 * `out` purely for the caller's own diagnostic logging. Exempt from unit
 * testing -- reaches into the exempt VMCB/GPR fields this backend's
 * real VMRUN produces; hype_svm_decode_ioio_info1() itself is already
 * fully tested in isolation.
 */
void hype_svm_vcpu_handle_unknown_ioio(hype_vcpu_ctx_t *ctx, hype_svm_ioio_t *out);

/*
 * FW-1: routes an IOIO VM-exit to `pci`'s legacy CF8/CFC config-space
 * ports (devices/pci.h's hype_pci_cf8_write()/_read()/
 * hype_cf8_config_read()/_write()) -- confirmed necessary the hard way:
 * without this, every guest read of the host bridge's PCI ID via these
 * ports silently absorbed as all-1s (hype_svm_vcpu_handle_unknown_ioio())
 * made this project's own vendored OVMF conclude it was running on
 * QEMU's "microvm" machine type (whose sentinel host-bridge device ID
 * happens to BE 0xFFFF), sending it down a completely wrong,
 * fw_cfg-FDT-based init path that eventually crashed. Returns 0 if the
 * port was 0xCF8 or 0xCFC-0xCFF (handled, RIP already advanced via
 * EXITINFO2) or -1 for any other port (caller falls through to its own
 * next handler in the chain, same composable-handler-chain shape as
 * hype_svm_vcpu_handle_ioio()). Width-aware (1/2/4-byte IN/OUT),
 * unlike hype_svm_vcpu_handle_ioio()'s PIC/PIT (always 8-bit) --
 * merges/extracts the guest's RAX the same way
 * hype_mmio_merge_read_value()/hype_mmio_extract_write_value() do for
 * MMIO. Exempt from unit testing -- reaches into the exempt VMCB/GPR
 * fields this backend's real VMRUN produces; every function in
 * devices/pci.h this composes is already fully tested in isolation.
 */
int hype_svm_vcpu_handle_pci_cf8_ioio(hype_vcpu_ctx_t *ctx, hype_pci_t *pci);

/*
 * FW-1: routes an IOIO VM-exit to `cmos`'s index/data ports (0x70/0x71,
 * devices/cmos.h) -- confirmed necessary via source-level investigation
 * of this project's own vendored OVMF
 * (edk2/OvmfPkg/Library/PlatformInitLib/MemDetect.c,
 * PlatformGetSystemMemorySizeBelow4gb()): without fw_cfg's "etc/e820"
 * describing a nonzero low-memory size, OVMF falls back to reading
 * CMOS registers 0x34/0x35 for the system's memory size. Returns 0 if
 * the port was 0x70 or 0x71 (handled, RIP already advanced via
 * EXITINFO2), or -1 for any other port, same composable-handler-chain
 * shape as hype_svm_vcpu_handle_ioio()/hype_svm_vcpu_handle_pci_cf8_ioio().
 * Always 1-byte width (CMOS's own real-hardware convention -- there is
 * no wider access form). Exempt from unit testing -- reaches into the
 * exempt VMCB/GPR fields this backend's real VMRUN produces; every
 * function in devices/cmos.h this composes is already fully tested in
 * isolation.
 */
int hype_svm_vcpu_handle_cmos_ioio(hype_vcpu_ctx_t *ctx, hype_cmos_t *cmos);

/*
 * INPUT-1: routes an IOIO VM-exit to `kbd`'s data/status-command ports
 * (0x60/0x64, devices/ps2_keyboard.h). Returns 0 if the port was one of
 * these two (handled, RIP already advanced via EXITINFO2), or -1 for
 * any other port, same composable-handler-chain shape as every other
 * IOIO handler here. Always 1-byte width (the real i8042's own only
 * access form). Exempt from unit testing -- reaches into the exempt
 * VMCB/GPR fields this backend's real VMRUN produces; every function
 * in devices/ps2_keyboard.h this composes is already fully tested in
 * isolation.
 */
int hype_svm_vcpu_handle_ps2_kbd_ioio(hype_vcpu_ctx_t *ctx, hype_ps2_kbd_t *kbd);

/*
 * INPUT-2: routes an IOIO VM-exit to ports 0x60/0x64 across BOTH the
 * keyboard and mouse channels sharing them -- unlike
 * hype_svm_vcpu_handle_ps2_kbd_ioio() (kept as-is for INPUT-1's own
 * keyboard-only guests), this handles a real controller's own
 * channel-routing behavior: a 0x60 write goes to the mouse instead of
 * the keyboard exactly when `kbd`'s own
 * hype_ps2_kbd_take_aux_data_write() says the last controller command
 * was 0xD4; a 0x60 read (and port 0x64's own status bits) prefers the
 * mouse's own pending byte over the keyboard's whenever both happen to
 * have one ready (devices/ps2_mouse.h's own hype_ps2_mouse_has_pending_byte()),
 * setting HYPE_PS2_STATUS_AUX_DATA to say so -- matching real
 * hardware's own single shared data path across two channels. Returns
 * 0 if the port was 0x60 or 0x64 (handled, RIP already advanced via
 * EXITINFO2), or -1 for any other port. Exempt from unit testing --
 * reaches into the exempt VMCB/GPR fields this backend's real VMRUN
 * produces; every function this composes (in both
 * devices/ps2_keyboard.h and devices/ps2_mouse.h) is already fully
 * tested in isolation.
 */
int hype_svm_vcpu_handle_ps2_ioio(hype_vcpu_ctx_t *ctx, hype_ps2_kbd_t *kbd, hype_ps2_mouse_t *mouse);

/*
 * FW-1: services the ACPI PM Timer's own I/O port (hardcoded to 0x608
 * -- OVMF's own fixed ICH9_PMBASE_VALUE(0x600) + ACPI_TIMER_OFFSET(8),
 * both compile-time constants in edk2/OvmfPkg/Include/OvmfPlatforms.h,
 * not guest-computed, confirmed via source and this project's own live
 * trace once the ICH9 LPC bridge's PMBA register was correctly
 * programmable). Returns a real, monotonically-increasing 24-bit
 * counter (real_rdtsc() masked to 24 bits -- this project's own FADT,
 * devices/acpi.c, never sets the TMR_VAL_EXT flag, so the guest itself
 * expects a 24-bit, not 32-bit, counter) -- OVMF's own AcpiTimerLib
 * uses this for calibration/stall loops, which an always-0xFFFFFFFF
 * absorbed default can never satisfy. Returns 0 if the port was 0x608
 * (handled), -1 otherwise, same composable-handler-chain shape as
 * every other IOIO handler here. Exempt from unit testing -- reaches
 * into the exempt VMCB/GPR fields this backend's real VMRUN produces
 * and executes a real RDTSC instruction, same reasoning as
 * hype_svm_vcpu_handle_msr()'s own TSC case.
 */
int hype_svm_vcpu_handle_acpi_pm_timer_ioio(hype_vcpu_ctx_t *ctx);

/*
 * INT-1/INT-2: the high-level API a device model calls to deliver an
 * interrupt `vector` to this guest -- "now, or as soon as it genuinely
 * can" per hype_svm_can_accept_interrupt(). If the guest can accept it
 * immediately, writes EVENTINJ directly via
 * hype_svm_encode_eventinj_intr() (delivered on the very next VMRUN).
 * Otherwise arms an interrupt-window request
 * (hype_svm_arm_vintr_request(), HYPE_SVM_INTERCEPT_VINTR) and remembers
 * `vector` so hype_svm_vcpu_handle_vintr_window() can actually inject it
 * once that window opens -- this project only ever has one interrupt
 * genuinely in flight at a time (matching its own current single-IRQ-
 * source scope; a real multi-device PIC's own IRR/priority logic is
 * devices/pic.h's job, not this function's), so a second call before
 * the first is delivered overwrites the pending vector rather than
 * queuing -- acceptable for now, revisit if/when multiple concurrent
 * pending interrupts are ever a real scenario. Exempt from unit testing
 * -- reaches into the exempt VMCB fields this backend's real VMRUN
 * produces; every pure function this composes
 * (hype_svm_can_accept_interrupt()/hype_svm_encode_eventinj_intr()/
 * hype_svm_arm_vintr_request()) is already fully tested in isolation.
 */
void hype_svm_vcpu_request_interrupt(hype_vcpu_ctx_t *ctx, uint8_t vector);

/*
 * INT-2: handles an EXITCODE_VINTR VM-exit -- disarms the window
 * request (hype_svm_disarm_vintr_request(),
 * ~HYPE_SVM_INTERCEPT_VINTR) and, if a vector is still pending from an
 * earlier hype_svm_vcpu_request_interrupt() call, retries delivery
 * (which must now succeed, since this exit firing at all means the
 * window hardware was waiting for has genuinely opened). Does not
 * touch RIP -- unlike HLT/CPUID/MSR/IOIO, VINTR isn't an instruction
 * boundary; hardware doesn't advance RIP for this exit (confirmed
 * against the AMD SDM's own SEV-ES Exitcodes table), so there is
 * nothing to skip past. Exempt from unit testing, same reasoning as
 * hype_svm_vcpu_request_interrupt().
 */
void hype_svm_vcpu_handle_vintr_window(hype_vcpu_ctx_t *ctx);

/*
 * INPUT-1: the reusable "a device wired to `chip` just raised `irq`"
 * entry point -- combines devices/pic.h's own real-hardware modeling
 * (hype_pic_emu_raise_irq() sets IRR; hype_pic_emu_acknowledge_highest_priority()
 * performs the INTA-cycle equivalent, moving the highest-priority
 * pending/unmasked IRQ from IRR to ISR and computing its real vector
 * from the chip's own ICW2-programmed offset) with
 * hype_svm_vcpu_request_interrupt() (INT-1/INT-2) to actually deliver
 * that vector to the guest, now or once it genuinely can accept it.
 * If nothing pending is currently unmasked (acknowledge finds
 * nothing), this is a no-op beyond raising IRR -- exactly matching
 * real hardware, where a masked IRQ simply waits until unmasked.
 * Every future PIC-routed device (PS/2 mouse, etc.) should reuse this
 * same entry point rather than re-deriving the raise+acknowledge+
 * inject sequence itself. Exempt from unit testing -- reaches into
 * the exempt VMCB fields hype_svm_vcpu_request_interrupt() itself
 * already does; devices/pic.h's own two functions this composes are
 * already fully tested in isolation.
 */
void hype_svm_vcpu_deliver_pic_irq(hype_vcpu_ctx_t *ctx, hype_pic_emu_chip_t *chip, uint8_t irq);

/*
 * FW-1 real-hardware/real-firmware debugging: a snapshot of the guest
 * state fields that matter for diagnosing a fault reported against
 * this project's own "guest_rip" alone -- once a real-mode guest
 * reloads CS (e.g. OVMF's own real-mode-to-protected-mode transition),
 * CS.base is no longer the fixed reset-vector value this project's own
 * CS.base trick relies on, and RIP alone stops meaning anything without
 * knowing what CS.base/CR0.PE actually are at that moment.
 */
typedef struct {
    uint16_t cs_selector;
    uint64_t cs_base;
    uint64_t cr0;
    uint64_t cr2; /* faulting linear address, meaningful only after a guest #PF (vector 14) */
    uint64_t cr3;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
} hype_svm_debug_state_t;

void hype_svm_vcpu_get_debug_state(hype_vcpu_ctx_t *ctx, hype_svm_debug_state_t *out);

/*
 * FW-1: overrides the RIP hype_svm_vcpu_create() otherwise hardcodes to
 * 0 (correct for every synthetic real-mode test guest so far, which are
 * free to assume RIP=0 as their own starting point). Real x86 hardware
 * reset state is CS.base=0xFFFF0000, RIP=0xFFF0 -- NOT CS.base=
 * 0xFFFFFFF0, RIP=0, despite both producing the identical initial
 * linear address (base+rip=0xFFFFFFF0). The difference matters the
 * instant the guest's own code executes a near jump with a *negative*
 * displacement (EDK2's own ResetVector.asm does exactly this, jumping
 * "backward" to an earlier label in the same 64KB page): starting from
 * RIP=0xFFF0, a small negative displacement stays comfortably positive;
 * starting from RIP=0 (this project's own prior convention), the exact
 * same displacement underflows 16-bit arithmetic and wraps to a huge
 * positive offset, exceeding the real-mode CS limit (0xFFFF) and
 * raising #GP -- confirmed empirically (FW-1: guest faulted at
 * rip=0x10000, exactly one past the 16-bit limit, with CS.base/CR0
 * otherwise unchanged, i.e. still on the very first instruction).
 * Callers needing genuine reset-vector semantics for real firmware must
 * pass guest_rip=0xFFFF0000 (not 0xFFFFFFF0) to hype_svm_vcpu_create()
 * and then call this to set rip=0xFFF0 afterward.
 */
void hype_svm_vcpu_set_rip(hype_vcpu_ctx_t *ctx, uint64_t rip);

/*
 * Handles an MSR (CPUMSR-2, RDMSR/WRMSR) VM-exit: decodes direction
 * from EXITINFO1 bit 0 (0=RDMSR, 1=WRMSR, per AMD SDM) and the MSR
 * number from RCX, calls hype_msr_decide() (arch/x86_64/cpu/
 * msr_emulate.h) to look it up on this project's small allow-list, and
 * either services it (APIC_BASE synthesized read, EFER routed to/from
 * the VMCB's own save.efer, TSC computed via a real RDTSC plus the
 * VMCB's own tsc_offset) or returns -1 for anything not on the
 * allow-list -- the caller's job to treat as fatal, matching every
 * other unrecognized-access convention here (IOIO/NPF/CPUID). On
 * success, advances RIP by 2 (RDMSR/WRMSR's own fixed instruction
 * length, same convenience hype_svm_vcpu_handle_cpuid() already
 * relies on). Exempt from unit testing -- reaches into the exempt
 * VMCB/GPR fields this backend's real VMRUN produces and executes a
 * real RDTSC instruction; hype_msr_decide()/hype_msr_apic_base_value()
 * are already fully tested in isolation. Returns 0 on success, -1 for
 * a rejected MSR.
 */
int hype_svm_vcpu_handle_msr(hype_vcpu_ctx_t *ctx);

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

/*
 * Handles an NPF (PCI-1) VM-exit against `pci`, this project's own
 * minimal ECAM-based PCI configuration-space model (devices/pci.h),
 * mapped starting at guest-physical address `ecam_base_phys` and
 * previously marked not-present via hype_npt_mark_not_present() --
 * same MMIO-trap mechanism as every other NPF handler here, reusing
 * the same instruction decode (hype_mmio_decode(), reading the
 * faulting instruction directly out of guest memory at RIP) since ECAM
 * is accessed via ordinary MOV instructions just like pflash's/AHCI's.
 * Checks BOTH bounds of `[ecam_base_phys, ecam_base_phys +
 * HYPE_PCI_ECAM_BUS0_SIZE)` -- not just the lower one -- returning -1
 * if the fault is outside this device's own range; this matters once
 * PCI-2 introduces a second, independently NPT-trapped region (a
 * device's own dynamically-BAR-programmed MMIO window), which an
 * only-a-lower-bound check could otherwise mistake for an ECAM access.
 * Resolves the faulting guest-physical address into an ECAM byte
 * offset, decodes it via hype_pci_decode_ecam_offset(), and dispatches
 * to hype_pci_config_read()/_write() -- both of which always succeed
 * (see devices/pci.h's own top comment for why a config-space access
 * architecturally never faults the way a real memory access can), so
 * unlike every other NPF handler here, this one has no "unrecognized
 * access" failure mode of its own beyond being outside its own range;
 * it can still return -1 if the faulting instruction itself doesn't
 * decode (an unsupported MOV/MOVZX form), matching
 * hype_svm_vcpu_handle_npf()'s own convention for that case. Advances
 * RIP past the decoded instruction.
 * Exempt from unit testing -- reaches into the exempt VMCB fields this
 * backend's real VMRUN produces; hype_pci_decode_ecam_offset(),
 * hype_mmio_decode(), and hype_pci_config_read()/_write() are all
 * already fully tested in isolation.
 */
int hype_svm_vcpu_handle_pci_ecam_npf(hype_vcpu_ctx_t *ctx, hype_pci_t *pci, uint64_t ecam_base_phys);

/*
 * VIDEO-3's exempt NPF glue for the Bochs-VBE display adapter's BAR2
 * (MMIO register window, devices/bochs_vbe.h) -- deliberately NOT for
 * BAR0 (the framebuffer): the framebuffer is ordinary guest RAM this
 * project's blanket NPT identity map already leaves present, so pixel
 * writes take zero VM-exits, matching real VRAM's own behavior (see
 * boot/main.c's run_video_3_test()). Only BAR2's register window is
 * ever NPT-trapped.
 *
 * Checks both bounds against `mmio_base_phys` (same rationale as the
 * ECAM handler just above -- a second, independently NPT-trapped
 * region must not be mistaken for this one), decodes the faulting
 * instruction (rejecting anything but a 2-byte-wide MOV, since DISPI
 * registers are architecturally 16-bit-only), and dispatches to
 * hype_bochs_vbe_mmio_read()/_write() -- an offset within the MMIO BAR
 * but outside the DISPI register block (devices/bochs_vbe.h) reads as
 * 0 / ignores the write, the same "reserved reads as 0" convention
 * devices/ahci.h's own MMIO model already uses. Advances RIP past the
 * decoded instruction.
 * Exempt from unit testing -- reaches into the exempt VMCB fields this
 * backend's real VMRUN produces; hype_mmio_decode() and
 * hype_bochs_vbe_mmio_read()/_write() are already fully tested in
 * isolation.
 */
int hype_svm_vcpu_handle_bochs_vbe_npf(hype_vcpu_ctx_t *ctx, hype_bochs_vbe_t *dev,
                                        uint64_t mmio_base_phys);

/*
 * M5-1's exempt NPF glue for the virtio-blk device's single MMIO BAR
 * (devices/virtio_blk.h), covering all four virtio-pci capability
 * regions (COMMON_CFG/NOTIFY_CFG/ISR_CFG/DEVICE_CFG) this project lays
 * out within it. Both-bounds-checks against `mmio_base_phys` (same
 * rationale as the ECAM/Bochs-VBE handlers), then dispatches by the
 * fault offset's own sub-region:
 *   - COMMON_CFG: routed straight to hype_virtio_blk_common_cfg_read/
 *     _write(), which already enforce each register's own correct
 *     access width -- this glue just passes the decoded width through.
 *   - NOTIFY_CFG: a 4-byte write here (regardless of its actual value
 *     -- there is only one queue, so any notify write means "queue 0
 *     has new work") walks the virtqueue via the private
 *     process_virtio_blk_queue() helper below, ONLY once
 *     hype_virtio_blk_is_queue_ready() confirms the driver has
 *     finished setup -- a notify that arrives before DRIVER_OK/
 *     queue_enable is a driver bug this project doesn't need to
 *     humor. A read here is meaningless (real hardware convention:
 *     ignored, reads back 0).
 *   - ISR_CFG: hype_virtio_blk_isr_read() (read clears, real hardware
 *     semantics); writes ignored (read-only from the driver's own
 *     perspective).
 *   - DEVICE_CFG: hype_virtio_blk_device_cfg_read() only; writes
 *     ignored (also read-only from the driver's perspective).
 *
 * `backing`/`backing_bytes` is this milestone's own scope: a fixed,
 * already-allocated in-memory buffer standing in for a real disk --
 * a genuine host-file-backed store (blk_backend) is M5-3's job, not
 * this one's, matching M4-3 pflash's own "primitive now, integration
 * later" precedent. Descriptor-chain walking assumes exactly 3
 * descriptors per chain (header, one data segment, status) -- no
 * scatter-gather across multiple data descriptors, this project's own
 * single-segment simplification (mirrors AHCI's own "single ATAPI
 * device, one command at a time" scope-narrowing). Guest-supplied
 * descriptor addresses/lengths are dereferenced directly, the same
 * established (if VALID-1..4-pending) convention every other device
 * handler here already follows -- not a new gap this task introduces.
 * Advances RIP past the decoded instruction.
 * Exempt from unit testing -- reaches into the exempt VMCB fields this
 * backend's real VMRUN produces and walks guest memory directly;
 * hype_mmio_decode(), hype_virtq_decode_desc(), and every
 * hype_virtio_blk_*_cfg_read/write()/hype_virtio_blk_is_queue_ready()
 * call it makes are already fully tested in isolation.
 */
int hype_svm_vcpu_handle_virtio_blk_npf(hype_vcpu_ctx_t *ctx, hype_virtio_blk_t *dev, uint8_t *backing,
                                         uint64_t backing_bytes, uint64_t mmio_base_phys);

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
