#include <stdio.h>
#include "../../arch/x86_64/cpu/msr_emulate.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_apic_base_read_allowed(void) {
    CHECK_HEX("APIC_BASE read", HYPE_MSR_ACTION_READ_APIC_BASE,
              hype_msr_decide(HYPE_MSR_NUMBER_APIC_BASE, 0));
}

static void test_apic_base_write_rejected(void) {
    CHECK_HEX("APIC_BASE write", HYPE_MSR_ACTION_REJECT, hype_msr_decide(HYPE_MSR_NUMBER_APIC_BASE, 1));
}

static void test_efer_read_and_write_allowed(void) {
    CHECK_HEX("EFER read", HYPE_MSR_ACTION_READWRITE_EFER, hype_msr_decide(0xC0000080u, 0));
    CHECK_HEX("EFER write", HYPE_MSR_ACTION_READWRITE_EFER, hype_msr_decide(0xC0000080u, 1));
}

static void test_tsc_read_allowed_write_rejected(void) {
    CHECK_HEX("TSC read", HYPE_MSR_ACTION_READ_TSC, hype_msr_decide(HYPE_MSR_NUMBER_TSC, 0));
    CHECK_HEX("TSC write", HYPE_MSR_ACTION_REJECT, hype_msr_decide(HYPE_MSR_NUMBER_TSC, 1));
}

static void test_unknown_msr_rejected_both_directions(void) {
    CHECK_HEX("unknown MSR read", HYPE_MSR_ACTION_REJECT, hype_msr_decide(0xDEADBEEFu, 0));
    CHECK_HEX("unknown MSR write", HYPE_MSR_ACTION_REJECT, hype_msr_decide(0xDEADBEEFu, 1));
}

static void test_apic_base_value(void) {
    uint64_t value = hype_msr_apic_base_value(1);

    CHECK_HEX("base address bits", 0xFEE00000ULL, value & 0xFFFFF000ULL);
    CHECK_HEX("global enable bit set", 1, (value & (1ULL << 11)) != 0);
    CHECK_HEX("BSP bit set", 1, (value & (1ULL << 8)) != 0);
    CHECK_HEX("x2APIC bit clear", 0, (value & (1ULL << 10)) != 0);
}

/*
 * #190: an AP must report BSP=0. edk2's PEI MpInitLib branches on this bit to
 * choose between the HOB path (BSP) and reading CpuMpData off the AP's own
 * stack. An AP told BSP=1 takes the HOB path, which needs PEI services it
 * cannot reach, and faults. hype returned a hardcoded BSP=1 for every vCPU.
 */
static void test_apic_base_value_ap(void) {
    uint64_t bsp = hype_msr_apic_base_value(1);
    uint64_t ap = hype_msr_apic_base_value(0);

    CHECK_HEX("AP base address bits", 0xFEE00000ULL, ap & 0xFFFFF000ULL);
    CHECK_HEX("AP global enable bit set", 1, (ap & (1ULL << 11)) != 0);
    CHECK_HEX("AP BSP bit CLEAR", 0, (ap & (1ULL << 8)) != 0);
    CHECK_HEX("AP x2APIC bit clear", 0, (ap & (1ULL << 10)) != 0);
    CHECK_HEX("BSP and AP differ only in bit 8", (1ULL << 8), bsp ^ ap);
}

/*
 * #251: FS/GS base are read/write BOTH ways, unlike APIC_BASE and TSC. Rejecting
 * the write is what left a 64-bit guest with no usable GS base, faulting on the
 * kernel's first `MOV RAX, GS:[0x28]`.
 */
static void test_fs_gs_base_readwrite(void) {
    CHECK_HEX("FS_BASE read", HYPE_MSR_ACTION_READWRITE_FS_BASE,
              hype_msr_decide(0xC0000100u, 0));
    CHECK_HEX("FS_BASE write", HYPE_MSR_ACTION_READWRITE_FS_BASE,
              hype_msr_decide(0xC0000100u, 1));
    CHECK_HEX("GS_BASE read", HYPE_MSR_ACTION_READWRITE_GS_BASE,
              hype_msr_decide(0xC0000101u, 0));
    CHECK_HEX("GS_BASE write", HYPE_MSR_ACTION_READWRITE_GS_BASE,
              hype_msr_decide(0xC0000101u, 1));
}

static void test_fs_gs_base_are_distinct_actions(void) {
    /* One action for both would force the caller to re-decode the MSR number to
     * pick a VMCS field -- exactly the kind of duplicated decision this module
     * exists to remove. */
    CHECK_HEX("FS_BASE action differs from GS_BASE", 1,
              (unsigned)(hype_msr_decide(0xC0000100u, 1) != hype_msr_decide(0xC0000101u, 1)));
}

static void test_kernel_gs_base_still_rejected(void) {
    /* IA32_KERNEL_GS_BASE (0xC0000102) is deliberately NOT modelled: it has no
     * VMCS field, and SWAPGS does not exit, so a value captured at WRMSR time
     * would be wrong the moment the guest swaps. It needs the MSR-load/store
     * areas instead -- until then it must stay absorbed, not silently mapped
     * onto GS_BASE. */
    CHECK_HEX("KERNEL_GS_BASE not modelled yet", HYPE_MSR_ACTION_REJECT,
              hype_msr_decide(0xC0000102u, 1));
}

/* --- M7-1 (#91): Hyper-V synthetic MSRs --- */

static void test_hv_msrs_rejected_when_hv_disabled(void) {
    /* The gate that protects the working Linux guests: with hv off these are just
     * unknown MSRs, so a guest that was never shown the "Hv#1" signature cannot get a
     * success from a Hyper-V MSR. */
    CHECK_HEX("GUEST_OS_ID rejected (hv off)", HYPE_MSR_ACTION_REJECT,
              hype_msr_decide(HYPE_MSR_NUMBER_HV_GUEST_OS_ID, 1));
    CHECK_HEX("HYPERCALL rejected (hv off)", HYPE_MSR_ACTION_REJECT,
              hype_msr_decide(HYPE_MSR_NUMBER_HV_HYPERCALL, 1));
    CHECK_HEX("VP_INDEX rejected (hv off)", HYPE_MSR_ACTION_REJECT,
              hype_msr_decide(HYPE_MSR_NUMBER_HV_VP_INDEX, 0));
    CHECK_HEX("TIME_REF_COUNT rejected (hv off)", HYPE_MSR_ACTION_REJECT,
              hype_msr_decide(HYPE_MSR_NUMBER_HV_TIME_REF_COUNT, 0));
}

static void test_hv_os_id_and_hypercall_are_readwrite(void) {
    /* Windows writes GUEST_OS_ID before it has read anything back, so the write must
     * be absorbed rather than rejected -- a #GP that early kills the guest. */
    CHECK_HEX("GUEST_OS_ID write", HYPE_MSR_ACTION_READWRITE_HV_GUEST_OS_ID,
              hype_msr_decide_ex(HYPE_MSR_NUMBER_HV_GUEST_OS_ID, 1, 1));
    CHECK_HEX("GUEST_OS_ID read", HYPE_MSR_ACTION_READWRITE_HV_GUEST_OS_ID,
              hype_msr_decide_ex(HYPE_MSR_NUMBER_HV_GUEST_OS_ID, 0, 1));
    CHECK_HEX("HYPERCALL write", HYPE_MSR_ACTION_READWRITE_HV_HYPERCALL,
              hype_msr_decide_ex(HYPE_MSR_NUMBER_HV_HYPERCALL, 1, 1));
    CHECK_HEX("HYPERCALL read", HYPE_MSR_ACTION_READWRITE_HV_HYPERCALL,
              hype_msr_decide_ex(HYPE_MSR_NUMBER_HV_HYPERCALL, 0, 1));
}

static void test_hv_counters_are_read_only(void) {
    CHECK_HEX("VP_INDEX read", HYPE_MSR_ACTION_READ_HV_VP_INDEX,
              hype_msr_decide_ex(HYPE_MSR_NUMBER_HV_VP_INDEX, 0, 1));
    CHECK_HEX("VP_INDEX write rejected", HYPE_MSR_ACTION_REJECT,
              hype_msr_decide_ex(HYPE_MSR_NUMBER_HV_VP_INDEX, 1, 1));
    CHECK_HEX("TIME_REF_COUNT read", HYPE_MSR_ACTION_READ_HV_TIME_REF_COUNT,
              hype_msr_decide_ex(HYPE_MSR_NUMBER_HV_TIME_REF_COUNT, 0, 1));
    CHECK_HEX("TIME_REF_COUNT write rejected", HYPE_MSR_ACTION_REJECT,
              hype_msr_decide_ex(HYPE_MSR_NUMBER_HV_TIME_REF_COUNT, 1, 1));
}

static void test_hv_reference_tsc_msr_matches_cpuid(void) {
    /* The pairing this guards is what matters, not the direction: CPUID leaf
     * 0x40000003 and the MSR decoder must agree. Since #436 hype implements the
     * reference-TSC page, so the leaf claims AccessPartitionReferenceTsc and the
     * MSR is answered here -- claiming one without the other is the drift this
     * test exists to catch. Synic stays unclaimed on both sides. */
    CHECK_HEX("REFERENCE_TSC answered (matches the claimed CPUID bit)",
              HYPE_MSR_ACTION_READWRITE_HV_REFERENCE_TSC,
              hype_msr_decide_ex(0x40000021u, 0, 1));
    CHECK_HEX("SCONTROL (synic) still unknown", HYPE_MSR_ACTION_REJECT,
              hype_msr_decide_ex(0x40000080u, 1, 1));
}

static void test_hv_enable_does_not_disturb_existing_msrs(void) {
    CHECK_HEX("EFER unchanged with hv on", HYPE_MSR_ACTION_READWRITE_EFER,
              hype_msr_decide_ex(0xC0000080u, 1, 1));
    CHECK_HEX("TSC unchanged with hv on", HYPE_MSR_ACTION_READ_TSC,
              hype_msr_decide_ex(HYPE_MSR_NUMBER_TSC, 0, 1));
    CHECK_HEX("unknown MSR still rejected with hv on", HYPE_MSR_ACTION_REJECT,
              hype_msr_decide_ex(0x8Bu, 0, 1));
}

static void test_hv_ref_count_conversion(void) {
    /* 100ns units. One second of a 1 GHz TSC is 10,000,000 ticks. */
    CHECK_HEX("1s at 1GHz = 1e7 ticks", 10000000ULL,
              hype_msr_hv_ref_count_from_tsc(1000000000ULL, 1000000ULL));
    /* Sub-millisecond resolution has to survive: a plain tsc/khz would floor this to
     * 0 ms and report no time at all. */
    CHECK_HEX("100ns at 1GHz = 1 tick", 1ULL,
              hype_msr_hv_ref_count_from_tsc(100ULL, 1000000ULL));
    CHECK_HEX("1us at 3GHz = 10 ticks", 10ULL,
              hype_msr_hv_ref_count_from_tsc(3000ULL, 3000000ULL));
    /* An hour at 3GHz -- checks the split arithmetic does not overflow where a
     * naive delta*10000 would. */
    CHECK_HEX("1h at 3GHz = 3.6e10 ticks", 36000000000ULL,
              hype_msr_hv_ref_count_from_tsc(3000000000ULL * 3600ULL, 3000000ULL));
    /* Unknown timebase reports a stalled clock rather than dividing by zero. */
    CHECK_HEX("zero frequency yields 0", 0ULL,
              hype_msr_hv_ref_count_from_tsc(123456789ULL, 0ULL));
}

/* --- #601: the x2APIC MSR range and the IA32_APIC_BASE state machine --- */

static void test_x2apic_range_check(void) {
    CHECK_HEX("0x800 is in range", 1, hype_msr_is_x2apic_range(0x800u));
    CHECK_HEX("0x8FF is in range", 1, hype_msr_is_x2apic_range(0x8FFu));
    CHECK_HEX("0x830 (ICR) is in range", 1, hype_msr_is_x2apic_range(0x830u));
    CHECK_HEX("0x7FF is NOT in range", 0, hype_msr_is_x2apic_range(0x7FFu));
    CHECK_HEX("0x900 is NOT in range", 0, hype_msr_is_x2apic_range(0x900u));
    CHECK_HEX("APIC_BASE itself is NOT in the x2APIC MSR range", 0,
              hype_msr_is_x2apic_range(HYPE_MSR_NUMBER_APIC_BASE));
}

/*
 * hype_msr_apic_base_value() must stay the HYPE_APIC_MODE_XAPIC case forever --
 * this is the byte-identical-when-masked regression bar #601 set for itself,
 * proven directly rather than by inspection.
 */
static void test_apic_base_value_mode_matches_legacy_helper(void) {
    CHECK_HEX("BSP: mode helper matches legacy helper", hype_msr_apic_base_value(1),
              hype_msr_apic_base_value_mode(1, HYPE_APIC_MODE_XAPIC));
    CHECK_HEX("AP: mode helper matches legacy helper", hype_msr_apic_base_value(0),
              hype_msr_apic_base_value_mode(0, HYPE_APIC_MODE_XAPIC));
}

static void test_apic_base_value_mode_disabled_clears_en(void) {
    uint64_t value = hype_msr_apic_base_value_mode(1, HYPE_APIC_MODE_DISABLED);
    CHECK_HEX("Disabled: EN clear", 0, (value & (1ULL << 11)) != 0);
    CHECK_HEX("Disabled: EXTD clear", 0, (value & (1ULL << 10)) != 0);
    CHECK_HEX("Disabled: BSP bit unaffected", 1, (value & (1ULL << 8)) != 0);
}

static void test_apic_base_value_mode_x2apic_sets_extd(void) {
    uint64_t value = hype_msr_apic_base_value_mode(0, HYPE_APIC_MODE_X2APIC);
    CHECK_HEX("x2APIC: EN set", 1, (value & (1ULL << 11)) != 0);
    CHECK_HEX("x2APIC: EXTD set", 1, (value & (1ULL << 10)) != 0);
    CHECK_HEX("x2APIC: AP BSP bit clear", 0, (value & (1ULL << 8)) != 0);
}

/* Every legal transition the SDM's state machine allows. */
static void test_apic_base_transition_legal_paths(void) {
    int next;

    CHECK_HEX("Disabled -> xAPIC", 0,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_DISABLED, 1, 0, &next));
    CHECK_HEX("Disabled -> xAPIC: next", HYPE_APIC_MODE_XAPIC, next);

    CHECK_HEX("Disabled -> x2APIC", 0,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_DISABLED, 1, 1, &next));
    CHECK_HEX("Disabled -> x2APIC: next", HYPE_APIC_MODE_X2APIC, next);

    CHECK_HEX("xAPIC -> x2APIC (the guest-initiated upgrade)", 0,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_XAPIC, 1, 1, &next));
    CHECK_HEX("xAPIC -> x2APIC: next", HYPE_APIC_MODE_X2APIC, next);

    CHECK_HEX("xAPIC -> Disabled", 0,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_XAPIC, 0, 0, &next));
    CHECK_HEX("xAPIC -> Disabled: next", HYPE_APIC_MODE_DISABLED, next);

    CHECK_HEX("x2APIC -> Disabled (the required first step of a downgrade)", 0,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_X2APIC, 0, 0, &next));
    CHECK_HEX("x2APIC -> Disabled: next", HYPE_APIC_MODE_DISABLED, next);
}

static void test_apic_base_transition_noop_rewrites_are_legal(void) {
    int next;

    CHECK_HEX("Disabled -> Disabled (no-op)", 0,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_DISABLED, 0, 0, &next));
    CHECK_HEX("xAPIC -> xAPIC (no-op)", 0,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_XAPIC, 1, 0, &next));
    CHECK_HEX("x2APIC -> x2APIC (no-op)", 0,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_X2APIC, 1, 1, &next));
}

/*
 * The one guest-reachable illegal transition this ticket exists to guard:
 * downgrading straight from x2APIC to xAPIC without passing through Disabled.
 */
static void test_apic_base_transition_x2apic_to_xapic_is_illegal(void) {
    int next = 12345; /* poisoned: must stay untouched on the illegal path */
    CHECK_HEX("x2APIC -> xAPIC directly is illegal", -1,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_X2APIC, 1, 0, &next));
    CHECK_HEX("x2APIC -> xAPIC: *next_out untouched on failure", 12345, next);
}

static void test_apic_base_transition_en0_extd1_always_illegal(void) {
    int next;
    CHECK_HEX("Disabled: EN=0,EXTD=1 is illegal", -1,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_DISABLED, 0, 1, &next));
    CHECK_HEX("xAPIC: EN=0,EXTD=1 is illegal", -1,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_XAPIC, 0, 1, &next));
    CHECK_HEX("x2APIC: EN=0,EXTD=1 is illegal", -1,
              hype_apic_base_mode_transition(HYPE_APIC_MODE_X2APIC, 0, 1, &next));
}

int main(void) {
    test_fs_gs_base_readwrite();
    test_fs_gs_base_are_distinct_actions();
    test_kernel_gs_base_still_rejected();
    test_apic_base_read_allowed();
    test_apic_base_write_rejected();
    test_efer_read_and_write_allowed();
    test_tsc_read_allowed_write_rejected();
    test_unknown_msr_rejected_both_directions();
    test_apic_base_value();
    test_apic_base_value_ap();
    test_hv_msrs_rejected_when_hv_disabled();
    test_hv_os_id_and_hypercall_are_readwrite();
    test_hv_counters_are_read_only();
    test_hv_reference_tsc_msr_matches_cpuid();
    test_hv_enable_does_not_disturb_existing_msrs();
    test_hv_ref_count_conversion();
    test_x2apic_range_check();
    test_apic_base_value_mode_matches_legacy_helper();
    test_apic_base_value_mode_disabled_clears_en();
    test_apic_base_value_mode_x2apic_sets_extd();
    test_apic_base_transition_legal_paths();
    test_apic_base_transition_noop_rewrites_are_legal();
    test_apic_base_transition_x2apic_to_xapic_is_illegal();
    test_apic_base_transition_en0_extd1_always_illegal();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
