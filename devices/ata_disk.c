#include "ata_disk.h"

void hype_ata_disk_reset(hype_ata_disk_t *disk, uint8_t *media, uint64_t media_bytes) {
    disk->media = media;
    disk->be = 0;
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

    /*
     * #262: libata refuses a disk whose IDENTIFY looks pre-ATA-4. ata_dev_read_id()
     * takes the legacy path when `ata_id_major_version(id) < 4 || !ata_id_has_lba(id)`,
     * and that path calls ata_dev_init_params(dev, id[3], id[6]) -- which returns
     * AC_ERR_INVALID outright when heads/sectors are zero, without issuing anything.
     * A zero word 80 alone is enough to trigger it: the probe fails with
     * "failed to IDENTIFY (INIT_DEV_PARAMS failed, err_mask=0x80)" even though the
     * IDENTIFY itself succeeded and reported the right capacity. So declare a version
     * and a CHS geometry, and advertise DMA for the same reason the ATAPI model does
     * (hype_atapi_build_identify): without it libata configures PIO, which the AHCI
     * disk glue does not implement.
     *   w1/w3/w6 = 16383/16/63: the conventional CHS tuple every large drive reports,
     *              so even the legacy path would be valid rather than fatal.
     *   w49  = 0x0F00: DMA(8)+LBA(9)+IORDYdis(10)+IORDY(11).
     *   w53  = 0x0006: words 64-70 valid(1) + word 88 valid(2).
     *   w63  = 0x0007: MultiWord DMA modes 0-2 supported.
     *   w80  = 0x01F0: ATA-4..ATA8-ACS, so ata_id_major_version() reports 8.
     *   w88  = 0x203F: UDMA modes 0-5 supported, mode 5 selected.
     */
    out[2] = 0xFFu;   out[3] = 0x3Fu;   /* word 1  = 16383 cylinders */
    out[6] = 0x10u;   out[7] = 0x00u;   /* word 3  = 16 heads */
    out[12] = 0x3Fu;  out[13] = 0x00u;  /* word 6  = 63 sectors/track */
    out[98] = 0x00u;  out[99] = 0x0Fu;  /* word 49 = 0x0F00 */
    out[106] = 0x06u; out[107] = 0x00u; /* word 53 = 0x0006 */
    out[126] = 0x07u; out[127] = 0x00u; /* word 63 = 0x0007 */
    out[160] = 0xF0u; out[161] = 0x01u; /* word 80 = 0x01F0 */
    out[176] = 0x3Fu; out[177] = 0x20u; /* word 88 = 0x203F */

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

uint64_t hype_ata_lba28_from_fis(uint64_t fis_lba, uint8_t device) {
    return (fis_lba & 0xFFFFFFull) | ((uint64_t)(device & 0x0Fu) << 24);
}

uint32_t hype_ata_resolve_sector_count28(uint16_t raw_count) {
    uint32_t n = (uint32_t)(raw_count & 0xFFu);
    return (n == 0u) ? 256u : n;
}

int hype_ata_cmd_is_lba48(uint8_t command) {
    return (command == HYPE_ATA_CMD_READ_DMA_EXT || command == HYPE_ATA_CMD_WRITE_DMA_EXT ||
            command == HYPE_ATA_CMD_FLUSH_CACHE_EXT)
               ? 1
               : 0;
}

int hype_ata_disk_range_in_bounds(const hype_ata_disk_t *disk, uint64_t lba, uint32_t sector_count) {
    return (lba + (uint64_t)sector_count) <= disk->total_sectors;
}

void hype_ata_disk_set_backend(hype_ata_disk_t *disk, hype_blk_backend_t *be) {
    if (disk == 0) {
        return;
    }
    disk->be = be;
    /*
     * Capacity must come from the backend, or IDENTIFY reports a zero-sector disk.
     * That is not a harmless cosmetic: libata sees an LBA-capable drive of size 0,
     * decides it must be a legacy CHS device, and issues INITIALIZE DEVICE PARAMETERS
     * (0x91) -- which this model does not implement -- so the probe fails with
     * "failed to IDENTIFY (INIT_DEV_PARAMS failed)". Observed exactly that when the
     * backend was attached after a reset(disk, 0, 0) and the size was left behind.
     */
    if (be != 0) {
        disk->total_sectors = be->total_sectors;
        disk->media_bytes = be->total_sectors * (uint64_t)HYPE_ATA_SECTOR_SIZE;
    }
}

int hype_ata_prd_sector_range(uint64_t byte_off, uint32_t byte_len, uint64_t *lba_off,
                              uint32_t *nsec) {
    if (lba_off == 0 || nsec == 0) {
        return -1;
    }
    if ((byte_off % HYPE_ATA_SECTOR_SIZE) != 0u || (byte_len % HYPE_ATA_SECTOR_SIZE) != 0u) {
        return -1; /* a PRD splitting a sector: refuse rather than paper over it */
    }
    *lba_off = byte_off / HYPE_ATA_SECTOR_SIZE;
    *nsec = byte_len / HYPE_ATA_SECTOR_SIZE;
    return 0;
}
