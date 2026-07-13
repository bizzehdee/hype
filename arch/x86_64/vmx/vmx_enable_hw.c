#include "vmx.h"

/* UNVALIDATED -- see vmx.h. */

static uint8_t g_vmxon_region[4096] __attribute__((aligned(4096)));

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(uint64_t v) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(v));
}

int hype_vmx_enable(void) {
    uint64_t feature_control = rdmsr(HYPE_MSR_IA32_FEATURE_CONTROL);
    uint64_t vmx_basic;
    uint32_t revision_id;
    uint64_t region_phys;
    uint8_t fail;

    if (!hype_vmx_feature_control_allows_vmxon(feature_control)) {
        return -1; /* firmware locked VMX off outside SMX; nothing we can do */
    }
    if ((feature_control & HYPE_FEATURE_CONTROL_LOCK) == 0) {
        wrmsr(HYPE_MSR_IA32_FEATURE_CONTROL,
              hype_vmx_feature_control_with_vmxon_enabled(feature_control));
    }

    write_cr4(hype_vmx_cr4_with_vmxe(read_cr4()));

    vmx_basic = rdmsr(HYPE_MSR_IA32_VMX_BASIC);
    revision_id = (uint32_t)(vmx_basic & 0x7FFFFFFFu);
    *(uint32_t *)g_vmxon_region = revision_id;

    region_phys = (uint64_t)g_vmxon_region;
    __asm__ volatile(
        "vmxon %1\n\t"
        "setc %0"
        : "=q"(fail)
        : "m"(region_phys)
        : "cc");

    return fail ? -1 : 0;
}
