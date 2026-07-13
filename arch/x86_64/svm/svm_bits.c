#include "svm.h"

#include "../cpu/lapic.h"

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
