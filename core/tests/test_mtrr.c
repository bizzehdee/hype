#include <stdio.h>
#include <string.h>

#include "../mtrr.h"

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

#define DEF_WB (HYPE_MTRR_DEF_E | HYPE_MTRR_WB)

/* PHYSMASK for a naturally-aligned range of `size` bytes, with the valid bit set. */
static uint64_t mask_for(uint64_t size) {
    return (~(size - 1ull) & ~0xFFFull) | (1ull << 11);
}

static void test_no_range_matches_gives_the_default_type(void) {
    hype_mtrr_var_t v[1];
    v[0].base = 0x80000000ull | HYPE_MTRR_UC;
    v[0].mask = mask_for(0x10000000ull);
    CHECK_INT("address outside every range falls back to the default type", HYPE_MTRR_WB,
              hype_mtrr_type_for(0x4000000000ull, v, 1u, DEF_WB));
}

static void test_a_covering_range_wins_over_the_default(void) {
    hype_mtrr_var_t v[1];
    v[0].base = 0x4000000000ull | HYPE_MTRR_UC;
    v[0].mask = mask_for(0x40000000ull); /* 1 GB */
    CHECK_INT("an MTRR covering the framebuffer decides its type", HYPE_MTRR_UC,
              hype_mtrr_type_for(0x4000000000ull, v, 1u, DEF_WB));
    CHECK_INT("still covered near the top of the range", HYPE_MTRR_UC,
              hype_mtrr_type_for(0x403FFFF000ull, v, 1u, DEF_WB));
    CHECK_INT("just past the range is the default again", HYPE_MTRR_WB,
              hype_mtrr_type_for(0x4040000000ull, v, 1u, DEF_WB));
}

static void test_an_invalid_range_is_ignored(void) {
    hype_mtrr_var_t v[1];
    v[0].base = 0x4000000000ull | HYPE_MTRR_UC;
    v[0].mask = mask_for(0x40000000ull) & ~(1ull << 11); /* V clear */
    CHECK_INT("a range with the valid bit clear does not apply", HYPE_MTRR_WB,
              hype_mtrr_type_for(0x4000000000ull, v, 1u, DEF_WB));
}

/*
 * A zero mask would match every address, so an unprogrammed entry that happens to have V set
 * would otherwise claim the whole address space and force its type on everything.
 */
static void test_a_zero_mask_entry_does_not_swallow_the_address_space(void) {
    hype_mtrr_var_t v[1];
    v[0].base = HYPE_MTRR_UC;
    v[0].mask = 1ull << 11; /* V set, mask bits all zero */
    CHECK_INT("a zero-mask range is treated as unprogrammed", HYPE_MTRR_WB,
              hype_mtrr_type_for(0x4000000000ull, v, 1u, DEF_WB));
}

static void test_overlap_rules(void) {
    hype_mtrr_var_t v[2];
    v[0].base = 0x4000000000ull | HYPE_MTRR_WB;
    v[0].mask = mask_for(0x40000000ull);
    v[1].base = 0x4000000000ull | HYPE_MTRR_UC;
    v[1].mask = mask_for(0x40000000ull);
    CHECK_INT("UC wins over any other overlapping type", HYPE_MTRR_UC,
              hype_mtrr_type_for(0x4000000000ull, v, 2u, DEF_WB));

    v[1].base = 0x4000000000ull | HYPE_MTRR_WT;
    CHECK_INT("WT wins over WB", HYPE_MTRR_WT,
              hype_mtrr_type_for(0x4000000000ull, v, 2u, DEF_WB));

    /* Order must not change the answer, or the result depends on MTRR numbering. */
    v[0].base = 0x4000000000ull | HYPE_MTRR_WT;
    v[1].base = 0x4000000000ull | HYPE_MTRR_WB;
    CHECK_INT("WT still wins with the entries swapped", HYPE_MTRR_WT,
              hype_mtrr_type_for(0x4000000000ull, v, 2u, DEF_WB));
}

/*
 * With MTRRs disabled the architectural type is UC, but for a different reason than "an MTRR
 * says UC". Reporting plain UC would make a disabled-MTRR machine indistinguishable from a
 * correctly-programmed UC range, and this module exists to tell those apart.
 */
static void test_disabled_mtrrs_are_reported_as_such_not_as_uc(void) {
    hype_mtrr_var_t v[1];
    v[0].base = 0x4000000000ull | HYPE_MTRR_WC;
    v[0].mask = mask_for(0x40000000ull);
    CHECK_INT("MTRRs disabled is distinguishable from an MTRR of UC", HYPE_MTRR_INVALID,
              hype_mtrr_type_for(0x4000000000ull, v, 1u, HYPE_MTRR_WB /* E clear */));
}

static void test_degenerate_inputs(void) {
    CHECK_INT("a null array is the default type", HYPE_MTRR_WB,
              hype_mtrr_type_for(0x4000000000ull, 0, 4u, DEF_WB));
    CHECK_INT("a zero count is the default type", HYPE_MTRR_WB,
              hype_mtrr_type_for(0x4000000000ull, 0, 0u, DEF_WB));
}

/* An over-large count must be clamped, not walked off the end of the array. */
static void test_count_is_clamped_to_the_architectural_maximum(void) {
    hype_mtrr_var_t v[HYPE_MTRR_MAX_VAR];
    unsigned int i;
    for (i = 0; i < HYPE_MTRR_MAX_VAR; i++) {
        v[i].base = 0ull;
        v[i].mask = 0ull;
    }
    CHECK_INT("a count above the maximum is clamped", HYPE_MTRR_WB,
              hype_mtrr_type_for(0x4000000000ull, v, 1000u, DEF_WB));
}

static void test_type_names(void) {
    CHECK("UC", strcmp(hype_mtrr_type_name(HYPE_MTRR_UC), "UC") == 0);
    CHECK("WC", strcmp(hype_mtrr_type_name(HYPE_MTRR_WC), "WC") == 0);
    CHECK("WT", strcmp(hype_mtrr_type_name(HYPE_MTRR_WT), "WT") == 0);
    CHECK("WP", strcmp(hype_mtrr_type_name(HYPE_MTRR_WP), "WP") == 0);
    CHECK("WB", strcmp(hype_mtrr_type_name(HYPE_MTRR_WB), "WB") == 0);
    /* Never null: these go straight into a format string on the hardware path. */
    CHECK("an unknown encoding still names something", strcmp(hype_mtrr_type_name(0x33u), "??") == 0);
    CHECK("the invalid marker still names something",
          strcmp(hype_mtrr_type_name(HYPE_MTRR_INVALID), "??") == 0);
}

/*
 * hype's WC page tables select PA1, so entry 1 of IA32_PAT is the one that decides whether the
 * framebuffer blit is write-combining. This is the exact value hype_paging_set_pat_wc() writes.
 */
static void test_pat_entries(void) {
    uint64_t pat = 0x0007040600070106ull;
    CHECK_INT("PA0 is WB", HYPE_MTRR_WB, hype_pat_entry(pat, 0u));
    CHECK_INT("PA1 is WC -- the slot hype's framebuffer PTEs select", HYPE_MTRR_WC,
              hype_pat_entry(pat, 1u));
    CHECK_INT("PA2 is UC-", 0x07u, hype_pat_entry(pat, 2u));
    CHECK_INT("PA7 is UC", 0x00u, hype_pat_entry(pat, 7u));
    CHECK_INT("an out-of-range index is rejected", HYPE_MTRR_INVALID, hype_pat_entry(pat, 8u));

    /* The reset default has WT in PA1; that is what a clobbered PAT would look like, and it is
     * the difference between a 725 MB/s blit and a 16 MB/s one. */
    CHECK_INT("the PAT reset default leaves PA1 as WT, not WC", HYPE_MTRR_WT,
              hype_pat_entry(0x0007040600070406ull, 1u));
}

int main(void) {
    test_no_range_matches_gives_the_default_type();
    test_a_covering_range_wins_over_the_default();
    test_an_invalid_range_is_ignored();
    test_a_zero_mask_entry_does_not_swallow_the_address_space();
    test_overlap_rules();
    test_disabled_mtrrs_are_reported_as_such_not_as_uc();
    test_degenerate_inputs();
    test_count_is_clamped_to_the_architectural_maximum();
    test_type_names();
    test_pat_entries();
    if (failures == 0) printf("test_mtrr: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
