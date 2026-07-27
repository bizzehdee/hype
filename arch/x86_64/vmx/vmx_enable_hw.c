#include "vmx.h"
#include "../../../core/fatal.h"

/* UNVALIDATED on real Intel silicon -- see vmx.h. Instrumented with
 * per-step checkpoints + hardware-state dumps (M2-8 Intel pass): a #GP
 * from any of WRMSR / MOV-CRn / VMXON here lands in UEFI's own exception
 * handler and hard-locks with no clue which instruction faulted, so
 * every risky step prints before AND after, and the governing MSR/CR
 * values print up front so the fix can be computed from what the CPU
 * actually reports rather than guessed. */

/* The BSP's own VMXON region. VMXON is per-logical-processor and the CPU keeps
 * using this page for as long as that core is in VMX operation, so an AP MUST
 * NOT share it -- each core passes its own page to hype_vmx_enable_on() (the
 * exact counterpart of SVM's per-core VM_HSAVE_PA area). */
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

/*
 * The whole enable sequence, parameterised by which core's VMXON region to use
 * and whether to narrate. Every step here (FEATURE_CONTROL, the CR0/CR4 fixed
 * bits, VMXON itself) is per-logical-processor, so an AP must run all of it --
 * which is exactly why this is one shared function rather than a second,
 * simplified copy: the AP takes the code path already validated on real Intel
 * silicon, not a lookalike.
 *
 * `verbose` exists only because the BSP's narration is load-bearing for
 * real-hardware debugging (a #GP from any step lands in UEFI's handler and
 * hard-locks with no clue which instruction faulted) while an AP printing the
 * same lines would race the BSP's console -- see fw_1_ap_main in boot/main.c.
 */
static int vmx_enable_on_region(uint8_t *vmxon_region, int verbose) {
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
    if (verbose) hype_debug_print("vmx-enable: FEATURE_CONTROL=0x%llx CR0=0x%llx CR4=0x%llx\n",
                      (unsigned long long)feature_control, (unsigned long long)cr0,
                      (unsigned long long)cr4);

    if (!hype_vmx_feature_control_allows_vmxon(feature_control)) {
        if (verbose) hype_debug_print("vmx-enable: FEATURE_CONTROL locks VMX OFF outside SMX -- cannot VMXON\n");
        return -1; /* clean fail, not a lock */
    }
    if ((feature_control & HYPE_FEATURE_CONTROL_LOCK) == 0) {
        if (verbose) hype_debug_print("vmx-enable: FEATURE_CONTROL unlocked -- WRMSR enable+lock...\n");
        wrmsr(HYPE_MSR_IA32_FEATURE_CONTROL,
              hype_vmx_feature_control_with_vmxon_enabled(feature_control));
        if (verbose) hype_debug_print("vmx-enable: FEATURE_CONTROL WRMSR ok (now 0x%llx)\n",
                          (unsigned long long)rdmsr(HYPE_MSR_IA32_FEATURE_CONTROL));
    }

    /* Apply the VMX-required CR0/CR4 fixed bits before VMXON. */
    cr0_fixed0 = rdmsr(HYPE_MSR_IA32_VMX_CR0_FIXED0);
    cr0_fixed1 = rdmsr(HYPE_MSR_IA32_VMX_CR0_FIXED1);
    cr4_fixed0 = rdmsr(HYPE_MSR_IA32_VMX_CR4_FIXED0);
    cr4_fixed1 = rdmsr(HYPE_MSR_IA32_VMX_CR4_FIXED1);
    if (verbose) hype_debug_print("vmx-enable: CR0_FIXED0=0x%llx CR0_FIXED1=0x%llx CR4_FIXED0=0x%llx CR4_FIXED1=0x%llx\n",
                      (unsigned long long)cr0_fixed0, (unsigned long long)cr0_fixed1,
                      (unsigned long long)cr4_fixed0, (unsigned long long)cr4_fixed1);

    cr0_new = hype_vmx_cr_with_fixed_bits(cr0, cr0_fixed0, cr0_fixed1);
    if (cr0_new != cr0) {
        if (verbose) hype_debug_print("vmx-enable: MOV CR0 0x%llx -> 0x%llx...\n",
                          (unsigned long long)cr0, (unsigned long long)cr0_new);
        write_cr0(cr0_new);
        if (verbose) hype_debug_print("vmx-enable: MOV CR0 ok\n");
    }
    cr4_new = hype_vmx_cr_with_fixed_bits(cr4 | HYPE_CR4_VMXE, cr4_fixed0, cr4_fixed1);
    if (verbose) hype_debug_print("vmx-enable: MOV CR4 0x%llx -> 0x%llx (VMXE+fixed)...\n",
                      (unsigned long long)cr4, (unsigned long long)cr4_new);
    write_cr4(cr4_new);
    if (verbose) hype_debug_print("vmx-enable: MOV CR4 ok\n");

    vmx_basic = rdmsr(HYPE_MSR_IA32_VMX_BASIC);
    revision_id = (uint32_t)(vmx_basic & 0x7FFFFFFFu);
    *(uint32_t *)vmxon_region = revision_id;
    region_phys = (uint64_t)(uintptr_t)vmxon_region;
    if (verbose) hype_debug_print("vmx-enable: VMX_BASIC=0x%llx rev_id=0x%x vmxon_region_phys=0x%llx -- VMXON...\n",
                      (unsigned long long)vmx_basic, (unsigned int)revision_id,
                      (unsigned long long)region_phys);

    __asm__ volatile(
        "vmxon %1\n\t"
        "setc %0"
        : "=q"(fail)
        : "m"(region_phys)
        : "cc");

    if (verbose) hype_debug_print("vmx-enable: VMXON returned (CF/fail=%d)\n", (int)fail);
    return fail ? -1 : 0;
}

int hype_vmx_enable(void) {
    return vmx_enable_on_region(g_vmxon_region, 1);
}

/* Per-core counterpart of hype_svm_enable_on(): enters VMX operation on the
 * CALLING core using `vmxon_region` (a caller-owned, 4KB-aligned page that must
 * stay live for as long as this core is in VMX operation). Silent, for the same
 * reason the SVM one is -- an AP shares the BSP's console. */
int hype_vmx_enable_on(void *vmxon_region) {
    return vmx_enable_on_region((uint8_t *)vmxon_region, 0);
}
