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
 *   0            -- max basic leaf = 0xD; vendor string passed through from
 *                   the real CPU (#298 -- it was hardcoded "AuthenticAMD", which
 *                   told an Intel-hosted guest it was on AMD)
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
 *                    VMware convention). Reports the KVM signature
 *                    ("KVMKVMKVM") so a Linux/BSD guest enables
 *                    kvmclock, with only the pvclock feature bits set
 *                    at the features leaf -- see the KVM block in
 *                    cpuid_emulate.c. When the Hyper-V leaves are
 *                    enabled for a vCPU (M7-1, below) this leaf reports
 *                    "Microsoft Hv" instead and the KVM pair moves to
 *                    0x40000100.
 *   anything else -- all-zero (the safe universal fallback for an
 *                    unimplemented/reserved leaf).
 */
void hype_cpuid_emulate(uint32_t eax_in, uint32_t ecx_in, const hype_cpuid_result_t *real,
                         hype_cpuid_result_t *out);


/*
 * M7-1 (#91): Hyper-V-compatible hypervisor CPUID leaves (0x40000000-0x40000006).
 *
 * Windows will not enable its enlightenments -- and some builds behave badly -- unless
 * it recognises a Hyper-V-shaped hypervisor here. Linux and BSD instead want the KVM
 * signature so they enable kvmclock (which is what PERF-1's fix depends on).
 *
 * Both cannot occupy leaf 0x40000000. So this is OPT-IN, off by default:
 *
 *   - default (Linux/BSD guests): 0x40000000 reports "KVMKVMKVM" exactly as before, so
 *     the only guests that currently work are completely unaffected.
 *   - enabled (os_hint = windows): 0x40000000 reports "Microsoft Hv" and the KVM leaves
 *     MOVE to 0x40000100, which is the same relocation QEMU performs when both are
 *     present. A Linux guest under that setting still finds kvmclock; it just looks one
 *     block higher.
 *
 * Enabling it by default would change the behaviour of the guests that do work, to
 * benefit a Windows guest that cannot boot yet for unrelated reasons (#172). That trade
 * is not worth making before there is a Windows guest to test against.
 */
/*
 * The enable is a PER-vCPU parameter rather than a module-level flag on purpose. hype
 * runs two guests concurrently on two cores, so a file-global "Hyper-V on" would be
 * written by one core's setup and read by the other core's CPUID exit -- the exact
 * shared-singleton race that #237 and #276 already cost this project. A Windows guest
 * and a Linux guest must be able to see different hypervisor identities at the same
 * instant, which only a parameter can express.
 *
 * hype_cpuid_emulate() is the hv_enabled=0 case, kept as its own entry point so the
 * existing callers and tests read unchanged.
 */
void hype_cpuid_emulate_ex(uint32_t eax_in, uint32_t ecx_in, int hv_enabled,
                            const hype_cpuid_result_t *real, hype_cpuid_result_t *out);

/*
 * SMP-2 (#186): the CPU topology THIS vCPU's guest should see.
 *
 * Per-vCPU for the same reason hv_enabled is (see above): two guests with different vCPU
 * counts take CPUID exits on two cores at the same instant, and `apic_id` differs per vCPU
 * even within one guest. A file-global would be the #237/#276 shared-singleton race again.
 *
 * The topology reported is hype's OWN choice, never a mirror of the host's. A guest is told
 * one socket containing `vcpu_count / threads_per_core` cores of `threads_per_core` threads --
 * whatever hype actually gave it. #436 is the standing evidence for why leaking host values
 * here is harmful: passing the host's HT count and core count through made guests try to start
 * phantom APs that do not exist.
 */
typedef struct {
    uint32_t apic_id;          /* this vCPU's local APIC ID; 0 for the BSP */
    uint32_t vcpu_count;       /* logical processors in THIS VM; >= 1 */
    uint32_t threads_per_core; /* SMT threads per core hype gave this VM; >= 1 (§10 decision 40) */
} hype_cpuid_topology_t;

/*
 * Smallest s such that (1u << s) >= n -- the SDM's "number of bits to shift the x2APIC ID
 * right to reach the next topology level" for leaf 0x0B/0x1F. Exposed because it is the one
 * piece of arithmetic in the topology leaves that is easy to get subtly wrong (n=1 must give
 * 0, and non-powers-of-two must round UP, or a guest's APIC-ID decode silently overlaps
 * levels). n == 0 is treated as 1.
 */
uint32_t hype_cpuid_topo_shift(uint32_t n);

/*
 * As hype_cpuid_emulate_ex(), plus the guest-visible topology. `topo` may be 0, which means
 * one vCPU with APIC ID 0 -- exactly what the two entry points above report, so they are thin
 * wrappers over this and every existing caller and test reads unchanged.
 */
void hype_cpuid_emulate_topo(uint32_t eax_in, uint32_t ecx_in, int hv_enabled,
                             const hype_cpuid_topology_t *topo,
                             const hype_cpuid_result_t *real, hype_cpuid_result_t *out);

/* Where the KVM paravirt leaves live -- 0x40000000 normally, 0x40000100 when the
 * Hyper-V leaves have taken the base. */
uint32_t hype_cpuid_kvm_base(int hv_enabled);

/*
 * Synthesize one Hyper-V leaf. Returns 1 if `leaf` is one this fills, 0 otherwise.
 * Pure: no MSR state, no hardware. `vp_index` is the virtual processor index, which
 * leaf 0x40000002's version block does not carry but callers pass for future per-vCPU
 * leaves; ignored today rather than omitted so the signature does not churn.
 */
int hype_cpuid_hv_leaf(uint32_t leaf, uint32_t vp_index, hype_cpuid_result_t *out);
#endif /* HYPE_ARCH_CPUID_EMULATE_H */
