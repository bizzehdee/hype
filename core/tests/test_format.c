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


/* #238: the visible-truncation marker. */
static void test_mark_truncated(void) {
    char buf[32];
    int n;

    /* Fits: untouched, reports 0. */
    n = hype_snprintf(buf, sizeof buf, "short line\n");
    CHECK_INT("fit not marked", 0, hype_format_mark_truncated(buf, sizeof buf, n));
    CHECK_INT("fit content intact", 1, buf[10] == '\n' && buf[11] == '\0');

    /* Exactly full (written == bufsz-1): still a fit. */
    n = hype_snprintf(buf, sizeof buf, "%031d", 7); /* 31 chars + NUL */
    CHECK_INT("exact fit not marked", 0, hype_format_mark_truncated(buf, sizeof buf, n));

    /* One over: marked, ends with the marker AND a newline before the NUL. */
    n = hype_snprintf(buf, sizeof buf, "%032d", 7);
    CHECK_INT("overflow marked", 1, hype_format_mark_truncated(buf, sizeof buf, n));
    CHECK_STR("marker at the tail", "...[TRUNCATED]\n", buf + 16);

    /* A wildly over-long record is the same case. */
    n = hype_snprintf(buf, sizeof buf, "%0200d", 7);
    CHECK_INT("way-overflow marked", 1, hype_format_mark_truncated(buf, sizeof buf, n));
    CHECK_INT("still NUL-terminated with newline", 1, buf[31] == '\0' && buf[30] == '\n');

    /* Degenerate buffers: too small for the marker, left alone. */
    {
        char tiny[8];
        n = hype_snprintf(tiny, sizeof tiny, "0123456789");
        CHECK_INT("tiny buffer not marked", 0,
                  hype_format_mark_truncated(tiny, sizeof tiny, n));
        CHECK_INT("tiny buffer NUL intact", 1, tiny[7] == '\0');
    }
    CHECK_INT("bufsz 0 tolerated", 0, hype_format_mark_truncated(buf, 0u, 100));
    CHECK_INT("negative written treated as fit", 0,
              hype_format_mark_truncated(buf, sizeof buf, -1));
    /* Exactly marker-sized buffer: the whole content becomes the marker. */
    {
        char m[16];
        n = hype_snprintf(m, sizeof m, "AAAAAAAAAAAAAAAAAAAA");
        CHECK_INT("marker-sized buffer marked", 1,
                  hype_format_mark_truncated(m, sizeof m, n));
        CHECK_STR("entirely the marker", "...[TRUNCATED]\n", m);
    }
}

int main(void) {
    test_mark_truncated();
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

    /* Width + zero-pad (M4-6d4). Before this was supported, "%02x" fell
     * through the parser's default case -- it emitted the literal chars AND
     * consumed no vararg, silently shifting every following argument (a
     * wrong-pointer-into-%s hazard). */
    fmt(buf, sizeof(buf), "%02x", 0x5U);
    CHECK_STR("%02x zero-pads a byte", "05", buf);

    fmt(buf, sizeof(buf), "%02x", 0xABU);
    CHECK_STR("%02x already wide is unpadded", "ab", buf);

    fmt(buf, sizeof(buf), "%08x", 0x1234U);
    CHECK_STR("%08x zero-pads to 8", "00001234", buf);

    fmt(buf, sizeof(buf), "%016llx", 0xDEADBEEFULL);
    CHECK_STR("%016llx zero-pads a 64-bit value", "00000000deadbeef", buf);

    fmt(buf, sizeof(buf), "%4d", 42);
    CHECK_STR("%4d space-pads", "  42", buf);

    fmt(buf, sizeof(buf), "%04d", -7);
    CHECK_STR("%04d sign precedes zero-pad", "-007", buf);

    fmt(buf, sizeof(buf), "%3u", 12345U);
    CHECK_STR("width narrower than value does not truncate", "12345", buf);

    /* Regression guard for the arg-shift bug: a %02x before a %s must not
     * steal the string's argument. */
    fmt(buf, sizeof(buf), "b=%02x s=%s", 0x9U, "ok");
    CHECK_STR("%02x does not shift the following %s", "b=09 s=ok", buf);

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
