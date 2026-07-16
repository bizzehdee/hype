#include "vmx.h"
#include "../../../core/fatal.h"

/* UNVALIDATED on real Intel silicon -- see vmx.h. Instrumented with
 * per-step checkpoints + hardware-state dumps (M2-8 Intel pass): a #GP
 * from any of WRMSR / MOV-CRn / VMXON here lands in UEFI's own exception
 * handler and hard-locks with no clue which instruction faulted, so
 * every risky step prints before AND after, and the governing MSR/CR
 * values print up front so the fix can be computed from what the CPU
 * actually reports rather than guessed. */

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

static inline uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(uint64_t v) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(v));
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
    uint64_t cr0, cr4;
    uint64_t cr0_fixed0, cr0_fixed1, cr4_fixed0, cr4_fixed1;
    uint64_t cr0_new, cr4_new;
    uint32_t revision_id;
    uint64_t region_phys;
    uint8_t fail;

    cr0 = read_cr0();
    cr4 = read_cr4();
    hype_debug_print("vmx-enable: FEATURE_CONTROL=0x%llx CR0=0x%llx CR4=0x%llx\n",
                      (unsigned long long)feature_control, (unsigned long long)cr0,
                      (unsigned long long)cr4);

    if (!hype_vmx_feature_control_allows_vmxon(feature_control)) {
        hype_debug_print("vmx-enable: FEATURE_CONTROL locks VMX OFF outside SMX -- cannot VMXON\n");
        return -1; /* clean fail, not a lock */
    }
    if ((feature_control & HYPE_FEATURE_CONTROL_LOCK) == 0) {
        hype_debug_print("vmx-enable: FEATURE_CONTROL unlocked -- WRMSR enable+lock...\n");
        wrmsr(HYPE_MSR_IA32_FEATURE_CONTROL,
              hype_vmx_feature_control_with_vmxon_enabled(feature_control));
        hype_debug_print("vmx-enable: FEATURE_CONTROL WRMSR ok (now 0x%llx)\n",
                          (unsigned long long)rdmsr(HYPE_MSR_IA32_FEATURE_CONTROL));
    }

    /* Apply the VMX-required CR0/CR4 fixed bits before VMXON. */
    cr0_fixed0 = rdmsr(HYPE_MSR_IA32_VMX_CR0_FIXED0);
    cr0_fixed1 = rdmsr(HYPE_MSR_IA32_VMX_CR0_FIXED1);
    cr4_fixed0 = rdmsr(HYPE_MSR_IA32_VMX_CR4_FIXED0);
    cr4_fixed1 = rdmsr(HYPE_MSR_IA32_VMX_CR4_FIXED1);
    hype_debug_print("vmx-enable: CR0_FIXED0=0x%llx CR0_FIXED1=0x%llx CR4_FIXED0=0x%llx CR4_FIXED1=0x%llx\n",
                      (unsigned long long)cr0_fixed0, (unsigned long long)cr0_fixed1,
                      (unsigned long long)cr4_fixed0, (unsigned long long)cr4_fixed1);

    cr0_new = hype_vmx_cr_with_fixed_bits(cr0, cr0_fixed0, cr0_fixed1);
    if (cr0_new != cr0) {
        hype_debug_print("vmx-enable: MOV CR0 0x%llx -> 0x%llx...\n",
                          (unsigned long long)cr0, (unsigned long long)cr0_new);
        write_cr0(cr0_new);
        hype_debug_print("vmx-enable: MOV CR0 ok\n");
    }
    cr4_new = hype_vmx_cr_with_fixed_bits(cr4 | HYPE_CR4_VMXE, cr4_fixed0, cr4_fixed1);
    hype_debug_print("vmx-enable: MOV CR4 0x%llx -> 0x%llx (VMXE+fixed)...\n",
                      (unsigned long long)cr4, (unsigned long long)cr4_new);
    write_cr4(cr4_new);
    hype_debug_print("vmx-enable: MOV CR4 ok\n");

    vmx_basic = rdmsr(HYPE_MSR_IA32_VMX_BASIC);
    revision_id = (uint32_t)(vmx_basic & 0x7FFFFFFFu);
    *(uint32_t *)g_vmxon_region = revision_id;
    region_phys = (uint64_t)g_vmxon_region;
    hype_debug_print("vmx-enable: VMX_BASIC=0x%llx rev_id=0x%x vmxon_region_phys=0x%llx -- VMXON...\n",
                      (unsigned long long)vmx_basic, (unsigned int)revision_id,
                      (unsigned long long)region_phys);

    __asm__ volatile(
        "vmxon %1\n\t"
        "setc %0"
        : "=q"(fail)
        : "m"(region_phys)
        : "cc");

    hype_debug_print("vmx-enable: VMXON returned (CF/fail=%d)\n", (int)fail);
    return fail ? -1 : 0;
}
