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

static uint8_t g_avic_backing_page[4096] __attribute__((aligned(4096)));
static uint8_t g_avic_logical_table[4096] __attribute__((aligned(4096)));
static uint8_t g_avic_physical_table[4096] __attribute__((aligned(4096)));

void hype_svm_vcpu_enable_apic_accel(hype_vmcb_t *vmcb) {
    hype_vmcb_configure_avic(vmcb, HYPE_LAPIC_DEFAULT_BASE, (uint64_t)(uintptr_t)g_avic_backing_page,
                              (uint64_t)(uintptr_t)g_avic_logical_table,
                              (uint64_t)(uintptr_t)g_avic_physical_table, 0);
}
