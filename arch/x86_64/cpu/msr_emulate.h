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

typedef enum {
    /* Unknown MSR, or a write to a read-only one -- the caller's job
     * to treat as fatal, matching every other fail-closed handler
     * here. */
    HYPE_MSR_ACTION_REJECT = 0,
    /* Read-only: return hype_msr_apic_base_value()'s fixed value. */
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
     * Read/write the hypercall-page control MSR. The value is stored and returned,
     * and the enable bit is honoured only to the extent of reading back what was
     * written -- hype services no hypercalls (see #300). This is not a dead-feature
     * advertisement: CPUID leaf 0x40000004 recommends ZERO enlightenments, so a
     * conforming guest establishes the page and then has no reason to call through
     * it.
     */
    HYPE_MSR_ACTION_READWRITE_HV_HYPERCALL,
    /* Read-only: the virtual processor index, which is hype's vCPU index. */
    HYPE_MSR_ACTION_READ_HV_VP_INDEX,
    /*
     * Read-only: the partition reference counter, in 100ns units. Caller converts
     * from the guest timebase with hype_msr_hv_ref_count_from_tsc(). Read-only
     * because it is a counter, not a settable clock -- a write is a guest bug.
     */
    HYPE_MSR_ACTION_READ_HV_TIME_REF_COUNT
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
uint64_t hype_msr_apic_base_value(void);

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
