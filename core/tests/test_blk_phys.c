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

/* ---- #332: partition-scoped physical targets ---- */

static void test_scoped_offsets_every_read(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;

    reset_log();
    /* Partition at disk LBA 2048, 1000 sectors long. */
    hype_blk_phys_init_scoped(&p, &be, fake_read, fake_write, 0, 2048ull, 1000ull);

    /* The guest sees a disk of exactly the PARTITION's size -- that is what confines it. */
    CHECK_HEX("backend capacity is the partition length, not the disk", 1000ull, be.total_sectors);

    CHECK_HEX("read at scope LBA 0 ok", 0, hype_blk_backend_read(&be, 0, 8, g_buf));
    CHECK_HEX("one hw call", 1, g_ncalls);
    CHECK_HEX("...issued at the DISK-absolute LBA", 2048ull, g_calls[0].lba);

    reset_log();
    CHECK_HEX("read at scope LBA 10 ok", 0, hype_blk_backend_read(&be, 10, 8, g_buf));
    CHECK_HEX("offset applied", 2058ull, g_calls[0].lba);
}

static void test_scoped_offset_applied_once_not_per_chunk(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;

    reset_log();
    hype_blk_phys_init_scoped(&p, &be, fake_read, fake_write, 0, 4096ull,
                              3ull * HYPE_BLK_PHYS_MAX_CHUNK);

    /* A multi-chunk transfer: the base must be added ONCE, before the loop. Adding it per chunk
     * would leave chunk 1 at base*2 + MAX_CHUNK -- writing far outside the partition. */
    CHECK_HEX("multi-chunk read ok", 0,
              hype_blk_backend_read(&be, 0, 2u * HYPE_BLK_PHYS_MAX_CHUNK, g_buf));
    CHECK_HEX("two hw calls", 2, g_ncalls);
    CHECK_HEX("chunk0 at base", 4096ull, g_calls[0].lba);
    CHECK_HEX("chunk1 at base + MAX_CHUNK (base NOT re-added)",
              4096ull + HYPE_BLK_PHYS_MAX_CHUNK, g_calls[1].lba);
}

static void test_scoped_writes_are_offset_too(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;

    reset_log();
    hype_blk_phys_init_scoped(&p, &be, fake_read, fake_write, 0, 63ull, 100ull);
    CHECK_HEX("write ok", 0, hype_blk_backend_write(&be, 5, 4, g_buf));
    CHECK_HEX("one hw call", 1, g_ncalls);
    /* A write landing at the wrong LBA is the wipe-the-wrong-thing bug this ticket is about. */
    CHECK_HEX("write offset applied", 68ull, g_calls[0].lba);
}

static void test_scoped_guest_cannot_escape_the_partition(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;

    reset_log();
    hype_blk_phys_init_scoped(&p, &be, fake_read, fake_write, 0, 2048ull, 100ull);

    /* Refused by the dispatcher's EXISTING bounds check against total_sectors -- no new check was
     * added, which is the point of clamping capacity rather than filtering in the adapter. */
    CHECK_HEX("read starting past the partition end is refused", -1,
              hype_blk_backend_read(&be, 100, 1, g_buf));
    CHECK_HEX("read straddling the partition end is refused", -1,
              hype_blk_backend_read(&be, 98, 4, g_buf));
    CHECK_HEX("write past the end is refused", -1, hype_blk_backend_write(&be, 100, 1, g_buf));
    CHECK_HEX("no hw call was ever issued", 0, g_ncalls);

    /* The last sector inside the partition still works. */
    CHECK_HEX("the final in-range sector is allowed", 0, hype_blk_backend_read(&be, 99, 1, g_buf));
    CHECK_HEX("at the right absolute LBA", 2048ull + 99ull, g_calls[0].lba);
}

static void test_whole_disk_init_is_unchanged(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;

    reset_log();
    hype_blk_phys_init(&p, &be, fake_read, fake_write, 0, 5000ull);
    CHECK_HEX("base is 0", 0ull, p.base_lba);
    CHECK_HEX("capacity is the whole disk", 5000ull, be.total_sectors);
    CHECK_HEX("read ok", 0, hype_blk_backend_read(&be, 42, 1, g_buf));
    CHECK_HEX("no offset applied", 42ull, g_calls[0].lba);
}



/* --- #295: phys_writev batching --- */

/* Fake vectored hw: records (lba, nsegs, total) per call. */
#define VMAXCALLS 8
static struct { uint64_t lba; uint32_t nsegs; uint64_t total; } g_vcalls[VMAXCALLS];
static int g_nvcalls;
static int g_vfail_at = -1;

static void reset_vlog(void) { g_nvcalls = 0; g_vfail_at = -1; reset_log(); }

static int fake_writev(void *hw, uint64_t lba, const hype_blk_seg_t *segs, uint32_t nsegs) {
    int idx = g_nvcalls;
    uint32_t i;
    (void)hw;
    if (idx < VMAXCALLS) {
        uint64_t total = 0;
        for (i = 0; i < nsegs; i++) {
            total += segs[i].count;
        }
        g_vcalls[idx].lba = lba;
        g_vcalls[idx].nsegs = nsegs;
        g_vcalls[idx].total = total;
    }
    g_nvcalls++;
    return (idx == g_vfail_at) ? -1 : 0;
}

static void writev_backend(hype_blk_phys_t *p, hype_blk_backend_t *be, uint64_t base_lba,
                           uint64_t sectors, uint32_t max_segs, uint32_t max_sectors) {
    hype_blk_phys_init_scoped(p, be, fake_read, fake_write, 0, base_lba, sectors);
    hype_blk_phys_enable_writev(p, be, fake_writev, max_segs, max_sectors);
}

static void test_writev_one_batch(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;
    hype_blk_seg_t segs[3] = {{g_buf, 4u}, {g_buf, 2u}, {g_buf, 2u}};

    reset_vlog();
    writev_backend(&p, &be, 0u, 1000u, 8u, 64u);
    CHECK_HEX("writev slot armed", 1, be.writev != 0);
    CHECK_HEX("one batch ok", 0, hype_blk_backend_writev(&be, 100u, segs, 3u));
    CHECK_HEX("one vectored hw call", 1, g_nvcalls);
    CHECK_HEX("batch lba", 100u, g_vcalls[0].lba);
    CHECK_HEX("batch nsegs", 3u, g_vcalls[0].nsegs);
    CHECK_HEX("batch total sectors", 8u, g_vcalls[0].total);
    CHECK_HEX("no scalar hw calls", 0, g_ncalls);
}

static void test_writev_splits_on_max_segs(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;
    hype_blk_seg_t segs[5] = {{g_buf, 1u}, {g_buf, 1u}, {g_buf, 1u}, {g_buf, 1u}, {g_buf, 1u}};

    reset_vlog();
    writev_backend(&p, &be, 0u, 1000u, 2u, 64u); /* 2 segments per command */
    CHECK_HEX("5 segs ok", 0, hype_blk_backend_writev(&be, 10u, segs, 5u));
    /* 2+2 vectored, then a single leftover goes scalar (same command, less setup). */
    CHECK_HEX("two vectored calls", 2, g_nvcalls);
    CHECK_HEX("first batch at 10", 10u, g_vcalls[0].lba);
    CHECK_HEX("second batch at 12", 12u, g_vcalls[1].lba);
    CHECK_HEX("single leftover went scalar", 1, g_ncalls);
    CHECK_HEX("leftover lba", 14u, g_calls[0].lba);
}

static void test_writev_splits_on_max_sectors(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;
    hype_blk_seg_t segs[3] = {{g_buf, 6u}, {g_buf, 6u}, {g_buf, 6u}};

    reset_vlog();
    writev_backend(&p, &be, 0u, 1000u, 8u, 12u); /* 12 sectors per command */
    CHECK_HEX("18 sectors ok", 0, hype_blk_backend_writev(&be, 0u, segs, 3u));
    CHECK_HEX("first two segs merged", 2u, g_vcalls[0].nsegs);
    CHECK_HEX("first batch total", 12u, g_vcalls[0].total);
    CHECK_HEX("third seg went scalar (single-segment batch)", 1, g_ncalls);
    CHECK_HEX("third seg lba continues", 12u, g_calls[0].lba);
    CHECK_HEX("one vectored call total", 1, g_nvcalls);
}

static void test_writev_oversized_segment_takes_chunked_scalar_path(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;
    /* max_sectors 4: a 10-sector segment fits NO batch; MAX_CHUNK still bounds each hw write. */
    hype_blk_seg_t segs[2] = {{g_buf, 10u}, {g_buf, 2u}};

    reset_vlog();
    writev_backend(&p, &be, 0u, 1000u, 8u, 4u);
    CHECK_HEX("list ok", 0, hype_blk_backend_writev(&be, 20u, segs, 2u));
    /* Segment 0: one scalar hw call (10 <= MAX_CHUNK). Segment 1: 2 <= 4 -> single-seg scalar. */
    CHECK_HEX("two scalar calls", 2, g_ncalls);
    CHECK_HEX("oversized seg at 20", 20u, g_calls[0].lba);
    CHECK_HEX("oversized seg count intact", 10u, g_calls[0].count);
    CHECK_HEX("next seg resumes at 30", 30u, g_calls[1].lba);
    CHECK_HEX("no vectored call", 0, g_nvcalls);
}

static void test_writev_base_lba_added_once(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;
    hype_blk_seg_t segs[4] = {{g_buf, 1u}, {g_buf, 1u}, {g_buf, 1u}, {g_buf, 1u}};

    reset_vlog();
    writev_backend(&p, &be, 500u, 100u, 2u, 64u); /* partition scope at disk lba 500 */
    CHECK_HEX("scoped ok", 0, hype_blk_backend_writev(&be, 8u, segs, 4u));
    CHECK_HEX("first batch disk-absolute", 508u, g_vcalls[0].lba);
    /* #332's trap: the second batch must NOT add base_lba again. */
    CHECK_HEX("second batch adds the base ONCE", 510u, g_vcalls[1].lba);
}

static void test_writev_hw_error_propagates(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;
    hype_blk_seg_t segs[4] = {{g_buf, 1u}, {g_buf, 1u}, {g_buf, 1u}, {g_buf, 1u}};

    reset_vlog();
    writev_backend(&p, &be, 0u, 1000u, 2u, 64u);
    g_vfail_at = 1; /* second vectored command fails */
    CHECK_HEX("hw failure reported", -1, hype_blk_backend_writev(&be, 0u, segs, 4u));
    CHECK_HEX("stopped after the failing batch", 2, g_nvcalls);
}

static void test_enable_writev_refuses_read_only_and_null(void) {
    hype_blk_phys_t p;
    hype_blk_backend_t be;

    /* read-only backend: enable is a no-op, writev stays NULL. */
    hype_blk_phys_init(&p, &be, fake_read, 0, 0, 100u);
    hype_blk_phys_enable_writev(&p, &be, fake_writev, 8u, 64u);
    CHECK_HEX("read-only backend not armed", 1, be.writev == 0);

    /* NULL fn / zero caps: also no-ops. */
    hype_blk_phys_init(&p, &be, fake_read, fake_write, 0, 100u);
    hype_blk_phys_enable_writev(&p, &be, 0, 8u, 64u);
    CHECK_HEX("NULL fn not armed", 1, be.writev == 0);
    hype_blk_phys_enable_writev(&p, &be, fake_writev, 0u, 64u);
    CHECK_HEX("zero max_segs not armed", 1, be.writev == 0);
    hype_blk_phys_enable_writev(&p, &be, fake_writev, 8u, 0u);
    CHECK_HEX("zero max_sectors not armed", 1, be.writev == 0);
}

int main(void) {
    test_single_chunk();
    test_exact_chunk_boundary();
    test_three_chunks();
    test_write_chunking();
    test_read_only_backend();
    test_error_propagation();
    test_bounds_gate();
    test_scoped_offsets_every_read();
    test_scoped_offset_applied_once_not_per_chunk();
    test_scoped_writes_are_offset_too();
    test_scoped_guest_cannot_escape_the_partition();
    test_whole_disk_init_is_unchanged();

    test_writev_one_batch();
    test_writev_splits_on_max_segs();
    test_writev_splits_on_max_sectors();
    test_writev_oversized_segment_takes_chunked_scalar_path();
    test_writev_base_lba_added_once();
    test_writev_hw_error_propagates();
    test_enable_writev_refuses_read_only_and_null();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
