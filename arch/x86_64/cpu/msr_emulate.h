#ifndef HYPE_ARCH_MSR_EMULATE_H
#define HYPE_ARCH_MSR_EMULATE_H

#include <stdint.h>

/*
 * MSR interception (CPUMSR-2). Confirmed by grepping svm_vcpu.c that
 * g_msrpm was wired into both VMCBs' msrpm_base_pa but never filled --
 * stayed all-zero ("intercept nothing"), unlike g_iopm, which the
 * long-mode guest explicitly fills with 0xFF. Every guest RDMSR/WRMSR
 * reached real hardware unmediated, the same class of guest-isolation
 * gap CPUMSR-1 fixed for CPUID.
 *
 * HYPE_SVM_INTERCEPT_MSR_PROT (bit 28 of intercept_misc1) and
 * HYPE_SVM_EXITCODE_MSR (0x7C) are defined in arch/x86_64/svm/vmcb.h --
 * this project's own comment there already documented bit 28's
 * position, just never defined/set it.
 *
 * Design: a small, explicit allow-list rather than a full MSR
 * emulation layer -- CPUMSR-1's leaf-1 MTRR bit is already forced
 * clear specifically so well-behaved guest software never attempts an
 * MTRR MSR access in the first place, narrowing what actually needs
 * handling here. Everything not on the allow-list is fail-closed,
 * matching every other unrecognized-access convention already
 * established in this project (IOIO/NPF handlers) -- iterate this
 * list based on what a real OVMF/GRUB/Linux boot log actually demands,
 * not by guessing every possible MSR upfront.
 */

#define HYPE_MSR_NUMBER_APIC_BASE 0x1Bu
#define HYPE_MSR_NUMBER_TSC 0x10u

/*
 * #601: the x2APIC MSR interface (Intel SDM Vol 3A 10.12), MSRs 0x800-0x8FF. Every
 * x2APIC register lives at MSR address 0x800 + (its xAPIC MMIO byte offset >> 4) --
 * e.g. the ID register (MMIO 0x020) is MSR 0x802 -- with two exceptions handled in
 * devices/guest_lapic.h, not here: the ICR folds xAPIC's two 32-bit halves
 * (0x300/0x310) into one 64-bit MSR (0x830), and Self IPI (0x83F) has no xAPIC MMIO
 * counterpart at all. This module only recognizes the range; devices/guest_lapic.h
 * owns what each register means, so both access paths (MMIO trap and this MSR
 * range) read/write the exact same per-vCPU state.
 */
#define HYPE_MSR_X2APIC_RANGE_BASE 0x800u
#define HYPE_MSR_X2APIC_RANGE_LAST 0x8FFu

/*
 * #601: the IA32_APIC_BASE mode this vCPU's LAPIC is in. Numerically matches
 * devices/guest_lapic.h's HYPE_GUEST_LAPIC_MODE_* constants (0/1/2) -- kept as
 * separate #defines rather than a shared header because devices/ must not depend
 * on arch/x86_64/cpu/ (the reverse dependency already exists via svm.h/vmcs.h
 * including guest_lapic.h). The _hw callers cast between them; a comment at each
 * cast site says so.
 */
#define HYPE_APIC_MODE_DISABLED 0
#define HYPE_APIC_MODE_XAPIC 1
#define HYPE_APIC_MODE_X2APIC 2
/* IA32_FS_BASE / IA32_GS_BASE (#251). Duplicated here rather than pulled from a
 * backend header, same as HYPE_MSR_NUMBER_EFER above: these are architectural
 * constants, and this module deliberately depends on nothing. */
#define HYPE_MSR_NUMBER_FS_BASE 0xC0000100u
#define HYPE_MSR_NUMBER_GS_BASE 0xC0000101u

/*
 * Hyper-V synthetic MSRs (M7-1, #91). Live only for a vCPU whose Hyper-V CPUID leaves
 * are enabled -- hype_msr_decide_ex() takes that as a parameter, for the same
 * per-vCPU reason cpuid_emulate.h explains. A guest that was never told "Hv#1" and
 * pokes these still gets the fail-closed REJECT.
 */
#define HYPE_MSR_NUMBER_HV_GUEST_OS_ID 0x40000000u
#define HYPE_MSR_NUMBER_HV_HYPERCALL 0x40000001u
#define HYPE_MSR_NUMBER_HV_VP_INDEX 0x40000002u
#define HYPE_MSR_NUMBER_HV_TIME_REF_COUNT 0x40000020u
#define HYPE_MSR_NUMBER_HV_REFERENCE_TSC 0x40000021u
#define HYPE_MSR_NUMBER_HV_TSC_FREQUENCY 0x40000022u
#define HYPE_MSR_NUMBER_HV_APIC_FREQUENCY 0x40000023u

typedef enum {
    /* Unknown MSR, or a write to a read-only one -- the caller's job
     * to treat as fatal, matching every other fail-closed handler
     * here. */
    HYPE_MSR_ACTION_REJECT = 0,
    /* Read-only: return hype_msr_apic_base_value()'s value for this vCPU. */
    HYPE_MSR_ACTION_READ_APIC_BASE,
    /* Read/write: route directly to/from the VMCB's own
     * save.efer field (already the guest's tracked EFER state --
     * no new storage needed). */
    HYPE_MSR_ACTION_READWRITE_EFER,
    /* Read-only: caller computes real rdtsc() + the VMCB's own
     * tsc_offset control field. */
    HYPE_MSR_ACTION_READ_TSC,
    /*
     * Read/write the guest's FS/GS segment base (#251). Distinct actions rather
     * than one "segment base" action because the two land in different fields and
     * the caller must not have to re-decode the MSR number to tell them apart.
     *
     * These exist because a 64-bit guest addresses per-CPU data through GS, so
     * absorbing the write is not benign: the Linux kernel's very first
     * `MOV RAX, GS:[0x28]` (stack canary / per-CPU) faults. AMD never needed them
     * -- SVM's vmload/vmsave save and restore FS.base/GS.base architecturally --
     * so on VMX they must be applied to GUEST_FS_BASE/GUEST_GS_BASE by hand.
     *
     * IA32_KERNEL_GS_BASE is deliberately NOT here: it has no VMCS field, and
     * SWAPGS does not cause a VM exit, so a value tracked only at WRMSR time
     * would be wrong the moment the guest swaps. It needs the VM-entry/exit
     * MSR-load/store areas or bitmap passthrough instead.
     */
    HYPE_MSR_ACTION_READWRITE_FS_BASE,
    HYPE_MSR_ACTION_READWRITE_GS_BASE,
    /*
     * Read/write the guest's Hyper-V OS identity. Windows writes this
     * unconditionally as soon as it recognizes the "Hv#1" interface signature, and
     * before it can have read anything back -- so the write MUST be absorbed, not
     * rejected: a #GP there is fatal to the guest that early.
     */
    HYPE_MSR_ACTION_READWRITE_HV_GUEST_OS_ID,
    /*
     * Read/write the hypercall-page control MSR. An enabled write installs the
     * vendor-specific call stub into the guest page. CPUID leaf 0x40000004 remains
     * zero because hype does not implement any recommended enlightenment calls.
     */
    HYPE_MSR_ACTION_READWRITE_HV_HYPERCALL,
    /* Read-only: the virtual processor index, which is hype's vCPU index. */
    HYPE_MSR_ACTION_READ_HV_VP_INDEX,
    /*
     * Read-only: the partition reference counter, in 100ns units. Caller converts
     * from the guest timebase with hype_msr_hv_ref_count_from_tsc(). Read-only
     * because it is a counter, not a settable clock -- a write is a guest bug.
     */
    HYPE_MSR_ACTION_READ_HV_TIME_REF_COUNT,
    /*
     * #436: read-only frequency MSRs (TLFS AccessFrequencyMsrs). Windows'
     * bootlib reads HV_X64_MSR_TSC_FREQUENCY instead of calibrating the TSC
     * itself when the privilege bit is set -- its own calibration against
     * hype's timing sources produced a wildly wrong frequency, making every
     * bootlib Stall() near-infinite (cdboot's "press any key" hang, wedge #4).
     */
    HYPE_MSR_ACTION_READ_HV_TSC_FREQUENCY,
    HYPE_MSR_ACTION_READ_HV_APIC_FREQUENCY,
    /* #436: the reference TSC page -- Windows' primary clock under Hyper-V. */
    HYPE_MSR_ACTION_READWRITE_HV_REFERENCE_TSC
} hype_msr_action_t;

/*
 * Decides what should happen for RDMSR (is_write=0) or WRMSR
 * (is_write=1) against `msr_number`. Pure logic, no CPU/VMCB access of
 * its own -- fully unit tested.
 */
hype_msr_action_t hype_msr_decide(uint32_t msr_number, int is_write);

/*
 * As hype_msr_decide(), plus the Hyper-V synthetic MSRs when `hv_enabled`.
 * hype_msr_decide() is the hv_enabled=0 case.
 */
hype_msr_action_t hype_msr_decide_ex(uint32_t msr_number, int is_write, int hv_enabled);

/*
 * Fixed, synthesized APIC_BASE MSR value for this project's
 * single-vCPU scope: the real LAPIC base (arch/x86_64/cpu/lapic.h's
 * HYPE_LAPIC_DEFAULT_BASE) with Global Enable (bit 11) and BSP (bit 8)
 * set -- x2APIC (bit 10) left clear, matching M2-4's AVIC scope
 * (xAPIC only so far). Pure computation, no CPU access -- fully unit
 * tested.
 */
/*
 * #190: `is_bsp` drives bit 8, the BSP flag. It is NOT cosmetic. edk2's PEI
 * MpInitLib branches on it to decide how to find CpuMpData:
 *
 *   if (ApicBaseMsr.Bits.BSP == 1)  CpuMpData = GetCpuMpDataFromGuidedHob();
 *   else                            CpuMpData = ApStackData->MpData;
 *
 * The HOB path needs PEI services, which an AP cannot reach -- the shared AP
 * IDT has no PEI Services pointer below it. An AP told BSP=1 therefore takes
 * the BSP branch and faults on a null service table.
 */
uint64_t hype_msr_apic_base_value(int is_bsp);

/*
 * #601: as hype_msr_apic_base_value(), but reporting bits 11 (EN) and 10 (EXTD)
 * for the given `apic_mode` (a HYPE_APIC_MODE_* value) instead of hardcoding
 * "always xAPIC-enabled". hype_msr_apic_base_value() is the
 * HYPE_APIC_MODE_XAPIC case, so its result is unchanged by this addition.
 * Pure computation -- fully unit tested.
 */
uint64_t hype_msr_apic_base_value_mode(int is_bsp, int apic_mode);

/*
 * #601: is `msr_number` in the x2APIC MSR range (0x800-0x8FF)? Pure range
 * check -- fully unit tested. Whether the range is actually LEGAL to touch
 * still depends on the vCPU's current APIC mode (HYPE_APIC_MODE_X2APIC), which
 * this function does not know and does not need to.
 */
int hype_msr_is_x2apic_range(uint32_t msr_number);

/*
 * #601: the Intel SDM's IA32_APIC_BASE state machine (Vol 3A, "Local APIC",
 * the IA32_APIC_BASE MSR section). `current` and `*next_out` are
 * HYPE_APIC_MODE_* values; `want_en`/`want_extd` are the EN (bit 11) and EXTD
 * (bit 10) bits the guest is writing.
 *
 *   want_en=0, want_extd=0 -> Disabled   want_en=1, want_extd=0 -> xAPIC
 *   want_en=1, want_extd=1 -> x2APIC     want_en=0, want_extd=1 -> not a state (always illegal)
 *
 * xAPIC -> x2APIC and Disabled -> either enabled mode are legal upgrades.
 * x2APIC -> xAPIC directly is the one guest-reachable illegal transition: the
 * SDM requires a full disable (-> Disabled) first, then a separate write back
 * into xAPIC. Writing the CURRENT mode back is a legal no-op.
 *
 * Returns 0 and fills *next_out on a legal transition; returns -1 (the
 * caller injects #GP(0), same convention as this module's Hyper-V hypercall
 * actions) on an illegal one, leaving *next_out untouched. Pure logic, no
 * vCPU/MSR access of its own -- fully unit tested against every combination.
 */
int hype_apic_base_mode_transition(int current, int want_en, int want_extd, int *next_out);

/*
 * Convert a TSC delta into Hyper-V reference-counter units (100ns ticks), given the
 * TSC frequency in kHz.
 *
 * Ordered to keep the arithmetic in range: `tsc_delta / khz` would throw away up to a
 * millisecond of resolution, and `tsc_delta * 10000` overflows 64 bits after about
 * 21 days at 3GHz. Splitting into whole milliseconds plus a remainder keeps full
 * 100ns resolution with no overflow for any uptime hype will see.
 *
 * Returns 0 for a zero frequency rather than dividing by it -- an unknown timebase
 * reports "no time has passed", which a guest treats as a stalled clock it can
 * notice, instead of faulting.
 */
uint64_t hype_msr_hv_ref_count_from_tsc(uint64_t tsc_delta, uint64_t tsc_khz);

#endif /* HYPE_ARCH_MSR_EMULATE_H */
