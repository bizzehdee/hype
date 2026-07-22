#include "ahci_host.h"
#include "../devices/ata_disk.h" /* HYPE_ATA_CMD_READ_DMA_EXT */

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
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

int hype_ahci_host_build_read_dma_ext(uint8_t *cmd_table, uint64_t lba, uint16_t count,
                                      uint64_t dst_phys) {
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
    fis[2] = HYPE_ATA_CMD_READ_DMA_EXT;   /* 0x25 */
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
     * clear -- the read path polls PxCI). */
    for (i = 0; i < HYPE_AHCI_HOST_PRDT_ENTRY_SIZE; i++) {
        prd[i] = 0;
    }
    put_le32(prd + 0, (uint32_t)dst_phys);          /* DBA  */
    put_le32(prd + 4, (uint32_t)(dst_phys >> 32));  /* DBAU */
    dbc = (bytes - 1u) & 0x3FFFFFu;
    put_le32(prd + 12, dbc);
    return 0;
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
