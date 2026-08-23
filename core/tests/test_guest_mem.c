#include <stdio.h>
#include "../../core/guest_mem.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* FW-1-like two-region layout: 1 GiB of guest RAM at a host buffer, plus
 * a 4 MiB firmware-flash window just below 4 GiB. */
#define RAM_GBASE 0x0ULL
#define RAM_HBASE 0x37e00000ULL
#define RAM_LEN (1024ULL * 1024ULL * 1024ULL) /* 1 GiB */
#define FLASH_GBASE (0x100000000ULL - 0x400000ULL)
#define FLASH_HBASE 0x7da00000ULL
#define FLASH_LEN 0x400000ULL

static void build_map(hype_gpa_map_t *m) {
    hype_gpa_map_reset(m);
    CHECK_HEX("add RAM ok", 0, hype_gpa_map_add(m, RAM_GBASE, RAM_HBASE, RAM_LEN));
    CHECK_HEX("add flash ok", 0, hype_gpa_map_add(m, FLASH_GBASE, FLASH_HBASE, FLASH_LEN));
    CHECK_HEX("region count", 2, m->count);
}

static void test_translate_inside_ram(void) {
    hype_gpa_map_t m;
    build_map(&m);
    /* Start of RAM. */
    CHECK_HEX("gpa 0 -> host base", RAM_HBASE, hype_gpa_to_host(&m, 0, 1));
    /* Arbitrary offset, small length. */
    CHECK_HEX("gpa 0x1000 len 64", RAM_HBASE + 0x1000, hype_gpa_to_host(&m, 0x1000, 64));
    /* Range ending exactly at the region end is valid. */
    CHECK_HEX("last byte of RAM", RAM_HBASE + RAM_LEN - 1, hype_gpa_to_host(&m, RAM_LEN - 1, 1));
    CHECK_HEX("range ending at region end", RAM_HBASE + RAM_LEN - 8, hype_gpa_to_host(&m, RAM_LEN - 8, 8));
}

static void test_translate_inside_flash(void) {
    hype_gpa_map_t m;
    build_map(&m);
    CHECK_HEX("flash base", FLASH_HBASE, hype_gpa_to_host(&m, FLASH_GBASE, 16));
    CHECK_HEX("flash offset", FLASH_HBASE + 0x2000, hype_gpa_to_host(&m, FLASH_GBASE + 0x2000, 256));
}

static void test_reject_out_of_range(void) {
    hype_gpa_map_t m;
    build_map(&m);
    /* The MMIO hole between RAM end and flash base is not mapped. */
    CHECK_HEX("gap above RAM rejected", 0, hype_gpa_to_host(&m, RAM_LEN, 1));
    CHECK_HEX("gap mid-hole rejected", 0, hype_gpa_to_host(&m, 0x80000000ULL, 4));
    CHECK_HEX("just below flash rejected", 0, hype_gpa_to_host(&m, FLASH_GBASE - 1, 1));
    CHECK_HEX("at 4GB (past flash) rejected", 0, hype_gpa_to_host(&m, 0x100000000ULL, 1));
}

static void test_reject_range_overrunning_region_end(void) {
    hype_gpa_map_t m;
    build_map(&m);
    /* Last byte is in RAM, but len=2 runs one byte past the end. */
    CHECK_HEX("overrun by 1 rejected", 0, hype_gpa_to_host(&m, RAM_LEN - 1, 2));
    /* A large length from a valid start that overshoots the region. */
    CHECK_HEX("overshoot rejected", 0, hype_gpa_to_host(&m, RAM_LEN - 0x1000, 0x2000));
}

static void test_reject_straddling_two_regions(void) {
    hype_gpa_map_t m;
    /* Two adjacent regions; a range spanning the boundary must be
     * rejected even though both endpoints are individually mapped. */
    hype_gpa_map_reset(&m);
    hype_gpa_map_add(&m, 0x1000, 0xA000, 0x1000);
    hype_gpa_map_add(&m, 0x2000, 0xB000, 0x1000); /* guest-adjacent, host-adjacent */
    CHECK_HEX("within first region ok", 0xA800, hype_gpa_to_host(&m, 0x1800, 0x100));
    CHECK_HEX("straddle boundary rejected", 0, hype_gpa_to_host(&m, 0x1F00, 0x200));
}

static void test_reject_zero_length(void) {
    hype_gpa_map_t m;
    build_map(&m);
    CHECK_HEX("zero length rejected", 0, hype_gpa_to_host(&m, 0x1000, 0));
    CHECK_HEX("zero length invalid", 0, hype_gpa_range_valid(&m, 0x1000, 0));
}

static void test_range_valid_matches_translate(void) {
    hype_gpa_map_t m;
    build_map(&m);
    CHECK_HEX("valid range reported valid", 1, hype_gpa_range_valid(&m, 0x1000, 64));
    CHECK_HEX("invalid range reported invalid", 0, hype_gpa_range_valid(&m, 0x80000000ULL, 64));
    /* range_valid must not conflate a legitimate host address of 0 with
     * "invalid": a region mapped to host base 0 is still a valid range. */
    hype_gpa_map_reset(&m);
    hype_gpa_map_add(&m, 0x5000, 0x0, 0x1000);
    CHECK_HEX("host-base-0 region is valid", 1, hype_gpa_range_valid(&m, 0x5000, 16));
    CHECK_HEX("host-base-0 translate returns 0", 0, hype_gpa_to_host(&m, 0x5000, 16));
}

static void test_empty_map_rejects_everything(void) {
    hype_gpa_map_t m;
    hype_gpa_map_reset(&m);
    CHECK_HEX("empty map count", 0, m.count);
    CHECK_HEX("empty map rejects", 0, hype_gpa_to_host(&m, 0, 1));
    CHECK_HEX("empty map invalid", 0, hype_gpa_range_valid(&m, 0, 1));
}

static void test_add_rejects_malformed(void) {
    hype_gpa_map_t m;
    unsigned i;
    hype_gpa_map_reset(&m);
    CHECK_HEX("zero-length region rejected", -1, hype_gpa_map_add(&m, 0, 0, 0));
    CHECK_HEX("guest-base overflow rejected", -1, hype_gpa_map_add(&m, UINT64_MAX - 3, 0x1000, 16));
    CHECK_HEX("host-base overflow rejected", -1, hype_gpa_map_add(&m, 0x1000, UINT64_MAX - 3, 16));
    CHECK_HEX("count still zero after rejects", 0, m.count);
    /* Fill to capacity, then reject the overflow-by-one. */
    for (i = 0; i < HYPE_GPA_MAP_MAX_REGIONS; i++) {
        CHECK_HEX("fill region ok", 0, hype_gpa_map_add(&m, (uint64_t)i * 0x1000, 0x100000, 0x1000));
    }
    CHECK_HEX("full map count", HYPE_GPA_MAP_MAX_REGIONS, m.count);
    CHECK_HEX("add beyond capacity rejected", -1, hype_gpa_map_add(&m, 0x900000, 0x100000, 0x1000));
}

static void test_high_region_no_overflow(void) {
    hype_gpa_map_t m;
    hype_gpa_map_reset(&m);
    /* A region high in the address space but whose exclusive end does
     * NOT overflow: add succeeds, a full-span access is valid, and one
     * byte past the end is rejected (an overrun, computed without
     * wrapping). */
    CHECK_HEX("high region add ok", 0,
              hype_gpa_map_add(&m, UINT64_MAX - 0x1FFFULL, 0x1000, 0x1000));
    CHECK_HEX("full region span valid", 0x1000, hype_gpa_to_host(&m, UINT64_MAX - 0x1FFFULL, 0x1000));
    CHECK_HEX("one past region end rejected", 0, hype_gpa_to_host(&m, UINT64_MAX - 0x1FFFULL, 0x1001));

    /* A region whose exclusive end would be exactly 2^64 (last byte =
     * UINT64_MAX) cannot be represented without the end wrapping to 0, so
     * add must reject it rather than store a region the lookup math
     * would mishandle. */
    CHECK_HEX("top-of-space region rejected", -1,
              hype_gpa_map_add(&m, UINT64_MAX - 0xFFFULL, 0x2000, 0x1000));
}

/*
 * #669: hype_gpa_read()/hype_gpa_write() are the translate-then-copy helpers extracted from
 * boot/main.c's nvme_guest_read/write (unreachable from core/tests -- boot/main.c is never
 * linked into a test binary) so this VALID-3 shape is directly testable.
 */
static void test_gpa_read_legitimate_range_copies_bytes(void) {
    uint8_t ram[64];
    uint8_t dst[4] = {0, 0, 0, 0};
    hype_gpa_map_t m;
    int rc;
    unsigned i;
    for (i = 0; i < sizeof(ram); i++) ram[i] = (uint8_t)(0x10u + i);
    hype_gpa_map_reset(&m);
    hype_gpa_map_add(&m, 0x1000, (uint64_t)(uintptr_t)ram, sizeof(ram));

    rc = hype_gpa_read(&m, 0x1004, 4u, dst);
    CHECK_HEX("in-range read succeeds", 0, rc);
    CHECK_HEX("byte 0", 0x14u, dst[0]);
    CHECK_HEX("byte 3", 0x17u, dst[3]);
}

static void test_gpa_read_out_of_range_refuses_without_touching_dst(void) {
    uint8_t ram[64];
    uint8_t dst[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    hype_gpa_map_t m;
    int rc;
    hype_gpa_map_reset(&m);
    hype_gpa_map_add(&m, 0x1000, (uint64_t)(uintptr_t)ram, sizeof(ram));

    rc = hype_gpa_read(&m, 0x9000, 4u, dst);
    CHECK_HEX("out-of-range read is refused", (unsigned)-1, (unsigned)rc);
    CHECK_HEX("dst untouched on refusal", 0xAAu, dst[0]);
}

static void test_gpa_write_legitimate_range_copies_bytes(void) {
    uint8_t ram[64];
    uint8_t src[4] = {0x11, 0x22, 0x33, 0x44};
    hype_gpa_map_t m;
    int rc;
    unsigned i;
    for (i = 0; i < sizeof(ram); i++) ram[i] = 0;
    hype_gpa_map_reset(&m);
    hype_gpa_map_add(&m, 0x2000, (uint64_t)(uintptr_t)ram, sizeof(ram));

    rc = hype_gpa_write(&m, 0x2008, 4u, src);
    CHECK_HEX("in-range write succeeds", 0, rc);
    CHECK_HEX("ram[8]", 0x11u, ram[8]);
    CHECK_HEX("ram[11]", 0x44u, ram[11]);
}

static void test_gpa_write_out_of_range_refuses_without_touching_ram(void) {
    uint8_t ram[64];
    uint8_t src[4] = {0x11, 0x22, 0x33, 0x44};
    hype_gpa_map_t m;
    int rc;
    unsigned i;
    for (i = 0; i < sizeof(ram); i++) ram[i] = 0xEEu;
    hype_gpa_map_reset(&m);
    hype_gpa_map_add(&m, 0x2000, (uint64_t)(uintptr_t)ram, sizeof(ram));

    rc = hype_gpa_write(&m, 0x9000, 4u, src);
    CHECK_HEX("out-of-range write is refused", (unsigned)-1, (unsigned)rc);
    CHECK_HEX("ram untouched on refusal", 0xEEu, ram[0]);
}

/* #693/#694: NULL map means trusted identity-mapped guest -- returns gpa unchecked. */
static void test_guest_dma_xlate_null_map_is_identity(void) {
    CHECK_HEX("NULL map returns gpa unchanged", 0x123456u,
              hype_guest_dma_xlate(0, 0x123456u, 64u));
}

static void test_guest_dma_xlate_non_null_map_routes_through_gpa_to_host(void) {
    uint8_t ram[64];
    hype_gpa_map_t m;
    uint64_t want;

    hype_gpa_map_reset(&m);
    hype_gpa_map_add(&m, 0x2000, (uint64_t)(uintptr_t)ram, sizeof(ram));
    want = hype_gpa_to_host(&m, 0x2000u, 4u);

    CHECK_HEX("legitimate range translates the same as hype_gpa_to_host", want,
              hype_guest_dma_xlate(&m, 0x2000u, 4u));
    CHECK_HEX("out-of-range range refuses the same as hype_gpa_to_host", 0u,
              hype_guest_dma_xlate(&m, 0x9000u, 4u));
}

int main(void) {
    test_translate_inside_ram();
    test_translate_inside_flash();
    test_reject_out_of_range();
    test_reject_range_overrunning_region_end();
    test_reject_straddling_two_regions();
    test_reject_zero_length();
    test_range_valid_matches_translate();
    test_empty_map_rejects_everything();
    test_add_rejects_malformed();
    test_high_region_no_overflow();
    test_gpa_read_legitimate_range_copies_bytes();
    test_gpa_read_out_of_range_refuses_without_touching_dst();
    test_gpa_write_legitimate_range_copies_bytes();
    test_gpa_write_out_of_range_refuses_without_touching_ram();
    test_guest_dma_xlate_null_map_is_identity();
    test_guest_dma_xlate_non_null_map_routes_through_gpa_to_host();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
