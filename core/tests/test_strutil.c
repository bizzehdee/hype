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


static void test_ascii_to_utf16(void) {
    uint16_t buf[8];

    /* #261: config paths are ASCII char; the UEFI file protocols take CHAR16. */
    if (hype_ascii_to_utf16("\\iso\\a.iso", buf, 16u) != 0) {
        printf("FAIL: widening a normal path should succeed\n");
        failures++;
    }

    if (hype_ascii_to_utf16("abc", buf, 8u) != 0 || buf[0] != 'a' || buf[1] != 'b' ||
        buf[2] != 'c' || buf[3] != 0u) {
        printf("FAIL: 'abc' should widen to a NUL-terminated a,b,c\n");
        failures++;
    }

    /* Exactly fits: 7 chars + NUL in 8 words. */
    if (hype_ascii_to_utf16("1234567", buf, 8u) != 0 || buf[7] != 0u) {
        printf("FAIL: a string that exactly fits should succeed\n");
        failures++;
    }

    /* One too long must be REFUSED, not truncated -- silently truncating a path
     * means opening the wrong file. */
    if (hype_ascii_to_utf16("12345678", buf, 8u) == 0) {
        printf("FAIL: an over-long path must be refused, not truncated\n");
        failures++;
    }

    /* Non-ASCII cannot be widened by zero-extension; refuse rather than guess. */
    if (hype_ascii_to_utf16("a\xc3\xa9" "b", buf, 8u) == 0) {
        printf("FAIL: a non-ASCII byte must be refused\n");
        failures++;
    }

    if (hype_ascii_to_utf16(0, buf, 8u) == 0 || hype_ascii_to_utf16("a", 0, 8u) == 0 ||
        hype_ascii_to_utf16("a", buf, 0u) == 0) {
        printf("FAIL: NULL/zero-length arguments must be refused\n");
        failures++;
    }
}

int main(void) {
    test_ascii_to_utf16();
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
