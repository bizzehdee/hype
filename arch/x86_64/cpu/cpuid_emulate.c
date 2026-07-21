#include "cpuid_emulate.h"

#define HYPE_CPUID_HYPERVISOR_PRESENT_BIT (1u << 31)
#define HYPE_CPUID_LEAF1_EDX_MTRR_BIT (1u << 12)
#define HYPE_CPUID_LEAF1_ECX_TSC_DEADLINE_BIT (1u << 24)
#define HYPE_CPUID_LEAF1_ECX_X2APIC_BIT (1u << 21)
/* Highest basic leaf hype exposes. Must reach leaf 0xD (XSAVE state-component
 * enumeration) so the guest sees a COHERENT instruction-capability picture:
 * leaf 1 ECX passes the host's XSAVE(26)/OSXSAVE(27)/AVX(28)/FMA(12)/F16C(29)
 * bits straight through, so leaf 7 (AVX2/AVX-512/BMI/...) and leaf 0xD (the
 * XSAVE area sizes the guest needs to enable XCR0) must be reachable and
 * truthful too -- otherwise glibc resolves ifunc string/memcpy routines to AVX
 * variants off leaf 1 but the kernel never enabled XSAVE (leaf 0xD read as 0),
 * and the first AVX instruction faults -> early-userspace coredump (observed:
 * udevadm cores, fsnotify teardown hangs the Ubuntu/Fedora boot; musl/Alpine,
 * using no AVX ifuncs, was unaffected). hype is a type-1 VMM with one pinned
 * vCPU; SVM VMEXIT/VMRUN leave the x87/SSE/AVX register file untouched and
 * hype's handlers use no vector state, so guest XSAVE/AVX state persists across
 * exits with no explicit XSAVE/XRSTOR, and XSETBV is not intercepted (the guest
 * sets XCR0 natively). */
#define HYPE_CPUID_MAX_BASIC_LEAF 0x0Du
#define HYPE_CPUID_LEAF_STRUCTURED_EXT 0x07u
#define HYPE_CPUID_LEAF_XSAVE 0x0Du
/* Leaf-7 sub-leaf-0 EDX speculation-control mitigation bits, forced clear
 * because their control MSRs are not emulated: SPEC_CTRL(26)/STIBP(27)/
 * SSBD(31) need IA32_SPEC_CTRL(0x48); L1D_FLUSH(28) needs IA32_FLUSH_CMD(0x10b);
 * ARCH_CAPABILITIES(29) needs IA32_ARCH_CAPABILITIES(0x10a); CORE_CAPABILITIES(30)
 * needs IA32_CORE_CAPABILITIES(0xcf). Advertising them while the MSR reads back
 * dead leaves the guest's mitigation state self-inconsistent. */
#define HYPE_CPUID_LEAF7_EDX_SPECCTRL_MASK                                     \
    ((1u << 26) | (1u << 27) | (1u << 28) | (1u << 29) | (1u << 30) | (1u << 31))
#define HYPE_CPUID_EXT1_ECX_SVM_BIT (1u << 2)
#define HYPE_CPUID_EXT7_EDX_INVARIANT_TSC_BIT (1u << 8)
#define HYPE_CPUID_LEAF6_EAX_ARAT_BIT (1u << 2)

/* KVM paravirt CPUID (kvmclock). Signature leaf 0x40000000 reports "KVMKVMKVM"
 * (EBX/ECX/EDX) and the max KVM leaf (EAX); the features leaf 0x40000001 EAX
 * carries the KVM_FEATURE_* bits. Kept here (CPUID domain) rather than pulled
 * from devices/pvclock.h to avoid an arch->devices header dependency; the two
 * must agree (a mismatch just means the guest doesn't enable kvmclock). */
#define HYPE_CPUID_KVM_FEATURES_LEAF 0x40000001u
#define HYPE_CPUID_KVM_SIG_EBX 0x4b4d564bu /* "KVMK" */
#define HYPE_CPUID_KVM_SIG_ECX 0x564b4d56u /* "VMKV" */
#define HYPE_CPUID_KVM_SIG_EDX 0x0000004du /* "M\0\0\0" */
#define HYPE_CPUID_KVM_FEAT_CLOCKSOURCE (1u << 0)
#define HYPE_CPUID_KVM_FEAT_CLOCKSOURCE2 (1u << 3)
#define HYPE_CPUID_KVM_FEAT_CLOCKSOURCE_STABLE (1u << 24)

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
        out->eax = HYPE_CPUID_MAX_BASIC_LEAF; /* max basic leaf supported (reaches 0xD XSAVE) */
        out->ebx = 0x68747541u; /* "Auth" */
        out->edx = 0x69746e65u; /* "enti" */
        out->ecx = 0x444d4163u; /* "cAMD" */
        return;
    }

    if (eax_in == 1) {
        out->eax = real->eax;
        /* EBX[31:24] is the initial local APIC ID. Passed through, it carries
         * whichever pCPU the guest happened to run on (e.g. 1), but hype's
         * guest LAPIC reports ID 0 for the single vCPU -- the kernel flags the
         * disagreement ("[Firmware Bug]: APIC ID mismatch. CPUID: 0x0001 APIC:
         * 0x0000"). Force it to 0 so CPUID agrees with the modeled LAPIC. (Each
         * hype VM has exactly one 1:1-pinned vCPU whose LAPIC ID is 0.) */
        out->ebx = real->ebx & 0x00FFFFFFu;
        out->edx = real->edx & ~HYPE_CPUID_LEAF1_EDX_MTRR_BIT;
        /* Hypervisor-present set; MTRR already cleared above. Also clear
         * TSC_DEADLINE (ECX bit 24): with it set, a guest OS arms its
         * LAPIC timer via the IA32_TSC_DEADLINE MSR (0x6e0) -- a mode
         * this project does not model, so no timer interrupt would ever
         * fire and the guest's scheduler stalls (a real Linux kernel
         * idle-HLTs forever right after unpacking its initramfs).
         * Clearing it makes the guest fall back to the LAPIC timer's
         * initial-count mode, which FW-1b's guest LAPIC model does drive
         * and inject.
         *
         * Clear X2APIC (ECX bit 21) for the same class of reason: with it
         * advertised, Linux switches APIC access to the x2APIC MSR interface
         * (MSRs 0x800-0x8FF -- APIC routing "physical x2apic"), but hype models
         * only the xAPIC MMIO LAPIC (FW-1b, 0xFEE00000). In x2APIC mode every
         * timer program / current-count read / EOI becomes an unmodeled
         * RDMSR/WRMSR, so the kernel's APIC-timer calibration reads garbage,
         * logs "APIC frequency too slow, disabling apic timer", and -- with no
         * working clockevent -- idle-hangs (observed hang at "Mounting boot
         * media" in APIC mode). Clearing it keeps the guest on the modeled
         * MMIO LAPIC. */
        /* ECX passes the host's instruction-capability bits straight through
         * (SSE/AVX/XSAVE/OSXSAVE/FMA/AES/...), so the guest sees the real CPU's
         * feature set -- coherently, because leaf 7 and leaf 0xD are exposed too
         * (see HYPE_CPUID_MAX_BASIC_LEAF). Only the two bits tied to hype's own
         * unmodeled paths are forced off: TSC_DEADLINE (24, no MSR-armed LAPIC
         * timer) and X2APIC (21, MMIO-LAPIC-only). Hypervisor-present (31) set. */
        out->ecx = (real->ecx | HYPE_CPUID_HYPERVISOR_PRESENT_BIT) &
                   ~HYPE_CPUID_LEAF1_ECX_TSC_DEADLINE_BIT & ~HYPE_CPUID_LEAF1_ECX_X2APIC_BIT;
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

    if (eax_in == HYPE_CPUID_LEAF_STRUCTURED_EXT) {
        /* Structured extended features (AVX2/AVX-512/BMI/FSGSBASE/...): host
         * passthrough for the requested sub-leaf (`real` was read as
         * real_cpuid(eax_in, ecx_in), so it's sub-leaf-correct), EXCEPT the
         * EDX speculation-control mitigation bits, which are forced clear:
         * their control MSRs are NOT emulated (IA32_SPEC_CTRL 0x48 for
         * SPEC_CTRL/STIBP/SSBD, IA32_FLUSH_CMD 0x10b for L1D_FLUSH,
         * IA32_ARCH_CAPABILITIES 0x10a / IA32_CORE_CAPABILITIES 0xcf). hype
         * currently absorbs those MSRs as no-ops, so a guest that saw the
         * CPUID bit but got a dead MSR ended up inconsistent -- e.g. the
         * kernel disabled SPEC_CTRL (dead MSR) yet kept SSBD from CPUID ->
         * "x86 CPU feature dependency check failure: 18*32+31 enabled but
         * 18*32+26 disabled". Clearing them (only meaningful on the leaf-7
         * sub-leaf 0 EDX; other sub-leaves have these bits reserved/zero, so
         * masking is harmless) keeps the advertised mitigation set to exactly
         * what hype can back. */
        out->eax = real->eax;
        out->ebx = real->ebx;
        out->ecx = real->ecx;
        out->edx = real->edx & ~HYPE_CPUID_LEAF7_EDX_SPECCTRL_MASK;
        return;
    }

    if (eax_in == HYPE_CPUID_LEAF_XSAVE) {
        /* XSAVE state enumeration (leaf 0xD, per sub-leaf): full host
         * passthrough so the guest can size its XSAVE area and enable XCR0 --
         * the other half of making leaf 1's XSAVE/AVX bits coherent. */
        out->eax = real->eax;
        out->ebx = real->ebx;
        out->ecx = real->ecx;
        out->edx = real->edx;
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
        /* Hypervisor signature leaf. Presents the KVM identity ("KVMKVMKVM")
         * so a Linux/BSD guest enables kvmclock -- a paravirt clocksource that
         * bypasses the guest's own (failing) TSC calibration. This is NOT
         * pretending broad KVM compatibility: only the pvclock feature is
         * advertised in leaf 0x40000001 below; every other KVM paravirt
         * feature (async PF, PV EOI, steal time, PV IPI) is left off, so the
         * guest enables nothing hype doesn't back. EAX = max KVM leaf. */
        out->eax = HYPE_CPUID_KVM_FEATURES_LEAF;
        out->ebx = HYPE_CPUID_KVM_SIG_EBX; /* "KVMK" */
        out->ecx = HYPE_CPUID_KVM_SIG_ECX; /* "VMKV" */
        out->edx = HYPE_CPUID_KVM_SIG_EDX; /* "M\0\0\0" */
        return;
    }

    if (eax_in == HYPE_CPUID_KVM_FEATURES_LEAF) {
        /* KVM paravirt feature bits (EAX). Advertise only the pvclock
         * clocksources: CLOCKSOURCE2 (the modern MSR pair 0x4b564d0x) plus
         * CLOCKSOURCE (the legacy pair) for older guests, and TSC_STABLE_BIT
         * -- hype's guest TSC is invariant, passthrough, and 1:1-pinned (no
         * migration), so the guest may trust it for a vDSO fast read. */
        out->eax = HYPE_CPUID_KVM_FEAT_CLOCKSOURCE | HYPE_CPUID_KVM_FEAT_CLOCKSOURCE2 |
                   HYPE_CPUID_KVM_FEAT_CLOCKSOURCE_STABLE;
        out->ebx = 0;
        out->ecx = 0;
        out->edx = 0;
        return;
    }

    zero_result(out);
}
