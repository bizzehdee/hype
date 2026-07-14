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
    HYPE_MSR_ACTION_READ_TSC
} hype_msr_action_t;

/*
 * Decides what should happen for RDMSR (is_write=0) or WRMSR
 * (is_write=1) against `msr_number`. Pure logic, no CPU/VMCB access of
 * its own -- fully unit tested.
 */
hype_msr_action_t hype_msr_decide(uint32_t msr_number, int is_write);

/*
 * Fixed, synthesized APIC_BASE MSR value for this project's
 * single-vCPU scope: the real LAPIC base (arch/x86_64/cpu/lapic.h's
 * HYPE_LAPIC_DEFAULT_BASE) with Global Enable (bit 11) and BSP (bit 8)
 * set -- x2APIC (bit 10) left clear, matching M2-4's AVIC scope
 * (xAPIC only so far). Pure computation, no CPU access -- fully unit
 * tested.
 */
uint64_t hype_msr_apic_base_value(void);

#endif /* HYPE_ARCH_MSR_EMULATE_H */
