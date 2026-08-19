#include <stdio.h>
#include "../gop_mode.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), \
                   (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_find_matches_exact_wxh(void) {
    hype_gop_mode_t modes[3] = {
        {0, 800, 600},
        {1, 1920, 1080},
        {2, 3840, 2160},
    };
    CHECK_INT("finds mode 0", 0, hype_gop_mode_find(modes, 3, 800, 600));
    CHECK_INT("finds mode 1", 1, hype_gop_mode_find(modes, 3, 1920, 1080));
    CHECK_INT("finds mode 2", 2, hype_gop_mode_find(modes, 3, 3840, 2160));
}

static void test_find_returns_minus_one_when_absent(void) {
    hype_gop_mode_t modes[1] = {{0, 800, 600}};
    CHECK_INT("no match", -1, hype_gop_mode_find(modes, 1, 1920, 1080));
}

static void test_find_on_empty_list(void) {
    CHECK_INT("empty list never matches", -1, hype_gop_mode_find(0, 0, 1920, 1080));
}

/* Real hardware can report the same WxH at more than one mode number (different pixel formats/
 * stride) -- the first one in enumeration order wins, since that is what SetMode(first match)
 * will apply and there is no other tiebreaker to prefer. */
static void test_find_returns_first_match_on_duplicate_wxh(void) {
    hype_gop_mode_t modes[3] = {
        {0, 800, 600},
        {5, 1920, 1080},
        {9, 1920, 1080},
    };
    CHECK_INT("first duplicate wins", 1, hype_gop_mode_find(modes, 3, 1920, 1080));
}

static void test_find_does_not_confuse_width_and_height(void) {
    hype_gop_mode_t modes[1] = {{0, 1080, 1920}}; /* transposed, not the mode under test */
    CHECK_INT("1920x1080 does not match a 1080x1920 entry", -1,
              hype_gop_mode_find(modes, 1, 1920, 1080));
}

/* Width matching alone must not be enough -- exercises the height half of the && once the width
 * half has already passed, distinct from every other case here (which all differ on width first
 * and never reach the height comparison at all). */
static void test_find_requires_both_width_and_height_to_match(void) {
    hype_gop_mode_t modes[1] = {{0, 1920, 600}}; /* same width as the query, different height */
    CHECK_INT("matching width alone is not a match", -1,
              hype_gop_mode_find(modes, 1, 1920, 1080));
}

/*
 * #465: the `resolution 1920x1080` parse.
 *
 * The inline version this replaces handed hype_parse_uint() the whole "1920x1080" as the width.
 * That function requires the entire string to be digits, so it failed at the 'x' and EVERY
 * well-formed WxH was rejected -- with a message naming the exact format the operator had just
 * typed. It survived because the parse lived inline in boot/main.c where nothing could test it.
 */
static void test_parse_wxh_accepts_real_resolutions(void) {
    static const struct { const char *in; uint32_t w, h; } ok[] = {
        {"1920x1080", 1920u, 1080u},   /* the exact input that was reported failing */
        {"1280x800",  1280u, 800u},
        {"640x480",   640u,  480u},
        {"2560x1600", 2560u, 1600u},
        {"800X600",   800u,  600u},    /* uppercase separator */
        {"1x1",       1u,    1u},      /* minimal, still valid */
    };
    unsigned i;
    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        uint32_t w = 0, h = 0;
        int rc = hype_gop_mode_parse_wxh(ok[i].in, &w, &h);
        if (rc != 0 || w != ok[i].w || h != ok[i].h) {
            printf("FAIL: parse \"%s\" -> rc=%d %ux%u, expected 0 %ux%u\n", ok[i].in, rc, w, h,
                   ok[i].w, ok[i].h);
            failures++;
        }
    }
}

static void test_parse_wxh_rejects_malformed(void) {
    static const char *bad[] = {
        "",            /* empty */
        "1920",        /* no separator */
        "x1080",       /* nothing before the separator */
        "1920x",       /* nothing after it */
        "1920y1080",   /* wrong separator */
        "0x1080",      /* zero width -- and NOT to be read as hex */
        "1920x0",      /* zero height */
        "19a0x1080",   /* non-digit in the width */
        "1920x10 80",  /* embedded space in the height */
        "99999999999999999999x1080", /* overflows, and is longer than the width buffer */
    };
    unsigned i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint32_t w = 123u, h = 456u;
        if (hype_gop_mode_parse_wxh(bad[i], &w, &h) == 0) {
            printf("FAIL: parse \"%s\" was accepted, expected rejection\n", bad[i]);
            failures++;
        }
    }
    {
        uint32_t w = 0, h = 0;
        if (hype_gop_mode_parse_wxh(0, &w, &h) == 0) {
            printf("FAIL: NULL input was accepted\n");
            failures++;
        }
    }
}

/* #529 (decision 44): there is no resolution config key -- hype aims at 1920x1080 and takes the
 * nearest mode offered. */
static void test_nearest_mode_to_1080p(void) {
    hype_gop_mode_t modes[5];
    modes[0].mode_number = 0; modes[0].width = 800;  modes[0].height = 600;
    modes[1].mode_number = 1; modes[1].width = 1920; modes[1].height = 1080;
    modes[2].mode_number = 2; modes[2].width = 3840; modes[2].height = 2160;
    modes[3].mode_number = 3; modes[3].width = 1600; modes[3].height = 900;
    modes[4].mode_number = 4; modes[4].width = 1920; modes[4].height = 1200;

    CHECK_INT("an exact 1920x1080 wins", 1, (hype_gop_mode_find_nearest(modes, 5u, 1920u, 1080u) == 1) ? 1 : 0);
    /* Without the exact match, 1920x1200 beats 1600x900: nearer in total pixels, and the mode an
     * operator looking at the panel would call closer. */
    CHECK_INT("1920x1200 beats 1600x900 for a 1080p target", 1, (hype_gop_mode_find_nearest(modes + 2, 3u, 1920u, 1080u) == 2) ? 1 : 0);
    CHECK_INT("an empty list has no answer", 1, (hype_gop_mode_find_nearest(modes, 0u, 1920u, 1080u) == -1) ? 1 : 0);
    CHECK_INT("a null list has no answer", 1, (hype_gop_mode_find_nearest(0, 5u, 1920u, 1080u) == -1) ? 1 : 0);
    CHECK_INT("a single offered mode is the answer whatever it is", 1, (hype_gop_mode_find_nearest(modes, 1u, 1920u, 1080u) == 0) ? 1 : 0);
}

/* Equidistant modes must resolve the same way every boot, or the console size becomes a coin
 * toss between builds. */
static void test_nearest_mode_tie_breaks_on_width(void) {
    hype_gop_mode_t modes[2];
    modes[0].mode_number = 0; modes[0].width = 1000; modes[0].height = 2073;
    modes[1].mode_number = 1; modes[1].width = 2073; modes[1].height = 1000;
    CHECK_INT("the wider of two equidistant modes wins", 1, (hype_gop_mode_find_nearest(modes, 2u, 1440u, 1440u) == 1) ? 1 : 0);
}


int main(void) {
    test_nearest_mode_to_1080p();
    test_nearest_mode_tie_breaks_on_width();
    test_find_matches_exact_wxh();
    test_find_returns_minus_one_when_absent();
    test_parse_wxh_accepts_real_resolutions();
    test_parse_wxh_rejects_malformed();
    test_find_on_empty_list();
    test_find_returns_first_match_on_duplicate_wxh();
    test_find_does_not_confuse_width_and_height();
    test_find_requires_both_width_and_height_to_match();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
