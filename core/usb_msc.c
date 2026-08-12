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
           rd_le32(csw + 8) == 0u && /* dCSWDataResidue: exact BOT stages only */
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

void hype_scsi_cdb_synchronize_cache10(uint8_t cdb[10]) {
    unsigned int i;
    for (i = 0; i < 10u; i++) cdb[i] = 0;
    cdb[0] = 0x35u; /* SYNCHRONIZE CACHE(10) */
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

void hype_scsi_cdb_inquiry_vpd(uint8_t cdb[6], uint8_t page, uint8_t alloc_len) {
    unsigned int i;
    for (i = 0; i < 6u; i++) cdb[i] = 0;
    cdb[0] = 0x12u;     /* INQUIRY */
    cdb[1] = 0x01u;     /* EVPD: return the vital product data page, not standard data */
    cdb[2] = page;
    cdb[4] = alloc_len;
}

int hype_scsi_vpd80_serial(const uint8_t *resp, unsigned int resp_len, char *out,
                           unsigned int out_cap) {
    unsigned int page_len;
    unsigned int start;
    unsigned int end;
    unsigned int i;
    unsigned int n;

    if (resp == 0 || out == 0 || out_cap < 2u || resp_len < 4u) return -1;
    if (resp[1] != 0x80u) return -1; /* not the Unit Serial Number page */
    page_len = resp[3];
    if (page_len == 0u || 4u + page_len > resp_len) return -1;

    /*
     * SPC-3 7.6.10: the product serial number field holds ASCII graphic codes
     * (20h..7Eh), right-aligned with left space padding. A byte outside that
     * range means the field is not a usable identity; producing a mangled
     * serial that matches nothing the operator can read fails #323's rule
     * worse than producing none.
     */
    start = 4u;
    end = 4u + page_len;
    while (start < end && resp[start] == 0x20u) start++;
    while (end > start && (resp[end - 1u] == 0x20u || resp[end - 1u] == 0x00u)) end--;
    if (start == end) return -1; /* all padding: no identity */
    for (i = start; i < end; i++) {
        if (resp[i] < 0x20u || resp[i] > 0x7Eu) return -1;
    }
    n = end - start;
    if (n > out_cap - 1u) return -1; /* a truncated serial is a different serial */
    for (i = 0; i < n; i++) out[i] = (char)resp[start + i];
    out[n] = '\0';
    return (int)n;
}
