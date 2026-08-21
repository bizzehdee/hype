#include <stdio.h>
#include <string.h>
#include "../blk_backend.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_range_in_bounds(void) {
    CHECK_HEX("in bounds", 1, hype_blk_range_in_bounds(100, 0, 10));
    CHECK_HEX("touches last sector exactly", 1, hype_blk_range_in_bounds(100, 90, 10));
    CHECK_HEX("one sector past end rejected", 0, hype_blk_range_in_bounds(100, 91, 10));
    CHECK_HEX("lba at capacity rejected", 0, hype_blk_range_in_bounds(100, 100, 1));
    CHECK_HEX("count 0 rejected", 0, hype_blk_range_in_bounds(100, 0, 0));
    CHECK_HEX("lba+count overflow rejected", 0,
              hype_blk_range_in_bounds(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 8));
    CHECK_HEX("single sector at 0", 1, hype_blk_range_in_bounds(1, 0, 1));
}

/* A 4-sector file image with each sector filled with its own index byte. */
static void fill_pattern(uint8_t *buf, unsigned sectors) {
    unsigned s;
    for (s = 0; s < sectors; s++) {
        memset(buf + s * HYPE_BLK_SECTOR_SIZE, (int)(0x10 + s), HYPE_BLK_SECTOR_SIZE);
    }
}

static void test_file_read(void) {
    uint8_t img[4 * HYPE_BLK_SECTOR_SIZE];
    uint8_t out[HYPE_BLK_SECTOR_SIZE];
    hype_blk_file_t f;
    hype_blk_backend_t be;

    fill_pattern(img, 4);
    hype_blk_file_init(&f, &be, img, sizeof(img));
    CHECK_HEX("capacity = 4 sectors", 4ull, be.total_sectors);

    CHECK_HEX("read sector 2 ok", 0, hype_blk_backend_read(&be, 2, 1, out));
    CHECK_HEX("sector 2 byte 0", 0x12u, out[0]);
    CHECK_HEX("sector 2 byte 511", 0x12u, out[HYPE_BLK_SECTOR_SIZE - 1u]);
}

static void test_file_write_roundtrip(void) {
    uint8_t img[4 * HYPE_BLK_SECTOR_SIZE];
    uint8_t in[HYPE_BLK_SECTOR_SIZE];
    hype_blk_file_t f;
    hype_blk_backend_t be;

    fill_pattern(img, 4);
    hype_blk_file_init(&f, &be, img, sizeof(img));

    memset(in, 0xAB, sizeof(in));
    CHECK_HEX("write sector 1 ok", 0, hype_blk_backend_write(&be, 1, 1, in));
    /* sector 1 changed... */
    CHECK_HEX("sector 1 now 0xAB", 0xABu, img[1 * HYPE_BLK_SECTOR_SIZE]);
    /* ...neighbours untouched. */
    CHECK_HEX("sector 0 untouched", 0x10u, img[0]);
    CHECK_HEX("sector 2 untouched", 0x12u, img[2 * HYPE_BLK_SECTOR_SIZE]);
}

static void test_bounds_gate_rejects_oob(void) {
    uint8_t img[4 * HYPE_BLK_SECTOR_SIZE];
    uint8_t buf[HYPE_BLK_SECTOR_SIZE];
    hype_blk_file_t f;
    hype_blk_backend_t be;

    fill_pattern(img, 4);
    hype_blk_file_init(&f, &be, img, sizeof(img));

    CHECK_HEX("oob read (sector 4 of 4) rejected", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_read(&be, 4, 1, buf));
    CHECK_HEX("straddling read rejected", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_read(&be, 3, 2, buf));
    CHECK_HEX("oob write rejected", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_write(&be, 4, 1, buf));
}

static void test_dispatch_null_guards(void) {
    uint8_t buf[HYPE_BLK_SECTOR_SIZE];
    hype_blk_backend_t be;

    CHECK_HEX("NULL backend read rejected", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_read((const hype_blk_backend_t *)0, 0, 1, buf));
    CHECK_HEX("NULL backend write rejected", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_write((const hype_blk_backend_t *)0, 0, 1, buf));

    /* Read-only backend: write pointer NULL => write rejected, read still works. */
    be.read = (int (*)(void *, uint64_t, uint32_t, void *))0;
    be.write = (int (*)(void *, uint64_t, uint32_t, const void *))0;
    be.ctx = (void *)0;
    be.total_sectors = 8;
    CHECK_HEX("NULL read fn rejected", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_read(&be, 0, 1, buf));
    CHECK_HEX("NULL write fn (read-only) rejected", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_write(&be, 0, 1, buf));
}

static void test_partial_trailing_sector_unreachable(void) {
    uint8_t img[3 * HYPE_BLK_SECTOR_SIZE];
    uint8_t buf[HYPE_BLK_SECTOR_SIZE];
    hype_blk_file_t f;
    hype_blk_backend_t be;

    /* 2 sectors + 100 trailing bytes: total_sectors floors to 2, sector 2 gone. */
    hype_blk_file_init(&f, &be, img, 2ull * HYPE_BLK_SECTOR_SIZE + 100ull);
    CHECK_HEX("capacity floors to 2 sectors", 2ull, be.total_sectors);
    CHECK_HEX("partial trailing sector unreachable", (unsigned long long)(-1),
              (unsigned long long)hype_blk_backend_read(&be, 2, 1, buf));
}

/* --- #265 write-side instrumentation --- */

static uint64_t fake_now_value;
static uint64_t fake_now(void) { return fake_now_value; }

static void test_wstats_buckets(void) {
    /* Bucket 0 is the pathological case the histogram exists to expose: one AHCI
     * round trip per single sector. Boundaries are checked on both sides. */
    struct { uint32_t count; unsigned want; } cases[] = {
        {0u, 0u}, {1u, 0u}, {2u, 1u}, {7u, 1u}, {8u, 2u}, {31u, 2u},
        {32u, 3u}, {127u, 3u}, {128u, 4u}, {1023u, 4u}, {1024u, 5u}, {8192u, 5u},
    };
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned got = hype_blk_wstats_bucket(cases[i].count);
        if (got != cases[i].want) {
            printf("FAIL: bucket(%u) = %u, want %u\n", cases[i].count, got, cases[i].want);
            failures++;
        }
        if (got >= HYPE_BLK_WSTATS_BUCKETS) {
            printf("FAIL: bucket(%u) = %u is out of range\n", cases[i].count, got);
            failures++;
        }
    }
}

static void test_wstats_record_accumulates(void) {
    hype_blk_wstats_t s;
    hype_blk_wstats_set_clock(0);
    hype_blk_wstats_reset(&s);
    hype_blk_wstats_record(&s, 1u);
    hype_blk_wstats_record(&s, 1u);
    hype_blk_wstats_record(&s, 64u);
    if (s.writes != 3u || s.sectors != 66u) {
        printf("FAIL: expected 3 writes / 66 sectors, got %llu / %llu\n",
               (unsigned long long)s.writes, (unsigned long long)s.sectors);
        failures++;
    }
    if (s.max_count != 64u) {
        printf("FAIL: max_count should track the largest request, got %u\n", s.max_count);
        failures++;
    }
    if (s.hist[0] != 2u || s.hist[3] != 1u) {
        printf("FAIL: histogram should be 2 in bucket 0 and 1 in bucket 3, got %u/%u\n",
               s.hist[0], s.hist[3]);
        failures++;
    }
    hype_blk_wstats_record(0, 1u); /* must not crash */
    hype_blk_wstats_reset(0);
}

static void test_wstats_first_tsc_is_the_first_write(void) {
    hype_blk_wstats_t s;
    hype_blk_wstats_reset(&s);
    hype_blk_wstats_set_clock(fake_now);
    fake_now_value = 1000u;
    hype_blk_wstats_record(&s, 8u);
    fake_now_value = 9999u;
    hype_blk_wstats_record(&s, 8u);
    /* Stamped once, at the FIRST write -- measuring from boot instead would dilute
     * the write phase's throughput with everything that preceded it. */
    if (s.first_tsc != 1000u) {
        printf("FAIL: first_tsc should be the first write's clock, got %llu\n",
               (unsigned long long)s.first_tsc);
        failures++;
    }
    hype_blk_wstats_set_clock(0);
    hype_blk_wstats_reset(&s);
    hype_blk_wstats_record(&s, 8u);
    if (s.first_tsc != 0u) {
        printf("FAIL: with no clock installed first_tsc must stay 0\n");
        failures++;
    }
}

static void test_wstats_kbps(void) {
    hype_blk_wstats_t s;
    hype_blk_wstats_set_clock(0);
    hype_blk_wstats_reset(&s);
    /* 2048 sectors = 1024 KB. Over 1000 ms that is 1024 KB/s. */
    hype_blk_wstats_record(&s, 2048u);
    if (hype_blk_wstats_kbps(&s, 1000u) != 1024u) {
        printf("FAIL: 2048 sectors in 1000ms should be 1024 KB/s, got %llu\n",
               (unsigned long long)hype_blk_wstats_kbps(&s, 1000u));
        failures++;
    }
    /* Must not divide by zero before any time has elapsed. */
    if (hype_blk_wstats_kbps(&s, 0u) != 0u) {
        printf("FAIL: zero elapsed must yield 0, not a division by zero\n");
        failures++;
    }
    if (hype_blk_wstats_kbps(0, 1000u) != 0u) {
        printf("FAIL: NULL stats must yield 0\n");
        failures++;
    }
}

static void test_backend_write_records_only_on_success(void) {
    /* A failed write must not inflate the counters -- otherwise a failing disk would
     * read as healthy throughput. */
    hype_blk_wstats_t *g = hype_blk_wstats();
    uint64_t before = g->writes;
    hype_blk_backend_t ro;
    ro.read = 0;
    ro.write = 0; /* read-only backend: the write is rejected */
    ro.ctx = 0;
    ro.total_sectors = 16u;
    if (hype_blk_backend_write(&ro, 0u, 1u, "x") == 0) {
        printf("FAIL: a write through a read-only backend must be rejected\n");
        failures++;
    }
    if (g->writes != before) {
        printf("FAIL: a rejected write must not be counted\n");
        failures++;
    }
}


/* --- #295: the vectored write dispatcher --- */

/* A recording fake: logs every writev/write call so tests can assert WHICH path served it. */
#define VLOG_MAX 16
typedef struct {
    int vec_calls;
    int scalar_calls;
    uint64_t last_lba;
    uint32_t last_nsegs;
    uint64_t scalar_lbas[VLOG_MAX];
    uint32_t scalar_counts[VLOG_MAX];
    int fail_on_call; /* 1-based call number to fail on; 0 = never */
    uint8_t sink[64 * HYPE_BLK_SECTOR_SIZE];
} vfake_t;

static int vfake_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    vfake_t *f = (vfake_t *)ctx;
    f->scalar_calls++;
    if (f->fail_on_call != 0 && f->scalar_calls == f->fail_on_call) {
        return -1;
    }
    if (f->scalar_calls <= VLOG_MAX) {
        f->scalar_lbas[f->scalar_calls - 1] = lba;
        f->scalar_counts[f->scalar_calls - 1] = count;
    }
    memcpy(f->sink + lba * HYPE_BLK_SECTOR_SIZE, buf, (size_t)count * HYPE_BLK_SECTOR_SIZE);
    return 0;
}

static int vfake_writev(void *ctx, uint64_t lba, const hype_blk_seg_t *segs, uint32_t nsegs) {
    vfake_t *f = (vfake_t *)ctx;
    uint32_t i;
    f->vec_calls++;
    f->last_lba = lba;
    f->last_nsegs = nsegs;
    for (i = 0; i < nsegs; i++) {
        memcpy(f->sink + lba * HYPE_BLK_SECTOR_SIZE, segs[i].buf,
               (size_t)segs[i].count * HYPE_BLK_SECTOR_SIZE);
        lba += segs[i].count;
    }
    return 0;
}

static void vfake_backend(vfake_t *f, hype_blk_backend_t *be, int with_vec) {
    memset(f, 0, sizeof(*f));
    be->read = 0;
    be->write = vfake_write;
    be->writev = with_vec ? vfake_writev : 0;
    be->ctx = f;
    be->total_sectors = 64u;
}

static void test_writev_vectored_path_and_stats(void) {
    vfake_t f;
    hype_blk_backend_t be;
    uint8_t a[2 * HYPE_BLK_SECTOR_SIZE], b[HYPE_BLK_SECTOR_SIZE], c[3 * HYPE_BLK_SECTOR_SIZE];
    hype_blk_seg_t segs[3] = {{a, 2u}, {b, 1u}, {c, 3u}};
    hype_blk_wstats_t *g = hype_blk_wstats();
    uint64_t w0, v0, s0;

    memset(a, 0xA1, sizeof(a));
    memset(b, 0xB2, sizeof(b));
    memset(c, 0xC3, sizeof(c));
    vfake_backend(&f, &be, 1);
    w0 = g->writes; v0 = g->vec_writes; s0 = g->vec_segs;

    CHECK_HEX("vectored writev accepted", 0, hype_blk_backend_writev(&be, 10u, segs, 3u));
    CHECK_HEX("one vectored impl call", 1, f.vec_calls);
    CHECK_HEX("no scalar calls", 0, f.scalar_calls);
    CHECK_HEX("impl saw the start lba", 10u, f.last_lba);
    CHECK_HEX("impl saw all 3 segments", 3u, f.last_nsegs);
    CHECK_HEX("recorded as ONE write", w0 + 1u, g->writes);
    CHECK_HEX("vec_writes counted", v0 + 1u, g->vec_writes);
    CHECK_HEX("vec_segs counted", s0 + 3u, g->vec_segs);
    /* Data landed contiguously: [10,12)=A1, [12,13)=B2, [13,16)=C3. */
    CHECK_HEX("seg 0 first byte", 0xA1, f.sink[10u * HYPE_BLK_SECTOR_SIZE]);
    CHECK_HEX("seg 1 first byte", 0xB2, f.sink[12u * HYPE_BLK_SECTOR_SIZE]);
    CHECK_HEX("seg 2 last byte", 0xC3, f.sink[16u * HYPE_BLK_SECTOR_SIZE - 1u]);
}

static void test_writev_fallback_loops_scalar_writes(void) {
    vfake_t f;
    hype_blk_backend_t be;
    uint8_t a[HYPE_BLK_SECTOR_SIZE], b[2 * HYPE_BLK_SECTOR_SIZE];
    hype_blk_seg_t segs[2] = {{a, 1u}, {b, 2u}};
    hype_blk_wstats_t *g = hype_blk_wstats();
    uint64_t w0, v0;

    memset(a, 0x11, sizeof(a));
    memset(b, 0x22, sizeof(b));
    vfake_backend(&f, &be, 0); /* no writev impl */
    w0 = g->writes; v0 = g->vec_writes;

    CHECK_HEX("fallback writev accepted", 0, hype_blk_backend_writev(&be, 5u, segs, 2u));
    CHECK_HEX("no vectored calls", 0, f.vec_calls);
    CHECK_HEX("two scalar calls", 2, f.scalar_calls);
    CHECK_HEX("scalar call 0 lba", 5u, f.scalar_lbas[0]);
    CHECK_HEX("scalar call 1 lba advanced", 6u, f.scalar_lbas[1]);
    CHECK_HEX("scalar call 1 count", 2u, f.scalar_counts[1]);
    CHECK_HEX("recorded as TWO writes (honest histogram)", w0 + 2u, g->writes);
    CHECK_HEX("vec counters untouched by the fallback", v0, g->vec_writes);
    CHECK_HEX("fallback data seg 1", 0x22, f.sink[6u * HYPE_BLK_SECTOR_SIZE]);
}

static void test_writev_validates_whole_list_before_any_byte(void) {
    vfake_t f;
    hype_blk_backend_t be;
    uint8_t a[HYPE_BLK_SECTOR_SIZE];
    /* Total = 1 + 63 = 64 sectors starting at lba 1 -> [1,65) exceeds the 64-sector backend. */
    hype_blk_seg_t oob[2] = {{a, 1u}, {a, 63u}};
    hype_blk_seg_t zero[2] = {{a, 1u}, {a, 0u}};

    vfake_backend(&f, &be, 0);
    CHECK_HEX("out-of-bounds tail refused", -1, hype_blk_backend_writev(&be, 1u, oob, 2u));
    CHECK_HEX("NOTHING was written for the oob list", 0, f.scalar_calls);
    CHECK_HEX("zero-count segment refused", -1, hype_blk_backend_writev(&be, 0u, zero, 2u));
    CHECK_HEX("NOTHING was written for the zero-seg list", 0, f.scalar_calls);
    CHECK_HEX("empty list refused", -1, hype_blk_backend_writev(&be, 0u, oob, 0u));
    CHECK_HEX("NULL segs refused", -1, hype_blk_backend_writev(&be, 0u, 0, 1u));
    CHECK_HEX("NULL backend refused", -1, hype_blk_backend_writev(0, 0u, oob, 1u));
}

static void test_writev_read_only_backend_refused(void) {
    hype_blk_backend_t ro;
    uint8_t a[HYPE_BLK_SECTOR_SIZE];
    hype_blk_seg_t segs[1] = {{a, 1u}};
    ro.read = 0;
    ro.write = 0;
    ro.writev = 0;
    ro.ctx = 0;
    ro.total_sectors = 16u;
    CHECK_HEX("read-only backend refuses writev", -1, hype_blk_backend_writev(&ro, 0u, segs, 1u));
}

static void test_writev_fallback_failure_stops_and_reports(void) {
    vfake_t f;
    hype_blk_backend_t be;
    uint8_t a[HYPE_BLK_SECTOR_SIZE];
    hype_blk_seg_t segs[3] = {{a, 1u}, {a, 1u}, {a, 1u}};
    hype_blk_wstats_t *g = hype_blk_wstats();
    uint64_t w0;

    vfake_backend(&f, &be, 0);
    f.fail_on_call = 2;
    w0 = g->writes;
    CHECK_HEX("mid-list failure reported", -1, hype_blk_backend_writev(&be, 0u, segs, 3u));
    CHECK_HEX("stopped at the failing segment", 2, f.scalar_calls);
    CHECK_HEX("only the SUCCESSFUL segment was counted", w0 + 1u, g->writes);
}

static void test_writev_32bit_total_overflow_refused(void) {
    vfake_t f;
    hype_blk_backend_t be;
    uint8_t a[HYPE_BLK_SECTOR_SIZE];
    /* Two segments summing past 2^32 sectors must be refused by arithmetic, not by luck. */
    hype_blk_seg_t big[2] = {{a, 0xFFFFFFFFu}, {a, 2u}};

    vfake_backend(&f, &be, 0);
    be.total_sectors = 0xFFFFFFFFFFFFFFFFull; /* capacity is NOT the guard under test */
    CHECK_HEX("total > 32 bits refused", -1, hype_blk_backend_writev(&be, 0u, big, 2u));
    CHECK_HEX("nothing written", 0, f.scalar_calls);
}

int main(void) {
    test_range_in_bounds();
    test_file_read();
    test_file_write_roundtrip();
    test_bounds_gate_rejects_oob();
    test_dispatch_null_guards();
    test_partial_trailing_sector_unreachable();

    test_wstats_buckets();
    test_wstats_record_accumulates();
    test_wstats_first_tsc_is_the_first_write();
    test_wstats_kbps();
    test_backend_write_records_only_on_success();

    test_writev_vectored_path_and_stats();
    test_writev_fallback_loops_scalar_writes();
    test_writev_validates_whole_list_before_any_byte();
    test_writev_read_only_backend_refused();
    test_writev_fallback_failure_stops_and_reports();
    test_writev_32bit_total_overflow_refused();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
