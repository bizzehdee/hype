#ifndef HYPE_ARCH_SVM_H
#define HYPE_ARCH_SVM_H

#include <stdint.h>

#include "../cpu/vmm_ops.h"
#include "../cpu/mmio_decode.h"
#include "../cpu/cpuid_emulate.h"
#include "../cpu/msr_emulate.h"
#include "../../../devices/pic.h"
#include "../../../devices/nvme.h" /* #202: the guest NVMe model the NPF handler drives */
#include "../../../devices/pit.h"
#include "../../../devices/hpet.h" /* #436: the HPET block the NPF handler drives */
#include "../../../devices/pflash.h"
#include "../../../core/virtio_net_ring.h" /* NET-2 (#81) */
#include "../../../core/e1000_dev_ring.h"   /* NET-3 (#82) */
#include "../../../devices/fw_cfg.h"
#include "../../../devices/ahci.h"
#include "../../../devices/atapi.h"
#include "../../../devices/pci.h"
#include "../../../devices/cmos.h"
#include "../../../devices/ps2_keyboard.h"
#include "../../../devices/ps2_mouse.h"
#include "../../../devices/bochs_vbe.h"
#include "../../../devices/guest_lapic.h"
#include "../../../devices/ioapic.h"
#include "../../../devices/guest_uart.h"
#include "../../../devices/virtio_blk.h"
#include "../../../core/blk_backend.h"
#include "../../../devices/ata_disk.h"
#include "../../../core/guest_mem.h"
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

/*
 * #316: EFER field layout, from AMD APM Vol 2 §3.1.7 / Figure 3-9
 * (research/24593_3.44_APM_Vol2.pdf p.56). Needed because a guest's WRMSR to EFER is
 * INTERCEPTED and emulated, so hype -- not hardware -- is what enforces the rules.
 */
#define HYPE_EFER_SCE (1ULL << 0)   /* SysCall extensions */
#define HYPE_EFER_LME (1ULL << 8)   /* Long Mode Enable */
#define HYPE_EFER_LMA (1ULL << 10)  /* Long Mode Active -- hardware-owned, see below */
#define HYPE_EFER_NXE (1ULL << 11)  /* No-Execute Enable */
/* Bits 7:1 are RAZ (read as zero), so a write to them is dropped rather than faulted. */
#define HYPE_EFER_RAZ 0x00000000000000FEULL
/*
 * Bits that must be zero: 9, 16, 19 and 63:22. Writing any of them is a #GP on real
 * hardware, and leaving one set in the VMCB makes VMRUN fail its "Any MBZ bit of EFER is
 * set" consistency check (APM §15.5.1) -- which kills the hypervisor, not the guest.
 */
#define HYPE_EFER_MBZ 0xFFFFFFFFFFC90200ULL

/* CR bits the EFER cross-checks in APM §15.5.1's illegal-state list depend on. */
#define HYPE_CR0_PE (1ULL << 0)
#define HYPE_CR0_PG (1ULL << 31)
#define HYPE_CR4_PAE (1ULL << 5)

/*
 * #316: decide what a guest's WRMSR to EFER should actually put in the VMCB.
 *
 * Returns 0 and writes *out when the write is legal, or -1 when a real WRMSR would raise
 * #GP(0) and the caller should inject that instead of storing the value.
 *
 * This exists because storing the guest's value verbatim is a guest-triggerable way to kill
 * hype. VMRUN refuses a VMCB whose GUEST EFER has SVME clear (APM §15.5.1's very first
 * illegal-state condition), and the APM says so in as many words at §3.1.7:
 *
 *     "The effect of turning off EFER.SVME while a guest is running is undefined; therefore,
 *      the VMM should always prevent guests from writing EFER."
 *
 * OpenBSD 7.9 is the guest that found it. Its kernel's long-mode re-entry rebuilds EFER from
 * scratch rather than read-modify-writing it -- `rdmsr; mov %eax,%ebx; xor %eax,%eax; or
 * $0x101,%eax; ... wrmsr` -- so the value it writes has SVME clear. Linux, FreeBSD and OVMF
 * all happen to OR into the value they read, which preserved hype's SVME bit by accident and
 * is why no guest had exposed this before.
 *
 * So: SVME is forced back on, LMA is taken from the CURRENT value (hardware owns it; the APM
 * requires software to preserve it and faults a mismatch), RAZ bits are dropped, and anything
 * that would leave an illegal EFER/CR0/CR4 combination is refused as #GP rather than handed to
 * VMRUN. Pure, so it is unit tested directly.
 */
int hype_svm_guest_efer_write(uint64_t current_efer, uint64_t requested, uint64_t cr0,
                              uint64_t cr4, uint64_t *out);

/*
 * #316: what a guest's RDMSR of EFER should report -- the stored value with SVME masked off.
 *
 * The guest never asked for SVME; hype forces it in because VMRUN requires it. Reporting it
 * back would tell the guest SVM is available while hype's own CPUID clears the SVM feature bit
 * (arch/x86_64/cpu/cpuid_emulate.c), and a guest that believed it could then try to use SVM
 * instructions. Masking keeps the two answers consistent.
 */
uint64_t hype_svm_guest_efer_read(uint64_t stored_efer);

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
 * #244: SVM ASID allocation.
 *
 * AMD-V tags TLB entries by ASID, and a TLB hit is decided by the ASID tag plus the
 * linear page frame -- the nested-paging root (nCR3) does NOT participate in the tag.
 * So two guests running with the SAME ASID and DIFFERENT nCR3 can genuinely alias
 * each other's translations. This is the load-bearing difference from the VMX side:
 * there, combined mappings are tagged by EP4TA as well, which is why distinct EPT
 * roots kept guests apart regardless of VPID and why #273 was NOT a correctness fix.
 * Here it is one.
 *
 * ASID 0 is reserved for the host, so usable guest ASIDs are 1..NASID-1.
 */

/* Fn8000_000A EBX is NASID, the number of ASIDs the CPU implements. */
uint32_t hype_svm_nasid_from_cpuid_ebx(uint32_t ebx);

/*
 * A distinct nonzero ASID for a vCPU pool slot, clamped to what this CPU supports.
 * Returns 0 only if the CPU reports it cannot support even one guest ASID, which the
 * caller must treat as "do not run a guest" rather than as ASID 0 (the host's).
 */
uint32_t hype_svm_asid_for_slot(unsigned slot, uint32_t nasid);

/* VMCB TLB_CONTROL values (APM: VMCB offset 0x058, bits 39:32). */
#define HYPE_SVM_TLB_CTL_NOTHING 0u
#define HYPE_SVM_TLB_CTL_FLUSH_ALL 1u
#define HYPE_SVM_TLB_CTL_FLUSH_GUEST 3u

/*
 * Enables SVM on the calling physical CPU: sets EFER.SVME and points
 * VM_HSAVE_PA at a host save area. Returns 0 on success. Exempt from
 * unit testing per AGENTS.md -- real RDMSR/WRMSR, nothing to observe
 * without a real CPU; hype_svm_efer_with_svme() above holds the only
 * real logic and is fully tested.
 */
int hype_svm_enable(void);

/* Enables SVM on the CURRENT core with the caller-supplied host-save page.
 * SVME/VM_HSAVE_PA are per-core, so a second core (M8-0b AP) enabling SVM to
 * run its own guest must call this with its own page. No console output (an
 * AP calls it, and printing there would race the BSP). */
int hype_svm_enable_on(uint64_t hsave_pa);

/* vmm_ops.enable_on adapter for hype_svm_enable_on() -- see vmm_ops.h. */
int hype_svm_enable_on_page(void *percore_page);

/* #412: allocate the VMCB + vCPU-ctx pools sized to `count`, before any vCPU is
 * created. `alloc_zeroed_pages(pages)` returns the host address of `pages`
 * page-aligned, zeroed 4 KiB pages. */
void hype_svm_vcpu_pool_alloc(unsigned count, uint64_t (*alloc_zeroed_pages)(unsigned pages));

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

/* #244: this vCPU's ASID (0 = none, i.e. sharing the host tag). */
uint32_t hype_svm_vcpu_tlb_tag(hype_vcpu_ctx_t *ctx);

/*
 * M8-4 (VM Start/reboot): re-initialise an EXISTING real-mode vCPU context to
 * fresh reset state, reusing its already-allocated pool slot / VMCB (unlike
 * hype_svm_vcpu_create(), which allocates a new slot each call -- calling that
 * on every reboot would exhaust the pool). Rebuilds the VMCB (real-mode guest,
 * IOPM/MSRPM, nested paging) and zeroes the GPRs, exactly as create does. Exempt
 * from unit testing (real VMCB layout), same as create.
 */
void hype_svm_vcpu_reset_realmode(hype_vcpu_ctx_t *ctx, uint64_t guest_rip, uint64_t guest_rsp,
                                  uint64_t npt_root);

/* M8-6: model the ACPI PM1a control register (16-bit) at I/O `port`. On an IN,
 * load RAX with *value (SLP_EN, bit 13, always reads back 0 per ACPI) and return
 * 1. On an OUT, store the written value (minus SLP_EN) into *value, set *slp_en
 * to whether SLP_EN was written (a sleep/power-off request), and return 0.
 * Returns -1 (no state change) if this exit isn't an access to `port`. Advances
 * RIP when it handles the access. Modelling the register (vs. absorbing it as
 * all-ones) is required: OVMF read-modify-writes PM1a_CNT during boot, and an
 * all-ones read fed back as a write spuriously carries SLP_EN. Exempt from unit
 * testing (real VMCB). */
int hype_svm_vcpu_handle_pm1_cnt_ioio(hype_vcpu_ctx_t *ctx, uint16_t port, uint16_t *value,
                                      int *slp_en);

/* #94: 0xCF9 reset control. 0 = handled write (*reset_requested set),
 * 1 = handled read, -1 = not this port. */
int hype_svm_vcpu_handle_reset_ctl_ioio(hype_vcpu_ctx_t *ctx, uint16_t port, int *reset_requested);

/* #436: PM1a event block (status/enable) -- see the definition's comment. */
int hype_svm_vcpu_handle_pm1_evt_ioio(hype_vcpu_ctx_t *ctx, uint16_t base, uint16_t *enable);

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
 * #535: rebuild an EXISTING vCPU as a long-mode guest, keeping its VMCB pool slot and this VM's
 * NPT root -- the reset counterpart of hype_svm_vcpu_reset_realmode, for `boot = kernel`.
 * Exempt from unit testing for the same reason as the create above.
 */
void hype_svm_vcpu_reset_longmode(hype_vcpu_ctx_t *ctx, uint64_t guest_rip, uint64_t guest_cr3,
                                  uint64_t guest_rsp, uint64_t npt_root);

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

/* M4-6d4: enable SVM PAUSE-filtering (INTERCEPT_PAUSE + filter count/
 * threshold) so a guest spin loop is intercepted after a burst of PAUSEs.
 * Caller must first confirm hype_cpu_has_pause_filter(). Exempt glue. */
void hype_svm_vcpu_enable_pause_filter(hype_vcpu_ctx_t *ctx, uint16_t count, uint16_t threshold);

/* RT-2b: enable physical-INTR interception so hype's periodic timer can
 * preempt this guest (#VMEXIT EXITCODE_INTR) even during a long
 * non-intercepting stretch. Exempt glue. */
void hype_svm_vcpu_enable_intr_intercept(hype_vcpu_ctx_t *ctx);

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

/* #438: retire an intercepted RDTSC with the hypervisor's advancing timebase
 * plus the VMCB offset, avoiding a nested L2 counter that is observed frozen. */
void hype_svm_vcpu_handle_rdtsc(hype_vcpu_ctx_t *ctx);

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

/* PERF-1: read an IOIO exit's decoded port/dir/size without consuming the exit
 * (no RIP/RAX change) -- for per-port histogram instrumentation. */
void hype_svm_vcpu_peek_ioio(hype_vcpu_ctx_t *ctx, hype_svm_ioio_t *out);

/* GLADDER-1: absorb an MMIO NPF to an unmodeled region (read -> all-ones,
 * write -> dropped, RIP advanced). Returns 0 if decoded+absorbed, -1 if the
 * instruction couldn't be decoded (caller must not advance RIP). Fallback only,
 * AFTER every real device handler has declined the address. */
int hype_svm_vcpu_absorb_mmio_npf(hype_vcpu_ctx_t *ctx, const uint8_t *guest_insn_bytes);

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
/* Handles a guest PS/2 controller access (data port 0x60 / status+command
 * port 0x64) against the emulated keyboard + mouse. If out_kbd_wait is
 * non-NULL it is set to 1 when this access was a keyboard status-port read
 * that found the output buffer empty -- i.e. the guest is polling, waiting
 * for a keystroke (FW-1g uses a run of these to time a key injection into
 * an interactive prompt's poll window); 0 for every other access. Returns
 * 0 if the access was a PS/2 port, -1 otherwise. */
int hype_svm_vcpu_handle_ps2_ioio(hype_vcpu_ctx_t *ctx, hype_ps2_kbd_t *kbd, hype_ps2_mouse_t *mouse,
                                   int *out_kbd_wait);

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
 * can" per hype_svm_can_accept_interrupt() and the guest's virtual
 * interrupt priority. If the guest can accept it immediately, writes EVENTINJ directly via
 * hype_svm_encode_eventinj_intr() (delivered on the very next VMRUN).
 * Otherwise sets `vector`'s bit in a 256-bit pending IRR and arms an interrupt-window request
 * (hype_svm_arm_vintr_request(), HYPE_SVM_INTERCEPT_VINTR), so
 * hype_svm_vcpu_handle_vintr_window() can drain the highest-priority pending vector once that
 * window opens. Multiple different vectors CAN be pending at once and none is lost; requesting a
 * vector that is already pending coalesces, which is what an IRR bit means -- one delivery per set
 * bit, not one per request.
 *
 * #356: this comment used to say the opposite -- that only one interrupt was ever in flight and
 * that a second request "overwrites the pending vector rather than queuing". That described an
 * older implementation. It was wrong for long enough that I read it during the #318 investigation,
 * believed it over the code, and filed an issue against a defect that did not exist. Wrong
 * comments are not harmless; this one cost a bug report and a wrong diagnosis. Exempt from unit testing
 * -- reaches into the exempt VMCB fields this backend's real VMRUN
 * produces; every pure function this composes
 * (hype_svm_can_accept_interrupt()/hype_svm_encode_eventinj_intr()/
 * hype_svm_arm_vintr_request()) is already fully tested in isolation.
 */
void hype_svm_vcpu_request_interrupt(hype_vcpu_ctx_t *ctx, uint8_t vector);

/* #318: how many times `vector` was requested, and how many times it was actually staged into
 * EVENTINJ. Counters rather than a bounded trace, because a trace of interrupt traffic is always
 * dominated by the periodic timer and the vector under investigation never gets a line. */
/* #359: per-vCPU -- the global version summed both guests, making its one lead
 * (a requested-vs-injected gap on a single vector) unattributable. */
/* #92 diag: 8-entry MRU histograms of CPUID leaves + MSR indices the guest hits, with the
 * last RIP for each kind -- names a spin loop the EXHIST totals can only count. */
void hype_svm_vcpu_get_spin_diag(uint32_t *cpuid_keys, uint64_t *cpuid_cnts, uint32_t *msr_keys,
                                 uint64_t *msr_cnts, unsigned n, uint64_t *cpuid_rip,
                                 uint64_t *msr_rip);
void hype_svm_vcpu_get_vec_counts(hype_vcpu_ctx_t *ctx, uint8_t vector, uint32_t *out_req,
                                  uint32_t *out_inj);

/* #311: one-command AHCI timeline. Armed by the first kernel-era access to the HBA (OVMF drives
 * the same registers long before, and swamped five earlier attempts at this measurement), then a
 * bounded serial event log shared by the device path and the FW-1 loop's raise. The log is serial,
 * so its ORDER is the measurement. */

/* GLADDER-6c DIAG: reinject an intercepted guest exception via EVENTINJ so the
 * guest takes it through its own IDT on the next VMRUN (used when hype
 * intercepts a vector only to observe/log it). has_error_code + error_code for
 * faults that push one (#GP/#PF/#DF); pass 0 for #UD and other no-error faults. */
/* #484: inject an NMI into this vCPU. Call on the vCPU's own core -- see the note at the
 * definition on why a cross-core VMCB write can be lost. */
void hype_svm_vcpu_inject_nmi(hype_vcpu_ctx_t *ctx);

void hype_svm_vcpu_reinject_exception(hype_vcpu_ctx_t *ctx, uint8_t vector,
                                      int has_error_code, uint32_t error_code);

/*
 * #455: drop a vector from the pending IRR without delivering it. request_interrupt()
 * translates an acknowledged IRQ line into a CPU vector and queues it the instant the
 * guest can't yet accept it (IF=0) -- eagerly, at acknowledge time, decoupled from
 * there on from whatever later masks that IRQ line. A vector queued this way is
 * NEVER re-checked against the PIC's current IMR before delivery, so a line masked
 * AFTER acknowledge (routine during early boot: BIOS/loader traffic acknowledged
 * while unmasked, then the guest masks everything before its own interrupt
 * subsystem is ready) still delivers, late, into a kernel that never asked for it.
 * Real hardware cannot do this -- a masked line simply never reaches the CPU, no
 * matter how long ago it was raised. Call this once a line's mask state is known to
 * have changed, for every vector that line could still have queued.
 */
void hype_svm_vcpu_cancel_pending_vector(hype_vcpu_ctx_t *ctx, uint8_t vector);

/* #512: mark a just-queued vector as PIC-sourced / cancel only such vectors. The #455 prune
 * must not cancel an IO-APIC or MSI vector that merely shares the masked PIC's vector range. */
void hype_svm_vcpu_note_pic_pending(hype_vcpu_ctx_t *ctx, uint8_t vector);
void hype_svm_vcpu_cancel_pic_pending(hype_vcpu_ctx_t *ctx, uint8_t vector);

/* SMP-2 (#186): set the CPU topology this vCPU's guest sees. Called once per vCPU after
 * creation; see hype_cpuid_topology_t on why it is per-vCPU rather than global. */
void hype_svm_vcpu_set_topology(hype_vcpu_ctx_t *ctx, uint32_t apic_id, uint32_t vcpu_count,
                                uint32_t threads_per_core);

/*
 * #456: pop one vector that hype has staged into EVENTINJ since the last call, or return 0
 * once none remain. The caller uses this to mark the guest's emulated LAPIC ISR at the moment
 * of commitment. Marking at REQUEST time is wrong in both directions: a request that is
 * deferred (or cancelled) leaves an ISR bit for a vector the guest never took, and a vector
 * drained out of pending_irr after the guest EOI'd an earlier delivery is committed with no
 * request of its own, so its ISR bit is missing. FreeBSD derives the vector it dispatches from
 * `bsr` over the ISR dword, so either error hands it the wrong vector.
 */
int hype_svm_vcpu_take_injected_vector(hype_vcpu_ctx_t *ctx, uint8_t *out_vector);

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

/* M4-6d2: if an interrupt is deferred (waiting on a VINTR window) and the
 * guest can now accept one, inject it directly this instant rather than
 * waiting for the VINTR intercept to fire -- closes a delivery gap where
 * a deferred timer IRQ stayed stranded (guest halted/ready but the window
 * never fired), freezing jiffies. Call once per vCPU-loop iteration.
 * Returns 1 if it injected. Exempt VMCB glue. */
int hype_svm_vcpu_deliver_pending_if_ready(hype_vcpu_ctx_t *ctx);

/* M4-6d2: retire an intercepted HLT and consume its STI interrupt-shadow,
 * modelling an interrupt waking a halted CPU (RIP advances past the 1-byte
 * HLT so the guest resumes after it). Call on a HLT intercept immediately
 * before injecting the waking interrupt. Without this, a `sti; hlt` idle
 * wait deadlocks: the shadow that covered the HLT is never consumed (we
 * intercept the HLT before it retires), so the pending interrupt stays
 * blocked and the guest never wakes. Exempt VMCB glue. */
void hype_svm_vcpu_wake_hlt(hype_vcpu_ctx_t *ctx);

/*
 * M4-6d2 DIAG: snapshot of the guest's interrupt-acceptance state -- see
 * hype_vmm_intr_state_t in ../cpu/vmm_ops.h, where the struct itself now
 * lives.
 *
 * VMX-4 (#236) moved it there because the FW-1 loop reads it to make real
 * decisions, not just to log: it gates `pic_ready` and drives the HLT
 * idle-wait path, so VMX needs to fill the same shape. This alias keeps every
 * existing SVM use (and the name in the diagnostics) working unchanged.
 */
typedef hype_vmm_intr_state_t hype_svm_intr_state_t;

void hype_svm_vcpu_get_intr_state(hype_vcpu_ctx_t *ctx, hype_svm_intr_state_t *out);

/* M4-6d2 DIAG: read the interrupt-injection path counters -- how many
 * requests took the direct-EVENTINJ path (guest could accept), how many
 * were deferred to a VINTR window, how many VINTR windows fired to
 * deliver a deferred vector, and how many deferrals clobbered a still-
 * undelivered pending vector. defer >> window (or overwrite > 0) means
 * deferred injections are getting stuck/lost. */
void hype_svm_vcpu_get_int_diag(hype_vcpu_ctx_t *ctx, unsigned long long *eventinj,
                                 unsigned long long *defer,
                                 unsigned long long *window, unsigned long long *overwrite);

/* M4-6b2: count of interrupt requests that found EVENTINJ already staged for
 * the next VMRUN and QUEUED the vector in the IRR instead of clobbering it.
 * Nonzero proves same-iteration IRQ collisions occur (they were previously an
 * invisible lost-interrupt) -- now safely serialized, never dropped. */
unsigned long long hype_svm_vcpu_get_eventinj_collisions(hype_vcpu_ctx_t *ctx);

/* #343: ATAPI transfer accounting. `short_xfers` counts transfers that ended with bytes still owed
 * because the guest's PRDT list ran out -- reported to the guest as SUCCESS, so nothing else can
 * notice. Any non-zero value means a guest was handed a partially-filled buffer and told it was
 * complete. */
void hype_svm_vcpu_get_atapi_diag(unsigned long long *xfers, unsigned long long *short_xfers,
                                  unsigned long long *req_bytes, unsigned long long *done_bytes,
                                  unsigned long long *owed_bytes);

#if HYPE_343_VERIFY_READS
/* #343 diagnostic builds only: how many streamed reads were re-read and compared against what was
 * written into guest memory, and how many DIFFERED. Non-zero mismatched means hype handed the guest
 * something other than the ISO's bytes. */
void hype_svm_vcpu_get_read_verify(unsigned long long *checked, unsigned long long *mismatched);
#endif

/*
 * INPUT-1: the reusable "a device wired to `chip` just raised `irq`"
 * entry point -- combines devices/pic.h's own real-hardware modeling
 * (hype_pic_emu_raise_irq() sets IRR; hype_pic_emu_acknowledge_highest_priority()
 * performs the INTA-cycle equivalent, moving the highest-priority
 * pending/unmasked IRQ from IRR to ISR and computing its real vector
 * from the chip's own ICW2-programmed offset) with
 * hype_svm_vcpu_request_interrupt() (INT-1/INT-2) to actually deliver
 * that vector to the guest, now or once IF/shadow and virtual priority permit it.
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
    /* For an intercepted #PF this is THE faulting linear address (APM
     * Vol 2 24593 Rev 3.44 §15.12.15). It is NOT redundant with the
     * save-area CR2 above: §15.12.15 says the intercept is tested
     * BEFORE CR2 is written by the exception, so save.cr2 is stale for
     * an intercepted #PF and exitinfo2 is the only valid fault address.
     * (An earlier session had this backwards, trusting cr2 and treating
     * exitinfo2 as a mere cross-check.) */
    uint64_t exitinfo2;
    /* Real-hardware investigation (rip=-1 / rflags=reset-value / cr2=0
     * all seen together, an internally-inconsistent single-fault
     * picture -- a data read of linear 0 cannot be executing at rip=-1).
     * APM Vol 2 (24593 Rev 3.44) §15.7.2 / §15.20: EXITINTINFO (control
     * 0x088) is written on #VMEXIT when the intercept fired *while the
     * guest was delivering a prior interrupt/exception through its own
     * IDT*. If VALID (bit 31) here, this #PF is a nested fault taken
     * mid-delivery (e.g. the IDT/handler/stack page itself unmapped
     * under FW-1's partial NPT), NOT the original faulting instruction
     * -- which fully explains why the saved rip/rflags don't describe a
     * clean single fault. If NOT valid, that hypothesis is dead and an
     * erratum-class save-completeness issue moves to the front. Either
     * way it is the single most decisive field for this puzzle, and
     * reading it is free (already in the VMCB we hold). */
    uint64_t exitintinfo;
    /* control 0x0C8: NRIP, the address of the *next* instruction. For
     * decode-assisted intercepts hardware fills this; captured as an
     * independent cross-check on the save-area rip. */
    uint64_t nrip;
    /* PERF-1 memory-type probe: guest CR4 and the guest PAT the VMCB loads
     * under nested paging. CR0.CD (bit30) forces UC for all guest memory; the
     * guest PAT + page-table PAT index otherwise pick the type. */
    uint64_t cr4;
    uint64_t g_pat;
    /* SMP-6: the AP locates OVMF's CpuMpData through its own IDTR --
     * MpLib.c: "All APs share one separate IDT. So AP can get the address
     * of CpuMpData by using IDTR.BASE + IDTR.LIMIT + 1". A wrong IDTR
     * therefore yields a garbage structure pointer and an indirect call
     * into nothing, which is what the #UD at 0x80201A looks like. */
    uint64_t idtr_base;
    uint32_t idtr_limit;
} hype_svm_debug_state_t;

void hype_svm_vcpu_get_debug_state(hype_vcpu_ctx_t *ctx, hype_svm_debug_state_t *out);

/* PERF-1 memory-type probe: counts of guest MTRR MSR accesses (MTRRcap 0xFE,
 * MTRRdefType 0x2FF, variable/fixed MTRRs) + the last values the guest tried to
 * WRITE (hype currently stubs these, returning 0 on read). If the guest reads
 * MTRRs as 0 -> "no MTRRs / disabled" -> x86 default memory type is UC. */
void hype_svm_vcpu_get_mtrr_diag(unsigned long long *reads, unsigned long long *writes,
                                 uint64_t *last_deftype_wr, uint64_t *last_var_wr);

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

/* Overrides the VMCB's exception-intercept bitmap (bit per vector 0-31).
 * The builders default to 0xFFFFFFFF (intercept every exception) -- a
 * strict fault-catch that is right for the synthetic milestone test
 * guests (which are not supposed to fault at all) and was invaluable for
 * the OVMF bring-up. A *real* guest OS, by contrast, handles its own
 * exceptions as routine operation (a Linux kernel takes #PF for demand
 * paging, #GP/#UD probing CPU features, #NM for lazy FPU, ...), so
 * intercepting them is fatal to it. FW-1 sets this to 0 once OVMF hands
 * off to a booted OS: the guest owns every vector, and an unrecoverable
 * triple fault still returns to us as HYPE_SVM_EXITCODE_SHUTDOWN. */
void hype_svm_vcpu_set_exception_intercepts(hype_vcpu_ctx_t *ctx, uint32_t mask);

/*
 * #436: arm DR0 as a 1-byte execution breakpoint at `gva` in the guest, or
 * disarm it when `gva` is 0. hype owns the guest's debug registers through the
 * VMCB, so this needs no cooperation from the guest -- which is the point: it
 * observes code that never gets the chance to report on itself.
 */
void hype_svm_vcpu_arm_exec_breakpoint(hype_vcpu_ctx_t *ctx, uint64_t gva);

/* #436: DR7-write exit (0x37) -- re-apply the breakpoint and step over. */
int hype_svm_vcpu_handle_dr_write(hype_vcpu_ctx_t *ctx);

/* Returns the AMD SVM "decode assists" guest instruction bytes captured
 * by hardware on the current NPF/#PF intercept (VMCB control area
 * 0xD0/0xD1), writing the fetched byte count to *out_num. When the count
 * is nonzero these are the faulting instruction's bytes regardless of
 * the guest's paging -- the only way to decode an MMIO access once a
 * guest runs its own virtual address space (a Linux kernel's RIP is a
 * high-canonical virtual address, not a guest-physical one, so fetching
 * via a guest-physical translation of RIP no longer works). A zero count
 * means decode assists did not populate them (older/emulated CPUs); the
 * caller should fall back to translating RIP for an identity-paged guest
 * (e.g. OVMF). */
const uint8_t *hype_svm_vcpu_guest_insn_bytes(hype_vcpu_ctx_t *ctx, uint8_t *out_num);

/* The guest's current CR3 (VMCB save.cr3) -- the physical base of its
 * top-level page table, for walking guest virtual -> guest-physical when
 * decode assists are unavailable (e.g. QEMU+KVM nested SVM). */
uint64_t hype_svm_vcpu_get_cr3(hype_vcpu_ctx_t *ctx);
/* One guest general-purpose register by x86-64 encoding (0=RAX..15=R15). Indices 0 and 4 are
 * not held in gprs[] -- RAX lives in the VMCB and RSP is managed by VMRUN -- so they read 0. */
uint64_t hype_svm_vcpu_get_gpr(hype_vcpu_ctx_t *ctx, unsigned idx);

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

/* M7-1b (#300): service an intercepted Hyper-V VMMCALL and retire it. */
int hype_svm_vcpu_handle_hypercall(hype_vcpu_ctx_t *ctx);

/* PVCLOCK (kvmclock): register the guest-memory map (for reaching the pvclock
 * pages) and the host TSC frequency, so the KVM SYSTEM_TIME/WALL_CLOCK MSR
 * writes can fill the guest's pvclock pages. Call once before running the
 * guest. Without it, those MSR writes are accepted but fill nothing. */
/*
 * M7-1 (#91): show this vCPU's guest the Hyper-V hypervisor identity (CPUID
 * 0x40000000-0x40000006 + the synthetic MSRs) instead of the KVM one. Off by default;
 * driven from the VM's os_hint, and per-vCPU because two guests with different
 * os_hints run at the same time on different cores.
 */
void hype_svm_vcpu_set_hv_enabled(hype_vcpu_ctx_t *ctx, int enabled);

void hype_svm_vcpu_set_pvclock(hype_vcpu_ctx_t *ctx, const hype_gpa_map_t *map, uint64_t tsc_hz);

/* Count of pvclock time-info page fills -- nonzero proves the guest enabled
 * kvmclock and hype backed it. Diagnostic. */
extern volatile uint32_t g_hype_pvclock_arm_count;

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
 * convenience as hype_svm_vcpu_handle_ioio(). Handles the classic
 * select/data ports (0x510/0x511), the DMA ports (0x514/0x518), and
 * SVM-STRIO string-IN (`rep insb/insw/insd`) on the data port -- the form
 * OVMF's QemuFwCfgLib actually uses. `dma_map` translates + bounds-checks
 * the guest-physical addresses the guest hands the DMA and string paths
 * (NULL = trusted identity-mapped test guest; see guest_dma_xlate). Exempt
 * from unit testing -- reaches into the exempt VMCB fields this backend's
 * real VMRUN produces and does its own guest-memory access; the pure
 * pieces it dispatches to (hype_fw_cfg_*(), hype_svm_build_string_io_plan())
 * are each fully tested in isolation.
 */
int hype_svm_vcpu_handle_fw_cfg_ioio(hype_vcpu_ctx_t *ctx, hype_fw_cfg_t *fw,
                                     const hype_gpa_map_t *dma_map);

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
 * FW-1h/VALID-3 variant of hype_svm_vcpu_handle_ahci_npf() for a guest
 * whose NPT does NOT identity-map RAM. Real OVMF (FW-1) runs with guest-
 * physical [0, GUEST_RAM) remapped to a separately-allocated host buffer
 * plus a flash window near 4GB, so every guest-physical address the
 * guest's AHCI driver programs into the Command List/Table, received-FIS
 * area and PRDT -- plus the faulting instruction's own RIP -- is
 * translated AND bounds-checked through dma_map (VALID-1) with its
 * access length before being dereferenced; an out-of-range address
 * fails the command rather than reaching host memory. The plain handler
 * above is the NULL-map (trusted identity) case. Same unit-test
 * exemption. */
int hype_svm_vcpu_handle_ahci_npf_map(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_atapi_t *atapi,
                                       uint64_t ahci_base_phys, const hype_gpa_map_t *dma_map,
                                       const uint8_t *guest_insn_bytes);

/*
 * M5-2's counterpart to hype_svm_vcpu_handle_ahci_npf() above -- same
 * hype_ahci_t register model and NPF/PxCI-write trigger, but for a
 * SECOND, independent AHCI controller instance backing a plain ATA
 * disk (devices/ata_disk.h) rather than the ATAPI optical drive. Two
 * separate controller instances rather than a second port on the
 * existing one: hype_ahci_t was written for exactly one port (see its
 * own top comment) -- extending it to genuinely multi-port would mean
 * touching M4-5's already-tested code for no real benefit, when two
 * independent single-port controllers (a real, valid hardware
 * topology too) get the same result with zero risk to working code.
 *
 * Dispatches on the H2D Register FIS's own command byte (hype_ahci_
 * decode_h2d_fis(), not the Command Header's "A"/ATAPI bit this
 * project's existing ATAPI path uses) to IDENTIFY DEVICE, READ DMA
 * EXT, WRITE DMA EXT, or FLUSH CACHE EXT; anything else is rejected
 * (-1) as an unrecognized command, this project's own scope not
 * modeling the rest of the ATA command set. READ/WRITE DMA EXT bounds-
 * check the resolved LBA range (hype_ata_disk_range_in_bounds(),
 * "0 count means 65536 sectors" already resolved via hype_ata_disk_
 * resolve_sector_count()) before touching the backing buffer at all,
 * reporting IDNF (ID Not Found, error register 0x10) with the ERR
 * status bit set on an out-of-range request rather than silently
 * reading/writing past the disk's own end. Streams through the PRDT
 * list exactly like the existing ATAPI path (same chunking loop), just
 * in whichever direction (guest<-backing-store for reads/IDENTIFY,
 * backing-store<-guest for writes) the command requires. Builds the
 * D2H completion FIS the same way the ATAPI path already does (status/
 * error registers, PxTFD, PxIS D2H-FIS bit, PxCI slot-0 clear).
 * Advances RIP past the decoded instruction (delegated to the same
 * bounds-check + hype_mmio_decode() shape hype_svm_vcpu_handle_
 * ahci_npf() already uses for PxCI writes).
 * Exempt from unit testing -- reaches into the exempt VMCB fields this
 * backend's real VMRUN produces and walks guest memory directly;
 * hype_ahci_decode_cmd_header()/_decode_prdt_entry()/_decode_h2d_fis(),
 * hype_ata_disk_build_identify()/_resolve_sector_count()/
 * _range_in_bounds() are all already fully tested in isolation.
 */
int hype_svm_vcpu_handle_ahci_disk_npf(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci, hype_ata_disk_t *disk,
                                        uint64_t ahci_base_phys);

/*
 * #262 slice 3: as above, but for a guest whose RAM is remapped. `dma_map` translates
 * every guest-physical address the command carries -- command list, command table,
 * each PRD data pointer, the RX FIS -- and `guest_insn_bytes` supplies already-fetched
 * instruction bytes (pass 0 to read them at the guest RIP through the same map).
 * Mirrors hype_svm_vcpu_handle_ahci_npf_map's contract for the ATAPI controller.
 */
int hype_svm_vcpu_handle_ahci_disk_npf_map(hype_vcpu_ctx_t *ctx, hype_ahci_t *ahci,
                                           hype_ata_disk_t *disk, uint64_t ahci_base_phys,
                                           const hype_gpa_map_t *dma_map,
                                           const uint8_t *guest_insn_bytes);

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
 *
 * `guest_insn_bytes` is the faulting instruction's bytes, resolved to a
 * host address by the caller (for the identity-mapped test guests that
 * is just the guest RIP; FW-1 remaps RAM/flash and must translate --
 * see hype_svm_vcpu_handle_lapic_npf's own note).
 */
int hype_svm_vcpu_handle_pci_ecam_npf(hype_vcpu_ctx_t *ctx, hype_pci_t *pci, uint64_t ecam_base_phys,
                                       const uint8_t *guest_insn_bytes);

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
                                        uint64_t mmio_base_phys, const uint8_t *insn);

/*
 * FW-1b's exempt NPF glue for the guest Local APIC (devices/guest_lapic.h)
 * at [lapic_base_phys, lapic_base_phys + HYPE_GUEST_LAPIC_MMIO_SIZE).
 * Same range-check->decode->dispatch->advance-RIP flow as the other
 * MMIO handlers here; xAPIC registers are 32-bit, so a non-4-byte
 * access returns -1 (fail closed). Returns 0 if the fault was in the
 * LAPIC window and handled, -1 otherwise (so the FW-1 loop can fall
 * through to its unhandled-NPF fatal).
 *
 * Unlike the pflash/bochs_vbe handlers, the faulting instruction bytes
 * are passed IN (`guest_insn_bytes`) rather than read from save.rip as
 * a host pointer: FW-1's NPT remaps both guest RAM and the firmware
 * flash window away from an identity map, so the caller must translate
 * the guest RIP to its real host backing address first (FW-1's guest
 * paging is identity, so guest-linear RIP == guest-physical there).
 */
/* #436: HPET MMIO block -- see the definition's comment. */
int hype_svm_vcpu_handle_hpet_npf(hype_vcpu_ctx_t *ctx, hype_hpet_t *hpet,
                                   uint64_t hpet_base_phys, const uint8_t *guest_insn_bytes);

int hype_svm_vcpu_handle_lapic_npf(hype_vcpu_ctx_t *ctx, hype_guest_lapic_t *lapic,
                                    uint64_t lapic_base_phys, const uint8_t *guest_insn_bytes);

/* #457: FW-1-grade pflash NPF glue -- takes the faulting instruction's bytes like the LAPIC
 * handler above, because a live guest's RIP is not a host pointer. Bounds to pf->size. */
int hype_svm_vcpu_handle_pflash_npf_insn(hype_vcpu_ctx_t *ctx, hype_pflash_t *pf,
                                         uint64_t pf_base_phys, const uint8_t *guest_insn_bytes);

/* #457: arm a flush-this-guest for the next entry, after a runtime NPT edit. */
void hype_svm_vcpu_request_tlb_flush(hype_vcpu_ctx_t *ctx);

/* M4-6b3: FW-1's exempt NPF glue for the emulated I/O APIC (devices/ioapic.h)
 * at 0xFEC00000 -- same shape/exemption reasoning as the LAPIC glue above. */
int hype_svm_vcpu_handle_ioapic_npf(hype_vcpu_ctx_t *ctx, hype_ioapic_t *ioapic,
                                    uint64_t ioapic_base_phys, const uint8_t *guest_insn_bytes);

/*
 * FW-1e's exempt IOIO glue for the guest 16550 UART (devices/guest_uart.h)
 * at COM1 [base_port, base_port+8). Byte-wide IN/OUT via AL, dispatched
 * to the register model; TX bytes the guest writes to THR are buffered
 * in the model for the caller to drain and forward to hype's console.
 * Returns 0 if the port was in range and handled, -1 otherwise (so the
 * FW-1 IOIO chain falls through to the next device / the absorb path).
 */
int hype_svm_vcpu_handle_uart_ioio(hype_vcpu_ctx_t *ctx, hype_guest_uart_t *uart, uint16_t base_port);

/*
 * FW-1g: the QEMU/bochs debug-io port (default 0x402), where OVMF's
 * PlatformDebugLibIoPort (SEC/PEI) writes its DEBUG() log one byte at a
 * time. A read must return the 0xE9 presence signature so OVMF enables
 * the channel; a write hands the byte back in *out_byte for the caller
 * to forward to hype's console. Returns 0 if it was a write (byte in
 * *out_byte), 1 if it was a read (handled, nothing to forward), or -1
 * if the port wasn't `base_port`. Advances RIP when handled.
 */
/*
 * #286: returns 0 when the guest WROTE to the debug port, with *out_n bytes placed in
 * out_bytes; 1 for a read (the presence signature is answered internally); -1 when the
 * exit is not this port.
 *
 * A buffer rather than one byte because EDK2's PlatformDebugLibIoPort emits its DEBUG text
 * with `rep outsb` -- the data lives in guest memory at DS:RSI, so ONE exit carries a whole
 * string. `dma_map` is the guest's bounds-checked translation (VALID-1); a range outside it
 * is refused rather than read out of host memory.
 */
int hype_svm_vcpu_handle_debug_port_ioio(hype_vcpu_ctx_t *ctx, uint16_t base_port,
                                         const hype_gpa_map_t *dma_map, uint8_t *out_bytes,
                                         unsigned int out_cap, unsigned int *out_n);

/* FW-1g: enable/disable per-access tracing of guest 0x60/0x64 (PS/2)
 * accesses in hype_svm_vcpu_handle_ps2_ioio (default off). FW-1 turns it
 * on after injecting a keystroke to see whether OVMF's WaitForKey poll
 * reads the status/scancode. */
void hype_svm_set_ps2_trace(int enabled);

/* FW-1h: enable/disable per-command tracing of AHCI command-slot
 * dispatches (the ATAPI CDB opcode + resulting status, and any non-
 * PACKET command that reaches the ATAPI path) in the AHCI NPF handlers
 * (default off). FW-1's guest turns it on to see what OVMF's storage
 * stack asks the emulated CD-ROM for during boot-device discovery. */

/*
 * #315 (APM Vol 2 §15.7.2 / §15.7.3): what to do with a recorded EXITINTINFO event.
 *
 * EXITINTINFO.V set on a #VMEXIT means the intercept landed WHILE the guest was delivering an
 * event through its own IDT. For an external interrupt the processor has already run the
 * interrupt-acknowledge cycle with the PIC/APIC, so the vector is CONSUMED -- nothing will re-raise
 * it, and the VMCB field is the only surviving copy. Discarding it silently loses a device
 * interrupt, which is the class of bug that costs days (#311's twelve candidates).
 *
 * This is deliberately NOT a blanket re-stage, because a blanket re-stage is wrong in three ways:
 *
 *   - An EXCEPTION does not need re-staging. It was produced by an instruction, and restarting that
 *     instruction produces it again; re-injecting it as well delivers it twice. Only the ack'd
 *     external event is genuinely unrecoverable, which is precisely why the APM frames the mechanism
 *     around "no longer possible to recreate the event".
 *   - A SOFTWARE interrupt (INT n) has next-RIP semantics hype does not model. Re-injecting it would
 *     re-execute or skip the instruction depending on details not captured here.
 *   - If the hypervisor is ALREADY injecting something for this exit, re-staging would clobber it,
 *     since EVENTINJ holds one event.
 *
 * So the safe set is INTR and NMI, and everything else is reported rather than guessed at. Refusing
 * loudly beats a plausible-looking injection: a wrong event delivered into a guest IDT is far harder
 * to attribute than a log line saying hype declined to act.
 *
 * Pure -- no VMCB, no vCPU -- so the decision is unit tested without a VM, the same split
 * hype_svm_decode_npf_info() uses.
 */
typedef struct {
    int valid;               /* EXITINTINFO.V (bit 31) */
    unsigned int type;       /* EXITINTINFO type field (0=INTR, 2=NMI, 3=exception, 4=soft INT) */
    unsigned int vector;
    int has_error_code;      /* EXITINTINFO.EV (bit 11) */
    uint32_t error_code;
    /* The exit hype is currently handling is itself an exception intercept, and hype intends to
     * reflect it back to the guest. Both facts matter: only then is EVENTINJ already spoken for. */
    int hypervisor_will_inject;
} hype_svm_evtinfo_t;

typedef enum {
    HYPE_SVM_EVTREPLAY_NONE = 0,    /* nothing recorded -- the overwhelmingly common case */
    HYPE_SVM_EVTREPLAY_REINJECT,    /* an ack'd INTR/NMI: re-stage it, it cannot be recreated */
    HYPE_SVM_EVTREPLAY_SELF_HEALS,  /* an exception: restarting the instruction reproduces it */
    HYPE_SVM_EVTREPLAY_REFUSE       /* cannot decide safely -- report, never guess */
} hype_svm_evtreplay_t;

hype_svm_evtreplay_t hype_svm_decide_event_replay(const hype_svm_evtinfo_t *in);

/* #315: run totals, for the end-of-run diagnostic summary. A non-zero refused count is the
 * signal that a guest may have lost an event hype declined to re-stage. */
void hype_svm_get_evtreplay_counts(unsigned long *restaged, unsigned long *refused);

/* Decodes a raw EXITINTINFO qword into the struct above. `will_inject` is the caller's own
 * knowledge about this exit; nothing in the field itself can tell us that. */
void hype_svm_decode_exitintinfo(uint64_t exitintinfo, int will_inject, hype_svm_evtinfo_t *out);

/* Human-readable form, for the one log line that accompanies a non-NONE decision. */
const char *hype_svm_evtreplay_str(hype_svm_evtreplay_t d);

void hype_svm_set_ahci_trace(int enabled);

/* M4-6: when on, an MSR the allow-list doesn't recognize is logged and
 * handled permissively (RDMSR -> 0, WRMSR -> ignored) rather than being
 * fatal -- a discovery aid to reveal, in one real-guest boot, the full
 * set of MSRs a Linux kernel touches. Off by default (the handler stays
 * fail-closed for isolation). */
void hype_svm_set_msr_trace(int enabled);

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
 * later" precedent. Descriptor-chain walking handles a header, ANY
 * number of data segments, and a status descriptor (#268 -- it used to
 * require exactly 3, which capped every request at one contiguous
 * segment and made a dataless FLUSH chain uncompletable). No
 * segment-count limit is imposed, so there is nothing to advertise via
 * VIRTIO_BLK_F_SEG_MAX. Guest-supplied
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
int hype_svm_vcpu_handle_virtio_blk_npf(hype_vcpu_ctx_t *ctx, hype_virtio_blk_t *dev,
                                         const hype_blk_backend_t *be, const hype_gpa_map_t *dma_map,
                                         uint64_t mmio_base_phys, const uint8_t *insn);


/*
 * NET-2 (#81): the guest virtio-net BAR window. Same shape as the virtio-blk handler above -- the
 * transport layout is identical and the sub-offsets are shared -- with one difference that matters:
 * the NOTIFY region is per-queue, so which queue was rung is derived from the offset within it
 * rather than assumed. Ringing the wrong doorbell would drain the transmit ring on a receive
 * notify, and the symptom is packets that appear only when traffic happens to flow the other way.
 *
 * `sink` and `user` are how a frame leaves: the caller supplies them, so this file needs to know
 * nothing about NAT or peer forwarding. `scratch` is the caller's per-VM gather buffer -- see
 * core/virtio_net_ring.h on why it is not a static here.
 *
 * Exempt from unit testing for the same reason as its siblings: it reaches into the exempt VMCB
 * fields a real VMRUN produces. Everything it calls -- hype_mmio_decode(), the
 * hype_virtio_net_*_cfg_read/write() family, hype_virtio_net_drain_tx() -- is tested in isolation.
 */
/*
 * NET-3 (#82): the guest e1000's register window. A write to TDT is the transmit doorbell -- there is
 * no separate notify region -- so the drain hangs off that one register write.
 *
 * Exempt from unit testing like its siblings: it reaches into the exempt VMCB fields a real VMRUN
 * produces. Everything it calls is tested in isolation (core/tests/test_e1000_dev.c and
 * test_e1000_dev_ring.c).
 */
int hype_svm_vcpu_handle_e1000_dev_npf(hype_vcpu_ctx_t *ctx, hype_e1000_dev_t *dev,
                                       const hype_gpa_map_t *dma_map, uint64_t mmio_base_phys,
                                       hype_virtio_net_tx_fn sink, void *user, uint8_t *scratch,
                                       unsigned int scratch_len,
                                       hype_virtio_net_ring_stats_t *stats, const uint8_t *insn);

int hype_svm_vcpu_handle_virtio_net_npf(hype_vcpu_ctx_t *ctx, hype_virtio_net_t *dev,
                                        const hype_gpa_map_t *dma_map, uint64_t mmio_base_phys,
                                        hype_virtio_net_tx_fn sink, void *user, uint8_t *scratch,
                                        unsigned int scratch_len,
                                        hype_virtio_net_ring_stats_t *stats, const uint8_t *insn);

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

/* Enable/disable the per-VM-exit CLGI/VMLOAD/VMRUN trace prints in
 * hype_svm_vcpu_run (default on). A long-running guest (real OVMF, with
 * thousands of exits) disables it after the first entry so it doesn't
 * flood the console; short test guests leave it on. */
void hype_svm_set_vmrun_trace(int enabled);

extern const hype_vmm_ops_t hype_svm_ops;

/* The MSR index (guest RCX) at the last MSR intercept, for diagnostics. */
uint32_t hype_svm_vcpu_get_msr_index(hype_vcpu_ctx_t *ctx);


/*
 * #202 slice 6a: NVMe BAR0 MMIO. Same shape as the virtio-blk handler above -- decode the faulting
 * instruction, emulate the 32-bit register access, advance RIP.
 *
 * `bar_size` is passed rather than assumed so the caller and the PCI BAR cannot disagree: a handler
 * that accepted a wider range than the BAR actually claims would emulate accesses to addresses the
 * guest was never told belonged to this device.
 *
 * A write to a SUBMISSION QUEUE doorbell is what makes the controller do work, so the handler drains
 * that queue before returning. That is deliberate: hype has no worker thread, and a doorbell whose
 * commands are never fetched is indistinguishable to the guest from a dead controller.
 */
int hype_svm_vcpu_handle_nvme_npf(hype_vcpu_ctx_t *ctx, hype_nvme_t *dev,
                                  const hype_nvme_ctx_t *nctx, uint64_t mmio_base_phys,
                                  uint32_t bar_size, const uint8_t *insn);

#endif /* HYPE_ARCH_SVM_H */
