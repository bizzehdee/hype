#include "ata_disk.h"

void hype_ata_disk_reset(hype_ata_disk_t *disk, uint8_t *media, uint64_t media_bytes) {
    disk->media = media;
    disk->media_bytes = media_bytes;
    disk->total_sectors = media_bytes / HYPE_ATA_SECTOR_SIZE;
}

/* ATA convention: each word's first character lands in the high byte,
 * second in the low byte -- byte-swapped relative to normal string
 * order. Never reads past `str`'s own null terminator, regardless of
 * how much shorter it is than `field_bytes` (the remainder is
 * space-padded, the standard ATA string convention). */
static void write_swapped_ascii(uint8_t *out, const char *str, uint32_t field_bytes) {
    uint32_t len = 0;
    uint32_t i;

    while (str[len] != '\0') {
        len++;
    }

    for (i = 0; i < field_bytes; i += 2u) {
        uint8_t c0 = (i < len) ? (uint8_t)str[i] : (uint8_t)' ';
        uint8_t c1 = (i + 1u < len) ? (uint8_t)str[i + 1u] : (uint8_t)' ';
        out[i] = c1;
        out[i + 1u] = c0;
    }
}

void hype_ata_disk_build_identify(const hype_ata_disk_t *disk, uint8_t out[HYPE_ATA_IDENTIFY_SIZE]) {
    uint32_t i;
    uint32_t lba28_capacity;
    uint64_t lba48_capacity;

    for (i = 0; i < HYPE_ATA_IDENTIFY_SIZE; i++) {
        out[i] = 0;
    }

    /* Word 0: general configuration -- bit 15 clear = ATA device (not
     * ATAPI); 0x0040 is the well-established convention for a
     * non-removable, fixed disk. */
    out[0] = 0x40u;
    out[1] = 0x00u;

    write_swapped_ascii(out + 20, "HYPE0000000000000001", 20u); /* words 10-19: serial number */
    write_swapped_ascii(out + 46, "1.0", 8u);                   /* words 23-26: firmware revision */
    write_swapped_ascii(out + 54, "HYPE VIRTUAL DISK", 40u);    /* words 27-46: model number */

    out[98] = 0x00u; /* word 49: capabilities -- bit 9 = LBA supported */
    out[99] = 0x02u;

    lba28_capacity =
        (disk->total_sectors > 0x0FFFFFFFull) ? 0x0FFFFFFFu : (uint32_t)disk->total_sectors;
    out[120] = (uint8_t)(lba28_capacity & 0xFFu); /* words 60-61: 28-bit LBA capacity */
    out[121] = (uint8_t)((lba28_capacity >> 8) & 0xFFu);
    out[122] = (uint8_t)((lba28_capacity >> 16) & 0xFFu);
    out[123] = (uint8_t)((lba28_capacity >> 24) & 0xFFu);

    /* Word 83: 48-bit Address feature set supported (bit 10 = 0x0400)
     * plus the words-82-84 validity marker (bit 14 set, bit 15
     * clear) -- combined 0x4400. */
    out[166] = 0x00u;
    out[167] = 0x44u;
    out[172] = 0x00u; /* word 86: 48-bit Address feature set enabled (bit 10) */
    out[173] = 0x04u;

    lba48_capacity = disk->total_sectors;
    for (i = 0; i < 8u; i++) { /* words 100-103: 48-bit LBA capacity, 64-bit LE */
        out[200 + i] = (uint8_t)((lba48_capacity >> (8u * i)) & 0xFFu);
    }
}

uint32_t hype_ata_disk_resolve_sector_count(uint16_t raw_count) {
    return (raw_count == 0u) ? 65536u : (uint32_t)raw_count;
}

int hype_ata_disk_range_in_bounds(const hype_ata_disk_t *disk, uint64_t lba, uint32_t sector_count) {
    return (lba + (uint64_t)sector_count) <= disk->total_sectors;
}
