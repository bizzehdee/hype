#include <stdio.h>
#include "../../devices/ata_disk.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static uint8_t g_media[HYPE_ATA_SECTOR_SIZE * 16]; /* 16 sectors */

static void test_reset_computes_total_sectors(void) {
    hype_ata_disk_t disk;

    hype_ata_disk_reset(&disk, g_media, sizeof(g_media));
    CHECK_HEX("total_sectors derived from media_bytes/512", 16u, disk.total_sectors);
    CHECK_HEX("media pointer stored", (unsigned long long)(uintptr_t)g_media, (unsigned long long)(uintptr_t)disk.media);
    CHECK_HEX("media_bytes stored", sizeof(g_media), disk.media_bytes);
}

static void test_resolve_sector_count(void) {
    CHECK_HEX("nonzero count passes through unchanged", 5u, hype_ata_disk_resolve_sector_count(5));
    CHECK_HEX("0 means the 48-bit EXT command's own max (65536)", 65536u,
              hype_ata_disk_resolve_sector_count(0));
    CHECK_HEX("the largest real 16-bit count passes through unchanged", 0xFFFFu,
              hype_ata_disk_resolve_sector_count(0xFFFFu));
}

static void test_range_in_bounds(void) {
    hype_ata_disk_t disk;

    hype_ata_disk_reset(&disk, g_media, sizeof(g_media)); /* 16 sectors */
    CHECK_HEX("a range entirely inside the disk is in bounds", 1, hype_ata_disk_range_in_bounds(&disk, 0, 16));
    CHECK_HEX("a range starting mid-disk and ending exactly at capacity is in bounds", 1,
              hype_ata_disk_range_in_bounds(&disk, 10, 6));
    CHECK_HEX("a range one sector past capacity is out of bounds", 0,
              hype_ata_disk_range_in_bounds(&disk, 0, 17));
    CHECK_HEX("an lba already at capacity with any nonzero count is out of bounds", 0,
              hype_ata_disk_range_in_bounds(&disk, 16, 1));
    CHECK_HEX("a zero-length range at exactly capacity is in bounds (degenerate no-op)", 1,
              hype_ata_disk_range_in_bounds(&disk, 16, 0));
}

static uint16_t read_word_le(const uint8_t *buf, uint32_t word_index) {
    uint32_t byte_offset = word_index * 2u;
    return (uint16_t)(buf[byte_offset] | (buf[byte_offset + 1u] << 8));
}

static void test_identify_general_config_and_capabilities(void) {
    hype_ata_disk_t disk;
    uint8_t identify[HYPE_ATA_IDENTIFY_SIZE];

    hype_ata_disk_reset(&disk, g_media, sizeof(g_media));
    hype_ata_disk_build_identify(&disk, identify);

    CHECK_HEX("word 0 bit 15 clear -- ATA, not ATAPI", 0u, read_word_le(identify, 0) & 0x8000u);
    CHECK_HEX("word 49 bit 9 set -- LBA supported", 0x0200u, read_word_le(identify, 49) & 0x0200u);
    CHECK_HEX("word 83 bit 10 set -- LBA48 supported", 0x0400u, read_word_le(identify, 83) & 0x0400u);
    CHECK_HEX("word 83 validity marker (bit14 set, bit15 clear)", 0x4000u,
              read_word_le(identify, 83) & 0xC000u);
    CHECK_HEX("word 86 bit 10 set -- LBA48 enabled", 0x0400u, read_word_le(identify, 86) & 0x0400u);
}

static void test_identify_capacity_fields_small_disk(void) {
    hype_ata_disk_t disk;
    uint8_t identify[HYPE_ATA_IDENTIFY_SIZE];
    uint32_t lba28;
    uint64_t lba48;
    unsigned i;

    hype_ata_disk_reset(&disk, g_media, sizeof(g_media)); /* 16 sectors */
    hype_ata_disk_build_identify(&disk, identify);

    lba28 = (uint32_t)read_word_le(identify, 60) | ((uint32_t)read_word_le(identify, 61) << 16);
    CHECK_HEX("28-bit LBA capacity matches total_sectors for a small disk", 16u, lba28);

    lba48 = 0;
    for (i = 0; i < 4; i++) {
        lba48 |= (uint64_t)read_word_le(identify, 100u + i) << (16u * i);
    }
    CHECK_HEX("48-bit LBA capacity matches total_sectors", 16ull, lba48);
}

static void test_identify_capacity_capped_for_huge_disk(void) {
    hype_ata_disk_t disk;
    uint8_t identify[HYPE_ATA_IDENTIFY_SIZE];
    uint32_t lba28;
    uint64_t lba48;
    unsigned i;

    /* A disk larger than the 28-bit LBA field can express. */
    disk.media = 0;
    disk.media_bytes = 0;
    disk.total_sectors = 0x0FFFFFFFull + 1000ull;
    hype_ata_disk_build_identify(&disk, identify);

    lba28 = (uint32_t)read_word_le(identify, 60) | ((uint32_t)read_word_le(identify, 61) << 16);
    CHECK_HEX("28-bit LBA capacity is capped at its own field max", 0x0FFFFFFFu, lba28);

    lba48 = 0;
    for (i = 0; i < 4; i++) {
        lba48 |= (uint64_t)read_word_le(identify, 100u + i) << (16u * i);
    }
    CHECK_HEX("48-bit LBA capacity reports the real, uncapped value", disk.total_sectors, lba48);
}

static void test_identify_strings_are_byte_swapped_per_word(void) {
    hype_ata_disk_t disk;
    uint8_t identify[HYPE_ATA_IDENTIFY_SIZE];

    hype_ata_disk_reset(&disk, g_media, sizeof(g_media));
    hype_ata_disk_build_identify(&disk, identify);

    /* Model number starts at word 27 (byte 54): "HYPE VIRTUAL DISK".
     * First two real characters are 'H','Y' -- byte-swapped means the
     * word's low byte holds 'Y' and high byte holds 'H'. */
    CHECK_HEX("model string word 0 low byte is the 2nd character ('Y')", (uint8_t)'Y', identify[54]);
    CHECK_HEX("model string word 0 high byte is the 1st character ('H')", (uint8_t)'H', identify[55]);

    /* Firmware revision "1.0" -- padded with spaces, byte-swapped.
     * Word 0 (bytes 46-47) covers characters '1','.'; word 1 (bytes
     * 48-49) covers '0' plus one padding space beyond the string's
     * own 3 real characters -- byte-swapped, so the pad lands in the
     * word's LOW byte (byte 48) and '0' lands in the HIGH byte
     * (byte 49). */
    CHECK_HEX("firmware revision word 0 low byte is '.'", (uint8_t)'.', identify[46]);
    CHECK_HEX("firmware revision word 0 high byte is '1'", (uint8_t)'1', identify[47]);
    CHECK_HEX("firmware revision word 1 low byte is padding", (uint8_t)' ', identify[48]);
    CHECK_HEX("firmware revision word 1 high byte is the 3rd real character '0'", (uint8_t)'0',
              identify[49]);
}

int main(void) {
    test_reset_computes_total_sectors();
    test_resolve_sector_count();
    test_range_in_bounds();
    test_identify_general_config_and_capabilities();
    test_identify_capacity_fields_small_disk();
    test_identify_capacity_capped_for_huge_disk();
    test_identify_strings_are_byte_swapped_per_word();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
