#include "ahci_host.h"
#include "../devices/ata_disk.h" /* HYPE_ATA_CMD_READ_DMA_EXT */

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int hype_ahci_host_settle_continue(unsigned pending, unsigned negotiating, unsigned elapsed) {
    if (pending == 0u) {
        return 0; /* every implemented port is established -- nothing left to wait for */
    }
    if (negotiating != 0u) {
        return 1; /* a real device is still coming up; it gets the caller's full ceiling */
    }
    return (elapsed < HYPE_AHCI_HOST_SETTLE_EMPTY_SPINS) ? 1 : 0;
}

void hype_ahci_host_build_cmd_header(uint8_t slot[32], int is_write, uint16_t prdtl,
                                     uint64_t cmd_table_phys) {
    uint32_t opts;
    unsigned i;

    for (i = 0; i < 32u; i++) {
        slot[i] = 0;
    }
    /* dword 0: CFL[4:0] = FIS length in dwords (20-byte H2D FIS = 5), W bit6,
     * PRDTL[31:16]. (A bit5 = ATAPI stays 0 -- this is a plain ATA command.) */
    opts = 5u | (is_write ? (1u << 6) : 0u) | ((uint32_t)prdtl << 16);
    put_le32(slot + 0, opts);
    /* dword 1 (PRDBC, bytes 4-7) left 0: the HBA writes actual bytes there. */
    put_le32(slot + 8, (uint32_t)cmd_table_phys);         /* CTBA  */
    put_le32(slot + 12, (uint32_t)(cmd_table_phys >> 32)); /* CTBAU */
}

/* Shared encoder for READ/WRITE DMA EXT: a H2D Register FIS carrying `ata_cmd`
 * with the 48-bit LBA + 16-bit count, and a single PRDT entry for count*512
 * bytes at `buf_phys` (the data source for a write, destination for a read).
 * The read vs write direction the HBA acts on comes from the command-list
 * header's W bit (hype_ahci_host_build_cmd_header), not from here. */
static int build_rw_dma_ext(uint8_t *cmd_table, uint64_t lba, uint16_t count, uint64_t buf_phys,
                            uint8_t ata_cmd) {
    uint8_t *fis = cmd_table + HYPE_AHCI_HOST_CT_CFIS_OFF;
    uint8_t *prd = cmd_table + HYPE_AHCI_HOST_CT_PRDT_OFF;
    uint32_t bytes;
    uint32_t dbc;
    unsigned i;

    if (count == 0u || (uint32_t)count * HYPE_AHCI_HOST_SECTOR_SIZE > HYPE_AHCI_HOST_PRDT_MAX_BYTES) {
        return -1;
    }
    bytes = (uint32_t)count * HYPE_AHCI_HOST_SECTOR_SIZE;

    /* Host-to-Device Register FIS (20 bytes) -- inverse of
     * hype_ahci_decode_h2d_fis(): type 0x27, C=1, ATA command, 48-bit LBA split
     * low-24 (bytes 4-6) / high-24 (bytes 8-10), LBA-mode device (0x40), and the
     * 16-bit sector count (bytes 12-13). */
    for (i = 0; i < 20u; i++) {
        fis[i] = 0;
    }
    fis[0] = 0x27u;                       /* FIS type: Register - Host to Device */
    fis[1] = 0x80u;                       /* C bit: this FIS carries a command */
    fis[2] = ata_cmd;                     /* READ (0x25) or WRITE (0x35) DMA EXT */
    fis[4] = (uint8_t)(lba);
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40u;                       /* device: LBA mode */
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)(count);
    fis[13] = (uint8_t)(count >> 8);

    /* First PRDT entry -- inverse of hype_ahci_decode_prdt_entry(): 64-bit data
     * base, and DBC = bytes-1 in bits 21:0 (bit31 = interrupt-on-completion, left
     * clear -- the read/write path polls PxCI). */
    for (i = 0; i < HYPE_AHCI_HOST_PRDT_ENTRY_SIZE; i++) {
        prd[i] = 0;
    }
    put_le32(prd + 0, (uint32_t)buf_phys);          /* DBA  */
    put_le32(prd + 4, (uint32_t)(buf_phys >> 32));  /* DBAU */
    dbc = (bytes - 1u) & 0x3FFFFFu;
    put_le32(prd + 12, dbc);
    return 0;
}

int hype_ahci_host_build_read_dma_ext(uint8_t *cmd_table, uint64_t lba, uint16_t count,
                                      uint64_t dst_phys) {
    return build_rw_dma_ext(cmd_table, lba, count, dst_phys, HYPE_ATA_CMD_READ_DMA_EXT);
}

int hype_ahci_host_build_write_dma_ext(uint8_t *cmd_table, uint64_t lba, uint16_t count,
                                       uint64_t src_phys) {
    return build_rw_dma_ext(cmd_table, lba, count, src_phys, HYPE_ATA_CMD_WRITE_DMA_EXT);
}

void hype_ahci_host_build_identify(uint8_t *cmd_table, uint64_t dst_phys) {
    uint8_t *fis = cmd_table + HYPE_AHCI_HOST_CT_CFIS_OFF;
    uint8_t *prd = cmd_table + HYPE_AHCI_HOST_CT_PRDT_OFF;
    uint32_t dbc;
    unsigned i;

    /* H2D Register FIS carrying IDENTIFY DEVICE. Unlike READ DMA EXT there is
     * no LBA/count and no LBA-mode device byte -- IDENTIFY takes none. */
    for (i = 0; i < 20u; i++) {
        fis[i] = 0;
    }
    fis[0] = 0x27u;                          /* FIS type: Register - Host to Device */
    fis[1] = 0x80u;                          /* C bit: this FIS carries a command */
    fis[2] = HYPE_ATA_CMD_IDENTIFY_DEVICE;   /* 0xEC */

    /* Single PRDT entry for the 512-byte IDENTIFY response. DBC = bytes-1. */
    for (i = 0; i < HYPE_AHCI_HOST_PRDT_ENTRY_SIZE; i++) {
        prd[i] = 0;
    }
    put_le32(prd + 0, (uint32_t)dst_phys);
    put_le32(prd + 4, (uint32_t)(dst_phys >> 32));
    dbc = (HYPE_ATA_IDENTIFY_SIZE - 1u) & 0x3FFFFFu;
    put_le32(prd + 12, dbc);
}

/* Inverse of devices/ata_disk.c's write_swapped_ascii: each ATA word stores the
 * first character in its high byte (src[i+1]) and the second in its low byte
 * (src[i]). Writes `field_bytes` chars + a NUL, then trims trailing spaces. */
static void read_swapped_ascii(char *dst, const uint8_t *src, unsigned field_bytes) {
    unsigned i;

    for (i = 0; i < field_bytes; i += 2u) {
        dst[i] = (char)src[i + 1u];
        dst[i + 1u] = (char)src[i];
    }
    dst[field_bytes] = '\0';
    while (field_bytes > 0u && dst[field_bytes - 1u] == ' ') {
        dst[field_bytes - 1u] = '\0';
        field_bytes--;
    }
}

void hype_ahci_host_parse_identify(const uint8_t id[512], hype_host_disk_info_t *out) {
    uint64_t lba48 = 0;
    uint32_t lba28;
    unsigned i;

    read_swapped_ascii(out->serial, id + 20, 20u); /* words 10-19 */
    read_swapped_ascii(out->model, id + 54, 40u);  /* words 27-46 */

    for (i = 0; i < 8u; i++) { /* words 100-103: 48-bit LBA capacity, 64-bit LE */
        lba48 |= (uint64_t)id[200 + i] << (8u * i);
    }
    lba28 = (uint32_t)id[120] | ((uint32_t)id[121] << 8) |
            ((uint32_t)id[122] << 16) | ((uint32_t)id[123] << 24); /* words 60-61 */

    /* Word 83 bit 10 (high byte 167, bit 2 = 0x04) = 48-bit addressing supported. */
    if ((id[167] & 0x04u) != 0u && lba48 != 0u) {
        out->total_sectors = lba48;
    } else {
        out->total_sectors = lba28;
    }
}

void hype_ahci_host_build_cmd_header_atapi(uint8_t slot[32], uint16_t prdtl,
                                           uint64_t cmd_table_phys) {
    uint32_t opts;
    unsigned i;

    for (i = 0; i < 32u; i++) {
        slot[i] = 0;
    }
    /* Same as the ATA header except bit 5 (A) is SET: without it the HBA never sends the ACMD block
     * and the drive sees a PACKET command with no packet. W stays clear -- hype never writes to an
     * optical drive, and there is no code path here that could. */
    opts = 5u | (1u << 5) | ((uint32_t)prdtl << 16);
    put_le32(slot + 0, opts);
    put_le32(slot + 8, (uint32_t)cmd_table_phys);
    put_le32(slot + 12, (uint32_t)(cmd_table_phys >> 32));
}

int hype_ahci_host_build_atapi_read10(uint8_t *cmd_table, uint32_t lba2k, uint16_t count2k,
                                      uint64_t dst_phys) {
    uint8_t *fis = cmd_table + HYPE_AHCI_HOST_CT_CFIS_OFF;
    uint8_t *acmd = cmd_table + HYPE_AHCI_HOST_CT_ACMD_OFF;
    uint8_t *prd = cmd_table + HYPE_AHCI_HOST_CT_PRDT_OFF;
    uint32_t bytes;
    unsigned i;

    if (count2k == 0u || count2k > HYPE_AHCI_HOST_ATAPI_MAX_BLOCKS ||
        (uint32_t)count2k * HYPE_AHCI_HOST_CD_SECTOR_SIZE > HYPE_AHCI_HOST_PRDT_MAX_BYTES) {
        return -1;
    }
    bytes = (uint32_t)count2k * HYPE_AHCI_HOST_CD_SECTOR_SIZE;

    for (i = 0; i < 20u; i++) {
        fis[i] = 0;
    }
    fis[0] = 0x27u; /* Register FIS - Host to Device */
    fis[1] = 0x80u; /* C: this FIS carries a command */
    fis[2] = HYPE_ATA_CMD_PACKET;
    /*
     * For PACKET the LBA mid/high bytes are the BYTE COUNT LIMIT, not an address -- the drive may
     * return less than this but never more. The address itself lives in the CDB below. Getting this
     * wrong produces a transfer that either truncates or overruns rather than an obvious failure.
     */
    fis[5] = (uint8_t)(bytes & 0xFFu);        /* LBA mid  = byte count low  */
    fis[6] = (uint8_t)((bytes >> 8) & 0xFFu); /* LBA high = byte count high */
    fis[7] = 0x40u;                           /* device: LBA mode */

    /* The 12-byte ATAPI CDB: READ(10), big-endian LBA and transfer length. */
    for (i = 0; i < 16u; i++) {
        acmd[i] = 0;
    }
    acmd[0] = 0x28u; /* READ(10) */
    acmd[2] = (uint8_t)((lba2k >> 24) & 0xFFu);
    acmd[3] = (uint8_t)((lba2k >> 16) & 0xFFu);
    acmd[4] = (uint8_t)((lba2k >> 8) & 0xFFu);
    acmd[5] = (uint8_t)(lba2k & 0xFFu);
    acmd[7] = (uint8_t)((count2k >> 8) & 0xFFu);
    acmd[8] = (uint8_t)(count2k & 0xFFu);

    /* One PRDT entry, same shape as the ATA path: DBC is a byte count minus one. */
    for (i = 0; i < 16u; i++) {
        prd[i] = 0;
    }
    put_le32(prd + 0, (uint32_t)dst_phys);
    put_le32(prd + 4, (uint32_t)(dst_phys >> 32));
    put_le32(prd + 12, bytes - 1u);
    return 0;
}

int hype_ahci_host_atapi_lba512_to_lba2k(uint64_t lba512, uint32_t count512, uint32_t *out_lba2k,
                                         uint16_t *out_count2k) {
    const uint32_t per = HYPE_AHCI_HOST_CD_SECTOR_SIZE / HYPE_AHCI_HOST_SECTOR_SIZE; /* 4 */

    if (out_lba2k == 0 || out_count2k == 0) {
        return -1;
    }
    /*
     * Refuse rather than round. A misaligned request means the caller's arithmetic is wrong, and
     * quietly serving the containing 2 KiB sector would return the wrong 512-byte slice -- the exact
     * off-by-4 this conversion exists to prevent, and invisible in a log.
     */
    if (count512 == 0u || (lba512 % per) != 0u || (count512 % per) != 0u) {
        return -1;
    }
    if ((count512 / per) > 0xFFFFu) {
        return -1;
    }
    *out_lba2k = (uint32_t)(lba512 / per);
    *out_count2k = (uint16_t)(count512 / per);
    return 0;
}
