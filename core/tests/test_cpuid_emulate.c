#include <stdio.h>
#include "../../arch/x86_64/cpu/cpuid_emulate.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Vendor strings as CPUID returns them, in EBX/EDX/ECX order. */
#define VENDOR_INTEL_EBX 0x756E6547u /* "Genu" */
#define VENDOR_INTEL_EDX 0x49656E69u /* "ineI" */
#define VENDOR_INTEL_ECX 0x6C65746Eu /* "ntel" */
#define VENDOR_AMD_EBX 0x68747541u   /* "Auth" */
#define VENDOR_AMD_EDX 0x69746E65u   /* "enti" */
#define VENDOR_AMD_ECX 0x444D4163u   /* "cAMD" */

/*
 * #298: the vendor string must come from the REAL CPU, not a constant.
 *
 * The test this replaces asserted the opposite -- it passed real={0,0,0,0} with
 * the comment "unused for leaf 0" and then checked for the AMD constants. So the
 * bug was not merely untested, it was pinned by a green test that made a
 * hardcoded vendor look like the intended design. A hardware run on Intel is
 * what eventually caught it: the guest read "AuthenticAMD" and probed amd_pstate
 * on an Intel CPU.
 *
 * Both vendors are exercised deliberately. Checking only one cannot distinguish
 * "passes the vendor through" from "hardcodes the vendor I happened to test".
 */
static void test_leaf0_vendor_is_passed_through(void) {
    hype_cpuid_result_t out;

    {
        hype_cpuid_result_t intel = {0x00000020u, VENDOR_INTEL_EBX, VENDOR_INTEL_ECX,
                                     VENDOR_INTEL_EDX};
        hype_cpuid_emulate(0, 0, &intel, &out);
        CHECK_HEX("Intel host: max basic leaf still clamped", 0x0Du, out.eax);
        CHECK_HEX("Intel host: ebx \"Genu\"", VENDOR_INTEL_EBX, out.ebx);
        CHECK_HEX("Intel host: edx \"ineI\"", VENDOR_INTEL_EDX, out.edx);
        CHECK_HEX("Intel host: ecx \"ntel\"", VENDOR_INTEL_ECX, out.ecx);
    }
    {
        hype_cpuid_result_t amd = {0x00000010u, VENDOR_AMD_EBX, VENDOR_AMD_ECX, VENDOR_AMD_EDX};
        hype_cpuid_emulate(0, 0, &amd, &out);
        CHECK_HEX("AMD host: max basic leaf still clamped", 0x0Du, out.eax);
        CHECK_HEX("AMD host: ebx \"Auth\"", VENDOR_AMD_EBX, out.ebx);
        CHECK_HEX("AMD host: edx \"enti\"", VENDOR_AMD_EDX, out.edx);
        CHECK_HEX("AMD host: ecx \"cAMD\"", VENDOR_AMD_ECX, out.ecx);
    }
}

/* Extended leaf 0x80000000 carries the vendor string too, and it must AGREE with
 * leaf 0 -- a guest that reads both and finds them different has no sane
 * interpretation available. Asserted as equality between the two leaves rather
 * than against a constant, so the requirement is "they match", which is the
 * thing that actually matters. */
static void test_ext_leaf0_vendor_matches_basic_leaf0(void) {
    hype_cpuid_result_t intel = {0x00000020u, VENDOR_INTEL_EBX, VENDOR_INTEL_ECX,
                                 VENDOR_INTEL_EDX};
    hype_cpuid_result_t basic, ext;

    hype_cpuid_emulate(0, 0, &intel, &basic);
    hype_cpuid_emulate(0x80000000u, 0, &intel, &ext);

    CHECK_HEX("max extended leaf", 0x80000008u, ext.eax);
    CHECK_HEX("ext vendor ebx matches leaf 0", basic.ebx, ext.ebx);
    CHECK_HEX("ext vendor edx matches leaf 0", basic.edx, ext.edx);
    CHECK_HEX("ext vendor ecx matches leaf 0", basic.ecx, ext.ecx);
}

static void test_leaf1_forces_hypervisor_bit_and_clears_mtrr(void) {
    hype_cpuid_result_t real = {0x00A00F11u, 0x12345678u, 0x7FFAFBFFu, 0xFFFAFBFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(1, 0, &real, &out);

    CHECK_HEX("eax passthrough", real.eax, out.eax);
    /* ebx[31:24] (initial APIC ID) forced to 0; [23:16] (logical-processor count)
     * forced to 1 (#436: single modeled vCPU); [15:0] (brand idx + CLFLUSH size) pass
     * through. real.ebx=0x12345678 -> 0x00015678. */
    CHECK_HEX("ebx initial-APIC-ID forced 0", 0, (out.ebx >> 24) & 0xFFu);
    CHECK_HEX("ebx logical-processor count forced 1", 1u, (out.ebx >> 16) & 0xFFu);
    CHECK_HEX("ebx low 16 bits (brand+clflush) passthrough", real.ebx & 0x0000FFFFu,
              out.ebx & 0x0000FFFFu);
    /* #436: HTT (edx bit 28) forced clear so the guest sees a single logical processor. */
    CHECK_HEX("HTT bit forced clear", 0, (out.edx & (1u << 28)) != 0);
    CHECK_HEX("hypervisor-present bit forced set", 1, (out.ecx & (1u << 31)) != 0);
    CHECK_HEX("TSC_DEADLINE bit forced clear", 0, (out.ecx & (1u << 24)) != 0);
    CHECK_HEX("X2APIC bit forced clear", 0, (out.ecx & (1u << 21)) != 0);
    /* XSAVE-domain instruction-capability bits pass straight through from the
     * host (coherent with the exposed leaf 7/0xD): FMA(12)/XSAVE(26)/OSXSAVE(27)/
     * AVX(28)/F16C(29) -- real.ecx has them all set, so they survive. */
    CHECK_HEX("FMA bit passthrough", 1, (out.ecx & (1u << 12)) != 0);
    CHECK_HEX("XSAVE bit passthrough", 1, (out.ecx & (1u << 26)) != 0);
    /*
     * OSXSAVE is deliberately NOT passed through -- it mirrors the guest's CR4.OSXSAVE, and
     * this call supplies no CR4, so it must read clear. Telling a guest OSXSAVE is on when its
     * own CR4 says otherwise made an OVMF AP skip enabling it and #UD on XGETBV.
     */
    CHECK_HEX("OSXSAVE is NOT passed through -- it mirrors guest CR4", 0,
              (out.ecx & (1u << 27)) != 0);
    CHECK_HEX("AVX bit passthrough", 1, (out.ecx & (1u << 28)) != 0);
    CHECK_HEX("F16C bit passthrough", 1, (out.ecx & (1u << 29)) != 0);
    /* ecx = real | hypervisor-present, minus TSC_DEADLINE(24), X2APIC(21), MONITOR(3) (#256: an
     * un-intercepted MWAIT never exits, so the guest must not be offered it), VMX(5) (#552: the
     * Intel half of #316's "hype does not expose virtualization to guests", which this expectation
     * did not include until the ported CPUMSR microtest found a guest being told VT-x existed), and
     * OSXSAVE(27), which is a mirror of the guest's CR4 rather than a capability and is 0 because
     * this call supplies no CR4. */
    CHECK_HEX("ecx otherwise passthrough",
              (real.ecx | (1u << 31)) & ~(1u << 24) & ~(1u << 21) & ~(1u << 3) & ~(1u << 5) &
                  ~(1u << 27),
              out.ecx);
    /* #436: MTRR is REPORTED, because hype models the MTRR MSRs (MTRRcap,
     * MTRRdefType, the variable pairs and the fixed ranges). Clearing it while
     * implementing it made a strict guest see a processor missing a required
     * feature -- Windows answers that with bugcheck 0x5D UNSUPPORTED_PROCESSOR,
     * read out of its own bugcheck record. */
    CHECK_HEX("MTRR bit reported (the MSRs are modelled)", 1, (out.edx & (1u << 12)) != 0);
    /* Only HTT (bit 28) is cleared now: one logical processor. */
    CHECK_HEX("edx otherwise passthrough", real.edx & ~(1u << 28), out.edx);
}

static void test_leaf1_mtrr_absent_on_the_host_stays_absent(void) {
    /* If a host somehow lacked MTRR, hype must not invent it -- the bit is
     * passed through, not forced on. */
    hype_cpuid_result_t real = {0, 0, 0, 0xFFFFEFFFu}; /* MTRR bit already 0 */
    hype_cpuid_result_t out;

    hype_cpuid_emulate(1, 0, &real, &out);

    /* #436: HTT (bit 28) is cleared even when MTRR was already clear. */
    CHECK_HEX("edx has HTT cleared when MTRR already clear", real.edx & ~(1u << 28), out.edx);
}

static void test_leaf_ext1_clears_svm_bit(void) {
    hype_cpuid_result_t real = {0x00800F11u, 0, 0xFFFFFFFFu, 0x2C100800u};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000001u, 0, &real, &out);

    CHECK_HEX("eax passthrough", real.eax, out.eax);
    CHECK_HEX("edx passthrough (NX/LM bits)", real.edx, out.edx);
    CHECK_HEX("SVM bit forced clear", 0, (out.ecx & (1u << 2)) != 0);
    CHECK_HEX("ecx otherwise passthrough", real.ecx & ~(1u << 2), out.ecx);
    CHECK_HEX("ebx is zero", 0, out.ebx);
}

static void test_leaf_ext1_svm_already_clear_is_idempotent(void) {
    hype_cpuid_result_t real = {0, 0, 0xFFFFFFFBu, 0}; /* SVM bit already 0 */
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000001u, 0, &real, &out);

    CHECK_HEX("ecx unchanged when SVM bit already clear", real.ecx, out.ecx);
}

static void test_leaf_ext8_address_sizes_passthrough(void) {
    /* real.ecx carries the host core count: NC[7:0]=0x0F (16 cores), ApicIdCoreIdSize[15:12]=4. */
    hype_cpuid_result_t real = {0x00003028u, 0x00000000u, 0x0000400Fu, 0x00000000u};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000008u, 0, &real, &out);

    CHECK_HEX("eax passthrough (phys/linear address widths)", real.eax, out.eax);
    CHECK_HEX("ebx passthrough", real.ebx, out.ebx);
    /* #436: ecx (core count NC + ApicIdCoreIdSize) forced to 0 = single core, so the guest
     * does not try to start phantom APs. */
    CHECK_HEX("ecx (core count) forced to single core", 0u, out.ecx);
    CHECK_HEX("edx passthrough", real.edx, out.edx);
}

static void test_leaf6_advertises_arat_only(void) {
    /* Real hardware reports thermal/power bits here; hype advertises only
     * ARAT (EAX bit 2) so Linux trusts the LAPIC timer in idle instead of
     * falling back to the 100 Hz PIT broadcast. */
    hype_cpuid_result_t real = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(6u, 0, &real, &out);

    CHECK_HEX("eax = ARAT (bit 2) only", (1u << 2), out.eax);
    CHECK_HEX("ebx zeroed", 0u, out.ebx);
    CHECK_HEX("ecx zeroed", 0u, out.ecx);
    CHECK_HEX("edx zeroed", 0u, out.edx);
}

static void test_leaf_ext7_advertises_invariant_tsc_only(void) {
    /* Real hardware would report power-management bits here; hype ignores
     * them and advertises only Invariant TSC (EDX bit 8) so the guest keeps
     * its passthrough TSC instead of marking it unstable. */
    hype_cpuid_result_t real = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000007u, 0, &real, &out);

    CHECK_HEX("eax zeroed", 0u, out.eax);
    CHECK_HEX("ebx zeroed", 0u, out.ebx);
    CHECK_HEX("ecx zeroed", 0u, out.ecx);
    CHECK_HEX("edx = Invariant TSC (bit 8) only", (1u << 8), out.edx);
}

static void test_hypervisor_signature_is_kvm(void) {
    /* PVCLOCK: present the KVM identity so a Linux/BSD guest enables kvmclock
     * (bypasses the guest's failing TSC calibration). "KVMKVMKVM\0\0\0". */
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x40000000u, 0, &real, &out);

    CHECK_HEX("max hypervisor leaf = 0x40000001", 0x40000001u, out.eax);
    CHECK_HEX("ebx \"KVMK\"", 0x4b4d564bu, out.ebx);
    CHECK_HEX("ecx \"VMKV\"", 0x564b4d56u, out.ecx);
    CHECK_HEX("edx \"M\\0\\0\\0\"", 0x0000004du, out.edx);
}

static void test_kvm_features_leaf_advertises_only_pvclock(void) {
    hype_cpuid_result_t real = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x40000001u, 0, &real, &out);

    /* Only CLOCKSOURCE (bit 0), CLOCKSOURCE2 (bit 3), TSC_STABLE (bit 24). */
    CHECK_HEX("kvm features eax", (1u << 0) | (1u << 3) | (1u << 24), out.eax);
    CHECK_HEX("kvm features ebx zeroed", 0u, out.ebx);
    CHECK_HEX("kvm features ecx zeroed", 0u, out.ecx);
    CHECK_HEX("kvm features edx zeroed", 0u, out.edx);
    CHECK_HEX("async-PF NOT advertised (bit 2)", 0, (out.eax & (1u << 2)) != 0);
    CHECK_HEX("steal-time NOT advertised (bit 5)", 0, (out.eax & (1u << 5)) != 0);
}

static void test_leaf7_structured_ext_passthrough(void) {
    /* Leaf 7 (AVX2/AVX-512/BMI/...) passes straight through from the host for
     * the requested sub-leaf, so leaf 1's XSAVE/AVX advertisement is coherent. */
    /* edx = 0xFC000000 sets exactly the speculation-control bits 26-31, which
     * must be masked off (their control MSRs aren't emulated); eax/ebx/ecx pass
     * through. ebx 0x239C27A9 carries AVX2(5)/BMI(3,8)/... which stay. */
    hype_cpuid_result_t real = {0x00000001u, 0x239C27A9u, 0x00000000u, 0xFC000000u};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(7, 0, &real, &out);

    CHECK_HEX("leaf7 eax passthrough (max sub-leaf)", real.eax, out.eax);
    CHECK_HEX("leaf7 ebx passthrough", real.ebx, out.ebx);
    CHECK_HEX("leaf7 ecx passthrough", real.ecx, out.ecx);
    CHECK_HEX("leaf7 edx spec-ctrl bits masked", 0u, out.edx);
}

static void test_leaf7_edx_nonspec_bits_survive_mask(void) {
    /* Non-speculation EDX bits (e.g. FSRM=4, SERIALIZE=14) pass through; only
     * bits 26-31 are cleared. real.edx = FSRM|SERIALIZE|SPEC_CTRL|SSBD. */
    hype_cpuid_result_t real = {0, 0, 0, (1u << 4) | (1u << 14) | (1u << 26) | (1u << 31)};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(7, 0, &real, &out);

    CHECK_HEX("leaf7 edx keeps non-spec bits, clears spec bits",
              (1u << 4) | (1u << 14), out.edx);
}

static void test_leafD_xsave_passthrough(void) {
    /* Leaf 0xD (XSAVE state-component enumeration) passes through so the guest
     * can size its XSAVE area / enable XCR0 -- without it, leaf 1's XSAVE bit is
     * a lie and glibc's AVX ifunc path faults. Sub-leaf carried via `real`. */
    hype_cpuid_result_t real = {0x00000207u, 0x00000340u, 0x00000340u, 0x00000000u};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x0Du, 0, &real, &out);

    CHECK_HEX("leafD eax passthrough (XCR0 low mask)", real.eax, out.eax);
    CHECK_HEX("leafD ebx passthrough (enabled area size)", real.ebx, out.ebx);
    CHECK_HEX("leafD ecx passthrough (max area size)", real.ecx, out.ecx);
    CHECK_HEX("leafD edx passthrough (XCR0 high mask)", real.edx, out.edx);
}

static void test_unhandled_leaf_returns_all_zero(void) {
    hype_cpuid_result_t real = {0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu, 0xDDDDDDDDu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(2, 0, &real, &out); /* cache descriptors -- not implemented */

    CHECK_HEX("eax", 0, out.eax);
    CHECK_HEX("ebx", 0, out.ebx);
    CHECK_HEX("ecx", 0, out.ecx);
    CHECK_HEX("edx", 0, out.edx);
}

/*
 * #361: this test used to assert that 0x80000004 -- a BRAND-STRING leaf -- returned zeroes, with
 * the comment "brand string -- not implemented". It was encoding the defect as expected
 * behaviour: leaf 0x80000000 advertises 0x80000008, so the guest is told that leaf exists.
 *
 * Retargeted at a leaf that really is unmodelled and NOT advertised. 0x80000009 is above the
 * advertised maximum, so returning zeroes there is correct rather than a promise broken.
 */
static void test_unhandled_extended_leaf_returns_all_zero(void) {
    hype_cpuid_result_t real = {1, 2, 3, 4};
    hype_cpuid_result_t out;

    hype_cpuid_emulate(0x80000009u, 0, &real, &out); /* above the advertised max extended leaf */

    CHECK_HEX("eax", 0, out.eax);
    CHECK_HEX("ebx", 0, out.ebx);
    CHECK_HEX("ecx", 0, out.ecx);
    CHECK_HEX("edx", 0, out.edx);
}

/*
 * #552: leaf 1 ECX bit 5, VMX. The Intel mirror of the SVM mask on 0x80000001, and it was missing
 * -- so a guest on an Intel host was told VT-x was available while a guest on AMD correctly saw no
 * SVM. #316's rule is that hype does not expose virtualization to guests; this is the half of it
 * that was never applied.
 *
 * The test that should have caught it is test_leaf_ext1_clears_svm_bit above, which existed for the
 * AMD bit alone. Written as its deliberate mirror.
 */
static void test_leaf1_masks_vmx(void) {
    hype_cpuid_result_t real, out;
    real.eax = 0; real.ebx = 0; real.edx = 0;
    real.ecx = 0xFFFFFFFFu; /* a host advertising everything, VMX included */
    hype_cpuid_emulate(1u, 0u, &real, &out);
    if ((out.ecx & (1u << 5)) != 0u) {
        printf("FAIL: leaf 1 ECX VMX (bit 5) must be masked -- hype does not expose "
               "virtualization to guests (#316/#552)\n");
        failures++;
    }
    /* And it must stay masked when the host does not advertise it either -- idempotent, the same
     * property test_leaf_ext1_svm_already_clear_is_idempotent asserts for SVM. */
    real.ecx = 0xFFFFFFFFu & ~(1u << 5);
    hype_cpuid_emulate(1u, 0u, &real, &out);
    if ((out.ecx & (1u << 5)) != 0u) {
        printf("FAIL: leaf 1 ECX VMX must stay clear when the host's is clear\n");
        failures++;
    }
}

static void test_leaf1_masks_monitor_mwait(void) {
    /* #256: hype does not intercept MONITOR/MWAIT, and an un-intercepted MWAIT never
     * exits -- so a guest that picks MWAIT for its idle loop never yields and hype
     * gets no idle signal at all (hlt=0 across 655k exits). Mask the bit so Linux
     * falls back to HLT idle, which hype does model. */
    hype_cpuid_result_t real, out;
    real.eax = 0; real.ebx = 0; real.edx = 0;
    real.ecx = 0xFFFFFFFFu; /* a host advertising everything, MONITOR included */
    hype_cpuid_emulate(1u, 0u, &real, &out);
    if ((out.ecx & (1u << 3)) != 0u) {
        printf("FAIL: leaf 1 ECX MONITOR (bit 3) must be masked\n");
        failures++;
    }
    /* The neighbours it sits with must stay masked too, and hypervisor-present set. */
    if ((out.ecx & (1u << 24)) != 0u || (out.ecx & (1u << 21)) != 0u) {
        printf("FAIL: TSC_DEADLINE and X2APIC must stay masked\n");
        failures++;
    }
    if ((out.ecx & (1u << 31)) == 0u) {
        printf("FAIL: hypervisor-present (bit 31) must be set\n");
        failures++;
    }
    /* Everything else still passes through -- masking must be surgical, not a
     * wholesale clear that would cost the guest SSE/AVX/XSAVE. */
    if ((out.ecx & (1u << 0)) == 0u || (out.ecx & (1u << 26)) == 0u) {
        printf("FAIL: unrelated ECX feature bits must still pass through\n");
        failures++;
    }
}

static void test_leafd_masks_xsaves(void) {
    /* #252/#269: XSAVES manages SUPERVISOR state through IA32_XSS (0xDA0), and hype
     * models no such MSR -- guest writes are absorbed. Advertising the instruction
     * while its control register is dead is the same inconsistency leaf 7 guards
     * against for the speculation-control bits.
     *
     * It also made Linux disagree with itself: kernel_size came from the compacted
     * path (because XSAVES was advertised) while `size` was computed on the
     * uncompacted layout, and fpu__init_system_xstate WARNed that 840 != 2696.
     * Masking the bit removed the WARNING on real Intel hardware. */
    hype_cpuid_result_t real, out;
    real.eax = 0xFu; /* XSAVEOPT|XSAVEC|XGETBV1|XSAVES, as the host reports */
    real.ebx = 0x348u; real.ecx = 0u; real.edx = 0u;

    hype_cpuid_emulate(0xDu, 1u, &real, &out);
    if ((out.eax & (1u << 3)) != 0u) {
        printf("FAIL: leaf 0xD sub-leaf 1 must mask XSAVES (EAX bit 3)\n");
        failures++;
    }
    /* XSAVEOPT/XSAVEC/XGETBV1 must survive -- masking has to be surgical, or the
     * guest loses the compacted XSAVE it legitimately can use. */
    if ((out.eax & 0x7u) != 0x7u) {
        printf("FAIL: XSAVEOPT/XSAVEC/XGETBV1 must still pass through, got 0x%x\n", out.eax);
        failures++;
    }
    /* The size fields are untouched -- only the capability bit is masked. */
    if (out.ebx != 0x348u) {
        printf("FAIL: sub-leaf 1 EBX must pass through unchanged, got 0x%x\n", out.ebx);
        failures++;
    }

    /* Other sub-leaves must be untouched: sub-leaf 0 EAX is the XCR0-supported
     * feature mask, where bit 3 has an entirely different meaning. */
    real.eax = 0x207u;
    hype_cpuid_emulate(0xDu, 0u, &real, &out);
    if (out.eax != 0x207u) {
        printf("FAIL: sub-leaf 0 EAX must pass through unmasked, got 0x%x\n", out.eax);
        failures++;
    }
    real.eax = 0x100u;
    hype_cpuid_emulate(0xDu, 2u, &real, &out);
    if (out.eax != 0x100u) {
        printf("FAIL: per-component sub-leaves must pass through, got 0x%x\n", out.eax);
        failures++;
    }
}

/* --- M7-1 (#91): Hyper-V leaves, opt-in per vCPU --- */

static void test_hv_disabled_leaves_kvm_identity_untouched(void) {
    /* The regression that matters most: with hv off, nothing about the leaves the
     * working Linux/BSD guests read may change. */
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    hype_cpuid_emulate_ex(0x40000000u, 0, 0, &real, &out);
    CHECK_HEX("hv off: 0x40000000 still \"KVMK\"", 0x4b4d564bu, out.ebx);
    CHECK_HEX("hv off: kvm base is 0x40000000", 0x40000000u, hype_cpuid_kvm_base(0));

    hype_cpuid_emulate_ex(0x40000001u, 0, 0, &real, &out);
    CHECK_HEX("hv off: pvclock bits at 0x40000001", (1u << 0) | (1u << 3) | (1u << 24), out.eax);

    /* And the Hyper-V leaves must not appear at all. */
    hype_cpuid_emulate_ex(0x40000002u, 0, 0, &real, &out);
    CHECK_HEX("hv off: 0x40000002 is all-zero", 0u, out.eax | out.ebx | out.ecx | out.edx);
}

static void test_hv_enabled_reports_microsoft_hv_signature(void) {
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    hype_cpuid_emulate_ex(0x40000000u, 0, 1, &real, &out);
    CHECK_HEX("hv max leaf", 0x40000006u, out.eax);
    CHECK_HEX("ebx \"Micr\"", 0x7263694du, out.ebx);
    CHECK_HEX("ecx \"osof\"", 0x666F736Fu, out.ecx);
    CHECK_HEX("edx \"t Hv\"", 0x76482074u, out.edx);

    hype_cpuid_emulate_ex(0x40000001u, 0, 1, &real, &out);
    CHECK_HEX("interface signature \"Hv#1\"", 0x31237648u, out.eax);
}

static void test_hv_enabled_relocates_kvm_leaves(void) {
    /* Windows takes 0x40000000, so kvmclock must still be findable one block up --
     * otherwise enabling the Hyper-V identity would silently cost a Linux guest the
     * paravirt clocksource PERF-1's fix depends on. */
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    CHECK_HEX("hv on: kvm base relocated", 0x40000100u, hype_cpuid_kvm_base(1));

    hype_cpuid_emulate_ex(0x40000100u, 0, 1, &real, &out);
    CHECK_HEX("relocated sig ebx \"KVMK\"", 0x4b4d564bu, out.ebx);
    CHECK_HEX("relocated sig ecx \"VMKV\"", 0x564b4d56u, out.ecx);
    CHECK_HEX("relocated sig edx \"M\\0\\0\\0\"", 0x0000004du, out.edx);
    CHECK_HEX("relocated max leaf points at its own features leaf", 0x40000101u, out.eax);

    hype_cpuid_emulate_ex(0x40000101u, 0, 1, &real, &out);
    CHECK_HEX("relocated pvclock bits", (1u << 0) | (1u << 3) | (1u << 24), out.eax);
}

static void test_hv_privilege_mask_only_claims_backed_msrs(void) {
    /* Each bit here promises an MSR group that hype_msr_decide_ex() must actually
     * answer. Reference TSC (bit 9) and the frequency MSRs (bit 11) ARE claimed
     * since #436: hype fills a real reference-TSC page (devices/../cpu/hyperv.c)
     * and answers the TSC/APIC frequency MSRs, so the promise is backed. The
     * groups still absent -- synic, synthetic timers -- stay absent, because
     * claiming an MSR group nothing answers is the failure this test exists to
     * catch. */
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    hype_cpuid_emulate_ex(0x40000003u, 0, 1, &real, &out);
    CHECK_HEX("privileges: ref counter | hypercall MSRs | vp index | ref TSC | freq MSRs",
              (1u << 1) | (1u << 5) | (1u << 6) | (1u << 9) | (1u << 11), out.eax);
    CHECK_HEX("reference TSC claimed (bit 9) -- hype fills the page", 1,
              (out.eax & (1u << 9)) != 0);
    CHECK_HEX("frequency MSRs claimed (bit 11) -- hype answers them", 1,
              (out.eax & (1u << 11)) != 0);
    CHECK_HEX("synic NOT claimed (bit 2)", 0, (out.eax & (1u << 2)) != 0);
    CHECK_HEX("synthetic timers NOT claimed (bit 3)", 0, (out.eax & (1u << 3)) != 0);
    CHECK_HEX("privilege mask high empty", 0u, out.ebx);
    CHECK_HEX("misc features empty", 0u, out.edx);
}

static void test_hv_recommends_no_enlightenments(void) {
    /* hype services no hypercalls, so recommending an enlightenment would tell
     * Windows to replace a working native operation with a call into nothing. */
    hype_cpuid_result_t real = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    hype_cpuid_result_t out;

    hype_cpuid_emulate_ex(0x40000004u, 0, 1, &real, &out);
    CHECK_HEX("no enlightenments recommended", 0u, out.eax | out.ebx | out.ecx | out.edx);
    hype_cpuid_emulate_ex(0x40000005u, 0, 1, &real, &out);
    CHECK_HEX("no implementation limits", 0u, out.eax | out.ebx | out.ecx | out.edx);
    hype_cpuid_emulate_ex(0x40000006u, 0, 1, &real, &out);
    CHECK_HEX("no hardware features claimed", 0u, out.eax | out.ebx | out.ecx | out.edx);
}

static void test_hv_leaf_beyond_max_is_not_claimed(void) {
    /* 0x40000007 is past the advertised maximum, so hype_cpuid_hv_leaf() must decline
     * it rather than returning stale register contents. */
    hype_cpuid_result_t out = {0xAAu, 0xBBu, 0xCCu, 0xDDu};

    CHECK_HEX("leaf past max declined", 0, hype_cpuid_hv_leaf(0x40000007u, 0, &out));
    CHECK_HEX("leaf below base declined", 0, hype_cpuid_hv_leaf(0x3FFFFFFFu, 0, &out));
    CHECK_HEX("NULL out declined", 0, hype_cpuid_hv_leaf(0x40000000u, 0, (hype_cpuid_result_t *)0));
    CHECK_HEX("declined leaf left `out` untouched", 0xAAu, out.eax);
}

static void test_hv_version_leaf_is_populated(void) {
    /* Windows logs/branches on the version; an all-zero build+version reads as an
     * unidentifiable hypervisor. */
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;

    hype_cpuid_emulate_ex(0x40000002u, 0, 1, &real, &out);
    CHECK_HEX("build number nonzero", 1u, out.eax);
    CHECK_HEX("major 6 minor 3", (6u << 16) | 3u, out.ebx);
}


/*
 * #361: leaf 0x80000000 advertises 0x80000008 as the max extended leaf, which promises the guest
 * that the brand-string leaves exist. They returned zeroes, so OpenBSD printed "Opteron or Athlon
 * 64" on Intel silicon -- a fallback guess from an empty string, which destroyed the evidence for
 * the #298 cross-vendor check.
 */
static void test_brand_string_leaves_are_not_zero(void) {
    hype_cpuid_result_t real, out;
    uint32_t leaf;
    /* "Intel(R) Co" ... arbitrary non-zero host values; the point is they must reach the guest. */
    for (leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        real.eax = 0x65746E49u + leaf;
        real.ebx = 0x2952286Cu;
        real.ecx = 0x726F4320u;
        real.edx = 0x00296D65u;
        out.eax = out.ebx = out.ecx = out.edx = 0xDEADBEEFu;
        hype_cpuid_emulate(leaf, 0, &real, &out);
        CHECK_HEX("brand leaf eax passes through", real.eax, out.eax);
        CHECK_HEX("brand leaf ebx passes through", real.ebx, out.ebx);
        CHECK_HEX("brand leaf ecx passes through", real.ecx, out.ecx);
        CHECK_HEX("brand leaf edx passes through", real.edx, out.edx);
    }
}

/* The promise and the implementation must not drift apart again: every leaf up to the advertised
 * maximum should return something, and the three brand leaves specifically must not be zero. */
static void test_advertised_max_extended_leaf_is_backed_by_the_brand_leaves(void) {
    hype_cpuid_result_t real, out;
    real.eax = real.ebx = real.ecx = real.edx = 0u;
    out.eax = out.ebx = out.ecx = out.edx = 0u;
    hype_cpuid_emulate(0x80000000u, 0, &real, &out);
    CHECK_HEX("max extended leaf still advertised as 0x80000008", 0x80000008u, out.eax);
    CHECK_HEX("the brand leaves are within what leaf 0x80000000 promises", 1u,
              (0x80000004u <= out.eax) ? 1u : 0u);
}

/* A host that genuinely reports zeroes must still yield zeroes -- pass-through, not invention. */
static void test_brand_string_is_not_fabricated(void) {
    hype_cpuid_result_t real, out;
    real.eax = real.ebx = real.ecx = real.edx = 0u;
    out.eax = 0xDEADBEEFu;
    hype_cpuid_emulate(0x80000003u, 0, &real, &out);
    CHECK_HEX("a zero host brand leaf stays zero", 0u, out.eax);
}

/* ---- SMP-2 (#186): guest-visible SMP topology ---- */

static void test_topo_shift_rounds_up(void) {
    /* EAX[4:0] of leaf 0x0B is "bits to shift the x2APIC ID right to reach the next level".
     * One item needs no bits; non-powers-of-two must round UP, or a guest decoding an APIC ID
     * reads bits belonging to the level above. */
    CHECK_HEX("shift(0) treated as 1 -> 0", 0u, hype_cpuid_topo_shift(0u));
    CHECK_HEX("shift(1) -> 0", 0u, hype_cpuid_topo_shift(1u));
    CHECK_HEX("shift(2) -> 1", 1u, hype_cpuid_topo_shift(2u));
    CHECK_HEX("shift(3) rounds up -> 2", 2u, hype_cpuid_topo_shift(3u));
    CHECK_HEX("shift(4) -> 2", 2u, hype_cpuid_topo_shift(4u));
    CHECK_HEX("shift(5) rounds up -> 3", 3u, hype_cpuid_topo_shift(5u));
    CHECK_HEX("shift(8) -> 3", 3u, hype_cpuid_topo_shift(8u));
}

static void test_null_topology_reproduces_the_uniprocessor_report(void) {
    /* The whole point of the null default: every pre-SMP caller and test must read unchanged. */
    hype_cpuid_result_t real = {0x00A00F11u, 0x12345678u, 0x7FFAFBFFu, 0xFFFAFBFFu};
    hype_cpuid_result_t a, b;

    hype_cpuid_emulate(1, 0, &real, &a);
    hype_cpuid_emulate_topo(1, 0, 0, (const hype_cpuid_topology_t *)0, 0ull, &real, &b);
    CHECK_HEX("null topo == legacy entry point (ebx)", a.ebx, b.ebx);
    CHECK_HEX("null topo == legacy entry point (edx)", a.edx, b.edx);
    CHECK_HEX("uniprocessor: 1 logical processor in ebx[23:16]", 1u, (a.ebx >> 16) & 0xFFu);
    CHECK_HEX("uniprocessor: APIC ID 0 in ebx[31:24]", 0u, (a.ebx >> 24) & 0xFFu);
    CHECK_HEX("uniprocessor: HTT clear", 0u, a.edx & (1u << 28));
}

static void test_leaf1_reports_this_vcpus_apic_id_and_the_vm_count(void) {
    hype_cpuid_result_t real = {0x00A00F11u, 0x99887766u, 0x7FFAFBFFu, 0xFFFAFBFFu};
    hype_cpuid_result_t out;
    hype_cpuid_topology_t topo;
    topo.vcpu_count = 4u;
    topo.threads_per_core = 1u;

    topo.apic_id = 0u;
    hype_cpuid_emulate_topo(1, 0, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("BSP: apic id 0", 0u, (out.ebx >> 24) & 0xFFu);
    CHECK_HEX("4 logical processors", 4u, (out.ebx >> 16) & 0xFFu);
    CHECK_HEX("HTT set once there is more than one", 1u << 28, out.edx & (1u << 28));
    /* Host brand index + CLFLUSH size still pass through -- they are not topology. */
    CHECK_HEX("ebx low half preserved", 0x7766u, out.ebx & 0xFFFFu);

    topo.apic_id = 3u;
    hype_cpuid_emulate_topo(1, 0, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("AP: apic id 3", 3u, (out.ebx >> 24) & 0xFFu);
    CHECK_HEX("count is per-VM, not per-vCPU", 4u, (out.ebx >> 16) & 0xFFu);

    /* x2APIC must STAY masked whatever the topology -- hype is MMIO-LAPIC-only. */
    CHECK_HEX("x2APIC still cleared with 4 vCPUs", 0u, out.ecx & (1u << 21));
}

static void test_leaf_b_reports_smt_and_core_levels(void) {
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;
    hype_cpuid_topology_t topo;

    /*
     * 4 LOGICAL CPUs as 2 cores x 2 threads -- what `vcpus = 2` yields on an SMT host, since a
     * vCPU is a physical core and SMT is a bonus (§10 decision 47). vcpu_count at this layer is
     * the count the GUEST sees, not the config's core count.
     */
    topo.apic_id = 2u;
    topo.vcpu_count = 4u;
    topo.threads_per_core = 2u;

    hype_cpuid_emulate_topo(0xBu, 0u, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("SMT level: type 1", 1u, (out.ecx >> 8) & 0xFFu);
    CHECK_HEX("SMT level: echoes input level", 0u, out.ecx & 0xFFu);
    CHECK_HEX("SMT level: 2 threads", 2u, out.ebx);
    CHECK_HEX("SMT level: shift 1 past the thread bit", 1u, out.eax);
    CHECK_HEX("SMT level: x2APIC id is this vCPU's", 2u, out.edx);

    hype_cpuid_emulate_topo(0xBu, 1u, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("core level: type 2", 2u, (out.ecx >> 8) & 0xFFu);
    CHECK_HEX("core level: 4 logical processors", 4u, out.ebx);
    CHECK_HEX("core level: shift 2 past the whole package", 2u, out.eax);

    hype_cpuid_emulate_topo(0xBu, 2u, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("level 2 terminates enumeration (ebx 0)", 0u, out.ebx);
    CHECK_HEX("level 2 terminates enumeration (type 0)", 0u, (out.ecx >> 8) & 0xFFu);
}

static void test_leaf_b_single_vcpu_is_still_a_consistent_machine(void) {
    /* #436's original complaint: an all-zero leaf 0x0B has no consistent reading. One vCPU
     * must still enumerate as one thread in one core, not as zero of anything. */
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;
    hype_cpuid_topology_t topo = {0u, 1u, 1u};

    hype_cpuid_emulate_topo(0xBu, 0u, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("1 thread at the SMT level", 1u, out.ebx);
    CHECK_HEX("no shift needed", 0u, out.eax);
    hype_cpuid_emulate_topo(0xBu, 1u, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("1 logical processor at the core level", 1u, out.ebx);
}

static void test_amd_leaf_8_reports_the_vm_not_the_host(void) {
    /* ECX[7:0] = logical processors - 1. #436: passing the host's through made guests start
     * phantom APs; reporting fewer than the MADT lists is the same defect mirrored. */
    hype_cpuid_result_t real = {0x00003030u, 0, 0x0000700Fu, 0};
    hype_cpuid_result_t out;
    hype_cpuid_topology_t topo = {0u, 4u, 1u};

    hype_cpuid_emulate_topo(0x80000008u, 0, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("NC = vcpus - 1", 3u, out.ecx & 0xFFu);
    CHECK_HEX("ApicIdCoreIdSize covers 4 ids", 2u, (out.ecx >> 12) & 0xFu);

    topo.vcpu_count = 1u;
    hype_cpuid_emulate_topo(0x80000008u, 0, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("single vCPU: NC 0", 0u, out.ecx & 0xFFu);
    CHECK_HEX("single vCPU: no core id bits", 0u, (out.ecx >> 12) & 0xFu);
}

static void test_topology_leaves_agree_with_each_other(void) {
    /* The failure this guards is a guest seeing two different CPU counts from two leaves and
     * distrusting one -- which is what the MADT/CPUID mismatch warning is. */
    hype_cpuid_result_t real = {0x00A00F11u, 0x12345678u, 0x7FFAFBFFu, 0xFFFAFBFFu};
    hype_cpuid_result_t l1, lb, l8;
    hype_cpuid_topology_t topo = {1u, 8u, 2u};

    hype_cpuid_emulate_topo(1u, 0, 0, &topo, 0ull, &real, &l1);
    hype_cpuid_emulate_topo(0xBu, 1u, 0, &topo, 0ull, &real, &lb);
    hype_cpuid_emulate_topo(0x80000008u, 0, 0, &topo, 0ull, &real, &l8);
    CHECK_HEX("leaf 1 count == leaf 0xB core-level count", (l1.ebx >> 16) & 0xFFu, lb.ebx);
    CHECK_HEX("leaf 1 count == leaf 0x80000008 NC + 1", (l1.ebx >> 16) & 0xFFu,
              (l8.ecx & 0xFFu) + 1u);
}

static void test_degenerate_topology_values_do_not_produce_nonsense(void) {
    /* A caller that forgets to set the struct must not make the guest read "0 processors". */
    hype_cpuid_result_t real = {0, 0, 0, 0};
    hype_cpuid_result_t out;
    hype_cpuid_topology_t topo = {0u, 0u, 0u};

    hype_cpuid_emulate_topo(0xBu, 0u, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("zero threads_per_core floors at 1", 1u, out.ebx);
    hype_cpuid_emulate_topo(1u, 0, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("zero vcpu_count floors at 1", 1u, (out.ebx >> 16) & 0xFFu);
}

static void test_osxsave_mirrors_guest_cr4(void) {
    /* The SDM makes CPUID.1:ECX[27] a read-only mirror of CR4.OSXSAVE. Both directions matter:
     * reporting it set when the guest has not enabled XSAVE state is what killed an OVMF AP,
     * and reporting it clear when the guest HAS enabled it would make the guest disbelieve its
     * own successful XSETBV. */
    hype_cpuid_result_t real = {0x00A00F11u, 0x12345678u, 0xFFFFFFFFu, 0xFFFAFBFFu};
    hype_cpuid_result_t out;
    hype_cpuid_topology_t topo = {0u, 1u, 1u};

    hype_cpuid_emulate_topo(1u, 0, 0, &topo, 0ull, &real, &out);
    CHECK_HEX("CR4.OSXSAVE clear -> bit 27 clear", 0u, out.ecx & (1u << 27));

    hype_cpuid_emulate_topo(1u, 0, 0, &topo, HYPE_CR4_OSXSAVE_BIT, &real, &out);
    CHECK_HEX("CR4.OSXSAVE set -> bit 27 set", 1u << 27, out.ecx & (1u << 27));

    /* Other CR4 bits must not be mistaken for it. */
    hype_cpuid_emulate_topo(1u, 0, 0, &topo, 0x668ull, &real, &out);
    CHECK_HEX("CR4=0x668 (no OSXSAVE) -> bit 27 clear", 0u, out.ecx & (1u << 27));

    /* XSAVE (26) is a real capability and still passes through. */
    CHECK_HEX("XSAVE(26) still passed through", 1u << 26, out.ecx & (1u << 26));
}

int main(void) {
    test_osxsave_mirrors_guest_cr4();
    test_topo_shift_rounds_up();
    test_null_topology_reproduces_the_uniprocessor_report();
    test_leaf1_reports_this_vcpus_apic_id_and_the_vm_count();
    test_leaf_b_reports_smt_and_core_levels();
    test_leaf_b_single_vcpu_is_still_a_consistent_machine();
    test_amd_leaf_8_reports_the_vm_not_the_host();
    test_topology_leaves_agree_with_each_other();
    test_degenerate_topology_values_do_not_produce_nonsense();
    test_brand_string_leaves_are_not_zero();
    test_advertised_max_extended_leaf_is_backed_by_the_brand_leaves();
    test_brand_string_is_not_fabricated();
    test_leaf0_vendor_is_passed_through();
    test_ext_leaf0_vendor_matches_basic_leaf0();
    test_leaf1_forces_hypervisor_bit_and_clears_mtrr();
    test_leaf1_mtrr_absent_on_the_host_stays_absent();
    test_leaf_ext1_clears_svm_bit();
    test_leaf_ext1_svm_already_clear_is_idempotent();
    test_leaf_ext8_address_sizes_passthrough();
    test_leaf6_advertises_arat_only();
    test_leaf7_structured_ext_passthrough();
    test_leaf7_edx_nonspec_bits_survive_mask();
    test_leafD_xsave_passthrough();
    test_leaf_ext7_advertises_invariant_tsc_only();
    test_hypervisor_signature_is_kvm();
    test_kvm_features_leaf_advertises_only_pvclock();
    test_unhandled_leaf_returns_all_zero();
    test_unhandled_extended_leaf_returns_all_zero();

    test_hv_disabled_leaves_kvm_identity_untouched();
    test_hv_enabled_reports_microsoft_hv_signature();
    test_hv_enabled_relocates_kvm_leaves();
    test_hv_privilege_mask_only_claims_backed_msrs();
    test_hv_recommends_no_enlightenments();
    test_hv_leaf_beyond_max_is_not_claimed();
    test_hv_version_leaf_is_populated();

    test_leaf1_masks_vmx();
    test_leaf1_masks_monitor_mwait();
    test_leafd_masks_xsaves();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
