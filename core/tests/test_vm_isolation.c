#include <stdio.h>
#include "../vm_isolation.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_STR(desc, expected, actual) \
    do { \
        const char *e_ = (expected), *a_ = (actual); \
        const char *p_ = e_, *q_ = a_; \
        while (*p_ && *p_ == *q_) { p_++; q_++; } \
        if (*p_ != *q_) { \
            printf("FAIL: %s: expected \"%s\", got \"%s\"\n", (desc), e_, a_); \
            failures++; \
        } \
    } while (0)

/* 1 GiB apart, 1 GiB each, distinct roots -- the shape of two real FW-1 guests. */
#define A_BASE 0x100000000ull
#define B_BASE 0x140000000ull
#define SIZE_1G 0x40000000ull

static void test_isolated_pair(void) {
    CHECK_HEX("disjoint RAM + distinct roots is isolated", HYPE_VM_ISOLATION_OK,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, B_BASE, SIZE_1G, 0x2000));
}

static void test_ram_overlap(void) {
    /* b starts one byte inside a. */
    CHECK_HEX("partial overlap detected", HYPE_VM_ISOLATION_RAM_OVERLAP,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, A_BASE + SIZE_1G - 1, SIZE_1G,
                                      0x2000));
    /* Identical ranges. */
    CHECK_HEX("identical ranges detected", HYPE_VM_ISOLATION_RAM_OVERLAP,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, A_BASE, SIZE_1G, 0x2000));
    /* One range wholly inside the other. */
    CHECK_HEX("containment detected", HYPE_VM_ISOLATION_RAM_OVERLAP,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, A_BASE + 0x1000, 0x2000, 0x2000));
    /* Order must not matter. */
    CHECK_HEX("overlap is symmetric", HYPE_VM_ISOLATION_RAM_OVERLAP,
              hype_vm_isolation_check(A_BASE + 0x1000, 0x2000, 0x2000, A_BASE, SIZE_1G, 0x1000));
}

static void test_adjacent_is_not_overlap(void) {
    /* Ranges are half-open, so a's last byte immediately before b's first is
     * fine -- this is exactly how two consecutively-allocated guests sit. */
    CHECK_HEX("touching end-to-end does not overlap", HYPE_VM_ISOLATION_OK,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, A_BASE + SIZE_1G, SIZE_1G, 0x2000));
}

static void test_same_root(void) {
    /* The dangerous case #274 warns about: disjoint RAM would make a
     * liveness-only test pass, but one root means one address space. */
    CHECK_HEX("shared root detected even with disjoint RAM", HYPE_VM_ISOLATION_SAME_ROOT,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, B_BASE, SIZE_1G, 0x1000));
    CHECK_HEX("both faults reported together",
              HYPE_VM_ISOLATION_SAME_ROOT | HYPE_VM_ISOLATION_RAM_OVERLAP,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, A_BASE, SIZE_1G, 0x1000));
}

static void test_unconfigured(void) {
    /* Each argument gets its own case: a single || of six conditions is easy to
     * write with one operand wrong, and only per-operand cases catch that. */
    CHECK_HEX("zero size on a flagged, and does not pass as disjoint",
              HYPE_VM_ISOLATION_UNCONFIGURED,
              hype_vm_isolation_check(A_BASE, 0, 0x1000, B_BASE, SIZE_1G, 0x2000));
    CHECK_HEX("zero size on b flagged", HYPE_VM_ISOLATION_UNCONFIGURED,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, B_BASE, 0, 0x2000));
    CHECK_HEX("zero base on a flagged", HYPE_VM_ISOLATION_UNCONFIGURED,
              hype_vm_isolation_check(0, SIZE_1G, 0x1000, B_BASE, SIZE_1G, 0x2000));
    CHECK_HEX("zero base on b flagged", HYPE_VM_ISOLATION_UNCONFIGURED,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, 0, SIZE_1G, 0x2000));
    CHECK_HEX("zero root on a alone flagged", HYPE_VM_ISOLATION_UNCONFIGURED,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0, B_BASE, SIZE_1G, 0x2000));
    CHECK_HEX("zero root on b alone flagged", HYPE_VM_ISOLATION_UNCONFIGURED,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, B_BASE, SIZE_1G, 0));
    /* Two null roots are both unconfigured AND equal -- say both. */
    CHECK_HEX("null roots flagged as unconfigured and shared",
              HYPE_VM_ISOLATION_UNCONFIGURED | HYPE_VM_ISOLATION_SAME_ROOT,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0, B_BASE, SIZE_1G, 0));
}

static void test_size_wrap(void) {
    /*
     * A range that carries past the top of the address space is impossible for
     * real guest RAM. Clamping it to ~0 would answer correctly for the part above
     * `base` while silently dropping the part that wrapped to low addresses -- so
     * the answer must be "untrustworthy", not a confident verdict.
     */
    CHECK_HEX("wrapping range is reported unconfigured, not silently clamped",
              HYPE_VM_ISOLATION_UNCONFIGURED,
              hype_vm_isolation_check(0xFFFFFFFFFFFF0000ull, 0x20000ull, 0x1000, A_BASE, SIZE_1G,
                                      0x2000));
    /* Still detects a genuine overlap above the base while flagging the wrap. */
    CHECK_HEX("wrap plus real overlap reports both",
              HYPE_VM_ISOLATION_UNCONFIGURED | HYPE_VM_ISOLATION_RAM_OVERLAP,
              hype_vm_isolation_check(0xFFFFFFFFFFFF0000ull, 0x20000ull, 0x1000,
                                      0xFFFFFFFFFFFF8000ull, 0x1000ull, 0x2000));
    /* Exactly reaching the top does NOT wrap -- boundary, not overflow. Both
     * sides, since the two clamps are separate code paths. */
    CHECK_HEX("a ending exactly at 2^64 is fine", HYPE_VM_ISOLATION_OK,
              hype_vm_isolation_check(0xFFFFFFFF00000000ull, 0x100000000ull, 0x1000, A_BASE,
                                      SIZE_1G, 0x2000));
    CHECK_HEX("b ending exactly at 2^64 is fine", HYPE_VM_ISOLATION_OK,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, 0xFFFFFFFF00000000ull,
                                      0x100000000ull, 0x2000));
    CHECK_HEX("b-side wrap flagged too", HYPE_VM_ISOLATION_UNCONFIGURED,
              hype_vm_isolation_check(A_BASE, SIZE_1G, 0x1000, 0xFFFFFFFFFFFF0000ull, 0x20000ull,
                                      0x2000));
}

static void test_describe(void) {
    CHECK_STR("ok describes as isolated", "isolated",
              hype_vm_isolation_describe(HYPE_VM_ISOLATION_OK));
    CHECK_STR("shared root wins over overlap in the message",
              "SHARED TRANSLATION ROOT -- guests share one address space",
              hype_vm_isolation_describe(HYPE_VM_ISOLATION_SAME_ROOT |
                                         HYPE_VM_ISOLATION_RAM_OVERLAP));
    CHECK_STR("overlap alone", "RAM RANGES OVERLAP -- each guest can reach the other's memory",
              hype_vm_isolation_describe(HYPE_VM_ISOLATION_RAM_OVERLAP));
    CHECK_STR("unconfigured alone",
              "UNCONFIGURED -- a base/size/root was zero, result not trustworthy",
              hype_vm_isolation_describe(HYPE_VM_ISOLATION_UNCONFIGURED));
}

int main(void) {
    test_isolated_pair();
    test_ram_overlap();
    test_adjacent_is_not_overlap();
    test_same_root();
    test_unconfigured();
    test_size_wrap();
    test_describe();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
