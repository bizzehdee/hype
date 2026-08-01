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


/* #262 slice 1: PRD byte ranges -> whole sectors for the blk_backend path. */
static void test_prd_sector_range(void) {
    uint64_t lba_off;
    uint32_t nsec;

    if (hype_ata_prd_sector_range(0u, 512u, &lba_off, &nsec) != 0 || lba_off != 0u || nsec != 1u) {
        printf("FAIL: one sector at offset 0 should be lba_off=0 nsec=1\n");
        failures++;
    }
    if (hype_ata_prd_sector_range(4096u, 8192u, &lba_off, &nsec) != 0 || lba_off != 8u ||
        nsec != 16u) {
        printf("FAIL: 8KiB at offset 4KiB should be lba_off=8 nsec=16\n");
        failures++;
    }

    /* Refusing is the point: ATA DMA moves whole sectors, so a PRD that splits one
     * means an assumption is wrong, and read-modify-writing around it would hide
     * that rather than surface it. */
    if (hype_ata_prd_sector_range(0u, 500u, &lba_off, &nsec) == 0) {
        printf("FAIL: a non-sector-multiple LENGTH must be refused\n");
        failures++;
    }
    if (hype_ata_prd_sector_range(100u, 512u, &lba_off, &nsec) == 0) {
        printf("FAIL: a non-sector-aligned OFFSET must be refused\n");
        failures++;
    }
    if (hype_ata_prd_sector_range(0u, 0u, &lba_off, &nsec) != 0 || nsec != 0u) {
        printf("FAIL: a zero-length range is aligned and yields nsec=0\n");
        failures++;
    }
    if (hype_ata_prd_sector_range(0u, 512u, 0, &nsec) == 0 ||
        hype_ata_prd_sector_range(0u, 512u, &lba_off, 0) == 0) {
        printf("FAIL: NULL out-parameters must be refused\n");
        failures++;
    }

    /* A large offset must not overflow into a wrong sector number. */
    if (hype_ata_prd_sector_range(4294967296ull, 512u, &lba_off, &nsec) != 0 ||
        lba_off != 8388608ull) {
        printf("FAIL: a 4GiB offset should map to sector 8388608, got %llu\n",
               (unsigned long long)lba_off);
        failures++;
    }
}

static void test_set_backend(void) {
    hype_ata_disk_t d;
    static uint8_t media[1024];
    hype_blk_backend_t be;

    hype_ata_disk_reset(&d, media, sizeof(media));
    if (d.be != 0) {
        printf("FAIL: reset must clear the backend so a RAM-media disk stays RAM-backed\n");
        failures++;
    }
    be.total_sectors = 4096u;
    hype_ata_disk_set_backend(&d, &be);
    if (d.be != &be) {
        printf("FAIL: set_backend should attach the backend\n");
        failures++;
    }
    /* Capacity must follow the backend. A zero-sector LBA disk makes libata fall
     * back to CHS and issue INIT_DEV_PARAMS, which this model does not implement. */
    if (d.total_sectors != 4096u) {
        printf("FAIL: capacity should come from the backend, got %llu\n",
               (unsigned long long)d.total_sectors);
        failures++;
    }
    if (d.media_bytes != 4096ull * 512ull) {
        printf("FAIL: media_bytes should match the backend capacity\n");
        failures++;
    }
    hype_ata_disk_set_backend(0, &be); /* must not crash */
}

static void test_lba28_decode(void) {
    /* Bits 24-27 live in the device register's low nibble, not in the high LBA
     * bytes -- the whole reason a 28-bit command cannot reuse the 48-bit path. */
    if (hype_ata_lba28_from_fis(0x000000ull, 0xE0u) != 0ull) {
        printf("FAIL: lba28 of an all-zero address should be 0\n");
        failures++;
    }
    if (hype_ata_lba28_from_fis(0xABCDEFull, 0xE7u) != 0x7ABCDEFull) {
        printf("FAIL: lba28 must fold device[3:0] in as bits 24-27, got %llu\n",
               (unsigned long long)hype_ata_lba28_from_fis(0xABCDEFull, 0xE7u));
        failures++;
    }
    /* The high 24 bits of the raw FIS field belong to LBA48 only and must be
     * ignored here, or a 28-bit read lands at a wildly wrong sector. */
    if (hype_ata_lba28_from_fis(0xFFFFFFFFFFFFull, 0xE0u) != 0xFFFFFFull) {
        printf("FAIL: lba28 must ignore the LBA48-only high bytes\n");
        failures++;
    }
}

static void test_resolve_sector_count28(void) {
    if (hype_ata_resolve_sector_count28(1u) != 1u) {
        printf("FAIL: 28-bit count 1 should be 1 sector\n");
        failures++;
    }
    if (hype_ata_resolve_sector_count28(255u) != 255u) {
        printf("FAIL: 28-bit count 255 should be 255 sectors\n");
        failures++;
    }
    /* 0 means 256 for a 28-bit command, NOT the 65536 of the 48-bit rule. */
    if (hype_ata_resolve_sector_count28(0u) != 256u) {
        printf("FAIL: 28-bit count 0 should mean 256 sectors\n");
        failures++;
    }
    /* Only the low byte is a count in a 28-bit command. */
    if (hype_ata_resolve_sector_count28(0x1234u) != 0x34u) {
        printf("FAIL: 28-bit count must use only the low byte\n");
        failures++;
    }
    if (hype_ata_resolve_sector_count28(0xFF00u) != 256u) {
        printf("FAIL: 28-bit count with a zero low byte should mean 256\n");
        failures++;
    }
}

static void test_cmd_is_lba48(void) {
    if (!hype_ata_cmd_is_lba48(HYPE_ATA_CMD_READ_DMA_EXT) ||
        !hype_ata_cmd_is_lba48(HYPE_ATA_CMD_WRITE_DMA_EXT) ||
        !hype_ata_cmd_is_lba48(HYPE_ATA_CMD_FLUSH_CACHE_EXT)) {
        printf("FAIL: the EXT commands are 48-bit\n");
        failures++;
    }
    if (hype_ata_cmd_is_lba48(HYPE_ATA_CMD_READ_DMA) ||
        hype_ata_cmd_is_lba48(HYPE_ATA_CMD_WRITE_DMA) ||
        hype_ata_cmd_is_lba48(HYPE_ATA_CMD_FLUSH_CACHE) ||
        hype_ata_cmd_is_lba48(HYPE_ATA_CMD_IDENTIFY_DEVICE)) {
        printf("FAIL: the non-EXT commands are 28-bit\n");
        failures++;
    }
}

static void test_identify_declares_version_and_dma(void) {
    hype_ata_disk_t d;
    uint8_t id[HYPE_ATA_IDENTIFY_SIZE];
    hype_ata_disk_reset(&d, 0, 0);
    d.total_sectors = 8388608ull;
    hype_ata_disk_build_identify(&d, id);

    /* Word 80 must name a major version >= 4, or libata takes the pre-ATA-4 path
     * and fails the probe with INIT_DEV_PARAMS before issuing anything. */
    if ((uint16_t)(id[160] | (id[161] << 8)) != 0x01F0u) {
        printf("FAIL: word 80 must declare ATA-4..ATA8-ACS\n");
        failures++;
    }
    /* Word 49 bit 9 = LBA, bit 8 = DMA. Both matter: no DMA means libata picks
     * PIO, which the AHCI disk glue does not implement. */
    if (((uint16_t)(id[98] | (id[99] << 8)) & 0x0300u) != 0x0300u) {
        printf("FAIL: word 49 must advertise both LBA and DMA\n");
        failures++;
    }
    if ((uint16_t)(id[176] | (id[177] << 8)) != 0x203Fu) {
        printf("FAIL: word 88 must advertise UDMA modes\n");
        failures++;
    }
    /* A valid CHS tuple keeps even the legacy path harmless rather than fatal. */
    if (id[6] != 16u || id[12] != 63u) {
        printf("FAIL: words 3/6 must carry a valid heads/sectors geometry\n");
        failures++;
    }
}

int main(void) {
    test_prd_sector_range();
    test_set_backend();
    test_reset_computes_total_sectors();
    test_resolve_sector_count();
    test_range_in_bounds();
    test_identify_general_config_and_capabilities();
    test_identify_capacity_fields_small_disk();
    test_identify_capacity_capped_for_huge_disk();
    test_identify_strings_are_byte_swapped_per_word();
    test_lba28_decode();
    test_resolve_sector_count28();
    test_cmd_is_lba48();
    test_identify_declares_version_and_dma();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
