#include <stdio.h>
#include "../guest_mtrr.h"

static int failures;

#define CHECK_HEX(desc, want, got)                                                             \
    do {                                                                                       \
        unsigned long long w_ = (unsigned long long)(want), g_ = (unsigned long long)(got);     \
        if (w_ != g_) {                                                                        \
            printf("FAIL: %s -- want 0x%llx got 0x%llx\n", (desc), w_, g_);                    \
            failures++;                                                                        \
        }                                                                                      \
    } while (0)

#define CHECK_INT(desc, want, got)                                                             \
    do {                                                                                       \
        long w_ = (long)(want), g_ = (long)(got);                                              \
        if (w_ != g_) {                                                                        \
            printf("FAIL: %s -- want %ld got %ld\n", (desc), w_, g_);                          \
            failures++;                                                                        \
        }                                                                                      \
    } while (0)

/* Exactly the MSRs #436's model covers, and nothing else. IA32_PAT is deliberately not one:
 * both backends keep PAT in the vendor field the CPU loads on entry. */
static void test_is_msr(void) {
    CHECK_INT("MTRRcap 0xFE", 1, hype_guest_mtrr_is_msr(0xFEu));
    CHECK_INT("MTRRdefType 0x2FF", 1, hype_guest_mtrr_is_msr(0x2FFu));
    CHECK_INT("first variable pair 0x200", 1, hype_guest_mtrr_is_msr(0x200u));
    CHECK_INT("last variable pair 0x20F", 1, hype_guest_mtrr_is_msr(0x20Fu));
    CHECK_INT("fixed 0x250", 1, hype_guest_mtrr_is_msr(0x250u));
    CHECK_INT("fixed 0x258", 1, hype_guest_mtrr_is_msr(0x258u));
    CHECK_INT("fixed 0x259", 1, hype_guest_mtrr_is_msr(0x259u));
    CHECK_INT("fixed 0x268", 1, hype_guest_mtrr_is_msr(0x268u));
    CHECK_INT("fixed 0x26F", 1, hype_guest_mtrr_is_msr(0x26Fu));

    CHECK_INT("IA32_PAT is NOT ours", 0, hype_guest_mtrr_is_msr(0x277u));
    CHECK_INT("0x1FF is below the variable range", 0, hype_guest_mtrr_is_msr(0x1FFu));
    CHECK_INT("0x210 is above it", 0, hype_guest_mtrr_is_msr(0x210u));
    CHECK_INT("0x251 is not a fixed MTRR", 0, hype_guest_mtrr_is_msr(0x251u));
    CHECK_INT("0x270 is past the fixed range", 0, hype_guest_mtrr_is_msr(0x270u));
    CHECK_INT("EFER is not ours", 0, hype_guest_mtrr_is_msr(0xC0000080u));
}

/*
 * #481: the reset state is NOT all-zero. Zero means "MTRRs disabled, default type UC", i.e.
 * every byte of RAM uncached -- FreeBSD reads this, believes it, and panics. And FE (bit 10)
 * must stay clear or a DEBUG OVMF asserts in PEI.
 */
static void test_reset_is_write_back_not_zero(void) {
    hype_guest_mtrr_t m;
    unsigned int i;

    hype_guest_mtrr_reset(&m);
    CHECK_HEX("deftype = E|WB", 0x0806u, m.deftype);
    CHECK_INT("E (bit 11) set", 1, (int)((m.deftype >> 11) & 1u));
    CHECK_INT("FE (bit 10) CLEAR -- a DEBUG OVMF asserts otherwise", 0,
              (int)((m.deftype >> 10) & 1u));
    CHECK_HEX("default type is WB (6)", 6u, m.deftype & 0xFFu);
    for (i = 0; i < 11u; i++) {
        CHECK_HEX("fixed MTRRs are all WB", 0x0606060606060606ull, m.fix[i]);
    }
    for (i = 0; i < 16u; i++) {
        CHECK_HEX("variable MTRRs start disabled", 0u, m.var[i]);
    }
    hype_guest_mtrr_reset((hype_guest_mtrr_t *)0); /* must not fault */
}

/* The whole point: what the guest writes is what the guest reads. Without this, OVMF's
 * MtrrLib write-then-verify never converges. */
static void test_round_trip(void) {
    hype_guest_mtrr_t m;
    unsigned int i;

    hype_guest_mtrr_reset(&m);

    /* The exact value the failing micro/vmexit probe wrote. */
    hype_guest_mtrr_write(&m, 0x200u, 0x123456000ull);
    CHECK_HEX("var0 base round-trips", 0x123456000ull, hype_guest_mtrr_read(&m, 0x200u));
    CHECK_HEX("its neighbour is untouched", 0u, hype_guest_mtrr_read(&m, 0x201u));

    hype_guest_mtrr_write(&m, 0x20Fu, 0xFFFFFFFF800ull);
    CHECK_HEX("last variable pair round-trips", 0xFFFFFFFF800ull,
              hype_guest_mtrr_read(&m, 0x20Fu));

    hype_guest_mtrr_write(&m, 0x2FFu, 0xC00ull);
    CHECK_HEX("deftype round-trips", 0xC00ull, hype_guest_mtrr_read(&m, 0x2FFu));

    /* Each fixed MTRR must land in its own slot -- the three ranges are not contiguous, so an
     * off-by-one in the index maps two MSRs onto one word. */
    for (i = 0; i < 11u; i++) {
        static const uint32_t fixed[11] = {0x250u, 0x258u, 0x259u, 0x268u, 0x269u, 0x26Au,
                                           0x26Bu, 0x26Cu, 0x26Du, 0x26Eu, 0x26Fu};
        hype_guest_mtrr_write(&m, fixed[i], 0x1000ull + i);
    }
    for (i = 0; i < 11u; i++) {
        static const uint32_t fixed[11] = {0x250u, 0x258u, 0x259u, 0x268u, 0x269u, 0x26Au,
                                           0x26Bu, 0x26Cu, 0x26Du, 0x26Eu, 0x26Fu};
        CHECK_HEX("each fixed MTRR has its own slot", 0x1000ull + i,
                  hype_guest_mtrr_read(&m, fixed[i]));
    }
}

/*
 * MTRRcap must agree with the storage behind it. Advertising variable MTRRs that do not exist
 * is the original bug, not a lesser version of it.
 */
static void test_mtrrcap_is_read_only_and_consistent(void) {
    hype_guest_mtrr_t m;

    hype_guest_mtrr_reset(&m);
    CHECK_HEX("MTRRcap value", HYPE_GUEST_MTRRCAP, hype_guest_mtrr_read(&m, 0xFEu));
    CHECK_INT("VCNT matches the 8 pairs in var[16]", 8, (int)(HYPE_GUEST_MTRRCAP & 0xFFu));
    CHECK_INT("FIX supported", 1, (int)((HYPE_GUEST_MTRRCAP >> 8) & 1u));
    CHECK_INT("WC supported", 1, (int)((HYPE_GUEST_MTRRCAP >> 10) & 1u));

    /* A write to it is #GP on real hardware; hype drops it rather than corrupting the value. */
    hype_guest_mtrr_write(&m, 0xFEu, 0xDEADBEEFull);
    CHECK_HEX("MTRRcap is unchanged by a write", HYPE_GUEST_MTRRCAP,
              hype_guest_mtrr_read(&m, 0xFEu));
}

static void test_out_of_range_and_null(void) {
    hype_guest_mtrr_t m;

    hype_guest_mtrr_reset(&m);
    hype_guest_mtrr_write(&m, 0x277u, 0xAAAAull); /* PAT -- not ours, must be dropped */
    CHECK_HEX("an MSR we do not model reads 0", 0u, hype_guest_mtrr_read(&m, 0x277u));
    CHECK_HEX("and did not land in deftype", 0x0806u, hype_guest_mtrr_read(&m, 0x2FFu));

    hype_guest_mtrr_write((hype_guest_mtrr_t *)0, 0x200u, 1ull); /* must not fault */
    CHECK_HEX("a NULL model still reports MTRRcap", HYPE_GUEST_MTRRCAP,
              hype_guest_mtrr_read((const hype_guest_mtrr_t *)0, 0xFEu));
    CHECK_HEX("and 0 for everything else", 0u,
              hype_guest_mtrr_read((const hype_guest_mtrr_t *)0, 0x200u));
}

int main(void) {
    test_is_msr();
    test_reset_is_write_back_not_zero();
    test_round_trip();
    test_mtrrcap_is_read_only_and_consistent();
    test_out_of_range_and_null();
    if (failures == 0) {
        printf("test_guest_mtrr: all checks passed\n");
        return 0;
    }
    printf("test_guest_mtrr: %d failure(s)\n", failures);
    return 1;
}
