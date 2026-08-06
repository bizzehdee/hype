#include "nvme.h"

void hype_nvme_reset(hype_nvme_t *dev) {
    unsigned int q;

    dev->cc = 0;
    dev->csts = 0; /* not ready: the guest must set CC.EN first */
    dev->aqa = 0;
    dev->asq = 0;
    dev->acq = 0;
    for (q = 0; q < HYPE_NVME_MAX_QUEUES; q++) {
        dev->sq_tail[q] = 0;
        dev->cq_head[q] = 0;
        dev->cq_tail[q] = 0;
        /* See the header: phase MUST start at 1, or the first completion is invisible to a driver
         * polling a zeroed queue. */
        dev->cq_phase[q] = 1u;
    }
}

uint32_t hype_nvme_mmio_read32(const hype_nvme_t *dev, uint32_t off) {
    switch (off) {
        case HYPE_NVME_REG_CAP:
            /*
             * CAP low dword: MQES in 15:0 (entries MINUS ONE), CQR (bit 16) set because hype requires
             * physically contiguous queues, DSTRD in 35:32 -- i.e. the high dword.
             */
            return (uint32_t)HYPE_NVME_CAP_MQES | (1u << 16);
        case HYPE_NVME_REG_CAP + 4u:
            /* DSTRD is bits 35:32 of CAP, so bits 3:0 here. Zero => 4-byte stride. */
            return (uint32_t)HYPE_NVME_CAP_DSTRD;
        case HYPE_NVME_REG_VS:
            return 0x00010400u; /* 1.4.0 */
        case HYPE_NVME_REG_CC:
            return dev->cc;
        case HYPE_NVME_REG_CSTS:
            return dev->csts;
        case HYPE_NVME_REG_AQA:
            return dev->aqa;
        case HYPE_NVME_REG_ASQ:
            return (uint32_t)dev->asq;
        case HYPE_NVME_REG_ASQ + 4u:
            return (uint32_t)(dev->asq >> 32);
        case HYPE_NVME_REG_ACQ:
            return (uint32_t)dev->acq;
        case HYPE_NVME_REG_ACQ + 4u:
            return (uint32_t)(dev->acq >> 32);
        default:
            /* Unmodelled: read 0 rather than fault. A driver probing optional features must not be
             * able to take the VM down (same reasoning as GLADDER-1 absorbing unhandled MMIO). */
            return 0u;
    }
}

void hype_nvme_mmio_write32(hype_nvme_t *dev, uint32_t off, uint32_t value) {
    switch (off) {
        case HYPE_NVME_REG_CC:
            dev->cc = value;
            /*
             * CC.EN is the enable handshake: the driver sets it and then polls CSTS.RDY. Clearing it is
             * a controller RESET, which must drop every queue index -- a driver that resets and
             * re-creates queues would otherwise inherit stale tails and read the wrong entries.
             */
            if ((value & HYPE_NVME_CC_EN) != 0u) {
                dev->csts |= HYPE_NVME_CSTS_RDY;
            } else {
                unsigned int q;
                dev->csts &= ~(uint32_t)HYPE_NVME_CSTS_RDY;
                for (q = 0; q < HYPE_NVME_MAX_QUEUES; q++) {
                    dev->sq_tail[q] = 0;
                    dev->cq_head[q] = 0;
                    dev->cq_tail[q] = 0;
                    dev->cq_phase[q] = 1u; /* a fresh queue is polled against phase 1 again */
                }
            }
            return;
        case HYPE_NVME_REG_AQA:
            dev->aqa = value;
            return;
        case HYPE_NVME_REG_ASQ:
            dev->asq = (dev->asq & 0xFFFFFFFF00000000ull) | (uint64_t)value;
            return;
        case HYPE_NVME_REG_ASQ + 4u:
            dev->asq = (dev->asq & 0xFFFFFFFFull) | ((uint64_t)value << 32);
            return;
        case HYPE_NVME_REG_ACQ:
            dev->acq = (dev->acq & 0xFFFFFFFF00000000ull) | (uint64_t)value;
            return;
        case HYPE_NVME_REG_ACQ + 4u:
            dev->acq = (dev->acq & 0xFFFFFFFFull) | ((uint64_t)value << 32);
            return;
        case HYPE_NVME_REG_INTMS:
        case HYPE_NVME_REG_INTMC:
            /* Accepted and ignored: #335 established that OVMF never reads these, and hype delivers no
             * interrupt, so modelling a mask would be state nothing consults. */
            return;
        default:
            break;
    }

    { /* Doorbells. */
        unsigned int qid;
        int is_cq;
        if (hype_nvme_doorbell_decode(off, &qid, &is_cq) == 0) {
            /*
             * VALID-3: the INDEX the guest writes is bounds-checked here, not where it is used. A tail
             * beyond the queue would otherwise walk off the end of guest-supplied queue memory.
             */
            if (value >= HYPE_NVME_QUEUE_ENTRIES) {
                return; /* refuse: an out-of-range index is a guest bug, not something to wrap */
            }
            if (is_cq) {
                dev->cq_head[qid] = value;
            } else {
                dev->sq_tail[qid] = value;
            }
        }
    }
}

uint8_t hype_nvme_cq_advance(hype_nvme_t *dev, unsigned int qid) {
    uint8_t phase;

    if (qid >= HYPE_NVME_MAX_QUEUES) {
        return 1u;
    }
    /* The phase stamped on THIS entry is the current one; the toggle happens on wrap, after. */
    phase = dev->cq_phase[qid];
    dev->cq_tail[qid]++;
    if (dev->cq_tail[qid] >= HYPE_NVME_QUEUE_ENTRIES) {
        dev->cq_tail[qid] = 0;
        dev->cq_phase[qid] ^= 1u; /* toggle ONLY on wrap -- see the header for why */
    }
    return phase;
}

int hype_nvme_doorbell_decode(uint32_t off, unsigned int *out_qid, int *out_is_cq) {
    uint32_t rel, idx;

    if (out_qid == 0 || out_is_cq == 0 || off < HYPE_NVME_REG_DOORBELL_BASE) {
        return -1;
    }
    rel = off - HYPE_NVME_REG_DOORBELL_BASE;
    if ((rel % HYPE_NVME_DOORBELL_STRIDE) != 0u) {
        return -1; /* misaligned: refuse rather than round down onto a neighbouring doorbell */
    }
    idx = rel / HYPE_NVME_DOORBELL_STRIDE;
    /* Doorbells alternate SQ, CQ, SQ, CQ... one pair per queue. */
    *out_qid = idx / 2u;
    *out_is_cq = (int)(idx & 1u);
    if (*out_qid >= HYPE_NVME_MAX_QUEUES) {
        return -1;
    }
    return 0;
}

/* ---- slice 2: admin commands (#202) ---------------------------------------------------------- */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32b(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_le64b(uint8_t *p, uint64_t v) {
    put_le32b(p, (uint32_t)v);
    put_le32b(p + 4, (uint32_t)(v >> 32));
}

static void zero_buf(uint8_t *p, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        p[i] = 0;
    }
}

/* Fixed-width ASCII field, SPACE padded (NVMe 1.4 §5.15.2.1) -- not NUL padded. */
static void ascii_field(uint8_t *dst, unsigned int width, const char *src) {
    unsigned int i;
    for (i = 0; i < width; i++) {
        dst[i] = (src != 0 && src[i] != '\0' && i < width) ? (uint8_t)src[i] : (uint8_t)' ';
        if (src != 0 && src[i] == '\0') {
            src = 0; /* pad the remainder */
        }
    }
}

void hype_nvme_sqe_decode(const uint8_t sqe[HYPE_NVME_SQE_BYTES], hype_nvme_cmd_t *out) {
    uint32_t cdw0 = le32(sqe + 0);

    out->opcode = (uint8_t)(cdw0 & 0xFFu);
    out->cid = (uint16_t)(cdw0 >> 16);
    out->nsid = le32(sqe + 4);
    out->prp1 = le64(sqe + 24);
    out->prp2 = le64(sqe + 32);
    out->cdw10 = le32(sqe + 40);
    out->cdw11 = le32(sqe + 44);
    out->cdw12 = le32(sqe + 48);
}

void hype_nvme_cqe_build(uint8_t cqe[HYPE_NVME_CQE_BYTES], uint16_t cid, uint16_t sq_head,
                         uint16_t sqid, uint8_t phase, uint16_t status) {
    zero_buf(cqe, HYPE_NVME_CQE_BYTES);
    /* DW0/DW1: command-specific result. Zero for everything hype implements. */
    put_le16(cqe + 8, sq_head);
    put_le16(cqe + 10, sqid);
    put_le16(cqe + 12, cid);
    /*
     * DW3 high half: status field with the PHASE TAG in bit 0. The status code sits at bits 1..8 of
     * that half-word (SC in 8:1, SCT in 11:9), so it is shifted left by one -- forgetting that shift
     * puts the status where the phase belongs and makes every completion look like the wrong phase,
     * which a polling driver cannot see at all.
     */
    put_le16(cqe + 14, (uint16_t)((status << 1) | (phase & 1u)));
}

void hype_nvme_identify_controller(uint8_t buf[HYPE_NVME_IDENTIFY_BYTES], const char *serial) {
    zero_buf(buf, HYPE_NVME_IDENTIFY_BYTES);
    put_le16(buf + 0, 0x1AF4u);               /* VID: virtio/Red Hat range, as hype's other models use */
    put_le16(buf + 2, 0x1AF4u);               /* SSVID */
    ascii_field(buf + 4, 20u, serial);        /* SN  */
    ascii_field(buf + 24, 40u, "hype virtual NVMe");  /* MN */
    ascii_field(buf + 64, 8u, "1.0");         /* FR */
    /*
     * MDTS = 0: no maximum data transfer size. hype splits transfers itself, and advertising a limit
     * it does not need would only invite a driver to fragment more than necessary.
     */
    buf[77] = 0u;
    put_le32b(buf + 516, 1u);                 /* NN: exactly one namespace */
    /*
     * SQES/CQES: (max << 4) | min, each as a POWER OF TWO. 0x66 => 2^6 = 64-byte SQ entries; 0x44 =>
     * 2^4 = 16-byte CQ entries. These must agree with what the doorbell/queue arithmetic assumes, or
     * the controller and driver disagree about where entry N begins.
     */
    buf[512] = 0x66u;
    buf[513] = 0x44u;
}

void hype_nvme_identify_namespace(uint8_t buf[HYPE_NVME_IDENTIFY_BYTES], uint64_t total_sectors) {
    zero_buf(buf, HYPE_NVME_IDENTIFY_BYTES);
    put_le64b(buf + 0, total_sectors);  /* NSZE: size in blocks */
    put_le64b(buf + 8, total_sectors);  /* NCAP: capacity */
    put_le64b(buf + 16, total_sectors); /* NUSE: fully provisioned -- hype does not thin-provision */
    buf[25] = 0u;                       /* NLBAF: one format described (0 == "1 format") */
    buf[26] = 0u;                       /* FLBAS: format 0 in use */
    /*
     * LBAF0 at offset 128: MS in 15:0, LBADS (a POWER OF TWO) at byte 2, RP in 25:24.
     * LBADS = 9 => 512-byte blocks, matching hype_blk_backend's sector size everywhere else.
     */
    put_le16(buf + 128, 0u);
    buf[130] = 9u;
}
