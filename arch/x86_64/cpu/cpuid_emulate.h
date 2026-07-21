#ifndef HYPE_ARCH_CPUID_EMULATE_H
#define HYPE_ARCH_CPUID_EMULATE_H

#include <stdint.h>

/*
 * CPUID interception (CPUMSR-1). Confirmed by grepping this project's
 * own vmexit dispatch loop and VMCB builders that CPUID previously had
 * zero interception at all -- it executed natively against the real
 * host CPU, a guest-isolation gap (AGENTS.md) surfaced while scoping
 * M4-6 (a real OVMF/GRUB/Linux boot executes CPUID extensively).
 *
 * HYPE_SVM_INTERCEPT_CPUID (bit 18 of intercept_misc1) and
 * HYPE_SVM_EXITCODE_CPUID (0x72) are defined in arch/x86_64/svm/
 * vmcb.h, cross-referenced against the AMD SVM Intercept Vector 3 bit
 * layout and Appendix C exit-code table -- internally consistent with
 * this project's own already-established neighboring constants
 * (HLT=24/0x78, IOIO_PROT=27/0x7B, MSR_PROT=28/0x7C, SHUTDOWN=31/0x7F
 * all match the same real table this leaf's CPUID=18/0x72 comes from).
 *
 * Design: rather than fully synthesizing every field from scratch,
 * this project reads the REAL host CPU's own CPUID result for the
 * same (eax_in, ecx_in) pair (the exempt glue's job --
 * arch/x86_64/svm/svm_vcpu.c's hype_svm_vcpu_handle_cpuid()) and
 * passes it in as `real` -- family/model/stepping and most feature
 * bits are not security/isolation-sensitive and are simplest/safest to
 * pass straight through (a guest executing e.g. SSE/AVX instructions
 * needs no hypervisor mediation at all); only the handful of fields
 * that matter for guest-isolation or this project's own emulation
 * scope are actually curated. Everything not explicitly synthesized
 * below is deliberately minimal ("baseline") -- max basic/extended
 * leaf numbers are capped low so well-behaved guest software (which is
 * expected to check leaf 0's own reported max before querying further)
 * never reaches an unhandled leaf; anything queried anyway (buggy or
 * unusually curious software) safely falls back to all-zero, the same
 * convention real hardware uses for a reserved/future leaf. Iterate
 * this allow-list based on what a real OVMF/GRUB/Linux boot log
 * actually demands, not by guessing every leaf upfront.
 */

typedef struct {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} hype_cpuid_result_t;

/*
 * Synthesizes what the guest should see for CPUID(eax_in, ecx_in),
 * given `real` (the real host CPU's own raw result for that same
 * leaf/subleaf). Pure logic, no CPU access of its own -- fully unit
 * tested. Handled leaves:
 *
 *   0            -- max basic leaf = 0xD; vendor string "AuthenticAMD"
 *                    (this project only targets AMD hosts so far,
 *                    same scope as M2-8's own real-hardware gate). The
 *                    max is raised to 0xD so leaf 7 and leaf 0xD are
 *                    reachable -- required to make leaf 1's XSAVE/AVX
 *                    advertisement coherent (see leaf 1 / leaf 7 / 0xD).
 *   1             -- EAX/EBX passthrough (family/model/stepping,
 *                    brand/APIC-id info -- not isolation-sensitive);
 *                    EDX passthrough except MTRR support (bit 12)
 *                    forced clear, so well-behaved guest software
 *                    never attempts an MTRR MSR access this project
 *                    doesn't emulate (CPUMSR-2); ECX passthrough
 *                    (incl. the host's XSAVE/OSXSAVE/AVX/FMA/F16C
 *                    instruction-capability bits) except the
 *                    hypervisor-present bit (31) forced set and
 *                    TSC_DEADLINE (24) + X2APIC (21) forced clear.
 *   7             -- structured extended features (AVX2/AVX-512/BMI/
 *                    FSGSBASE/...): host passthrough for the requested
 *                    sub-leaf, so the guest sees the real vector ISA.
 *   0xD           -- XSAVE state-component enumeration: host passthrough
 *                    (per sub-leaf), so the guest can size its XSAVE
 *                    area and enable XCR0. Exposing 7 and 0xD truthfully
 *                    is what keeps leaf 1's XSAVE/AVX bits honest: a
 *                    glibc userspace (Ubuntu/Fedora) resolves ifunc
 *                    string/memcpy routines to AVX off leaf 1 and would
 *                    fault on the first AVX instruction if the XSAVE
 *                    enumeration were missing (musl/Alpine, using no AVX
 *                    ifuncs, tolerated the old capped-at-1 leaf set).
 *   0x40000000    -- hypervisor signature leaf: "KVMKVMKVM" + max KVM
 *                    leaf, so a Linux/BSD guest enables kvmclock (the
 *                    paravirt clocksource that bypasses the guest's own
 *                    TSC calibration -- which fails on an AMD guest
 *                    because hype's emulated PIT is too lumpy for
 *                    quick_pit_calibrate). This replaces the earlier
 *                    honest-but-useless "HypeHypeHype" signature.
 *   0x40000001    -- KVM feature bits (EAX): only the pvclock
 *                    clocksources (CLOCKSOURCE, CLOCKSOURCE2) plus
 *                    TSC_STABLE. No other KVM paravirt feature is
 *                    advertised, so the guest enables nothing hype
 *                    doesn't back.
 *   0x80000000    -- max extended leaf = 0x80000001; vendor string
 *                    repeated, same as leaf 0.
 *   0x80000001    -- EAX/EDX passthrough (EDX carries the NX/long-mode
 *                    bits a 64-bit guest genuinely needs correct, or
 *                    it can't boot in long mode at all); ECX
 *                    passthrough except the SVM bit (2) forced clear
 *                    -- this project does not emulate nested SVM for
 *                    guests, so it must not advertise the extension.
 *   0x40000000    -- the hypervisor CPUID leaf (Xen/KVM/Hyper-V/
 *                    VMware convention): EAX = 0x40000000 (no further
 *                    hypervisor-specific leaves implemented yet);
 *                    EBX/ECX/EDX = a distinct, honest 12-character
 *                    vendor signature ("HypeHypeHype") -- not
 *                    pretending KVM/Xen/Hyper-V compatibility (that's
 *                    M7-1's later, Windows-specific job), just
 *                    signaling "you are virtualized" so guest OSes
 *                    skip bare-metal-only workarounds/assumptions.
 *   anything else -- all-zero (the safe universal fallback for an
 *                    unimplemented/reserved leaf).
 */
void hype_cpuid_emulate(uint32_t eax_in, uint32_t ecx_in, const hype_cpuid_result_t *real,
                         hype_cpuid_result_t *out);

#endif /* HYPE_ARCH_CPUID_EMULATE_H */
