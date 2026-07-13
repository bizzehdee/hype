#include <stdio.h>
#include <string.h>
#include "../format.h"

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
        if ((expected) != (actual)) { \
            printf("FAIL: %s: expected %d, got %d\n", (desc), (int)(expected), (int)(actual)); \
            failures++; \
        } \
    } while (0)

static int fmt(char *buf, unsigned long long bufsz, const char *f, ...) {
    va_list ap;
    int r;
    va_start(ap, f);
    r = hype_vsnprintf(buf, bufsz, f, ap);
    va_end(ap);
    return r;
}

int main(void) {
    char buf[64];

    fmt(buf, sizeof(buf), "hype");
    CHECK_STR("plain string", "hype", buf);

    fmt(buf, sizeof(buf), "%s v%d", "hype", 1);
    CHECK_STR("%s and %d", "hype v1", buf);

    fmt(buf, sizeof(buf), "%d", -42);
    CHECK_STR("negative %d", "-42", buf);

    fmt(buf, sizeof(buf), "%d", 0);
    CHECK_STR("%d zero", "0", buf);

    fmt(buf, sizeof(buf), "%d", -2147483647 - 1);
    CHECK_STR("INT_MIN %d", "-2147483648", buf);

    fmt(buf, sizeof(buf), "%u", 4294967295U);
    CHECK_STR("%u max", "4294967295", buf);

    fmt(buf, sizeof(buf), "%x", 0xdeadbeefU);
    CHECK_STR("%x", "deadbeef", buf);

    fmt(buf, sizeof(buf), "%x", 0U);
    CHECK_STR("%x zero", "0", buf);

    fmt(buf, sizeof(buf), "%c%c%c", 'a', 'b', 'c');
    CHECK_STR("%c", "abc", buf);

    fmt(buf, sizeof(buf), "100%%");
    CHECK_STR("%%", "100%", buf);

    fmt(buf, sizeof(buf), "%s", (const char *)0);
    CHECK_STR("null %s", "(null)", buf);

    fmt(buf, sizeof(buf), "%p", (void *)0x1234ULL);
    CHECK_STR("%p", "0x1234", buf);

    fmt(buf, sizeof(buf), "%llu", 18446744073709551615ULL);
    CHECK_STR("%llu max", "18446744073709551615", buf);

    fmt(buf, sizeof(buf), "%llx", 0xdeadbeef12345678ULL);
    CHECK_STR("%llx", "deadbeef12345678", buf);

    fmt(buf, sizeof(buf), "%lld", -123456789012345LL);
    CHECK_STR("%lld negative", "-123456789012345", buf);

    fmt(buf, sizeof(buf), "%l!");
    CHECK_STR("bare %l passthrough", "%l!", buf);

    fmt(buf, sizeof(buf), "%llz");
    CHECK_STR("%ll with unknown specifier passthrough", "%llz", buf);

    fmt(buf, sizeof(buf), "%z");
    CHECK_STR("unknown specifier passthrough", "%z", buf);

    fmt(buf, sizeof(buf), "trailing%");
    CHECK_STR("dangling percent at end", "trailing%", buf);

    {
        char small[4];
        int r = fmt(small, sizeof(small), "hello");
        CHECK_STR("truncated output is NUL-terminated", "hel", small);
        CHECK_INT("truncated return value is untruncated length", 5, r);
    }

    {
        int r = hype_snprintf(buf, sizeof(buf), "%s=%d", "x", 7);
        CHECK_STR("hype_snprintf wrapper", "x=7", buf);
        CHECK_INT("hype_snprintf return value", 3, r);
    }

    {
        char zero[1];
        int r = fmt(zero, 0, "abc");
        CHECK_INT("bufsz==0 does not crash and reports full length", 3, r);
        (void)zero;
    }

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
