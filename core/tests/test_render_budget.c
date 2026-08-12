#include <stdio.h>
#include "../render_budget.h"

static int failures;

#define CHECK(desc, expected, actual)                                                   \
    do {                                                                                \
        unsigned e = (expected), a = (actual);                                          \
        if (e != a) {                                                                   \
            printf("FAIL: %s: expected %u, got %u\n", (desc), e, a);                  \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

static void test_fast_passes_expand_to_screen(void) {
    hype_render_budget_t budget;
    hype_render_budget_reset(&budget);
    CHECK("reset floor", 8u, hype_render_budget_rows(&budget, 135u));
    hype_render_budget_record(&budget, 135u, 100u, 1000u);
    CHECK("first fast pass doubles", 16u, hype_render_budget_rows(&budget, 135u));
    hype_render_budget_record(&budget, 135u, 100u, 1000u);
    hype_render_budget_record(&budget, 135u, 100u, 1000u);
    hype_render_budget_record(&budget, 135u, 100u, 1000u);
    hype_render_budget_record(&budget, 135u, 100u, 1000u);
    CHECK("fast passes clamp at screen", 135u, hype_render_budget_rows(&budget, 135u));
}

static void test_slow_passes_return_to_floor(void) {
    hype_render_budget_t budget = {64u};
    hype_render_budget_record(&budget, 135u, 10u, 9000u);
    CHECK("slow halves", 32u, budget.rows);
    hype_render_budget_record(&budget, 135u, 10u, 9000u);
    hype_render_budget_record(&budget, 135u, 10u, 9000u);
    CHECK("slow clamps at floor", 8u, budget.rows);
}

static void test_empty_pass_is_not_timing_evidence(void) {
    hype_render_budget_t budget = {32u};
    hype_render_budget_record(&budget, 135u, 0u, 1u);
    CHECK("empty fast pass unchanged", 32u, budget.rows);
    hype_render_budget_record(&budget, 135u, 0u, 100000u);
    CHECK("empty slow pass unchanged", 32u, budget.rows);
}

static void test_small_screen_clamps(void) {
    hype_render_budget_t budget;
    hype_render_budget_reset(&budget);
    CHECK("screen smaller than floor", 4u, hype_render_budget_rows(&budget, 4u));
    hype_render_budget_record(&budget, 4u, 1u, 1u);
    CHECK("small screen remains clamped", 4u, hype_render_budget_rows(&budget, 4u));
}

static void test_uninitialized_zero_and_middle_time(void) {
    hype_render_budget_t budget = {0u};
    CHECK("zero state uses floor", 8u, hype_render_budget_rows(&budget, 135u));
    hype_render_budget_record(&budget, 135u, 1u, 4000u);
    CHECK("middle duration keeps floor", 8u, budget.rows);
    hype_render_budget_record(&budget, 0u, 1u, 1u);
    CHECK("zero-height screen ignored", 8u, budget.rows);
    hype_render_budget_record(&budget, 135u, 1u, 9000u);
    CHECK("slow floor stays at floor", 8u, budget.rows);
}

int main(void) {
    test_fast_passes_expand_to_screen();
    test_slow_passes_return_to_floor();
    test_empty_pass_is_not_timing_evidence();
    test_small_screen_clamps();
    test_uninitialized_zero_and_middle_time();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
