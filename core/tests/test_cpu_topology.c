#include <stdio.h>
#include <string.h>

#include "../cpu_topology.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)
#define CHECK_INT(desc, expected, actual)                                                    \
    do {                                                                                     \
        long long e_ = (long long)(expected), a_ = (long long)(actual);                      \
        if (e_ != a_) {                                                                      \
            printf("FAIL: %s (expected %lld, got %lld)\n", (desc), e_, a_);                  \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

/* The AMD laptop: IDs 0,1,2,... The old hardcoded ap_start(1)/ap_start(2) was
 * correct here, which is exactly why the bug survived. */
static void test_consecutive_layout_matches_the_old_assumption(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add(&t, 0u, 1, 1);
    (void)hype_cpu_topology_add(&t, 1u, 0, 1);
    (void)hype_cpu_topology_add(&t, 2u, 0, 1);

    CHECK_INT("bsp is 0", 0, hype_cpu_topology_bsp(&t));
    CHECK_INT("first AP is 1", 1, hype_cpu_topology_ap(&t, 0));
    CHECK_INT("second AP is 2", 2, hype_cpu_topology_ap(&t, 1));
    CHECK_INT("two APs available", 2u, hype_cpu_topology_ap_count(&t));
    CHECK_INT("reported as consecutive", 1, hype_cpu_topology_is_consecutive(&t));
}

/*
 * The Intel i5-13420H case this ticket is about: the IDs are NOT 0,1,2. The old
 * code started ap_start(2), nothing answered, and the VM bound to that core
 * never ran even though its RAM, firmware and media had all resolved.
 */
static void test_sparse_layout_gives_real_ids_not_indices(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add(&t, 0u, 1, 1);
    (void)hype_cpu_topology_add(&t, 1u, 0, 1);
    (void)hype_cpu_topology_add(&t, 4u, 0, 1); /* the ID the old code assumed was 2 */
    (void)hype_cpu_topology_add(&t, 5u, 0, 1);

    CHECK_INT("first AP is still 1", 1, hype_cpu_topology_ap(&t, 0));
    CHECK_INT("second AP is 4, NOT 2", 4, hype_cpu_topology_ap(&t, 1));
    CHECK_INT("third AP is 5", 5, hype_cpu_topology_ap(&t, 2));
    CHECK_INT("three APs available", 3u, hype_cpu_topology_ap_count(&t));
    CHECK_INT("reported as NOT consecutive", 0, hype_cpu_topology_is_consecutive(&t));
}

/* A disabled processor must never be handed out: it is reported by firmware but
 * cannot be started, and returning it reproduces the original failure exactly --
 * an AP that never answers. */
static void test_disabled_processors_are_never_handed_out(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add(&t, 0u, 1, 1);
    CHECK_INT("disabled add is refused", -1, hype_cpu_topology_add(&t, 1u, 0, 0));
    (void)hype_cpu_topology_add(&t, 2u, 0, 1);

    CHECK_INT("only the enabled AP is recorded", 1u, hype_cpu_topology_ap_count(&t));
    CHECK_INT("and it is the enabled one", 2, hype_cpu_topology_ap(&t, 0));
    CHECK_INT("disabled one is not counted", 2u, t.count);
    CHECK_INT("disabled is not 'dropped' (that means table-full)", 0u, t.dropped);
}

/* The BSP is not an AP. It is running hype. */
static void test_bsp_is_excluded_from_the_ap_list(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    /* BSP is not necessarily first, nor ID 0. */
    (void)hype_cpu_topology_add(&t, 8u, 0, 1);
    (void)hype_cpu_topology_add(&t, 9u, 1, 1); /* BSP */
    (void)hype_cpu_topology_add(&t, 10u, 0, 1);

    CHECK_INT("bsp reported correctly", 9, hype_cpu_topology_bsp(&t));
    CHECK_INT("first AP skips the BSP", 8, hype_cpu_topology_ap(&t, 0));
    CHECK_INT("second AP skips the BSP", 10, hype_cpu_topology_ap(&t, 1));
    CHECK_INT("two APs, not three", 2u, hype_cpu_topology_ap_count(&t));
}

static void test_running_out_of_aps_is_reported_not_guessed(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add(&t, 0u, 1, 1);
    (void)hype_cpu_topology_add(&t, 1u, 0, 1);

    CHECK_INT("one AP exists", 1, hype_cpu_topology_ap(&t, 0));
    CHECK_INT("a second AP does not", -1, hype_cpu_topology_ap(&t, 1));
    CHECK_INT("nor a tenth", -1, hype_cpu_topology_ap(&t, 9));
}

/* A uniprocessor machine has a BSP and no APs -- no guest can be dispatched to
 * a second core, and the caller must be able to see that rather than being
 * handed something. */
static void test_uniprocessor(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add(&t, 0u, 1, 1);
    CHECK_INT("no APs", 0u, hype_cpu_topology_ap_count(&t));
    CHECK_INT("asking for one says so", -1, hype_cpu_topology_ap(&t, 0));
    CHECK_INT("bsp still known", 0, hype_cpu_topology_bsp(&t));
}

/* x2APIC IDs are full 32-bit values, so a valid ID can have the top bit set.
 * That is why the accessors return int64 -- an int return would make a real ID
 * indistinguishable from the -1 "no such AP" answer. */
static void test_large_apic_ids_are_not_confused_with_failure(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add(&t, 0u, 1, 1);
    (void)hype_cpu_topology_add(&t, 0xFFFFFFFFu, 0, 1);

    CHECK_INT("0xFFFFFFFF survives as itself", 0xFFFFFFFFll, hype_cpu_topology_ap(&t, 0));
    CHECK("and is distinguishable from -1", hype_cpu_topology_ap(&t, 0) != -1);
}

static void test_overflow_is_counted(void) {
    hype_cpu_topology_t t;
    unsigned int i;
    hype_cpu_topology_reset(&t);
    for (i = 0; i < HYPE_CPU_TOPOLOGY_MAX + 5u; i++) {
        (void)hype_cpu_topology_add(&t, i, i == 0u, 1);
    }
    CHECK_INT("count clamps", HYPE_CPU_TOPOLOGY_MAX, t.count);
    CHECK_INT("overflow counted", 5u, t.dropped);
}

static void test_null_safety_and_empty(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    CHECK_INT("empty has no APs", 0u, hype_cpu_topology_ap_count(&t));
    CHECK_INT("empty has no bsp", -1, hype_cpu_topology_bsp(&t));
    CHECK_INT("empty is not 'consecutive'", 0, hype_cpu_topology_is_consecutive(&t));
    CHECK_INT("null ap", -1, hype_cpu_topology_ap(0, 0));
    CHECK_INT("null count", 0u, hype_cpu_topology_ap_count(0));
    CHECK_INT("null bsp", -1, hype_cpu_topology_bsp(0));
    CHECK_INT("null consecutive", 0, hype_cpu_topology_is_consecutive(0));
    CHECK_INT("null add", -1, hype_cpu_topology_add(0, 1u, 0, 1));
    hype_cpu_topology_reset(0); /* must not fault */
}

/* No BSP flag from firmware: every recorded processor is then a candidate AP.
 * Refusing to hand out any would strand a machine that is otherwise fine. */
static void test_missing_bsp_flag_still_yields_aps(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add(&t, 3u, 0, 1);
    (void)hype_cpu_topology_add(&t, 7u, 0, 1);
    CHECK_INT("bsp unknown", -1, hype_cpu_topology_bsp(&t));
    CHECK_INT("all recorded are APs", 2u, hype_cpu_topology_ap_count(&t));
    CHECK_INT("first", 3, hype_cpu_topology_ap(&t, 0));
    CHECK_INT("second", 7, hype_cpu_topology_ap(&t, 1));
}

/*
 * A realistic Intel hybrid layout: 4 P-cores with SMT (2 threads each) and 4
 * single-threaded E-cores. UEFI reports all 12 as plain APs -- it has no notion
 * of core type -- so the SMT count is what distinguishes them here.
 */
static void test_hybrid_layout_smt_count_identifies_p_cores(void) {
    hype_cpu_topology_t t;
    unsigned int cores = 0, smt = 0;
    unsigned int c;
    hype_cpu_topology_reset(&t);
    /* 4 P-cores, 2 threads each */
    for (c = 0; c < 4u; c++) {
        (void)hype_cpu_topology_add_at(&t, c * 2u, c == 0u, 1, 0u, c, 0u);
        (void)hype_cpu_topology_add_at(&t, c * 2u + 1u, 0, 1, 0u, c, 1u);
    }
    /* 4 E-cores, 1 thread each */
    for (c = 0; c < 4u; c++) {
        (void)hype_cpu_topology_add_at(&t, 16u + c, 0, 1, 0u, 4u + c, 0u);
    }

    hype_cpu_topology_core_summary(&t, &cores, &smt);
    CHECK_INT("12 logical processors", 12u, t.count);
    CHECK_INT("8 physical cores", 8u, cores);
    CHECK_INT("4 of them SMT -- the P-cores", 4u, smt);
    CHECK_INT("11 APs (all but the BSP)", 11u, hype_cpu_topology_ap_count(&t));
    CHECK_INT("IDs are NOT consecutive", 0, hype_cpu_topology_is_consecutive(&t));
    /* The BSP is thread 0 of P-core 0, so the first AP is its SMT sibling. */
    CHECK_INT("first AP is the BSP's sibling", 1, hype_cpu_topology_ap(&t, 0));
    /* The E-cores are reachable, at their real IDs. */
    CHECK_INT("first E-core AP", 16, hype_cpu_topology_ap(&t, 7));
}

/* A non-SMT machine: every core has one thread, so none are "SMT cores". */
static void test_core_summary_without_smt(void) {
    hype_cpu_topology_t t;
    unsigned int cores = 0, smt = 0;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add_at(&t, 0u, 1, 1, 0u, 0u, 0u);
    (void)hype_cpu_topology_add_at(&t, 1u, 0, 1, 0u, 1u, 0u);
    hype_cpu_topology_core_summary(&t, &cores, &smt);
    CHECK_INT("two cores", 2u, cores);
    CHECK_INT("no SMT", 0u, smt);
}

/* Two packages can repeat core numbers; they must not be merged. */
static void test_core_summary_distinguishes_packages(void) {
    hype_cpu_topology_t t;
    unsigned int cores = 0, smt = 0;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add_at(&t, 0u, 1, 1, 0u, 0u, 0u);
    (void)hype_cpu_topology_add_at(&t, 1u, 0, 1, 1u, 0u, 0u); /* package 1, same core number */
    hype_cpu_topology_core_summary(&t, &cores, &smt);
    CHECK_INT("two distinct cores, not one", 2u, cores);
    CHECK_INT("neither is SMT", 0u, smt);
}

static void test_core_summary_null_safety(void) {
    hype_cpu_topology_t t;
    unsigned int cores = 99, smt = 99;
    hype_cpu_topology_reset(&t);
    hype_cpu_topology_core_summary(0, &cores, &smt);
    CHECK_INT("null topology yields zero cores", 0u, cores);
    CHECK_INT("null topology yields zero smt", 0u, smt);
    hype_cpu_topology_core_summary(&t, 0, 0); /* both outputs optional */
    CHECK_INT("null add_at", -1, hype_cpu_topology_add_at(0, 1u, 0, 1, 0u, 0u, 0u));
}


/* The real Intel i5-13420H layout, from MP Services on the actual box:
 * 0[p0/c0/t0](BSP) 1[p0/c0/t1] 8[p0/c4/t0] 9[p0/c4/t1] 16[p0/c8/t0] ... plus single-thread
 * E-cores at 40,42,44,46. Enumeration order picks APIC 1 first -- the BSP's own SMT sibling. */
static void add_intel_hybrid(hype_cpu_topology_t *t) {
    hype_cpu_topology_reset(t);
    (void)hype_cpu_topology_add_at(t, 0u, 1, 1, 0u, 0u, 0u);
    (void)hype_cpu_topology_add_at(t, 1u, 0, 1, 0u, 0u, 1u);
    (void)hype_cpu_topology_add_at(t, 8u, 0, 1, 0u, 4u, 0u);
    (void)hype_cpu_topology_add_at(t, 9u, 0, 1, 0u, 4u, 1u);
    (void)hype_cpu_topology_add_at(t, 16u, 0, 1, 0u, 8u, 0u);
    (void)hype_cpu_topology_add_at(t, 17u, 0, 1, 0u, 8u, 1u);
    (void)hype_cpu_topology_add_at(t, 40u, 0, 1, 0u, 20u, 0u);
    (void)hype_cpu_topology_add_at(t, 42u, 0, 1, 0u, 21u, 0u);
}

static void test_selection_never_lands_on_the_bsp_sibling(void) {
    hype_cpu_topology_t t;
    uint32_t sel[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    add_intel_hybrid(&t);
    CHECK_INT("two vCPUs placed", 2, hype_cpu_topology_select_isolated(&t, 2u, sel, 2u));
    CHECK("vm0 is NOT the BSP's SMT sibling (APIC 1)", sel[0] != 1u);
    CHECK("vm1 is NOT the BSP's SMT sibling (APIC 1)", sel[1] != 1u);
    CHECK("vm0 is not the BSP itself", sel[0] != 0u);
    /* First two whole cores after the BSP's. */
    CHECK_INT("vm0 gets the first free physical core", 8u, sel[0]);
    CHECK_INT("vm1 gets the next free physical core", 16u, sel[1]);
}

static void test_selected_cores_are_distinct_from_each_other(void) {
    hype_cpu_topology_t t;
    uint32_t sel[4];
    int n, i, j;
    add_intel_hybrid(&t);
    n = hype_cpu_topology_select_isolated(&t, 4u, sel, 4u);
    CHECK_INT("four distinct cores available besides the BSP's", 4, n);
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            CHECK("no two vCPUs share an APIC ID", sel[i] != sel[j]);
        }
    }
    /* 9 and 17 are the siblings of 8 and 16; picking either would put two VMs on one core. */
    for (i = 0; i < n; i++) {
        CHECK("sibling of a taken core is never selected", sel[i] != 9u && sel[i] != 17u);
    }
}

/* A machine with only one physical core beyond the BSP's must return 1, not overcommit to 2. */
static void test_selection_reports_short_rather_than_overcommitting(void) {
    hype_cpu_topology_t t;
    uint32_t sel[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add_at(&t, 0u, 1, 1, 0u, 0u, 0u);
    (void)hype_cpu_topology_add_at(&t, 1u, 0, 1, 0u, 0u, 1u);
    (void)hype_cpu_topology_add_at(&t, 2u, 0, 1, 0u, 1u, 0u);
    (void)hype_cpu_topology_add_at(&t, 3u, 0, 1, 0u, 1u, 1u);
    CHECK_INT("only one isolated core is available", 1,
              hype_cpu_topology_select_isolated(&t, 2u, sel, 2u));
    CHECK_INT("and it is the non-BSP core", 2u, sel[0]);
    CHECK("the second slot is left untouched", sel[1] == 0xFFFFFFFFu);
}

/* A valid consecutive, non-SMT topology. This covers machines where enumeration order already
 * names distinct physical cores; the real AMD laptop now uses the repaired SMT case below. */
static void test_non_smt_layout_is_unchanged(void) {
    hype_cpu_topology_t t;
    uint32_t sel[2];
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add_at(&t, 0u, 1, 1, 0u, 0u, 0u);
    (void)hype_cpu_topology_add_at(&t, 1u, 0, 1, 0u, 1u, 0u);
    (void)hype_cpu_topology_add_at(&t, 2u, 0, 1, 0u, 2u, 0u);
    CHECK_INT("two placed", 2, hype_cpu_topology_select_isolated(&t, 2u, sel, 2u));
    CHECK_INT("vm0 on APIC 1 as before", 1u, sel[0]);
    CHECK_INT("vm1 on APIC 2 as before", 2u, sel[1]);
}

static void test_selection_degenerate_inputs(void) {
    hype_cpu_topology_t t;
    uint32_t sel[2];
    add_intel_hybrid(&t);
    CHECK_INT("null topology selects nothing", 0,
              hype_cpu_topology_select_isolated(0, 2u, sel, 2u));
    CHECK_INT("null output selects nothing", 0,
              hype_cpu_topology_select_isolated(&t, 2u, 0, 2u));
    CHECK_INT("zero wanted selects nothing", 0,
              hype_cpu_topology_select_isolated(&t, 0u, sel, 2u));
    CHECK_INT("want is clamped to the output buffer", 1,
              hype_cpu_topology_select_isolated(&t, 2u, sel, 1u));
}

static void test_all_zero_firmware_locations_are_detected(void) {
    hype_cpu_topology_t t;
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add_at(&t, 0u, 1, 1, 0u, 0u, 0u);
    (void)hype_cpu_topology_add_at(&t, 1u, 0, 1, 0u, 0u, 0u);
    (void)hype_cpu_topology_add_at(&t, 2u, 0, 1, 0u, 0u, 0u);
    CHECK_INT("all-zero multi-CPU locations are degenerate", 1,
              hype_cpu_topology_locations_degenerate(&t));
    t.loc[2].core = 1u;
    CHECK_INT("one distinct location makes the table usable", 0,
              hype_cpu_topology_locations_degenerate(&t));
    CHECK_INT("null table is not reported degenerate", 0,
              hype_cpu_topology_locations_degenerate(0));
    hype_cpu_topology_reset(&t);
    (void)hype_cpu_topology_add_at(&t, 0u, 1, 1, 0u, 0u, 0u);
    CHECK_INT("one CPU is not evidence of bad topology", 0,
              hype_cpu_topology_locations_degenerate(&t));
}

static void test_amd_apic_layout_repairs_ryzen_smt_topology(void) {
    hype_cpu_topology_t t;
    uint32_t sel[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    unsigned int thread_bits = 99u, core_bits = 99u;
    unsigned int i;

    hype_cpu_topology_reset(&t);
    for (i = 0u; i < 8u; i++) {
        (void)hype_cpu_topology_add_at(&t, i, i == 0u, 1, 0u, 0u, 0u);
    }
    /* Ryzen 5 2500U: eight logical processors, four cores, two threads/core. */
    CHECK_INT("AMD CPUID layout accepted", 0,
              hype_cpu_topology_layout_from_amd(3u, 2u, &thread_bits, &core_bits));
    CHECK_INT("one SMT bit", 1u, thread_bits);
    CHECK_INT("two core bits", 2u, core_bits);
    CHECK_INT("APIC locations repaired", 0,
              hype_cpu_topology_apply_apic_layout(&t, thread_bits, core_bits));
    CHECK_INT("repaired table is no longer degenerate", 0,
              hype_cpu_topology_locations_degenerate(&t));
    CHECK_INT("two isolated guest cores selected", 2,
              hype_cpu_topology_select_isolated(&t, 2u, sel, 2u));
    CHECK_INT("vm0 skips BSP sibling and uses core 1", 2u, sel[0]);
    CHECK_INT("vm1 uses core 2", 4u, sel[1]);
}

static void test_topology_layout_validation(void) {
    hype_cpu_topology_t t;
    unsigned int tb = 0u, cb = 0u;
    CHECK_INT("leaf-B shifts decode", 0,
              hype_cpu_topology_layout_from_shifts(1u, 4u, &tb, &cb));
    CHECK_INT("leaf-B thread bits", 1u, tb);
    CHECK_INT("leaf-B core bits", 3u, cb);
    CHECK_INT("reversed shifts refused", -1,
              hype_cpu_topology_layout_from_shifts(4u, 3u, &tb, &cb));
    CHECK_INT("oversize shift refused", -1,
              hype_cpu_topology_layout_from_shifts(1u, 32u, &tb, &cb));
    CHECK_INT("null shift output refused", -1,
              hype_cpu_topology_layout_from_shifts(1u, 2u, 0, &cb));
    CHECK_INT("null core shift output refused", -1,
              hype_cpu_topology_layout_from_shifts(1u, 2u, &tb, 0));
    CHECK_INT("zero AMD width refused", -1,
              hype_cpu_topology_layout_from_amd(0u, 2u, &tb, &cb));
    CHECK_INT("oversize AMD width refused", -1,
              hype_cpu_topology_layout_from_amd(32u, 2u, &tb, &cb));
    CHECK_INT("zero AMD threads refused", -1,
              hype_cpu_topology_layout_from_amd(3u, 0u, &tb, &cb));
    CHECK_INT("too many AMD threads refused", -1,
              hype_cpu_topology_layout_from_amd(2u, 8u, &tb, &cb));
    CHECK_INT("non-dividing AMD threads refused", -1,
              hype_cpu_topology_layout_from_amd(3u, 3u, &tb, &cb));
    CHECK_INT("null AMD thread output refused", -1,
              hype_cpu_topology_layout_from_amd(3u, 2u, 0, &cb));
    CHECK_INT("null AMD output refused", -1,
              hype_cpu_topology_layout_from_amd(3u, 2u, &tb, 0));
    hype_cpu_topology_reset(&t);
    CHECK_INT("null topology apply refused", -1,
              hype_cpu_topology_apply_apic_layout(0, 1u, 2u));
    CHECK_INT("oversize layout refused", -1,
              hype_cpu_topology_apply_apic_layout(&t, 31u, 1u));
    CHECK_INT("oversize thread width refused", -1,
              hype_cpu_topology_apply_apic_layout(&t, 32u, 0u));
    CHECK_INT("oversize core width refused", -1,
              hype_cpu_topology_apply_apic_layout(&t, 0u, 32u));
    CHECK_INT("zero-width layout accepted", 0,
              hype_cpu_topology_apply_apic_layout(&t, 0u, 0u));

    /* Cover selection without a BSP marker and the explicit zero output size. */
    (void)hype_cpu_topology_add_at(&t, 7u, 0, 1, 0u, 1u, 0u);
    {
        uint32_t sel[1];
        CHECK_INT("zero output capacity selects nothing", 0,
                  hype_cpu_topology_select_isolated(&t, 1u, sel, 0u));
        CHECK_INT("missing BSP still selects an isolated AP", 1,
                  hype_cpu_topology_select_isolated(&t, 1u, sel, 1u));
        CHECK_INT("selected AP is recorded ID", 7u, sel[0]);
    }

    /* A second BSP marker must not replace the first. */
    (void)hype_cpu_topology_add_at(&t, 8u, 1, 1, 0u, 2u, 0u);
    (void)hype_cpu_topology_add_at(&t, 9u, 1, 1, 0u, 3u, 0u);
    CHECK_INT("first BSP marker remains authoritative", 8, hype_cpu_topology_bsp(&t));
}

int main(void) {
    test_consecutive_layout_matches_the_old_assumption();
    test_sparse_layout_gives_real_ids_not_indices();
    test_disabled_processors_are_never_handed_out();
    test_bsp_is_excluded_from_the_ap_list();
    test_running_out_of_aps_is_reported_not_guessed();
    test_uniprocessor();
    test_large_apic_ids_are_not_confused_with_failure();
    test_overflow_is_counted();
    test_null_safety_and_empty();
    test_missing_bsp_flag_still_yields_aps();
    test_hybrid_layout_smt_count_identifies_p_cores();
    test_core_summary_without_smt();
    test_core_summary_distinguishes_packages();
    test_core_summary_null_safety();
    test_selection_never_lands_on_the_bsp_sibling();
    test_selected_cores_are_distinct_from_each_other();
    test_selection_reports_short_rather_than_overcommitting();
    test_non_smt_layout_is_unchanged();
    test_selection_degenerate_inputs();
    test_all_zero_firmware_locations_are_detected();
    test_amd_apic_layout_repairs_ryzen_smt_topology();
    test_topology_layout_validation();
    if (failures != 0) {
        printf("test_cpu_topology: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_cpu_topology: all checks passed\n");
    return 0;
}
