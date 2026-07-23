#include <stdio.h>
#include <string.h>
#include "../phys_confirm.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), \
                   (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_TRUE(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

#define CHECK_HAS(desc, hay, needle) \
    do { if (strstr((hay), (needle)) == NULL) { \
        printf("FAIL: %s: '%s' missing '%s'\n", (desc), (hay), (needle)); \
        failures++; } } while (0)

#define CHECK_NOT(desc, hay, needle) \
    do { if (strstr((hay), (needle)) != NULL) { \
        printf("FAIL: %s: '%s' unexpectedly has '%s'\n", (desc), (hay), (needle)); \
        failures++; } } while (0)

static void test_request_prompt_pending(void) {
    hype_phys_confirm_t c;
    char buf[256];
    hype_phys_confirm_reset(&c);
    hype_phys_confirm_request(&c, "winvm", "Samsung SSD 990", "S4EWNF0",
                              500107862016ULL); /* 500 GB decimal = 465.7 GiB */
    CHECK_INT("state pending", HYPE_PHYS_CONFIRM_PENDING, c.state);
    CHECK_INT("not accepted yet", 0, hype_phys_confirm_is_accepted(&c));

    hype_phys_confirm_prompt(&c, buf, sizeof(buf));
    CHECK_HAS("prompt names vm", buf, "winvm");
    CHECK_HAS("prompt names model", buf, "Samsung SSD 990");
    CHECK_HAS("prompt names serial", buf, "S4EWNF0");
    CHECK_HAS("prompt warns destroy", buf, "DESTROYED");
    CHECK_HAS("prompt shows GiB size", buf, "465.7 GiB");
    CHECK_HAS("prompt gives confirm cmd", buf, "confirm S4EWNF0");
}

static void test_submit_mismatch_then_match(void) {
    hype_phys_confirm_t c;
    hype_phys_confirm_reset(&c);
    hype_phys_confirm_request(&c, "vm", "Model", "GOODSN", 1ULL << 30);

    CHECK_INT("wrong serial -> mismatch", HYPE_PHYS_CONFIRM_SUBMIT_MISMATCH,
              hype_phys_confirm_submit(&c, "BADSN"));
    CHECK_INT("still pending after mismatch", HYPE_PHYS_CONFIRM_PENDING, c.state);
    CHECK_INT("attempts counted", 1, c.attempts);
    CHECK_INT("not accepted after mismatch", 0, hype_phys_confirm_is_accepted(&c));

    CHECK_INT("empty typed -> mismatch", HYPE_PHYS_CONFIRM_SUBMIT_MISMATCH,
              hype_phys_confirm_submit(&c, ""));
    CHECK_INT("attempts 2", 2, c.attempts);

    CHECK_INT("null typed -> mismatch", HYPE_PHYS_CONFIRM_SUBMIT_MISMATCH,
              hype_phys_confirm_submit(&c, NULL));
    CHECK_INT("attempts 3", 3, c.attempts);

    CHECK_INT("right serial -> accepted", HYPE_PHYS_CONFIRM_SUBMIT_ACCEPTED,
              hype_phys_confirm_submit(&c, "GOODSN"));
    CHECK_INT("state accepted", HYPE_PHYS_CONFIRM_ACCEPTED, c.state);
    CHECK_INT("is_accepted true", 1, hype_phys_confirm_is_accepted(&c));
}

static void test_submit_trims_whitespace(void) {
    hype_phys_confirm_t c;
    hype_phys_confirm_reset(&c);
    hype_phys_confirm_request(&c, "vm", "M", "ABC123", 1ULL << 30);
    CHECK_INT("leading/trailing ws trimmed then matches",
              HYPE_PHYS_CONFIRM_SUBMIT_ACCEPTED,
              hype_phys_confirm_submit(&c, "   ABC123  "));
}

static void test_case_sensitive(void) {
    hype_phys_confirm_t c;
    hype_phys_confirm_reset(&c);
    hype_phys_confirm_request(&c, "vm", "M", "AbC123", 1ULL << 30);
    CHECK_INT("case mismatch rejected", HYPE_PHYS_CONFIRM_SUBMIT_MISMATCH,
              hype_phys_confirm_submit(&c, "abc123"));
}

static void test_submit_when_not_pending(void) {
    hype_phys_confirm_t c;
    hype_phys_confirm_reset(&c);
    /* idle */
    CHECK_INT("submit while idle -> none pending",
              HYPE_PHYS_CONFIRM_SUBMIT_NONE_PENDING,
              hype_phys_confirm_submit(&c, "whatever"));
    /* accept, then submitting again is a no-op none-pending */
    hype_phys_confirm_request(&c, "vm", "M", "SN", 1ULL << 30);
    hype_phys_confirm_submit(&c, "SN");
    CHECK_INT("submit after accepted -> none pending",
              HYPE_PHYS_CONFIRM_SUBMIT_NONE_PENDING,
              hype_phys_confirm_submit(&c, "SN"));
    CHECK_INT("still accepted", 1, hype_phys_confirm_is_accepted(&c));
}

static void test_reset_and_rerequest(void) {
    hype_phys_confirm_t c;
    char buf[128];
    hype_phys_confirm_reset(&c);
    hype_phys_confirm_request(&c, "vm", "M", "SN", 1ULL << 30);
    hype_phys_confirm_submit(&c, "SN");
    CHECK_INT("accepted", 1, hype_phys_confirm_is_accepted(&c));

    hype_phys_confirm_reset(&c);
    CHECK_INT("idle after reset", HYPE_PHYS_CONFIRM_IDLE, c.state);
    CHECK_INT("not accepted after reset", 0, hype_phys_confirm_is_accepted(&c));
    hype_phys_confirm_prompt(&c, buf, sizeof(buf));
    CHECK_INT("idle prompt empty", 0, (int)strlen(buf));

    /* a new request must start unconfirmed even after a prior acceptance */
    hype_phys_confirm_request(&c, "vm2", "M2", "SN2", 1ULL << 30);
    CHECK_INT("re-request pending", HYPE_PHYS_CONFIRM_PENDING, c.state);
    CHECK_INT("re-request unconfirmed", 0, hype_phys_confirm_is_accepted(&c));
    CHECK_INT("attempts reset", 0, (int)c.attempts);
}

static void test_accepted_prompt(void) {
    hype_phys_confirm_t c;
    char buf[128];
    hype_phys_confirm_reset(&c);
    hype_phys_confirm_request(&c, "dbvm", "M", "XSER", 1ULL << 30);
    hype_phys_confirm_submit(&c, "XSER");
    hype_phys_confirm_prompt(&c, buf, sizeof(buf));
    CHECK_HAS("accepted prompt says confirmed", buf, "CONFIRMED");
    CHECK_HAS("accepted prompt names vm", buf, "dbvm");
    CHECK_HAS("accepted prompt names serial", buf, "XSER");
    CHECK_NOT("accepted prompt not a warning", buf, "DESTROYED");
}

static void test_size_formatting(void) {
    hype_phys_confirm_t c;
    char buf[256];
    hype_phys_confirm_reset(&c);

    hype_phys_confirm_request(&c, "v", "M", "S", 512ULL); /* below 1 KiB */
    hype_phys_confirm_prompt(&c, buf, sizeof(buf));
    CHECK_HAS("bytes shown for <1KiB", buf, "512 B");

    hype_phys_confirm_request(&c, "v", "M", "S", 2048ULL); /* 2.0 KiB */
    hype_phys_confirm_prompt(&c, buf, sizeof(buf));
    CHECK_HAS("KiB shown", buf, "2.0 KiB");

    hype_phys_confirm_request(&c, "v", "M", "S", 3ULL << 30); /* 3.0 GiB */
    hype_phys_confirm_prompt(&c, buf, sizeof(buf));
    CHECK_HAS("GiB shown", buf, "3.0 GiB");

    hype_phys_confirm_request(&c, "v", "M", "S", 1ULL << 40); /* 1.0 TiB */
    hype_phys_confirm_prompt(&c, buf, sizeof(buf));
    CHECK_HAS("TiB shown", buf, "1.0 TiB");

    /* exercises the unit-cap: PiB is the top unit, larger still shows PiB */
    hype_phys_confirm_request(&c, "v", "M", "S", 5ULL << 50); /* 5.0 PiB */
    hype_phys_confirm_prompt(&c, buf, sizeof(buf));
    CHECK_HAS("PiB shown (top unit)", buf, "5.0 PiB");
    hype_phys_confirm_request(&c, "v", "M", "S", 4096ULL << 50); /* > PiB range */
    hype_phys_confirm_prompt(&c, buf, sizeof(buf));
    CHECK_HAS("beyond-PiB still uses PiB", buf, "PiB");

    /* zero-capacity buffer must be a safe no-op */
    CHECK_TRUE("n=0 prompt safe", hype_phys_confirm_prompt(&c, buf, 0) == buf);
}

static void test_prompt_truncation_and_nulls(void) {
    hype_phys_confirm_t c;
    char small[8];
    hype_phys_confirm_reset(&c);
    hype_phys_confirm_request(&c, "vm", "ModelName", "SERIALX", 1ULL << 30);
    /* must NUL-terminate within the tiny buffer, never overrun */
    hype_phys_confirm_prompt(&c, small, sizeof(small));
    CHECK_TRUE("truncated prompt is NUL-terminated", small[7] == '\0');

    /* NULL fields treated as empty, no crash */
    hype_phys_confirm_request(&c, NULL, NULL, NULL, 0);
    {
        char buf[128];
        hype_phys_confirm_prompt(&c, buf, sizeof(buf));
        CHECK_TRUE("prompt with null fields still produced", strlen(buf) > 0);
    }
    /* NULL context is safe on every entry point */
    CHECK_INT("null ctx not accepted", 0, hype_phys_confirm_is_accepted(NULL));
    CHECK_INT("null ctx submit none-pending", HYPE_PHYS_CONFIRM_SUBMIT_NONE_PENDING,
              hype_phys_confirm_submit(NULL, "x"));
    hype_phys_confirm_request(NULL, "a", "b", "c", 1); /* no-op, no crash */
    hype_phys_confirm_reset(NULL);                     /* no-op, no crash */
    {
        char buf[8];
        CHECK_TRUE("null-ctx prompt returns buf", hype_phys_confirm_prompt(NULL, buf, sizeof(buf)) == buf);
    }
}

int main(void) {
    test_request_prompt_pending();
    test_submit_mismatch_then_match();
    test_submit_trims_whitespace();
    test_case_sensitive();
    test_submit_when_not_pending();
    test_reset_and_rerequest();
    test_accepted_prompt();
    test_size_formatting();
    test_prompt_truncation_and_nulls();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
