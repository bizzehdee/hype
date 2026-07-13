#include <stdio.h>
#include <string.h>
#include "../strutil.h"

static int failures = 0;

#define CHECK_STR(desc, expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            printf("FAIL: %s: expected \"%s\", got \"%s\"\n", (desc), (expected), (actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_strlen(void) {
    CHECK_INT("strlen empty", 0, hype_strlen(""));
    CHECK_INT("strlen hype", 4, hype_strlen("hype"));
}

static void test_streq(void) {
    CHECK_INT("streq equal", 1, hype_streq("abc", "abc"));
    CHECK_INT("streq different length", 0, hype_streq("abc", "ab"));
    CHECK_INT("streq different content", 0, hype_streq("abc", "abd"));
    CHECK_INT("streq both empty", 1, hype_streq("", ""));
}

static void test_strneq(void) {
    CHECK_INT("strneq prefix match", 1, hype_strneq("abcdef", "abcxyz", 3));
    CHECK_INT("strneq mismatch within n", 0, hype_strneq("abcdef", "abXdef", 3));
    CHECK_INT("strneq stops at NUL in a", 1, hype_strneq("ab", "ab", 5));
}

static void test_strlcpy(void) {
    char buf[8];
    unsigned long long r;

    r = hype_strlcpy(buf, "hype", sizeof(buf));
    CHECK_STR("strlcpy fits", "hype", buf);
    CHECK_INT("strlcpy return value when it fits", 4, r);

    r = hype_strlcpy(buf, "way too long for this buffer", sizeof(buf));
    CHECK_STR("strlcpy truncates and NUL-terminates", "way too", buf);
    CHECK_INT("strlcpy return value is untruncated length", 28, r);

    r = hype_strlcpy(buf, "x", 0);
    CHECK_INT("strlcpy with dst_size==0 is a no-op, still reports length", 1, r);
}

static void test_is_digit_space(void) {
    CHECK_INT("is_digit '5'", 1, hype_is_digit('5'));
    CHECK_INT("is_digit 'a'", 0, hype_is_digit('a'));
    CHECK_INT("is_space ' '", 1, hype_is_space(' '));
    CHECK_INT("is_space '\\t'", 1, hype_is_space('\t'));
    CHECK_INT("is_space 'x'", 0, hype_is_space('x'));
}

static void test_parse_uint(void) {
    unsigned long long v;

    CHECK_INT("parse_uint simple", 0, hype_parse_uint("1234", &v));
    CHECK_INT("parse_uint simple value", 1234, v);

    CHECK_INT("parse_uint zero", 0, hype_parse_uint("0", &v));
    CHECK_INT("parse_uint zero value", 0, v);

    CHECK_INT("parse_uint with surrounding whitespace", 0, hype_parse_uint("  42  ", &v));
    CHECK_INT("parse_uint whitespace value", 42, v);

    CHECK_INT("parse_uint rejects empty", -1, hype_parse_uint("", &v));
    CHECK_INT("parse_uint rejects whitespace-only", -1, hype_parse_uint("   ", &v));
    CHECK_INT("parse_uint rejects non-digit", -1, hype_parse_uint("12a", &v));
    CHECK_INT("parse_uint rejects leading sign", -1, hype_parse_uint("-5", &v));
    CHECK_INT("parse_uint rejects overflow", -1, hype_parse_uint("99999999999999999999", &v));
}

static void test_str_trim(void) {
    char a[] = "  hello  ";
    char b[] = "notrim";
    char c[] = "   ";
    char d[] = "\ttab and newline\n";

    CHECK_STR("trim both ends", "hello", hype_str_trim(a));
    CHECK_STR("trim no-op", "notrim", hype_str_trim(b));
    CHECK_STR("trim all-whitespace becomes empty", "", hype_str_trim(c));
    CHECK_STR("trim tabs/newlines", "tab and newline", hype_str_trim(d));
}

int main(void) {
    test_strlen();
    test_streq();
    test_strneq();
    test_strlcpy();
    test_is_digit_space();
    test_parse_uint();
    test_str_trim();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
