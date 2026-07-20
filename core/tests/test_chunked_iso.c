#include <stdio.h>
#include <stdlib.h>
#include "../chunked_iso.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), \
                   (long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Build a 3-chunk ISO of `chunk_bytes` each, total = 3*chunk_bytes, filled so
 * logical byte i == (uint8_t)i. Chunks are separate malloc'd buffers (proving
 * the read works across non-contiguous backing). */
#define CB 16u
static uint8_t *g_bufs[3];
static hype_chunked_iso_t g_iso;

static void setup(void) {
    unsigned c, k;
    for (c = 0; c < 3; c++) {
        g_bufs[c] = malloc(CB);
        for (k = 0; k < CB; k++) {
            g_bufs[c][k] = (uint8_t)(c * CB + k); /* logical byte value */
        }
        g_iso.chunk_base[c] = (uint64_t)(uintptr_t)g_bufs[c];
    }
    g_iso.chunk_bytes = CB;
    g_iso.n_chunks = 3;
    g_iso.total_bytes = 3u * CB;
}

static int verify(uint64_t off, uint64_t len) {
    uint8_t out[64];
    uint64_t i;
    if (hype_chunked_iso_read(&g_iso, off, out, len) != 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (out[i] != (uint8_t)(off + i)) {
            return 0;
        }
    }
    return 1;
}

static void test_within_single_chunk(void) {
    CHECK_INT("read wholly inside chunk 0", 1, verify(2, 8));
    CHECK_INT("read wholly inside chunk 1", 1, verify(CB + 1, 5));
}

static void test_spans_one_boundary(void) {
    /* straddles the chunk0/chunk1 boundary at offset 16 */
    CHECK_INT("read spanning chunk0->chunk1", 1, verify(CB - 4, 8));
}

static void test_spans_multiple_boundaries(void) {
    /* whole ISO in one read -> crosses both boundaries */
    CHECK_INT("read the entire ISO across all 3 chunks", 1, verify(0, 3u * CB));
}

static void test_exact_boundary_start(void) {
    CHECK_INT("read starting exactly at a chunk boundary", 1, verify(CB, CB));
    CHECK_INT("read starting exactly at last chunk", 1, verify(2u * CB, CB));
}

static void test_out_of_bounds_rejected(void) {
    uint8_t out[8];
    CHECK_INT("read past end rejected", -1, hype_chunked_iso_read(&g_iso, 3u * CB - 2u, out, 8));
    CHECK_INT("offset past end rejected", -1, hype_chunked_iso_read(&g_iso, 3u * CB + 1u, out, 1));
    /* overflow-style: huge len */
    CHECK_INT("overflow len rejected", -1, hype_chunked_iso_read(&g_iso, 8, out, 0xFFFFFFFFFFFFFFF0ULL));
}

static void test_malformed_layout_rejected(void) {
    hype_chunked_iso_t bad = g_iso;
    uint8_t out[4];
    bad.chunk_bytes = 0;
    CHECK_INT("chunk_bytes==0 rejected", -1, hype_chunked_iso_read(&bad, 0, out, 4));
    CHECK_INT("NULL iso rejected", -1, hype_chunked_iso_read(0, 0, out, 4));
}

static void test_zero_length_ok(void) {
    uint8_t out[1] = {0xAA};
    CHECK_INT("zero-length read succeeds", 0, hype_chunked_iso_read(&g_iso, 4, out, 0));
    CHECK_INT("zero-length read touches nothing", 0xAA, out[0]);
}

int main(void) {
    setup();
    test_within_single_chunk();
    test_spans_one_boundary();
    test_spans_multiple_boundaries();
    test_exact_boundary_start();
    test_out_of_bounds_rejected();
    test_malformed_layout_rejected();
    test_zero_length_ok();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
