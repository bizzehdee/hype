#include "cpuid_emulate.h"

#define HYPE_CPUID_HYPERVISOR_PRESENT_BIT (1u << 31)
#define HYPE_CPUID_LEAF1_EDX_MTRR_BIT (1u << 12)
/*
 * #436: leaf 1 EDX bit 28 = HTT (Hyper-Threading / multiple logical processors present). Passed
 * through, it carries the HOST's multi-core answer, and leaf 1 EBX[23:16] then reports the host's
 * logical-processor count -- so the guest believes there are N CPUs while hype runs exactly ONE
 * 1:1-pinned vCPU. Linux/BSD dodge this (they take their CPU count from the MADT, which declares 1),
 * but Windows winload calls the UEFI EFI_MP_SERVICES protocol -- OVMF's MpInitLib -- which INIT-SIPIs
 * the phantom APs and spins forever waiting for them to check in (the #436 winload wedge). Clearing
 * HTT and forcing the counts to 1 makes CPUID agree with the modeled single vCPU.
 */
#define HYPE_CPUID_LEAF1_EDX_HTT_BIT (1u << 28)
#define HYPE_CPUID_LEAF1_ECX_TSC_DEADLINE_BIT (1u << 24)
#define HYPE_CPUID_LEAF1_ECX_X2APIC_BIT (1u << 21)
#define HYPE_CPUID_LEAF1_ECX_MONITOR_BIT (1u << 3)
#define HYPE_CPUID_LEAFD_SUB1_EAX_XSAVES_BIT (1u << 3)
#define HYPE_CPUID_LEAF7_ECX_WAITPKG_BIT (1u << 5)
#define HYPE_CPUID_LEAF1_ECX_OSXSAVE_BIT (1u << 27)
/* Highest basic leaf hype exposes. Must reach leaf 0xD (XSAVE state-component
 * enumeration) so the guest sees a COHERENT instruction-capability picture:
 * leaf 1 ECX passes the host's XSAVE(26)/AVX(28)/FMA(12)/F16C(29) bits straight through
 * (OSXSAVE(27) is NOT passed through -- it mirrors the guest's own CR4, see the leaf-1
 * handler), so leaf 7 (AVX2/AVX-512/BMI/...) and leaf 0xD (the
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
/*
 * #608: SPEC_CTRL(26)/STIBP(27)/SSBD(31) are now genuinely backed (IA32_SPEC_CTRL is virtualized
 * per-vCPU) and so are no longer masked -- only L1D_FLUSH(28)/ARCH_CAPABILITIES(29)/
 * CORE_CAPABILITIES(30) remain, whose own MSRs are still unimplemented. Kept as a distinct name
 * from the mask above rather than redefining it in place, so a `git blame`/history reader can see
 * exactly which bits moved and when, rather than the same macro name silently meaning a smaller
 * set at two different points in history.
 */
#define HYPE_CPUID_LEAF7_EDX_SPECCTRL_REMAINING_MASK ((1u << 28) | (1u << 29) | (1u << 30))
#define HYPE_CPUID_EXT1_ECX_SVM_BIT (1u << 2)
/*
 * #552: leaf 1 ECX bit 5, VMX. The Intel half of #316's rule -- hype does not expose
 * virtualization to guests -- which was applied to SVM on 0x80000001 and never here. An Intel guest
 * was told VT-x was available, which is what /proc/cpuinfo and lscpu then report to an operator,
 * and what a nested KVM/VirtualBox/vmm inside the guest acts on before executing a VMXON hype does
 * not emulate.
 */
#define HYPE_CPUID_LEAF1_ECX_VMX_BIT (1u << 5)
#define HYPE_CPUID_EXT7_EDX_INVARIANT_TSC_BIT (1u << 8)
#define HYPE_CPUID_LEAF6_EAX_ARAT_BIT (1u << 2)

/* KVM paravirt CPUID (kvmclock). The signature leaf (base, see
 * hype_cpuid_kvm_base()) reports "KVMKVMKVM" (EBX/ECX/EDX) and the max KVM leaf
 * (EAX); base+1 EAX carries the KVM_FEATURE_* bits. Kept here (CPUID domain) rather than pulled
 * from devices/pvclock.h to avoid an arch->devices header dependency; the two
 * must agree (a mismatch just means the guest doesn't enable kvmclock). */
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

/* --- M7-1 (#91): Hyper-V-compatible hypervisor leaves --- */

/*
 * Hyper-V leaf constants. Signature bytes are transcribed from the Hypervisor
 * Top-Level Functional Specification, not reconstructed -- a wrong byte here means
 * Windows silently declines every enlightenment, with no error to trace.
 */
#define HV_SIG_EAX 0x31237648u        /* "Hv#1" -- interface signature (leaf 0x40000001) */
#define HV_VENDOR_EBX 0x7263694du     /* "Micr" */
#define HV_VENDOR_ECX 0x666F736Fu     /* "osof" */
#define HV_VENDOR_EDX 0x76482074u     /* "t Hv" */
#define HV_MAX_LEAF 0x40000006u

/*
 * Leaf 0x40000003 EAX -- the partition privilege mask, i.e. which synthetic MSR
 * groups the guest may touch. Set ONLY the groups hype actually backs in
 * hype_msr_decide(): advertising a group whose MSRs then #GP is the
 * advertise-a-feature-that-is-not-there mistake that has already cost this project
 * guest boots.
 *
 * Notably absent: AccessPartitionReferenceTsc (bit 9). It would require hype to
 * maintain a guest-visible reference-TSC PAGE with a live sequence/scale/offset;
 * there is none, so the bit stays clear and the guest uses the reference counter
 * MSR instead.
 */
#define HV_PRIV_REF_COUNTER (1u << 1) /* HV_X64_MSR_TIME_REF_COUNT readable */
#define HV_PRIV_HYPERCALL_MSRS (1u << 5) /* GUEST_OS_ID + HYPERCALL MSRs */
#define HV_PRIV_VP_INDEX (1u << 6) /* HV_X64_MSR_VP_INDEX readable */
#define HV_PRIV_ACCESS_FREQUENCY_MSRS (1u << 11) /* TSC/APIC frequency MSRs readable (#436) */
#define HV_PRIV_ACCESS_REFERENCE_TSC (1u << 9)    /* HV_X64_MSR_REFERENCE_TSC page (#436) */

uint32_t hype_cpuid_kvm_base(int hv_enabled) {
    /* When Hyper-V holds 0x40000000, KVM moves up a block -- the same relocation QEMU
     * performs when both are present, so a Linux guest still finds kvmclock. */
    return hv_enabled ? 0x40000100u : 0x40000000u;
}

int hype_cpuid_hv_leaf(uint32_t leaf, uint32_t vp_index, hype_cpuid_result_t *out) {
    (void)vp_index; /* no leaf reports it -- VP index is delivered via its MSR */
    if (out == (hype_cpuid_result_t *)0) {
        return 0;
    }
    switch (leaf) {
        case 0x40000000u:
            /* Vendor signature + the highest leaf we implement. */
            out->eax = HV_MAX_LEAF;
            out->ebx = HV_VENDOR_EBX;
            out->ecx = HV_VENDOR_ECX;
            out->edx = HV_VENDOR_EDX;
            return 1;
        case 0x40000001u:
            /* Interface signature. "Hv#1" is what says "this is the Hyper-V
             * interface"; anything else and Windows treats the hypervisor as unknown. */
            out->eax = HV_SIG_EAX;
            out->ebx = 0; out->ecx = 0; out->edx = 0;
            return 1;
        case 0x40000002u:
            /* Hypervisor version. Build/major/minor are ours to choose; they are
             * reported, not matched against a real Hyper-V. */
            out->eax = 1u;                 /* build number */
            out->ebx = (6u << 16) | 3u;    /* major 6, minor 3 */
            out->ecx = 0u;                 /* service pack */
            out->edx = 0u;                 /* service branch/number */
            return 1;
        case 0x40000003u:
            out->eax = HV_PRIV_REF_COUNTER | HV_PRIV_HYPERCALL_MSRS | HV_PRIV_VP_INDEX |
                       HV_PRIV_ACCESS_FREQUENCY_MSRS | HV_PRIV_ACCESS_REFERENCE_TSC;
            out->ebx = 0u;                 /* privilege mask high -- nothing granted */
            out->ecx = 0u;
            out->edx = 0u;                 /* no misc features */
            return 1;
        case 0x40000004u:
            /*
             * Recommended enlightenments. ALL ZERO deliberately: each bit tells Windows
             * to replace a native operation with a specific hypercall. Hype services
             * the call ABI but implements none of those enlightenment operations.
             */
            out->eax = 0u; out->ebx = 0u; out->ecx = 0u; out->edx = 0u;
            return 1;
        case 0x40000005u:
            /* Implementation limits. 0 means "no limit specified", which is legal and
             * is the honest answer -- hype has no partition/VP accounting to report. */
            out->eax = 0u; out->ebx = 0u; out->ecx = 0u; out->edx = 0u;
            return 1;
        case 0x40000006u:
            /* Hardware features the hypervisor is USING. Zero: hype does not expose
             * APICv/AVIC or nested paging to the guest as Hyper-V features. */
            out->eax = 0u; out->ebx = 0u; out->ecx = 0u; out->edx = 0u;
            return 1;
        default:
            return 0;
    }
}

void hype_cpuid_emulate(uint32_t eax_in, uint32_t ecx_in, const hype_cpuid_result_t *real,
                         hype_cpuid_result_t *out) {
    hype_cpuid_emulate_ex(eax_in, ecx_in, 0, real, out);
}

uint32_t hype_cpuid_topo_shift(uint32_t n) {
    uint32_t s = 0;
    if (n <= 1u) {
        return 0u; /* one item at this level needs no ID bits */
    }
    n -= 1u; /* ceil(log2(n)) == bits needed to index 0..n-1 */
    while (n != 0u) {
        s++;
        n >>= 1;
    }
    return s;
}

void hype_cpuid_emulate_ex(uint32_t eax_in, uint32_t ecx_in, int hv_enabled,
                            const hype_cpuid_result_t *real, hype_cpuid_result_t *out) {
    /* guest_cr4 = 0: OSXSAVE reported clear, which is correct for any caller that has not
     * told us otherwise -- a guest that has not enabled CR4.OSXSAVE must not be told it has. */
    hype_cpuid_emulate_topo(eax_in, ecx_in, hv_enabled, (const hype_cpuid_topology_t *)0, 0ull,
                            real, out);
}

void hype_cpuid_emulate_topo(uint32_t eax_in, uint32_t ecx_in, int hv_enabled,
                             const hype_cpuid_topology_t *topo, uint64_t guest_cr4,
                             const hype_cpuid_result_t *real, hype_cpuid_result_t *out) {
    /* SMP-2 (#186): a null topo means the pre-SMP machine -- one vCPU, APIC ID 0 -- so every
     * caller that has not been taught about topology keeps reporting exactly what it did. */
    hype_cpuid_topology_t topo_default;
    if (topo == (const hype_cpuid_topology_t *)0) {
        topo_default.apic_id = 0u;
        topo_default.vcpu_count = 1u;
        topo_default.threads_per_core = 1u;
        topo = &topo_default;
    }
    {
        /* Defend the arithmetic below against a caller that never set these. */
        if (topo->vcpu_count == 0u || topo->threads_per_core == 0u) {
            topo_default.apic_id = topo->apic_id;
            topo_default.vcpu_count = topo->vcpu_count ? topo->vcpu_count : 1u;
            topo_default.threads_per_core = topo->threads_per_core ? topo->threads_per_core : 1u;
            topo = &topo_default;
        }
    }

    if (eax_in == 0) {
        out->eax = HYPE_CPUID_MAX_BASIC_LEAF; /* max basic leaf supported (reaches 0xD XSAVE) */
        /*
         * #298: the vendor string is PASSED THROUGH from the real CPU. It used to
         * be hardcoded to "AuthenticAMD" here and at leaf 0x80000000, which meant
         * every guest was told it was on AMD whatever the silicon.
         *
         * That was accurate for as long as hype was SVM-only, and stayed accurate
         * on the AMD box, so only a cross-vendor hardware run could expose it. On
         * the Intel box the guest's /proc/cpuinfo read "AuthenticAMD" and Linux
         * acted on it -- it probed amd_pstate, AMD's cpufreq driver, on an Intel
         * CPU.
         *
         * Worse than a wrong label: leaf 1 below passes the REAL
         * family/model/stepping through, so the guest saw genuine Intel model IDs
         * wearing an AMD vendor string and decoded them under AMD rules. Vendor
         * also steers speculative-execution mitigation selection, so a false one
         * is a security-relevant input, not a cosmetic string.
         *
         * Deliberately NOT hardcoded to Intel instead -- that just reproduces the
         * same bug on the other machine. The only correct source is the CPU.
         *
         * EAX stays synthesized: the max-leaf clamp is the one part of this leaf
         * hype must own, because it bounds which leaves a guest will ask for.
         */
        out->ebx = real->ebx;
        out->edx = real->edx;
        out->ecx = real->ecx;
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
        /* EBX: keep brand index [7:0] + CLFLUSH size [15:8] from the host; report THIS VM's
         * addressable-logical-processor count [23:16] and THIS vCPU's initial APIC ID [31:24].
         * #436: neither may be passed through -- the host's HT count made guests start
         * phantom APs, and the host's APIC ID disagreed with hype's guest LAPIC ("[Firmware
         * Bug]: APIC ID mismatch"). SMP-2 (#186): both now come from the modelled topology
         * rather than being forced to 1 and 0, which was correct only while a VM had one
         * vCPU. */
        out->ebx = (real->ebx & 0x0000FFFFu) |
                   ((topo->vcpu_count & 0xFFu) << 16) | ((topo->apic_id & 0xFFu) << 24);
        /*
         * EDX: clear HTT (#436: single logical processor -- see the
         * HYPE_CPUID_LEAF1_EDX_HTT_BIT comment).
         *
         * #436: MTRR is NO LONGER cleared. It was, on the grounds that MTRRs
         * were "unmodeled" -- but they have been modelled since the MTRR MSR
         * round-trip and WB-default work: hype answers MTRRcap, MTRRdefType,
         * the eight variable base/mask pairs and the fixed ranges. Denying the
         * feature while implementing it is the same describe-a-different-
         * machine defect as the MCFG bus range and the hardware-reduced FADT
         * flag, and it is the one a strict OS refuses outright: MTRR is a
         * required processor feature, so a guest that checks the bit sees an
         * unsupported CPU and stops rather than boots.
         */
        /* HTT means "more than one logical processor in this package", so it follows the
         * modelled vCPU count. Clearing it with several vCPUs present would contradict
         * EBX[23:16] above and leaf 0x0B below. */
        out->edx = (topo->vcpu_count > 1u) ? (real->edx | HYPE_CPUID_LEAF1_EDX_HTT_BIT)
                                           : (real->edx & ~HYPE_CPUID_LEAF1_EDX_HTT_BIT);
        /* Hypervisor-present set. Also clear
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
         * (see HYPE_CPUID_MAX_BASIC_LEAF). Only the bits tied to hype's own
         * unmodeled paths are forced off: TSC_DEADLINE (24, no MSR-armed LAPIC
         * timer), X2APIC (21, MMIO-LAPIC-only), and MONITOR (3).
         * Hypervisor-present (31) set.
         *
         * #256: MONITOR/MWAIT must be masked. hype intercepts neither instruction,
         * and an un-intercepted MWAIT never exits -- so passing the host's bit
         * through let Linux pick MWAIT for its idle loop and the guest NEVER executed
         * HLT (hlt=0 across 655k exits). hype then had no idle signal at all, which
         * is why the dashboard's CPU%, derived from HLT-wait time, sat pinned at
         * exactly 100% for an idle guest (#264). Masking the bit is what any
         * hypervisor that does not emulate MONITOR/MWAIT does; Linux falls back to
         * HLT idle, which hype already models. */
        /*
         * #552: VMX (5) joins them. It is the same rule as the SVM mask on 0x80000001 below, and it
         * was missing here -- so a guest on an Intel host saw VT-x while a guest on an AMD host
         * correctly saw no SVM. Found by the ported CPUMSR microtest asserting, from inside the
         * guest, what #316 says a guest must see.
         *
         * #601: X2APIC (21) unmasks ONLY in a build compiled with HYPE_ENABLE_X2APIC -- the x2APIC
         * MSR range (arch/x86_64/cpu/msr_emulate.c/vmcs_hw.c/svm_vcpu.c) that backs it is gated the
         * same way, and the default build must keep answering exactly as it did before this bit
         * existed (the regression bar this ticket set: masked means byte-identical). When the
         * bit does unmask it is still just a passthrough of the host's own bit -- a host that
         * cannot do x2APIC never advertises it to the guest either.
         */
        out->ecx = (real->ecx | HYPE_CPUID_HYPERVISOR_PRESENT_BIT) &
                   ~HYPE_CPUID_LEAF1_ECX_TSC_DEADLINE_BIT &
#if !defined(HYPE_ENABLE_X2APIC) || !HYPE_ENABLE_X2APIC
                   ~HYPE_CPUID_LEAF1_ECX_X2APIC_BIT &
#endif
                   ~HYPE_CPUID_LEAF1_ECX_MONITOR_BIT & ~HYPE_CPUID_LEAF1_ECX_VMX_BIT;
        /*
         * OSXSAVE (bit 27) is NOT a capability bit. The SDM defines it as a read-only mirror
         * of the executing processor's CR4.OSXSAVE, so it must track THIS vCPU's CR4, never
         * the host's.
         *
         * Passing the host's through was a real fault, not a purity issue: an OVMF AP read the
         * bit as set, concluded XSAVE was already enabled, skipped setting CR4.OSXSAVE, and
         * executed XGETBV -- which #UDs when CR4.OSXSAVE is clear. The AP died in CPUID
         * leaf-0xD feature detection with CR4=0x668 (bit 18 clear) every boot. Same class as
         * #436's phantom PCI buses: describe a machine hype is not providing, and the guest
         * walks into the gap.
         */
        if ((guest_cr4 & HYPE_CR4_OSXSAVE_BIT) != 0ull) {
            out->ecx |= HYPE_CPUID_LEAF1_ECX_OSXSAVE_BIT;
        } else {
            out->ecx &= ~HYPE_CPUID_LEAF1_ECX_OSXSAVE_BIT;
        }
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
        /*
         * #252/A2: WAITPKG (sub-leaf 0, ECX bit 5) is masked for the same reason as
         * the EDX bits below and as MONITOR in leaf 1: hype cannot back it. On VMX,
         * TPAUSE/UMWAIT/UMONITOR take #UD unless the "enable user wait and pause"
         * secondary control is set, and honouring them properly also means modelling
         * IA32_UMWAIT_CONTROL, which hype does not. Linux uses TPAUSE in
         * delay_halt_tpause(), i.e. inside udelay() -- so leaving this advertised
         * produced a #UD on every delay. Masked, Linux falls back to the ordinary
         * PAUSE delay loop, which needs nothing from the hypervisor.
         */
        if (ecx_in == 0u) {
            out->ecx = real->ecx & ~HYPE_CPUID_LEAF7_ECX_WAITPKG_BIT;
        }
        /*
         * #608: bits 26 (IBRS/IBPB)/27 (STIBP)/31 (SSBD) are no longer forced clear -- their
         * control MSRs (IA32_SPEC_CTRL 0x48, IA32_PRED_CMD 0x49) are now genuinely virtualized
         * per-vCPU (arch/x86_64/svm/svm_vcpu.c, arch/x86_64/vmx/vmcs_hw.c), so advertising them
         * is no longer the "CPUID says yes, the MSR is a dead no-op" mistake this mask existed to
         * prevent. Bits 28 (L1D_FLUSH)/29 (ARCH_CAPABILITIES)/30 (CORE_CAPABILITIES) stay masked:
         * their own MSRs (IA32_FLUSH_CMD 0x10b, IA32_ARCH_CAPABILITIES 0x10a,
         * IA32_CORE_CAPABILITIES 0xcf) are still unimplemented.
         */
        out->edx = real->edx & ~HYPE_CPUID_LEAF7_EDX_SPECCTRL_REMAINING_MASK;
        return;
    }

    if (eax_in == 0xBu) {
        /*
         * #436: extended topology enumeration. Leaf 0 advertises a max basic
         * leaf of 0xD, so this leaf is claimed -- but it was unmodelled and fell
         * through to the all-zero default, the exact advertise-a-feature-that-
         * is-not-there mistake the Hyper-V leaf code here already warns about.
         * A guest enumerating topology then reads "0 logical processors at level
         * 0", which no consistent interpretation covers.
         *
         * hype models exactly one logical processor per guest (leaf 1 clears HTT,
         * leaf 0x80000008 forces NC=0, the MADT and fw_cfg NB_CPUS both say 1),
         * so report that same machine here: one thread in the SMT level, one
         * logical processor in the core level, x2APIC ID 0. ECX[15:8] carries the
         * level type (1=SMT, 2=Core; 0=invalid terminates enumeration) and
         * ECX[7:0] echoes the input level, per the SDM's own algorithm.
         */
        /* SMP-2 (#186): report the modelled topology, not a hardcoded uniprocessor. EAX[4:0]
         * is the number of bits to shift an x2APIC ID right to reach the NEXT level, so the
         * SMT level shifts past the thread bits and the Core level past the whole package.
         * EBX is the count of logical processors AT OR BELOW that level. */
        out->edx = topo->apic_id; /* this logical processor's x2APIC ID */
        if (ecx_in == 0u) {
            out->eax = hype_cpuid_topo_shift(topo->threads_per_core);
            out->ebx = topo->threads_per_core;
            out->ecx = 0u | (1u << 8);   /* level 0, type SMT */
        } else if (ecx_in == 1u) {
            out->eax = hype_cpuid_topo_shift(topo->vcpu_count);
            out->ebx = topo->vcpu_count;
            out->ecx = 1u | (2u << 8);   /* level 1, type Core */
        } else {
            out->eax = 0u;
            out->ebx = 0u;               /* 0 processors + type 0 = end of enumeration */
            out->ecx = ecx_in & 0xFFu;
        }
        return;
    }

    if (eax_in == HYPE_CPUID_LEAF_XSAVE) {
        /* XSAVE state enumeration (leaf 0xD, per sub-leaf): host passthrough so the
         * guest can size its XSAVE area and enable XCR0 -- the other half of making
         * leaf 1's XSAVE/AVX bits coherent.
         *
         * EXCEPT XSAVES (sub-leaf 1, EAX bit 3), which is forced clear for the same
         * reason leaf 7's speculation-control bits are: the MSR that configures it is
         * not emulated. XSAVES manages SUPERVISOR state components through IA32_XSS
         * (0xDA0), and hype models no such MSR -- a guest's writes are absorbed and
         * discarded (#269). Advertising the instruction while its control register is
         * dead is exactly the inconsistency that comment warns about.
         *
         * #252: it also makes Linux disagree with itself. fpu__init_system_xstate()
         * takes the compacted path for kernel_size (because XSAVES is advertised) but
         * computes `size` on the uncompacted layout, then WARNs at xstate.c:332 that
         * they differ -- 840 vs 2696, which is exactly the pair in the observed
         * register dump. */
        out->eax = real->eax;
        if (ecx_in == 1u) {
            out->eax = real->eax & ~HYPE_CPUID_LEAFD_SUB1_EAX_XSAVES_BIT;
        }
        out->ebx = real->ebx;
        out->ecx = real->ecx;
        out->edx = real->edx;
        return;
    }

    if (eax_in == 0x80000000u) {
        out->eax = 0x80000008u; /* max extended leaf supported */
        /* #298: real vendor, same reasoning as leaf 0 -- and it must agree with
         * leaf 0, since a guest reading both and finding them different has no
         * sane interpretation available. Both extended leaves hype then models
         * (0x80000001/7/8) are defined on Intel too, and 0x80000001 already masks
         * the SVM bit off, so nothing here depended on the guest believing AMD. */
        out->ebx = real->ebx;
        out->edx = real->edx;
        out->ecx = real->ecx;
        return;
    }

    /*
     * #361: the 48-byte CPU brand string, leaves 0x80000002-4.
     *
     * Leaf 0x80000000 above advertises 0x80000008 as the maximum extended leaf, which tells the
     * guest these three exist. They were not implemented, so they fell through to zero_result()
     * and the guest read 48 bytes of zeroes -- the advertise-a-feature-that-is-not-there mistake
     * the Hyper-V leaf code in this same file explicitly warns against.
     *
     * Passed straight through from the host, like 0x80000001. There is nothing to sanitise: a
     * brand string is a name, it exposes no capability the guest can act on, and the leaf-1
     * feature bits it sits beside are already the host's own. Inventing a name instead would put
     * hype back in the business of telling guests something untrue, which is how #298 happened.
     *
     * This is not cosmetic. OpenBSD printed "cpu0: Opteron or Athlon 64" on Intel silicon --
     * a fallback guess from the empty string -- which destroyed the evidence for the #298
     * cross-vendor check, since the guest's own CPU line could no longer distinguish "the vendor
     * fix works" from "it does not".
     */
    if (eax_in >= 0x80000002u && eax_in <= 0x80000004u) {
        out->eax = real->eax;
        out->ebx = real->ebx;
        out->ecx = real->ecx;
        out->edx = real->edx;
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
        /* ECX[7:0] = NC (number of logical processors in the package - 1); [15:12] =
         * ApicIdCoreIdSize (bits of the APIC ID used for the core index; 0 means "use NC").
         * #436: never passed through -- the host's count (e.g. 15 for a 16-core host) is
         * another source that makes a guest start phantom APs. SMP-2 (#186): report THIS
         * VM's count, so AMD's enumeration agrees with leaf 1's EBX[23:16] and leaf 0x0B
         * rather than contradicting them. */
        out->ecx = ((topo->vcpu_count - 1u) & 0xFFu) |
                   ((hype_cpuid_topo_shift(topo->vcpu_count) & 0xFu) << 12);
        out->edx = real->edx;
        return;
    }

    /*
     * Hypervisor leaves. When the Hyper-V set is enabled (os_hint = windows) it takes
     * the architectural 0x40000000 block and KVM relocates to 0x40000100 -- so this
     * dispatch is written against hype_cpuid_kvm_base(), not a fixed leaf number.
     * Order matters: HV is tried first, because when both are present HV owns
     * 0x40000000 and KVM does not.
     */
    if (hv_enabled && eax_in >= 0x40000000u && eax_in <= HV_MAX_LEAF) {
        if (hype_cpuid_hv_leaf(eax_in, 0, out)) {
            return;
        }
    }

    {
        uint32_t kvm_base = hype_cpuid_kvm_base(hv_enabled);

        if (eax_in == kvm_base) {
            /* Hypervisor signature leaf. Presents the KVM identity ("KVMKVMKVM")
             * so a Linux/BSD guest enables kvmclock -- a paravirt clocksource that
             * bypasses the guest's own (failing) TSC calibration. This is NOT
             * pretending broad KVM compatibility: only the pvclock feature is
             * advertised in the features leaf below; every other KVM paravirt
             * feature (async PF, PV EOI, steal time, PV IPI) is left off, so the
             * guest enables nothing hype doesn't back. EAX = max KVM leaf. */
            out->eax = kvm_base + 1u;
            out->ebx = HYPE_CPUID_KVM_SIG_EBX; /* "KVMK" */
            out->ecx = HYPE_CPUID_KVM_SIG_ECX; /* "VMKV" */
            out->edx = HYPE_CPUID_KVM_SIG_EDX; /* "M\0\0\0" */
            return;
        }

        if (eax_in == kvm_base + 1u) {
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
    }

    zero_result(out);
}
