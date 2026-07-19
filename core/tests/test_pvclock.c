#include <stdio.h>
#include <stdlib.h>
#include "../../devices/pvclock.h"

static int failures = 0;

#define CHECK(desc, cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s\n", (desc)); \
            failures++; \
        } \
    } while (0)

#define CHECK_U64(desc, expected, actual) \
    do { \
        unsigned long long e_ = (unsigned long long)(expected), a_ = (unsigned long long)(actual); \
        if (e_ != a_) { \
            printf("FAIL: %s: expected %llu, got %llu\n", (desc), e_, a_); \
            failures++; \
        } \
    } while (0)

/* A one-second TSC delta must scale to ~1e9 ns, within rounding, at any freq. */
static void check_one_second(uint64_t tsc_hz) {
    uint32_t mul;
    int8_t shift;
    hype_pvclock_calc_scale(tsc_hz, &mul, &shift);
    uint64_t ns = hype_pvclock_scale_delta(tsc_hz, mul, shift);
    /* Allow 0.01% error from the fixed-point rounding. */
    uint64_t lo = 1000000000ULL - 100000ULL;
    uint64_t hi = 1000000000ULL + 100000ULL;
    char msg[64];
    snprintf(msg, sizeof(msg), "1s @ %llu Hz -> ~1e9 ns (got %llu)", (unsigned long long)tsc_hz,
             (unsigned long long)ns);
    CHECK(msg, ns >= lo && ns <= hi);
}

static void test_scale_1ghz_exact(void) {
    uint32_t mul;
    int8_t shift;
    hype_pvclock_calc_scale(1000000000ULL, &mul, &shift);
    /* Verified by hand: 1 GHz -> mul=0x80000000, shift=1, ns == delta. */
    CHECK_U64("1GHz mul", 0x80000000u, mul);
    CHECK_U64("1GHz shift", 1, shift);
    CHECK_U64("1GHz scale_delta identity", 12345678u, hype_pvclock_scale_delta(12345678u, mul, shift));
}

static void test_scale_various_frequencies(void) {
    check_one_second(1000000000ULL);   /* 1.0 GHz */
    check_one_second(2000000000ULL);   /* 2.0 GHz */
    check_one_second(2994000000ULL);   /* ~3 GHz (typical) */
    check_one_second(3600000000ULL);   /* 3.6 GHz */
    check_one_second(1193182ULL);      /* PIT rate, a low freq */
    check_one_second(4000000000ULL);   /* 4.0 GHz */
    check_one_second(5000000000ULL);   /* 5.0 GHz -- exercises the base>>32 path */
}

static void test_scale_2ghz_halves(void) {
    uint32_t mul;
    int8_t shift;
    hype_pvclock_calc_scale(2000000000ULL, &mul, &shift);
    /* 2 GHz: 1 cycle = 0.5 ns, so a 2e9 delta (1s) -> 1e9 ns. */
    CHECK_U64("2GHz: 200 cycles -> 100 ns", 100u, hype_pvclock_scale_delta(200u, mul, shift));
}

static void test_calc_scale_zero_freq_guarded(void) {
    uint32_t mul = 0xdead;
    int8_t shift = 42;
    hype_pvclock_calc_scale(0, &mul, &shift);
    CHECK_U64("zero freq -> mul 0", 0u, mul);
    CHECK_U64("zero freq -> shift 0", 0, shift);
}

static void test_write_time_info_version_and_fields(void) {
    struct hype_pvclock_vcpu_time_info ti;
    for (unsigned i = 0; i < sizeof(ti); i++) {
        ((volatile uint8_t *)&ti)[i] = 0;
    }
    hype_pvclock_write_time_info(&ti, 0x1111222233334444ULL, 0x5555666677778888ULL, 0xAABBCCDDu,
                                 (int8_t)-3, HYPE_PVCLOCK_TSC_STABLE_BIT);
    CHECK_U64("version even (0 -> 2)", 2u, ti.version);
    CHECK("version even means stable", (ti.version & 1u) == 0);
    CHECK_U64("tsc_timestamp", 0x1111222233334444ULL, ti.tsc_timestamp);
    CHECK_U64("system_time", 0x5555666677778888ULL, ti.system_time);
    CHECK_U64("mul", 0xAABBCCDDu, ti.tsc_to_system_mul);
    CHECK_U64("shift", (uint8_t)(int8_t)-3, (uint8_t)ti.tsc_shift);
    CHECK_U64("flags", HYPE_PVCLOCK_TSC_STABLE_BIT, ti.flags);
    CHECK_U64("pad0 cleared", 0u, ti.pad0);
}

static void test_write_time_info_second_update_bumps_version(void) {
    struct hype_pvclock_vcpu_time_info ti;
    for (unsigned i = 0; i < sizeof(ti); i++) {
        ((volatile uint8_t *)&ti)[i] = 0;
    }
    hype_pvclock_write_time_info(&ti, 1, 2, 3, 0, 0);
    CHECK_U64("first update -> version 2", 2u, ti.version);
    hype_pvclock_write_time_info(&ti, 9, 8, 7, 0, HYPE_PVCLOCK_TSC_STABLE_BIT);
    CHECK_U64("second update -> version 4", 4u, ti.version);
    CHECK_U64("fields refreshed", 9u, ti.tsc_timestamp);
}

static void test_write_wall_clock(void) {
    struct hype_pvclock_wall_clock wc;
    for (unsigned i = 0; i < sizeof(wc); i++) {
        ((volatile uint8_t *)&wc)[i] = 0;
    }
    hype_pvclock_write_wall_clock(&wc, 1700000000u, 500u);
    CHECK_U64("wall clock version even", 2u, wc.version);
    CHECK_U64("wall clock sec", 1700000000u, wc.sec);
    CHECK_U64("wall clock nsec", 500u, wc.nsec);
}

static void test_struct_layout_matches_kvm_abi(void) {
    /* The guest reads this at fixed byte offsets -- lock them down. */
    CHECK_U64("time_info size", 32u, sizeof(struct hype_pvclock_vcpu_time_info));
    CHECK_U64("version @0", 0u, __builtin_offsetof(struct hype_pvclock_vcpu_time_info, version));
    CHECK_U64("tsc_timestamp @8", 8u,
              __builtin_offsetof(struct hype_pvclock_vcpu_time_info, tsc_timestamp));
    CHECK_U64("system_time @16", 16u,
              __builtin_offsetof(struct hype_pvclock_vcpu_time_info, system_time));
    CHECK_U64("tsc_to_system_mul @24", 24u,
              __builtin_offsetof(struct hype_pvclock_vcpu_time_info, tsc_to_system_mul));
    CHECK_U64("tsc_shift @28", 28u, __builtin_offsetof(struct hype_pvclock_vcpu_time_info, tsc_shift));
    CHECK_U64("flags @29", 29u, __builtin_offsetof(struct hype_pvclock_vcpu_time_info, flags));
    CHECK_U64("wall_clock size", 12u, sizeof(struct hype_pvclock_wall_clock));
}

int main(void) {
    test_scale_1ghz_exact();
    test_scale_various_frequencies();
    test_scale_2ghz_halves();
    test_calc_scale_zero_freq_guarded();
    test_write_time_info_version_and_fields();
    test_write_time_info_second_update_bumps_version();
    test_write_wall_clock();
    test_struct_layout_matches_kvm_abi();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
