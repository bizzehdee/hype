#include "nvme_host.h"

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}
static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd_le64(const uint8_t *p) {
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

static void zero_sqe(uint8_t sqe[64]) {
    unsigned i;
    for (i = 0; i < HYPE_NVME_SQE_SIZE; i++) {
        sqe[i] = 0;
    }
}

void hype_nvme_build_read_sqe(uint8_t sqe[64], uint16_t cid, uint32_t nsid, uint64_t slba,
                              uint16_t nlb_0based, uint64_t prp1, uint64_t prp2) {
    zero_sqe(sqe);
    sqe[0] = HYPE_NVME_IO_READ;         /* CDW0: opcode */
    sqe[2] = (uint8_t)cid;              /* CDW0[31:16]: command id */
    sqe[3] = (uint8_t)(cid >> 8);
    put_le32(sqe + 4, nsid);            /* CDW1: NSID */
    put_le64(sqe + 24, prp1);           /* PRP1 */
    put_le64(sqe + 32, prp2);           /* PRP2 */
    put_le64(sqe + 40, slba);           /* CDW10-11: starting LBA */
    put_le32(sqe + 48, (uint32_t)nlb_0based); /* CDW12[15:0]: 0-based block count */
}

void hype_nvme_build_write_sqe(uint8_t sqe[64], uint16_t cid, uint32_t nsid, uint64_t slba,
                               uint16_t nlb_0based, uint64_t prp1, uint64_t prp2) {
    zero_sqe(sqe);
    sqe[0] = HYPE_NVME_IO_WRITE;        /* CDW0: opcode 0x01 */
    sqe[2] = (uint8_t)cid;              /* CDW0[31:16]: command id */
    sqe[3] = (uint8_t)(cid >> 8);
    put_le32(sqe + 4, nsid);            /* CDW1: NSID */
    put_le64(sqe + 24, prp1);           /* PRP1 */
    put_le64(sqe + 32, prp2);           /* PRP2 */
    put_le64(sqe + 40, slba);           /* CDW10-11: starting LBA */
    put_le32(sqe + 48, (uint32_t)nlb_0based); /* CDW12[15:0]: 0-based block count */
}

void hype_nvme_build_identify_sqe(uint8_t sqe[64], uint16_t cid, uint32_t cns, uint32_t nsid,
                                  uint64_t prp1) {
    zero_sqe(sqe);
    sqe[0] = HYPE_NVME_ADM_IDENTIFY;    /* CDW0: opcode */
    sqe[2] = (uint8_t)cid;
    sqe[3] = (uint8_t)(cid >> 8);
    put_le32(sqe + 4, nsid);            /* CDW1: NSID (used for CNS=0) */
    put_le64(sqe + 24, prp1);           /* PRP1: result buffer */
    put_le32(sqe + 40, cns);            /* CDW10: CNS */
}

int hype_nvme_cqe_phase(const uint8_t cqe[16]) {
    return (int)(rd_le16(cqe + 14) & 1u);
}

int hype_nvme_cqe_success(const uint8_t cqe[16]) {
    /* Status Field = status word bits 15:1; success is an all-zero field. */
    return (rd_le16(cqe + 14) >> 1) == 0u;
}

uint16_t hype_nvme_cqe_cid(const uint8_t cqe[16]) {
    return rd_le16(cqe + 12);
}

int hype_nvme_parse_identify_ns(const uint8_t idns[4096], uint64_t *total_blocks,
                                uint32_t *block_bytes) {
    uint64_t nsze = rd_le64(idns + 0);
    unsigned flbas = idns[26] & 0x0Fu;             /* current LBA format index */
    uint32_t lbaf = rd_le32(idns + 128u + flbas * 4u);
    unsigned lbads = (lbaf >> 16) & 0xFFu;         /* log2(logical block size) */

    if (nsze == 0u || lbads < 9u || lbads > 12u) { /* 512 B .. 4 KiB is the sane range */
        return -1;
    }
    *total_blocks = nsze;
    *block_bytes = 1u << lbads;
    return 0;
}

/* Copies a fixed-width, space-padded ASCII field [src, src+len) into out,
 * trimming leading/trailing spaces and NUL-terminating. out has len+1 bytes. */
static void copy_ascii_field(const uint8_t *src, unsigned len, char *out) {
    unsigned start = 0, end = len;
    unsigned i;
    while (start < len && src[start] == ' ') start++;
    while (end > start && src[end - 1] == ' ') end--;
    for (i = 0; start + i < end; i++) {
        out[i] = (char)src[start + i];
    }
    out[i] = '\0';
}

void hype_nvme_parse_identify_ctrl(const uint8_t idctrl[4096], char serial_out[21],
                                   char model_out[41]) {
    if (serial_out) copy_ascii_field(idctrl + 4, 20, serial_out);  /* SN */
    if (model_out)  copy_ascii_field(idctrl + 24, 40, model_out);  /* MN */
}

uint32_t hype_nvme_cap_dstrd(uint64_t cap) {
    return (uint32_t)((cap >> 32) & 0x0Fu);
}

uint32_t hype_nvme_doorbell_offset(uint32_t qid, int is_cq, uint32_t dstrd) {
    uint32_t stride = 4u << dstrd; /* bytes between adjacent doorbells */
    return HYPE_NVME_REG_DOORBELL_BASE + (2u * qid + (uint32_t)(is_cq ? 1 : 0)) * stride;
}
