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

int main(void) {
    test_range_in_bounds();
    test_file_read();
    test_file_write_roundtrip();
    test_bounds_gate_rejects_oob();
    test_dispatch_null_guards();
    test_partial_trailing_sector_unreachable();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
