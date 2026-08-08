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
    if (failures != 0) {
        printf("test_cpu_topology: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_cpu_topology: all checks passed\n");
    return 0;
}
