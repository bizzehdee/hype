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
