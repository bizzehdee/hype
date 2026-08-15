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

int main(void) {
    test_find_matches_exact_wxh();
    test_find_returns_minus_one_when_absent();
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
