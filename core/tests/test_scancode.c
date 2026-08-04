#include <stdio.h>
#include <string.h>

#include "../scancode.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_unshifted_is_make_then_break(void) {
    uint8_t b[HYPE_SCANCODE_MAX_PER_CHAR];
    CHECK_HEX("'a' is two bytes", 2, hype_ascii_to_set1('a', b, sizeof(b)));
    CHECK_HEX("make 0x1E", 0x1E, b[0]);
    /* The break matters: a make with no break leaves the guest believing the key is
     * still held. */
    CHECK_HEX("break 0x9E", 0x9E, b[1]);

    CHECK_HEX("newline maps to Enter", 2, hype_ascii_to_set1('\n', b, sizeof(b)));
    CHECK_HEX("Enter make 0x1C", 0x1C, b[0]);
    CHECK_HEX("carriage return maps to Enter too", 2, hype_ascii_to_set1('\r', b, sizeof(b)));
    CHECK_HEX("CR make 0x1C", 0x1C, b[0]);
    CHECK_HEX("space", 2, hype_ascii_to_set1(' ', b, sizeof(b)));
    CHECK_HEX("space make 0x39", 0x39, b[0]);
    CHECK_HEX("tab", 2, hype_ascii_to_set1('\t', b, sizeof(b)));
    CHECK_HEX("backspace", 2, hype_ascii_to_set1('\b', b, sizeof(b)));
}

static void test_shifted_wraps_the_key_in_shift(void) {
    uint8_t b[HYPE_SCANCODE_MAX_PER_CHAR];
    CHECK_HEX("'A' is four bytes", 4, hype_ascii_to_set1('A', b, sizeof(b)));
    /* ORDER is the assertion: shift down, key, key up, shift up. Any other order either
     * types the unshifted character or leaves shift stuck down for everything after. */
    CHECK_HEX("shift make first", 0x2A, b[0]);
    CHECK_HEX("then 'a' key make", 0x1E, b[1]);
    CHECK_HEX("then key break", 0x9E, b[2]);
    CHECK_HEX("then shift RELEASED", 0xAA, b[3]);

    /* Same key as its unshifted twin -- the shift decides, not a different code. */
    {
        uint8_t lower[HYPE_SCANCODE_MAX_PER_CHAR];
        (void)hype_ascii_to_set1('a', lower, sizeof(lower));
        CHECK_HEX("'A' uses the same key as 'a'", lower[0], b[1]);
    }
}

static void test_digit_row_shifted_symbols(void) {
    uint8_t b[HYPE_SCANCODE_MAX_PER_CHAR];
    /* The shifted digit row is !@#$%^&*() in KEYBOARD order, which has no arithmetic
     * relationship to the ASCII values -- the case a computed mapping gets wrong. */
    (void)hype_ascii_to_set1('!', b, sizeof(b));
    CHECK_HEX("'!' is shift+1", 0x02, b[1]);
    (void)hype_ascii_to_set1('@', b, sizeof(b));
    CHECK_HEX("'@' is shift+2", 0x03, b[1]);
    (void)hype_ascii_to_set1('*', b, sizeof(b));
    CHECK_HEX("'*' is shift+8", 0x09, b[1]);
    (void)hype_ascii_to_set1(')', b, sizeof(b));
    CHECK_HEX("')' is shift+0", 0x0B, b[1]);
    (void)hype_ascii_to_set1(':', b, sizeof(b));
    CHECK_HEX("':' is shift+;", 0x27, b[1]);
    (void)hype_ascii_to_set1('?', b, sizeof(b));
    CHECK_HEX("'?' is shift+/", 0x35, b[1]);
    (void)hype_ascii_to_set1('_', b, sizeof(b));
    CHECK_HEX("'_' is shift+-", 0x0C, b[1]);
}

static void test_unmapped_and_capacity_are_all_or_nothing(void) {
    uint8_t b[HYPE_SCANCODE_MAX_PER_CHAR];
    uint8_t small[3];

    /* Control characters with no single-key form are refused, not approximated. */
    CHECK_HEX("NUL unmapped", 0, hype_ascii_to_set1('\0', b, sizeof(b)));
    CHECK_HEX("bell unmapped", 0, hype_ascii_to_set1('\a', b, sizeof(b)));
    CHECK_HEX("escape unmapped", 0, hype_ascii_to_set1(0x1B, b, sizeof(b)));
    CHECK_HEX("high byte unmapped", 0, hype_ascii_to_set1((char)0xE9, b, sizeof(b)));
    CHECK_HEX("NULL out", 0, hype_ascii_to_set1('a', 0, 4));

    /*
     * A shifted character needs 4 bytes. With only 3 it must write NOTHING -- writing
     * shift+make+break and dropping the shift RELEASE would leave shift held for every
     * character that followed, silently turning the rest of a typed line into
     * uppercase.
     */
    small[0] = 0xFF;
    CHECK_HEX("shifted char refused when it would not fit", 0,
              hype_ascii_to_set1('A', small, 3));
    CHECK_HEX("and nothing was written", 0xFF, small[0]);
    /* An unshifted one fits in 2 and is allowed. */
    CHECK_HEX("unshifted fits in 3", 2, hype_ascii_to_set1('a', small, 3));
}

static void test_string_conversion(void) {
    uint8_t b[64];
    unsigned int n;

    n = hype_ascii_string_to_set1("ab", b, sizeof(b));
    CHECK_HEX("two chars = four bytes", 4, n);
    CHECK_HEX("a make", 0x1E, b[0]);
    CHECK_HEX("a break", 0x9E, b[1]);
    CHECK_HEX("b make", 0x30, b[2]);
    CHECK_HEX("b break", 0xB0, b[3]);

    /* Enter included, as a script's "send cmd\n" needs. */
    n = hype_ascii_string_to_set1("a\n", b, sizeof(b));
    CHECK_HEX("with newline", 4, n);
    CHECK_HEX("ends with Enter break", 0x9C, b[3]);

    /* Stops at an unmapped character rather than skipping it: silently dropping a
     * character produces a DIFFERENT command, which is worse than a short one. */
    n = hype_ascii_string_to_set1("a\x1b" "b", b, sizeof(b));
    CHECK_HEX("stops at the unmapped byte", 2, n);

    /* Runs out of room mid-string: stops on a character boundary. */
    n = hype_ascii_string_to_set1("aaaa", b, 5);
    CHECK_HEX("truncates on a boundary, not mid-character", 4, n);

    CHECK_HEX("NULL string", 0, hype_ascii_string_to_set1(0, b, sizeof(b)));
    CHECK_HEX("NULL out", 0, hype_ascii_string_to_set1("a", 0, 4));
    CHECK_HEX("empty string", 0, hype_ascii_string_to_set1("", b, sizeof(b)));
}

int main(void) {
    test_unshifted_is_make_then_break();
    test_shifted_wraps_the_key_in_shift();
    test_digit_row_shifted_symbols();
    test_unmapped_and_capacity_are_all_or_nothing();
    test_string_conversion();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
