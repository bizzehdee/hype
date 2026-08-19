#include <stdio.h>
#include <string.h>

#include "../log_level.h"

static int failures;

#define CHECK(msg, cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void test_named_levels_parse(void) {
    hype_log_level_t l = HYPE_LOG_DEBUG;
    CHECK("error", hype_log_level_parse("error", &l) == 0 && l == HYPE_LOG_ERROR);
    CHECK("warn", hype_log_level_parse("warn", &l) == 0 && l == HYPE_LOG_WARN);
    CHECK("info", hype_log_level_parse("info", &l) == 0 && l == HYPE_LOG_INFO);
    CHECK("debug", hype_log_level_parse("debug", &l) == 0 && l == HYPE_LOG_DEBUG);
}

/*
 * The rule this module exists for: anything unreadable leaves the caller's pre-seeded level alone,
 * and callers pre-seed DEBUG. A host that cannot read its config is the host whose log matters
 * most, so a broken value must never quiet it.
 */
static void test_anything_unreadable_leaves_debug_in_place(void) {
    hype_log_level_t l = HYPE_LOG_DEBUG;
    CHECK("an unknown name is refused", hype_log_level_parse("verbose", &l) == -1);
    CHECK("and the level is untouched", l == HYPE_LOG_DEBUG);
    CHECK("a null name is refused", hype_log_level_parse(0, &l) == -1);
    CHECK("still untouched", l == HYPE_LOG_DEBUG);
    CHECK("an empty name is refused", hype_log_level_parse("", &l) == -1);
    CHECK("a number is refused -- levels are named", hype_log_level_parse("3", &l) == -1);
    CHECK("case matters, like every other config value",
          hype_log_level_parse("DEBUG", &l) == -1);
    CHECK("a null out is refused", hype_log_level_parse("info", 0) == -1);

    /* And a caller that had already chosen a quiet level keeps it -- parse failure is not a reset. */
    l = HYPE_LOG_ERROR;
    CHECK("a bad value does not reset a chosen level", hype_log_level_parse("nope", &l) == -1);
    CHECK("which stays as it was", l == HYPE_LOG_ERROR);
}

static void test_filtering_is_inclusive_downwards(void) {
    CHECK("error shows errors", hype_log_level_enabled(HYPE_LOG_ERROR, HYPE_LOG_ERROR));
    CHECK("error hides warnings", !hype_log_level_enabled(HYPE_LOG_ERROR, HYPE_LOG_WARN));
    CHECK("error hides info", !hype_log_level_enabled(HYPE_LOG_ERROR, HYPE_LOG_INFO));
    CHECK("error hides debug", !hype_log_level_enabled(HYPE_LOG_ERROR, HYPE_LOG_DEBUG));

    CHECK("warn shows errors", hype_log_level_enabled(HYPE_LOG_WARN, HYPE_LOG_ERROR));
    CHECK("warn shows warnings", hype_log_level_enabled(HYPE_LOG_WARN, HYPE_LOG_WARN));
    CHECK("warn hides info", !hype_log_level_enabled(HYPE_LOG_WARN, HYPE_LOG_INFO));

    CHECK("info shows info", hype_log_level_enabled(HYPE_LOG_INFO, HYPE_LOG_INFO));
    CHECK("info hides debug", !hype_log_level_enabled(HYPE_LOG_INFO, HYPE_LOG_DEBUG));

    /* debug shows everything -- this is the default, and it is today's behaviour exactly. */
    CHECK("debug shows errors", hype_log_level_enabled(HYPE_LOG_DEBUG, HYPE_LOG_ERROR));
    CHECK("debug shows warnings", hype_log_level_enabled(HYPE_LOG_DEBUG, HYPE_LOG_WARN));
    CHECK("debug shows info", hype_log_level_enabled(HYPE_LOG_DEBUG, HYPE_LOG_INFO));
    CHECK("debug shows debug", hype_log_level_enabled(HYPE_LOG_DEBUG, HYPE_LOG_DEBUG));
}

static void test_names_round_trip_and_never_vanish(void) {
    hype_log_level_t l;
    const char *names[] = {"error", "warn", "info", "debug"};
    unsigned i;
    for (i = 0; i < 4u; i++) {
        CHECK("parses", hype_log_level_parse(names[i], &l) == 0);
        CHECK("and names itself back", strcmp(hype_log_level_name(l), names[i]) == 0);
    }
    /* An out-of-range level must report the loudest, not an empty string: a log that cannot say
     * which level it is running is the failure #522 already cost us once. */
    CHECK("an out-of-range level reads as debug",
          strcmp(hype_log_level_name((hype_log_level_t)99), "debug") == 0);
}

int main(void) {
    test_named_levels_parse();
    test_anything_unreadable_leaves_debug_in_place();
    test_filtering_is_inclusive_downwards();
    test_names_round_trip_and_never_vanish();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
}
