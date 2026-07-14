#include "cpuid_emulate.h"

#define HYPE_CPUID_HYPERVISOR_PRESENT_BIT (1u << 31)
#define HYPE_CPUID_LEAF1_EDX_MTRR_BIT (1u << 12)
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
        out->ecx = real->ecx | HYPE_CPUID_HYPERVISOR_PRESENT_BIT;
        return;
    }

    if (eax_in == 0x80000000u) {
        out->eax = 0x80000001u; /* max extended leaf supported */
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

    if (eax_in == 0x40000000u) {
        out->eax = 0x40000000u; /* no further hypervisor-specific leaves yet */
        out->ebx = 0x65707948u; /* "Hype" */
        out->ecx = 0x65707948u; /* "Hype" */
        out->edx = 0x65707948u; /* "Hype" */
        return;
    }

    zero_result(out);
}
