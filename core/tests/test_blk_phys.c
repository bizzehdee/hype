#include <stdio.h>
#include "../blk_phys.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Fake per-chunk hw: records each (lba, count) call; optionally fails at one. */
#define MAXCALLS 8
static struct { uint64_t lba; uint32_t count; } g_calls[MAXCALLS];
static int g_ncalls;
static int g_fail_at = -1; /* call index to fail at, or -1 for never */

static void reset_log(void) { g_ncalls = 0; g_fail_at = -1; }

static int record(uint64_t lba, uint32_t count) {
    int idx = g_ncalls;
    if (idx < MAXCALLS) {
        g_calls[idx].lba = lba;
        g_calls[idx].count = count;
    }
    g_ncalls++;
    return (idx == g_fail_at) ? -1 : 0;
}
static int fake_read(void *hw, uint64_t lba, uint32_t count, void *buf) {
    (void)hw; (void)buf; return record(lba, count);
}
static int fake_write(void *hw, uint64_t lba, uint32_t count, const void *buf) {
    (void)hw; (void)buf; return record(lba, count);
}

/* Big enough for the largest transfer under test (3 chunks: 8192+8192+1). */
static uint8_t g_buf[16385 * 512];

static void test_single_chunk(void) {
    hype_blk_phys_t p; hype_blk_backend_t be;
    reset_log();
    hype_blk_phys_init(&p, &be, fake_read, fake_write, (void *)0, 100000);
    CHECK_HEX("small read ok", 0, hype_blk_backend_read(&be, 10, 8, g_buf));
    CHECK_HEX("one hw call", 1, g_ncalls);
    CHECK_HEX("chunk lba", 10ull, g_calls[0].lba);
    CHECK_HEX("chunk count", 8u, g_calls[0].count);
}

static void test_exact_chunk_boundary(void) {
    hype_blk_phys_t p; hype_blk_backend_t be;
    reset_log();
    hype_blk_phys_init(&p, &be, fake_read, fake_write, (void *)0, 100000);
    CHECK_HEX("read exactly MAX_CHUNK ok", 0, hype_blk_backend_read(&be, 0, HYPE_BLK_PHYS_MAX_CHUNK, g_buf));
    CHECK_HEX("still one call", 1, g_ncalls);
    CHECK_HEX("count = MAX_CHUNK", HYPE_BLK_PHYS_MAX_CHUNK, g_calls[0].count);
}

static void test_three_chunks(void) {
    hype_blk_phys_t p; hype_blk_backend_t be;
    reset_log();
    hype_blk_phys_init(&p, &be, fake_read, fake_write, (void *)0, 100000);
    /* 8192 + 8192 + 1 */
    CHECK_HEX("large read ok", 0, hype_blk_backend_read(&be, 0, 2u * HYPE_BLK_PHYS_MAX_CHUNK + 1u, g_buf));
    CHECK_HEX("three hw calls", 3, g_ncalls);
    CHECK_HEX("chunk0 lba", 0ull, g_calls[0].lba);
    CHECK_HEX("chunk0 count", HYPE_BLK_PHYS_MAX_CHUNK, g_calls[0].count);
    CHECK_HEX("chunk1 lba", (uint64_t)HYPE_BLK_PHYS_MAX_CHUNK, g_calls[1].lba);
    CHECK_HEX("chunk1 count", HYPE_BLK_PHYS_MAX_CHUNK, g_calls[1].count);
    CHECK_HEX("chunk2 lba", (uint64_t)(2u * HYPE_BLK_PHYS_MAX_CHUNK), g_calls[2].lba);
    CHECK_HEX("chunk2 count", 1u, g_calls[2].count);
}

static void test_write_chunking(void) {
    hype_blk_phys_t p; hype_blk_backend_t be;
    reset_log();
    hype_blk_phys_init(&p, &be, fake_read, fake_write, (void *)0, 100000);
    CHECK_HEX("large write ok", 0, hype_blk_backend_write(&be, 5, HYPE_BLK_PHYS_MAX_CHUNK + 3u, g_buf));
    CHECK_HEX("two hw calls", 2, g_ncalls);
    CHECK_HEX("wchunk0 lba", 5ull, g_calls[0].lba);
    CHECK_HEX("wchunk0 count", HYPE_BLK_PHYS_MAX_CHUNK, g_calls[0].count);
    CHECK_HEX("wchunk1 lba", 5ull + HYPE_BLK_PHYS_MAX_CHUNK, g_calls[1].lba);
    CHECK_HEX("wchunk1 count", 3u, g_calls[1].count);
}

static void test_read_only_backend(void) {
    hype_blk_phys_t p; hype_blk_backend_t be;
    reset_log();
    hype_blk_phys_init(&p, &be, fake_read, (hype_blk_phys_write_fn)0, (void *)0, 100000);
    CHECK_HEX("read-only: write pointer NULL", 0, (be.write == (int (*)(void *, uint64_t, uint32_t, const void *))0) ? 0 : 1);
    CHECK_HEX("read-only: write rejected", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_write(&be, 0, 1, g_buf));
    CHECK_HEX("read-only: no hw call made", 0, g_ncalls);
    CHECK_HEX("read-only: read still works", 0, hype_blk_backend_read(&be, 0, 1, g_buf));
}

static void test_error_propagation(void) {
    hype_blk_phys_t p; hype_blk_backend_t be;
    reset_log();
    hype_blk_phys_init(&p, &be, fake_read, fake_write, (void *)0, 100000);
    g_fail_at = 1; /* fail the second chunk */
    CHECK_HEX("read fails when a chunk fails", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_read(&be, 0, 2u * HYPE_BLK_PHYS_MAX_CHUNK, g_buf));
    CHECK_HEX("stopped after the failing chunk", 2, g_ncalls);
}

static void test_bounds_gate(void) {
    hype_blk_phys_t p; hype_blk_backend_t be;
    reset_log();
    hype_blk_phys_init(&p, &be, fake_read, fake_write, (void *)0, 100); /* tiny disk */
    CHECK_HEX("oob transfer rejected by dispatcher", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_read(&be, 0, 8192, g_buf));
    CHECK_HEX("no hw call on oob", 0, g_ncalls);
}

int main(void) {
    test_single_chunk();
    test_exact_chunk_boundary();
    test_three_chunks();
    test_write_chunking();
    test_read_only_backend();
    test_error_propagation();
    test_bounds_gate();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
