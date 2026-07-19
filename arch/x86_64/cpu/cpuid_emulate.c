#include "cpuid_emulate.h"

#define HYPE_CPUID_HYPERVISOR_PRESENT_BIT (1u << 31)
#define HYPE_CPUID_LEAF1_EDX_MTRR_BIT (1u << 12)
#define HYPE_CPUID_LEAF1_ECX_TSC_DEADLINE_BIT (1u << 24)
#define HYPE_CPUID_LEAF1_ECX_XSAVE_BIT (1u << 26)
#define HYPE_CPUID_LEAF1_ECX_OSXSAVE_BIT (1u << 27)
#define HYPE_CPUID_EXT1_ECX_SVM_BIT (1u << 2)
#define HYPE_CPUID_EXT7_EDX_INVARIANT_TSC_BIT (1u << 8)
#define HYPE_CPUID_LEAF6_EAX_ARAT_BIT (1u << 2)
/* Max basic leaf hype reports (leaf 0 EAX). Raised from 1 to 0x16 so the
 * guest queries the TSC-frequency leaves 0x15/0x16 below. */
#define HYPE_CPUID_MAX_BASIC_LEAF 0x16u

static void zero_result(hype_cpuid_result_t *out) {
    out->eax = 0;
    out->ebx = 0;
    out->ecx = 0;
    out->edx = 0;
}

void hype_cpuid_emulate(uint32_t eax_in, uint32_t ecx_in, const hype_cpuid_result_t *real,
                         uint32_t tsc_khz, hype_cpuid_result_t *out) {
    (void)ecx_in; /* no leaf handled here uses a sub-leaf */

    if (eax_in == 0) {
        out->eax = HYPE_CPUID_MAX_BASIC_LEAF; /* max basic leaf supported */
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
         * and inject. Also clear XSAVE (26) + OSXSAVE (27): raising the
         * max basic leaf to 0x16 (for the TSC leaves) exposes leaf 0xD
         * (XSAVE enumeration) which this project doesn't model -- the
         * guest already ran FXSAVE-only when leaf 0xD was hidden, so
         * clearing XSAVE keeps that exact behavior and avoids unmediated
         * XCR0/XSETBV. */
        out->ecx = (real->ecx | HYPE_CPUID_HYPERVISOR_PRESENT_BIT) &
                   ~(HYPE_CPUID_LEAF1_ECX_TSC_DEADLINE_BIT | HYPE_CPUID_LEAF1_ECX_XSAVE_BIT |
                     HYPE_CPUID_LEAF1_ECX_OSXSAVE_BIT);
        return;
    }

    if (eax_in == 0x15u) {
        /* TSC / core-crystal clock ratio. Hand the guest an exact TSC
         * frequency so Linux keeps the TSC as its clocksource instead of
         * failing PIT-based calibration ("could not calculate TSC khz" ->
         * TSC unstable, observed on real HW: hype's PIT is advanced in
         * VM-exit-sized lumps, too noisy for quick_pit_calibrate). Linux
         * reads tsc_khz = (ECX/1000) * EBX/EAX; with EAX=EBX=1 and
         * ECX=tsc_khz*1000 that is exactly tsc_khz. tsc_khz==0 (not
         * published) -> all-zero, guest calibrates the legacy way. */
        if (tsc_khz != 0) {
            out->eax = 1;                 /* ratio denominator */
            out->ebx = 1;                 /* ratio numerator   */
            out->ecx = tsc_khz * 1000u;   /* core crystal clock, Hz */
            out->edx = 0;
        } else {
            out->eax = 0;
            out->ebx = 0;
            out->ecx = 0;
            out->edx = 0;
        }
        return;
    }

    if (eax_in == 0x16u) {
        /* Processor frequency leaf. EAX = base MHz -- a fallback Linux
         * uses for tsc_khz if it doesn't get it from leaf 0x15. */
        out->eax = tsc_khz / 1000u; /* kHz -> MHz (0 when tsc_khz==0) */
        out->ebx = 0;
        out->ecx = 0;
        out->edx = 0;
        return;
    }

    if (eax_in == 6) {
        /* M4-6b5 (b5c): Thermal & Power Management leaf. Advertise ONLY ARAT
         * (Always Running APIC Timer, EAX bit 2). hype's virtual guest LAPIC
         * timer is advanced from real elapsed time on every VM-exit,
         * independent of the guest's idle/C-state -- so it never stops, which
         * is exactly what ARAT asserts. Without this bit Linux flags the LAPIC
         * timer CLOCK_EVT_FEAT_C3STOP (assumes it halts in deep idle) and hands
         * IDLE timekeeping to the 100 Hz PIT broadcast device -> 10 ms-quantised
         * idle wakeups -> the ~22x-slow real-HW boot (confirmed: the guest arms
         * the LAPIC timer then re-masks it into broadcast mode). All
         * thermal/power-management bits stay 0 -- unmodeled, not
         * guest-isolation-relevant. */
        out->eax = HYPE_CPUID_LEAF6_EAX_ARAT_BIT;
        out->ebx = 0;
        out->ecx = 0;
        out->edx = 0;
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

    if (eax_in == 0x80000007u) {
        /* Advanced Power Management Information leaf. Advertise ONLY
         * Invariant TSC (EDX bit 8): hype passes the host TSC straight
         * through to the guest, and a modern AMD host TSC is invariant
         * (constant rate regardless of P-/C-states). Without this bit the
         * guest's clocksource watchdog sees the emulated PIT/PM-timer drift
         * against the raw TSC and marks the TSC unstable, falling back to a
         * slow, skewed clock ("clock skew detected" -- observed on real HW)
         * that makes timed boot work run long. All power-management bits
         * (EDX 0-7, and EAX/EBX/ECX) stay 0 -- unmodeled and not
         * guest-isolation-relevant. */
        out->eax = 0;
        out->ebx = 0;
        out->ecx = 0;
        out->edx = HYPE_CPUID_EXT7_EDX_INVARIANT_TSC_BIT;
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
