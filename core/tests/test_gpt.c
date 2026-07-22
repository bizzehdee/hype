#include <stdio.h>
#include <string.h>
#include "../gpt.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)v); put_le32(p + 4, (uint32_t)(v >> 32));
}

/* A synthetic 4-sector "disk": LBA0 protective MBR (unused here), LBA1 GPT
 * header, LBA2 partition-entry array (four 128-byte entries), LBA3 zero. */
static uint8_t g_disk[4][512];
static int g_valid_sig = 1;

static void build_fake_gpt(void) {
    uint8_t *hdr = g_disk[1];
    uint8_t *ents = g_disk[2];
    memset(g_disk, 0, sizeof(g_disk));

    /* Header (LBA 1). */
    if (g_valid_sig) {
        memcpy(hdr + 0x00, "EFI PART", 8);
    } else {
        memcpy(hdr + 0x00, "NOTGPT!!", 8);
    }
    put_le64(hdr + 0x48, 2);   /* partition entries start at LBA 2 */
    put_le32(hdr + 0x50, 128); /* 128 entries */
    put_le32(hdr + 0x54, 128); /* 128 bytes each */
    { /* disk GUID at offset 0x38: a recognizable 16-byte pattern */
        unsigned g;
        for (g = 0; g < 16u; g++) {
            hdr[0x38 + g] = (uint8_t)(0xA0 + g);
        }
    }

    /* Entry 0 (used): FAT ESP, LBA 2048..4095. Type GUID = nonzero. */
    ents[0] = 0x28; /* any nonzero byte in the type GUID => used */
    put_le64(ents + 0x20, 2048);
    put_le64(ents + 0x28, 4095);
    /* Entry 1 (used): the raw ISO partition, LBA 4096..1000000. */
    ents[128 + 0] = 0xA5;
    put_le64(ents + 128 + 0x20, 4096);
    put_le64(ents + 128 + 0x28, 1000000);
    /* Entry 2 (unused): type GUID all-zero (memset already). */
    /* Entry 3 (unused). */
}

static uint64_t g_fail_lba = (uint64_t)-1; /* if set, fake_read fails on this LBA */

static int fake_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (count != 1u || lba >= 4u || lba == g_fail_lba) {
        return -1;
    }
    memcpy(dst, g_disk[lba], 512);
    return 0;
}

static void test_find_first_partition(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    CHECK_HEX("find partition 1 ok", 0, hype_gpt_find_partition(fake_read, 0, 1, &p));
    CHECK_HEX("part1 first_lba", 2048u, p.first_lba);
    CHECK_HEX("part1 last_lba", 4095u, p.last_lba);
    CHECK_HEX("part1 size_bytes", (4095u - 2048u + 1u) * 512u, p.size_bytes);
}

static void test_find_second_partition(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    CHECK_HEX("find partition 2 ok", 0, hype_gpt_find_partition(fake_read, 0, 2, &p));
    CHECK_HEX("part2 first_lba (raw ISO)", 4096u, p.first_lba);
    CHECK_HEX("part2 last_lba", 1000000u, p.last_lba);
}

static void test_index_out_of_range(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    CHECK_HEX("only 2 used partitions -> index 3 fails", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 3, &p));
    CHECK_HEX("index 0 (not 1-based) fails", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 0, &p));
}

static void test_bad_signature(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 0; build_fake_gpt();
    CHECK_HEX("invalid GPT signature rejected", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 1, &p));
}

/* Negative paths through hype_gpt_find_partition -- each pokes one field of an
 * otherwise-valid table so a single guard fires. */
static void test_header_read_fail(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    g_fail_lba = 1; /* the GPT header read (LBA 1) fails */
    CHECK_HEX("header read failure rejected", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 1, &p));
    g_fail_lba = (uint64_t)-1;
}

static void test_entry_size_too_small(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    put_le32(g_disk[1] + 0x54, 64); /* < 128-byte minimum */
    CHECK_HEX("entry_size < 128 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 1, &p));
}

static void test_num_entries_zero(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    put_le32(g_disk[1] + 0x50, 0);
    CHECK_HEX("num_entries 0 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 1, &p));
}

static void test_num_entries_too_many(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    put_le32(g_disk[1] + 0x50, 5000); /* > 4096 cap */
    CHECK_HEX("num_entries > cap rejected", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 1, &p));
}

static void test_entry_size_not_128(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    put_le32(g_disk[1] + 0x54, 256); /* valid-but-unsupported larger entry */
    CHECK_HEX("entry_size != 128 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 1, &p));
}

static void test_entry_array_read_fail(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    put_le64(g_disk[1] + 0x48, 10); /* entry array at LBA 10 -> read fails (disk is 4 sectors) */
    CHECK_HEX("entry-array read failure rejected", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 1, &p));
}

static void test_entry_last_before_first(void) {
    hype_gpt_partition_t p;
    g_valid_sig = 1; build_fake_gpt();
    put_le64(g_disk[2] + 0x20, 5000); /* entry 0 first_lba */
    put_le64(g_disk[2] + 0x28, 100);  /* last_lba < first_lba => corrupt */
    CHECK_HEX("last_lba < first_lba rejected", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_find_partition(fake_read, 0, 1, &p));
}

static void test_disk_guid(void) {
    uint8_t guid[16];
    unsigned g;
    int mismatch = 0;

    g_valid_sig = 1; build_fake_gpt();
    CHECK_HEX("read disk GUID ok", 0, hype_gpt_disk_guid(fake_read, 0, guid));
    for (g = 0; g < 16u; g++) {
        if (guid[g] != (uint8_t)(0xA0 + g)) {
            mismatch = 1;
        }
    }
    CHECK_HEX("disk GUID bytes match", 0, mismatch);
}

static void test_disk_guid_bad_signature(void) {
    uint8_t guid[16];
    g_valid_sig = 0; build_fake_gpt();
    CHECK_HEX("disk GUID rejected on bad signature", (unsigned long long)(-1),
              (unsigned long long)hype_gpt_disk_guid(fake_read, 0, guid));
}

int main(void) {
    test_find_first_partition();
    test_find_second_partition();
    test_index_out_of_range();
    test_bad_signature();
    test_header_read_fail();
    test_entry_size_too_small();
    test_num_entries_zero();
    test_num_entries_too_many();
    test_entry_size_not_128();
    test_entry_array_read_fail();
    test_entry_last_before_first();
    test_disk_guid();
    test_disk_guid_bad_signature();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
