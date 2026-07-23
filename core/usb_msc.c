#include "usb_msc.h"

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

void hype_usb_bot_cbw(uint8_t cbw[31], uint32_t tag, uint32_t data_len, int dir_in,
                      unsigned int lun, const uint8_t *cdb, unsigned int cdb_len) {
    unsigned int i;
    for (i = 0; i < HYPE_USB_CBW_LEN; i++) cbw[i] = 0;
    put_le32(cbw + 0, HYPE_USB_CBW_SIGNATURE);
    put_le32(cbw + 4, tag);
    put_le32(cbw + 8, data_len);
    cbw[12] = dir_in ? 0x80u : 0x00u; /* bmCBWFlags: bit7 = Data-In */
    cbw[13] = (uint8_t)(lun & 0x0Fu);
    if (cdb_len > 16u) cdb_len = 16u;
    cbw[14] = (uint8_t)(cdb_len & 0x1Fu);
    for (i = 0; i < cdb_len; i++) cbw[15 + i] = cdb[i];
}

int hype_usb_bot_csw_ok(const uint8_t csw[13], uint32_t expect_tag) {
    return rd_le32(csw + 0) == HYPE_USB_CSW_SIGNATURE &&
           rd_le32(csw + 4) == expect_tag &&
           csw[12] == 0u; /* bCSWStatus: 0 = command passed */
}

void hype_scsi_cdb_read_capacity10(uint8_t cdb[10]) {
    unsigned int i;
    for (i = 0; i < 10u; i++) cdb[i] = 0;
    cdb[0] = 0x25u; /* READ CAPACITY(10) */
}

void hype_scsi_cdb_read10(uint8_t cdb[10], uint32_t lba, uint16_t blocks) {
    unsigned int i;
    for (i = 0; i < 10u; i++) cdb[i] = 0;
    cdb[0] = 0x28u;                 /* READ(10) */
    put_be32(cdb + 2, lba);         /* logical block address (big-endian) */
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t)blocks;       /* transfer length in blocks (big-endian) */
}

void hype_scsi_cdb_write10(uint8_t cdb[10], uint32_t lba, uint16_t blocks) {
    unsigned int i;
    for (i = 0; i < 10u; i++) cdb[i] = 0;
    cdb[0] = 0x2Au;                 /* WRITE(10) */
    put_be32(cdb + 2, lba);
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t)blocks;
}

void hype_scsi_cdb_inquiry(uint8_t cdb[6], uint8_t alloc_len) {
    unsigned int i;
    for (i = 0; i < 6u; i++) cdb[i] = 0;
    cdb[0] = 0x12u;       /* INQUIRY */
    cdb[4] = alloc_len;   /* allocation length */
}

void hype_scsi_parse_read_capacity10(const uint8_t rc[8], uint32_t *last_lba,
                                     uint32_t *block_size) {
    if (last_lba) *last_lba = rd_be32(rc + 0);
    if (block_size) *block_size = rd_be32(rc + 4);
}
