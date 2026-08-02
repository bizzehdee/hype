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

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
