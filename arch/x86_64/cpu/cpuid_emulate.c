#include "cpuid_emulate.h"

#define HYPE_CPUID_HYPERVISOR_PRESENT_BIT (1u << 31)
#define HYPE_CPUID_LEAF1_EDX_MTRR_BIT (1u << 12)
#define HYPE_CPUID_LEAF1_ECX_TSC_DEADLINE_BIT (1u << 24)
#define HYPE_CPUID_EXT1_ECX_SVM_BIT (1u << 2)

static void zero_result(hype_cpuid_result_t *out) {
    out->eax = 0;
    out->ebx = 0;
    out->ecx = 0;
    out->edx = 0;
}

void hype_cpuid_emulate(uint32_t eax_in, uint32_t ecx_in, const hype_cpuid_result_t *real,
                         hype_cpuid_result_t *out) {
    (void)ecx_in; /* no leaf handled here uses a sub-leaf */

    if (eax_in == 0) {
        out->eax = 1; /* max basic leaf supported */
        out->ebx = 0x68747541u; /* "Auth" */
        out->edx = 0x69746e65u; /* "enti" */
        out->ecx = 0x444d4163u; /* "cAMD" */
        return;
    }

    if (eax_in == 1) {
        out->eax = real->eax;
        out->ebx = real->ebx;
        out->edx = real->edx & ~HYPE_CPUID_LEAF1_EDX_MTRR_BIT;
        /* Hypervisor-present set; MTRR already cleared above. Also clear
         * TSC_DEADLINE (ECX bit 24): with it set, a guest OS arms its
         * LAPIC timer via the IA32_TSC_DEADLINE MSR (0x6e0) -- a mode
         * this project does not model, so no timer interrupt would ever
         * fire and the guest's scheduler stalls (a real Linux kernel
         * idle-HLTs forever right after unpacking its initramfs).
         * Clearing it makes the guest fall back to the LAPIC timer's
         * initial-count mode, which FW-1b's guest LAPIC model does drive
         * and inject. */
        out->ecx = (real->ecx | HYPE_CPUID_HYPERVISOR_PRESENT_BIT) &
                   ~HYPE_CPUID_LEAF1_ECX_TSC_DEADLINE_BIT;
        return;
    }

    if (eax_in == 0x80000000u) {
        out->eax = 0x80000008u; /* max extended leaf supported */
        out->ebx = 0x68747541u; /* "Auth" */
        out->edx = 0x69746e65u; /* "enti" */
        out->ecx = 0x444d4163u; /* "cAMD" */
        return;
    }

    if (eax_in == 0x80000001u) {
        out->eax = real->eax;
        out->edx = real->edx;
        out->ecx = real->ecx & ~HYPE_CPUID_EXT1_ECX_SVM_BIT;
        out->ebx = 0;
        return;
    }

    if (eax_in == 0x80000008u) {
        /* EAX bits 7:0/15:8 = physical/linear address widths -- real
         * firmware's own page-table setup (SEC/PEI, before permanent
         * memory is even found) reads this to decide how many
         * page-table levels/entries to build. Under-reporting it (this
         * leaf previously wasn't recognized at all, falling through to
         * the safe-looking but wrong all-zero default) let firmware
         * build page tables sized for a 0-bit address space, walking
         * off the end of them almost immediately -- confirmed via
         * FW-1's own real-OVMF boot attempt (a guest #PF, not an NPF,
         * right after CR0.PG was set). Passed through as-is: address
         * width isn't guest-isolation-sensitive, only correctness-
         * sensitive here. */
        out->eax = real->eax;
        out->ebx = real->ebx;
        out->ecx = real->ecx;
        out->edx = real->edx;
        return;
    }

    if (eax_in == 0x40000000u) {
        out->eax = 0x40000000u; /* no further hypervisor-specific leaves yet */
        out->ebx = 0x65707948u; /* "Hype" */
        out->ecx = 0x65707948u; /* "Hype" */
        out->edx = 0x65707948u; /* "Hype" */
        return;
    }

    zero_result(out);
}
