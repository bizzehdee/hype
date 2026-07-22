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
