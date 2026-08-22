#include <stdint.h>
#include <stdio.h>

#include "../ram_pool.h"

static int failures;

#define CHECK(msg, cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

#define MB (1024ull * 1024ull)
#define POOL_BASE 0x40000000ull /* 1 GiB, 2 MB aligned */

static void init_ok(hype_ram_pool_t *p, uint64_t size) {
    CHECK("init succeeds", hype_ram_pool_init(p, POOL_BASE, size) == HYPE_RAM_POOL_OK);
}

static void test_init_refuses_a_misaligned_base(void) {
    hype_ram_pool_t p;
    /* Refused, not rounded: firmware was asked for a 2 MB-aligned block, so a misaligned one
     * means the reservation is not what the caller believes it is. */
    CHECK("misaligned base is refused",
          hype_ram_pool_init(&p, POOL_BASE + 4096ull, 64ull * MB) == HYPE_RAM_POOL_ERR_BAD_ALIGN);
    CHECK("a null pool is refused",
          hype_ram_pool_init(0, POOL_BASE, 64ull * MB) == HYPE_RAM_POOL_ERR_UNINIT);
}

static void test_init_rounds_size_down_to_whole_2mb(void) {
    hype_ram_pool_t p;
    init_ok(&p, 64ull * MB + 4096ull);
    CHECK("trailing partial 2 MB is not offered", p.size == 64ull * MB);
    CHECK("nothing used yet", hype_ram_pool_used(&p) == 0ull);
    CHECK("all of it remains", hype_ram_pool_remaining(&p) == 64ull * MB);
}

static void test_every_carve_is_2mb_aligned(void) {
    hype_ram_pool_t p;
    uint64_t a = 0, b = 0, c = 0;
    init_ok(&p, 64ull * MB);
    /* Sizes deliberately not multiples of 2 MB: hype_npt_map_range() encodes the base into
     * PS=1 2 MB PDEs, so a merely page-aligned carve maps a guest at the wrong address. */
    CHECK("carve 1 MB", hype_ram_pool_carve(&p, 1ull * MB, 0u, 0u, &a, 0) == HYPE_RAM_POOL_OK);
    CHECK("carve 3 MB", hype_ram_pool_carve(&p, 3ull * MB, 1u, 0u, &b, 0) == HYPE_RAM_POOL_OK);
    CHECK("carve 1 byte", hype_ram_pool_carve(&p, 1ull, 2u, 0u, &c, 0) == HYPE_RAM_POOL_OK);
    CHECK("first is aligned", (a & (HYPE_RAM_POOL_ALIGN - 1ull)) == 0ull);
    CHECK("second is aligned", (b & (HYPE_RAM_POOL_ALIGN - 1ull)) == 0ull);
    CHECK("third is aligned", (c & (HYPE_RAM_POOL_ALIGN - 1ull)) == 0ull);
    CHECK("a 1 MB request consumes a whole 2 MB", b == a + 2ull * MB);
    CHECK("a 3 MB request consumes 4 MB", c == b + 4ull * MB);
    CHECK("used is the sum of the rounded sizes", hype_ram_pool_used(&p) == 8ull * MB);
}

static void test_carves_never_overlap(void) {
    hype_ram_pool_t p;
    unsigned i;
    init_ok(&p, 64ull * MB);
    for (i = 0; i < 8u; i++) {
        uint64_t base = 0;
        CHECK("carve succeeds",
              hype_ram_pool_carve(&p, (uint64_t)(i + 1u) * MB, i, 0u, &base, 0) ==
                  HYPE_RAM_POOL_OK);
    }
    /* True by construction for a bump allocator -- asserted here so a future free-list cannot
     * quietly break the property section 6i relies on. */
    CHECK("no two carves overlap", !hype_ram_pool_any_overlap(&p));
}

static void test_exhaustion_reports_the_shortfall(void) {
    hype_ram_pool_t p;
    uint64_t base = 0, shortfall = 0;
    init_ok(&p, 8ull * MB);
    CHECK("6 MB fits", hype_ram_pool_carve(&p, 6ull * MB, 0u, 0u, &base, 0) == HYPE_RAM_POOL_OK);
    CHECK("another 6 MB does not",
          hype_ram_pool_carve(&p, 6ull * MB, 1u, 0u, &base, &shortfall) ==
              HYPE_RAM_POOL_ERR_EXHAUSTED);
    /* 6 MB wanted, 2 MB left -- the caller has to be able to name the number (#290's bar). */
    CHECK("shortfall is what was missing", shortfall == 4ull * MB);
    CHECK("a failed carve consumes nothing", hype_ram_pool_remaining(&p) == 2ull * MB);
    CHECK("and records nothing", p.carve_count == 1u);
}

static void test_zero_and_uninitialised_are_errors(void) {
    hype_ram_pool_t p;
    hype_ram_pool_t empty;
    uint64_t base = 0;
    init_ok(&p, 8ull * MB);
    CHECK("a zero-byte carve is a caller bug, not a no-op",
          hype_ram_pool_carve(&p, 0ull, 0u, 0u, &base, 0) == HYPE_RAM_POOL_ERR_ZERO);
    empty.size = 0ull;
    CHECK("carving an uninitialised pool is refused",
          hype_ram_pool_carve(&empty, MB, 0u, 0u, &base, 0) == HYPE_RAM_POOL_ERR_UNINIT);
    CHECK("remaining of a null pool is zero", hype_ram_pool_remaining(0) == 0ull);
    CHECK("used of a null pool is zero", hype_ram_pool_used(0) == 0ull);
    CHECK("overlap of a null pool is false", !hype_ram_pool_any_overlap(0));
}

static void test_carve_table_is_bounded(void) {
    hype_ram_pool_t p;
    unsigned i;
    uint64_t base = 0;
    init_ok(&p, 4096ull * MB);
    for (i = 0; i < HYPE_RAM_POOL_MAX_CARVES; i++) {
        CHECK("carve fits in the table",
              hype_ram_pool_carve(&p, MB, i, 0u, &base, 0) == HYPE_RAM_POOL_OK);
    }
    CHECK("one past the table is refused, not written",
          hype_ram_pool_carve(&p, MB, 999u, 0u, &base, 0) == HYPE_RAM_POOL_ERR_TOO_MANY);
}

/*
 * #606 (plan.md §10 decision 33): HYPE_RAM_POOL_MAX_CARVES used to be a hardcoded 64, which
 * refused every 65th carve regardless of how much pool memory or how many cores the host had.
 * Pinned here by the historical number, not the macro, so this test still means something once
 * the macro's derivation changes again.
 */
static void test_65th_carve_no_longer_refused(void) {
    hype_ram_pool_t p;
    unsigned i;
    uint64_t base = 0;
    CHECK("today's derived bound comfortably exceeds the old hardcoded 64",
          HYPE_RAM_POOL_MAX_CARVES > 64u);
    init_ok(&p, 4096ull * MB);
    for (i = 0; i < 65u; i++) {
        CHECK("carve fits (including the 65th, once refused unconditionally)",
              hype_ram_pool_carve(&p, MB, i, 0u, &base, 0) == HYPE_RAM_POOL_OK);
    }
    CHECK("65 carves were actually made", p.carve_count == 65u);
}

static void test_find_returns_a_vms_existing_carve(void) {
    hype_ram_pool_t p;
    uint64_t a = 0, b = 0;
    const hype_ram_carve_t *c;
    init_ok(&p, 64ull * MB);
    CHECK("vm1 guest RAM", hype_ram_pool_carve(&p, 4ull * MB, 1u, 0u, &a, 0) == HYPE_RAM_POOL_OK);
    CHECK("vm1 firmware", hype_ram_pool_carve(&p, 2ull * MB, 1u, 1u, &b, 0) == HYPE_RAM_POOL_OK);
    c = hype_ram_pool_find(&p, 1u, 0u);
    CHECK("guest RAM carve is found", c != 0 && c->base == a);
    c = hype_ram_pool_find(&p, 1u, 1u);
    CHECK("firmware carve is a different one", c != 0 && c->base == b && b != a);
    CHECK("an unknown owner has none", hype_ram_pool_find(&p, 7u, 0u) == 0);
    CHECK("an unknown kind has none", hype_ram_pool_find(&p, 1u, 9u) == 0);
    CHECK("a null pool has none", hype_ram_pool_find(0, 1u, 0u) == 0);
}

static void test_range_ownership_is_checkable(void) {
    hype_ram_pool_t p;
    uint64_t a = 0, b = 0;
    init_ok(&p, 64ull * MB);
    hype_ram_pool_carve(&p, 4ull * MB, 0u, 0u, &a, 0);
    hype_ram_pool_carve(&p, 4ull * MB, 1u, 0u, &b, 0);
    CHECK("a VM owns its own range", hype_ram_pool_range_is_owned(&p, a, 4ull * MB, 0u));
    CHECK("a VM does NOT own its neighbour's", !hype_ram_pool_range_is_owned(&p, b, MB, 0u));
    CHECK("a range spilling past the carve is refused",
          !hype_ram_pool_range_is_owned(&p, a, 6ull * MB, 0u));
    CHECK("a range below the pool is refused",
          !hype_ram_pool_range_is_owned(&p, POOL_BASE - MB, MB, 0u));
    CHECK("a range past the pool is refused",
          !hype_ram_pool_range_is_owned(&p, POOL_BASE + 63ull * MB, 4ull * MB, 0u));
    CHECK("a zero-length range is refused", !hype_ram_pool_range_is_owned(&p, a, 0ull, 0u));
    CHECK("an overflowing length is refused",
          !hype_ram_pool_range_is_owned(&p, a, ~0ull, 0u));
    CHECK("a null pool owns nothing", !hype_ram_pool_range_is_owned(0, a, MB, 0u));
}

static void test_status_strings_are_distinct(void) {
    CHECK("ok", hype_ram_pool_status_str(HYPE_RAM_POOL_OK)[0] == 'o');
    CHECK("uninit names the pool", hype_ram_pool_status_str(HYPE_RAM_POOL_ERR_UNINIT)[0] == 'p');
    CHECK("zero names the carve", hype_ram_pool_status_str(HYPE_RAM_POOL_ERR_ZERO)[0] == 'z');
    CHECK("exhausted", hype_ram_pool_status_str(HYPE_RAM_POOL_ERR_EXHAUSTED)[0] == 'p');
    CHECK("too many", hype_ram_pool_status_str(HYPE_RAM_POOL_ERR_TOO_MANY)[0] == 't');
    CHECK("bad align", hype_ram_pool_status_str(HYPE_RAM_POOL_ERR_BAD_ALIGN)[0] == 'p');
    CHECK("an out-of-range status still names itself",
          hype_ram_pool_status_str((hype_ram_pool_status_t)42)[0] == 'u');
}


/* The optional out-params really are optional: a caller that only wants to know whether the
 * carve succeeded must not have to invent somewhere to put the base. */
static void test_optional_out_params_may_be_null(void) {
    hype_ram_pool_t p;
    init_ok(&p, 8ull * MB);
    CHECK("carve with no out-params at all",
          hype_ram_pool_carve(&p, 2ull * MB, 0u, 0u, 0, 0) == HYPE_RAM_POOL_OK);
    CHECK("exhaustion with no shortfall pointer",
          hype_ram_pool_carve(&p, 16ull * MB, 1u, 0u, 0, 0) == HYPE_RAM_POOL_ERR_EXHAUSTED);
    CHECK("the failed carve still consumed nothing", hype_ram_pool_used(&p) == 2ull * MB);
}

/* A pool smaller than one 2 MB granule rounds to nothing, and must then refuse every carve
 * rather than hand out a base inside memory it does not own. */
static void test_a_pool_below_one_granule_holds_nothing(void) {
    hype_ram_pool_t p;
    uint64_t base = 0, shortfall = 0;
    CHECK("init accepts it", hype_ram_pool_init(&p, POOL_BASE, MB) == HYPE_RAM_POOL_OK);
    CHECK("but it holds nothing", p.size == 0ull);
    CHECK("and carving is refused as uninitialised",
          hype_ram_pool_carve(&p, MB, 0u, 0u, &base, &shortfall) == HYPE_RAM_POOL_ERR_UNINIT);
    CHECK("remaining is zero", hype_ram_pool_remaining(&p) == 0ull);
}

/* Exactly-fits and one-byte-over, either side of the boundary. */
static void test_the_last_granule_is_usable(void) {
    hype_ram_pool_t p;
    uint64_t base = 0, shortfall = 0;
    init_ok(&p, 4ull * MB);
    CHECK("first 2 MB", hype_ram_pool_carve(&p, 2ull * MB, 0u, 0u, &base, 0) == HYPE_RAM_POOL_OK);
    CHECK("the last 2 MB fits exactly",
          hype_ram_pool_carve(&p, 2ull * MB, 1u, 0u, &base, 0) == HYPE_RAM_POOL_OK);
    CHECK("nothing remains", hype_ram_pool_remaining(&p) == 0ull);
    CHECK("one byte more is refused",
          hype_ram_pool_carve(&p, 1ull, 2u, 0u, &base, &shortfall) == HYPE_RAM_POOL_ERR_EXHAUSTED);
    CHECK("and the shortfall is a whole granule", shortfall == 2ull * MB);
}

int main(void) {
    test_init_refuses_a_misaligned_base();
    test_init_rounds_size_down_to_whole_2mb();
    test_every_carve_is_2mb_aligned();
    test_carves_never_overlap();
    test_exhaustion_reports_the_shortfall();
    test_zero_and_uninitialised_are_errors();
    test_carve_table_is_bounded();
    test_65th_carve_no_longer_refused();
    test_find_returns_a_vms_existing_carve();
    test_range_ownership_is_checkable();
    test_optional_out_params_may_be_null();
    test_a_pool_below_one_granule_holds_nothing();
    test_the_last_granule_is_usable();
    test_status_strings_are_distinct();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
