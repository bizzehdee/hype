#include "svm.h"

#include "../cpu/lapic.h"

uint32_t hype_svm_nasid_from_cpuid_ebx(uint32_t ebx) {
    return ebx;
}

uint32_t hype_svm_asid_for_slot(unsigned slot, uint32_t nasid) {
    uint32_t max_guest_asid;

    /* NASID 0 or 1 means no usable guest ASID at all (0 belongs to the host). Report
     * 0 so the caller refuses to run rather than silently sharing the host's tag. */
    if (nasid < 2u) {
        return 0u;
    }
    max_guest_asid = nasid - 1u;
    /* slot 0 -> 1, never 0. Slots beyond the CPU's range wrap onto the top usable
     * ASID: vCPU slots are already aliased loudly when the pool is exhausted
     * (svm_alloc_vcpu_slot), and inventing an out-of-range ASID would fail VMRUN
     * instead of degrading. A shared ASID is then flushed by TLB_CONTROL. */
    if ((uint32_t)slot + 1u > max_guest_asid) {
        return max_guest_asid;
    }
    return (uint32_t)slot + 1u;
}

uint64_t hype_svm_efer_with_svme(uint64_t old_efer) {
    return old_efer | HYPE_EFER_SVME;
}

int hype_svm_guest_efer_write(uint64_t current_efer, uint64_t requested, uint64_t cr0,
                              uint64_t cr4, uint64_t *out) {
    uint64_t value;

    /* MBZ bits: a real WRMSR raises #GP, and letting one through would fail VMRUN's own
     * "Any MBZ bit of EFER is set" check -- killing hype instead of the guest. */
    if ((requested & HYPE_EFER_MBZ) != 0ull) {
        return -1;
    }
    /*
     * LMA is hardware-owned. APM §3.1.7: "When writing the EFER register the value of this bit
     * must be preserved. [...] An attempt to write a value that differs from the state
     * determined by hardware results in a #GP fault." Faulting a mismatch is what lets a guest
     * that reads-modifies-writes correctly through, while refusing one that invents long-mode
     * state it has not earned by enabling paging.
     */
    if ((requested & HYPE_EFER_LMA) != (current_efer & HYPE_EFER_LMA)) {
        return -1;
    }
    /*
     * Enabling long mode requires paging to be OFF at the moment LME is set -- software sets
     * LME first and CR0.PG second. Accepting LME while PG is already set would produce
     * exactly APM §15.5.1's "EFER.LME and CR0.PG are both set and CR4.PAE is zero" /
     * "...and CR0.PE is zero" illegal states on the next entry.
     */
    if ((requested & HYPE_EFER_LME) != 0ull && (current_efer & HYPE_EFER_LME) == 0ull &&
        (cr0 & HYPE_CR0_PG) != 0ull) {
        return -1;
    }
    /* And catch the illegal combinations directly, for the case where LME was already set. */
    if ((requested & HYPE_EFER_LME) != 0ull && (cr0 & HYPE_CR0_PG) != 0ull &&
        ((cr4 & HYPE_CR4_PAE) == 0ull || (cr0 & HYPE_CR0_PE) == 0ull)) {
        return -1;
    }

    /* RAZ bits read as zero, so they are dropped rather than faulted; SVME is non-negotiable. */
    value = (requested & ~HYPE_EFER_RAZ) | HYPE_EFER_SVME;
    *out = value;
    return 0;
}

uint64_t hype_svm_guest_efer_read(uint64_t stored_efer) {
    return stored_efer & ~HYPE_EFER_SVME;
}

static uint8_t g_avic_backing_page[4096] __attribute__((aligned(4096)));
static uint8_t g_avic_logical_table[4096] __attribute__((aligned(4096)));
static uint8_t g_avic_physical_table[4096] __attribute__((aligned(4096)));

void hype_svm_vcpu_enable_apic_accel(hype_vmcb_t *vmcb) {
    hype_vmcb_configure_avic(vmcb, HYPE_LAPIC_DEFAULT_BASE, (uint64_t)(uintptr_t)g_avic_backing_page,
                              (uint64_t)(uintptr_t)g_avic_logical_table,
                              (uint64_t)(uintptr_t)g_avic_physical_table, 0);
}
