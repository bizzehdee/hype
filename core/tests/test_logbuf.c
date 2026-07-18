#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../logbuf.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual)                                                    \
    do {                                                                                     \
        long long e_ = (long long)(expected), a_ = (long long)(actual);                      \
        if (e_ != a_) {                                                                      \
            printf("FAIL: %s (expected %lld, got %lld)\n", (desc), e_, a_);                  \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static void test_append_accumulates_in_order(void) {
    hype_logbuf_reset();
    hype_logbuf_append("hello ");
    hype_logbuf_append("world\n");
    CHECK_INT("len is sum of appends", 12, hype_logbuf_len());
    CHECK_INT("not truncated", 0, hype_logbuf_truncated());
    CHECK_INT("content matches", 0,
              memcmp(hype_logbuf_data(), "hello world\n", 12));
}

static void test_reset_clears(void) {
    hype_logbuf_append("stuff");
    hype_logbuf_reset();
    CHECK_INT("len 0 after reset", 0, hype_logbuf_len());
    CHECK_INT("not truncated after reset", 0, hype_logbuf_truncated());
}

static void test_null_append_is_noop(void) {
    hype_logbuf_reset();
    hype_logbuf_append(0);
    CHECK_INT("NULL append leaves len 0", 0, hype_logbuf_len());
}

static void test_truncates_at_capacity(void) {
    /* Fill exactly to capacity, then one more byte must be dropped and
     * latch the truncated flag; the retained content stays intact. */
    static char big[HYPE_LOGBUF_CAPACITY + 16];
    unsigned int i;
    hype_logbuf_reset();
    for (i = 0; i < sizeof(big) - 1; i++) {
        big[i] = 'A';
    }
    big[sizeof(big) - 1] = '\0';
    hype_logbuf_append(big);
    CHECK_INT("len capped at capacity", (long long)HYPE_LOGBUF_CAPACITY, hype_logbuf_len());
    CHECK_INT("truncated flag latched", 1, hype_logbuf_truncated());
    CHECK_INT("first retained byte intact", 'A', hype_logbuf_data()[0]);
    CHECK_INT("last retained byte intact", 'A', hype_logbuf_data()[HYPE_LOGBUF_CAPACITY - 1]);
    /* A further append stays dropped. */
    hype_logbuf_append("x");
    CHECK_INT("still capped after extra append", (long long)HYPE_LOGBUF_CAPACITY, hype_logbuf_len());
}

/* Lay down a header at the struct's field offsets (magic@0, version@8,
 * len@12, truncated@16, checksum@20, data@24) so find()/validate() can be
 * exercised against a synthetic region without a full 2MB struct. */
static void put_header(unsigned char *p, uint64_t magic, uint32_t ver, uint32_t len,
                       uint32_t trunc, uint32_t cksum, const char *data) {
    memcpy(p + 0, &magic, 8);
    memcpy(p + 8, &ver, 4);
    memcpy(p + 12, &len, 4);
    memcpy(p + 16, &trunc, 4);
    memcpy(p + 20, &cksum, 4);
    if (data && len) {
        memcpy(p + 24, data, len);
    }
}

static void test_reset_stamps_header(void) {
    hype_logbuf_reset();
    const hype_logbuf_t *h = hype_logbuf_get();
    CHECK_INT("reset stamps magic", (long long)HYPE_LOGBUF_MAGIC, (long long)h->magic);
    CHECK_INT("reset stamps version", (long long)HYPE_LOGBUF_VERSION, (long long)h->version);
    CHECK_INT("empty buffer validates", 1, hype_logbuf_validate(h));
}

static void test_live_buffer_validates_and_is_findable(void) {
    hype_logbuf_reset();
    hype_logbuf_append("abc");
    const hype_logbuf_t *h = hype_logbuf_get();
    CHECK_INT("checksum tracks appended bytes", (long long)('a' + 'b' + 'c'), (long long)h->checksum);
    CHECK_INT("populated buffer validates", 1, hype_logbuf_validate(h));
    /* The real RT-1b path: scan a region starting at the header, find it. */
    CHECK_INT("find locates the live buffer at offset 0", 1,
              hype_logbuf_find(h, sizeof(hype_logbuf_t), 8u) == h);
}

static void test_find_at_offset_and_rejections(void) {
    static unsigned char buf[512] __attribute__((aligned(8)));
    /* Valid header at a non-zero 8-aligned offset with 3 data bytes. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 3, 0, 'a' + 'b' + 'c', "abc");
    CHECK_INT("find locates a header at a nonzero offset", 1,
              hype_logbuf_find(buf, sizeof(buf), 8u) == (const hype_logbuf_t *)(buf + 64));

    /* Wrong magic -> not found. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, 0xDEADBEEFDEADBEEFULL, HYPE_LOGBUF_VERSION, 0, 0, 0, 0);
    CHECK_INT("wrong magic is not found", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    /* Right magic, wrong version -> validate fails, not found. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION + 1u, 0, 0, 0, 0);
    CHECK_INT("wrong version rejected", 0,
              hype_logbuf_validate((const hype_logbuf_t *)(buf + 64)));
    CHECK_INT("wrong version not found", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    /* Right magic/version, checksum doesn't match the data -> rejected. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 3, 0, 0 /*wrong*/, "abc");
    CHECK_INT("bad checksum rejected", 0,
              hype_logbuf_validate((const hype_logbuf_t *)(buf + 64)));
    CHECK_INT("bad checksum not found", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    /* A zeroed region has no magic anywhere. */
    memset(buf, 0, sizeof(buf));
    CHECK_INT("zeroed region yields nothing", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    /* Claimed len runs past the scanned region -> not found (no over-read). */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 100000u, 0, 0, 0);
    CHECK_INT("oversized len past region not found", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    CHECK_INT("NULL base not found", 1, hype_logbuf_find(0, sizeof(buf), 8u) == 0);
    CHECK_INT("NULL header does not validate", 0, hype_logbuf_validate(0));
}

/* RT-1d: the real RT-1b sweep steps by HYPE_LOGBUF_SCAN_ALIGN (4 KB) over
 * page-aligned RAM. Verify a header on a page boundary is found at that
 * stride, that a too-small stride is clamped (still finds an 8-aligned
 * header), and that the page stride correctly skips a header sitting off a
 * page boundary (the alignment contract the fast scan relies on). */
static void test_find_honours_stride(void) {
    static unsigned char buf[3 * 4096] __attribute__((aligned(4096)));

    /* Header exactly on the second page -> found by a 4 KB stride. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 4096, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 3, 0, 'a' + 'b' + 'c', "abc");
    CHECK_INT("page-strided find locates a page-aligned header", 1,
              hype_logbuf_find(buf, sizeof(buf), HYPE_LOGBUF_SCAN_ALIGN) ==
                  (const hype_logbuf_t *)(buf + 4096));

    /* Same header, but a stride below 8 is clamped up to 8 and still finds
     * it (4096 is a multiple of 8). */
    CHECK_INT("sub-8 stride is clamped and still finds it", 1,
              hype_logbuf_find(buf, sizeof(buf), 1u) == (const hype_logbuf_t *)(buf + 4096));

    /* Header off a page boundary -> a 4 KB stride steps past it (documents
     * the contract: the fast scan only works because the buffer is
     * page-aligned). An 8-byte stride still catches it. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 4096 + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 3, 0, 'a' + 'b' + 'c', "abc");
    CHECK_INT("page stride skips an off-page header", 1,
              hype_logbuf_find(buf, sizeof(buf), HYPE_LOGBUF_SCAN_ALIGN) == 0);
    CHECK_INT("8-byte stride still finds the off-page header", 1,
              hype_logbuf_find(buf, sizeof(buf), 8u) == (const hype_logbuf_t *)(buf + 4096 + 64));
}

int main(void) {
    test_append_accumulates_in_order();
    test_reset_clears();
    test_null_append_is_noop();
    test_truncates_at_capacity();
    test_reset_stamps_header();
    test_live_buffer_validates_and_is_findable();
    test_find_at_offset_and_rejections();
    test_find_honours_stride();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
