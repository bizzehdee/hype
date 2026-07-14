#include "svm.h"

#include "../../../core/fatal.h"

/* Host Save Area: a 4KB scratch page the CPU saves host state into on
 * every VMRUN and restores from on every #VMEXIT. We never read it
 * ourselves (that's what the VMCB's save-state area and our own
 * pre/post-VMRUN register save/restore are for) -- it just has to
 * exist and be pointed at. */
static uint8_t g_hsave_area[4096] __attribute__((aligned(4096)));

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

/*
 * Exempt from unit testing per AGENTS.md: real RDMSR/WRMSR, nothing to
 * observe without a real CPU. hype_svm_efer_with_svme() in svm_bits.c
 * holds the only real logic (what the new EFER value should be) and is
 * fully tested.
 */
int hype_svm_enable(void) {
    uint64_t vm_cr;
    uint64_t efer;

    /* Real-hardware debugging: VM_CR.SVMDIS (bit 4) can lock SVM off
     * independently of the "SVM enabled" BIOS toggle -- if it's set,
     * the EFER WRMSR below takes a #GP that (at this point in boot,
     * before our own IDT is loaded) has nowhere safe to land. Reading
     * VM_CR itself is a plain RDMSR and cannot fault this way, so this
     * print is always safe and always the last screen/serial line
     * before the first genuinely risky instruction in this function. */
    vm_cr = rdmsr(HYPE_MSR_VM_CR);
    hype_debug_print("svm: VM_CR=0x%llx SVMDIS=%d\n", (unsigned long long)vm_cr,
                      (vm_cr & HYPE_MSR_VM_CR_SVMDIS) != 0);

    efer = rdmsr(HYPE_MSR_EFER);
    hype_debug_print("svm: EFER(before)=0x%llx -- about to WRMSR EFER (set SVME)...\n",
                      (unsigned long long)efer);
    wrmsr(HYPE_MSR_EFER, hype_svm_efer_with_svme(efer));
    hype_debug_print("svm: EFER WRMSR done -- about to WRMSR VM_HSAVE_PA=0x%llx...\n",
                      (unsigned long long)(uint64_t)(uintptr_t)g_hsave_area);
    wrmsr(HYPE_MSR_VM_HSAVE_PA, (uint64_t)g_hsave_area);
    hype_debug_print("svm: VM_HSAVE_PA WRMSR done -- SVM enable complete\n");
    return 0;
}
