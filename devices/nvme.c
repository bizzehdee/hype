#include "nvme.h"

/*
 * #202: an optional one-shot notification that a guest enabled the controller. Defined weakly so this
 * module stays pure and unit-testable with no logging dependency -- boot/main.c overrides it.
 */
__attribute__((weak)) void hype_nvme_report_enabled(const hype_nvme_t *dev) {
    (void)dev;
}

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
        dev->sq_head[q] = 0;
        /* Until the guest declares a size, assume the maximum hype advertises. A guest that never sets
         * AQA is malformed, but defaulting to the advertised MQES keeps the arithmetic in range rather
         * than dividing by zero. */
        dev->sq_entries[q] = HYPE_NVME_QUEUE_ENTRIES;
        dev->cq_entries[q] = HYPE_NVME_QUEUE_ENTRIES;
        dev->io_sq_base[q] = 0; /* #358: per-queue; index 0 unused (admin uses asq/acq) */
        dev->io_cq_base[q] = 0;
    }
    /* #372: permissive default, overridden by the live path -- see the field comment. */
    dev->bus_master = 1;
}

void hype_nvme_set_bus_master(hype_nvme_t *dev, int enabled) {
    if (dev == 0) {
        return;
    }
    dev->bus_master = (enabled != 0) ? 1 : 0;
}

uint32_t hype_nvme_mmio_read32(const hype_nvme_t *dev, uint32_t off) {
    switch (off) {
        case HYPE_NVME_REG_CAP:
            /*
             * CAP low dword: MQES in 15:0 (entries MINUS ONE), CQR (bit 16) set because hype requires
             * physically contiguous queues, TO in 31:24, DSTRD in 35:32 -- i.e. the high dword.
             */
            return (uint32_t)HYPE_NVME_CAP_MQES | (1u << 16) |
                   ((uint32_t)HYPE_NVME_CAP_TO << 24);
        case HYPE_NVME_REG_CAP + 4u:
            /* High dword: DSTRD in 3:0 (zero => 4-byte stride), CSS in 12:5. CSS bit 0 says the
             * NVM command set is supported, without which EDK2 rejects the controller (#358). */
            return (uint32_t)HYPE_NVME_CAP_DSTRD | (uint32_t)HYPE_NVME_CAP_CSS_NVM;
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
                /*
                 * #202: a guest driver setting CC.EN is the moment it has BOUND this controller -- it is
                 * the only externally visible proof that enumeration succeeded, and per #335 the failure
                 * mode without it is a silent spin to timeout rather than an error. Reported once so a
                 * boot log answers "did the driver find it" without tracing every register.
                 */
                if ((dev->csts & HYPE_NVME_CSTS_RDY) == 0u) {
                    hype_nvme_report_enabled(dev);
                }
                dev->csts |= HYPE_NVME_CSTS_RDY;
            } else {
                unsigned int q;
                dev->csts &= ~(uint32_t)HYPE_NVME_CSTS_RDY;
                for (q = 0; q < HYPE_NVME_MAX_QUEUES; q++) {
                    dev->sq_tail[q] = 0;
                    dev->cq_head[q] = 0;
                    dev->cq_tail[q] = 0;
                    dev->cq_phase[q] = 1u; /* a fresh queue is polled against phase 1 again */
                    dev->sq_head[q] = 0;
                    dev->sq_entries[q] = HYPE_NVME_QUEUE_ENTRIES;
                    dev->cq_entries[q] = HYPE_NVME_QUEUE_ENTRIES;
                }
                {
                    unsigned int qi;
                    for (qi = 0; qi < HYPE_NVME_MAX_QUEUES; qi++) {
                        dev->io_sq_base[qi] = 0;
                        dev->io_cq_base[qi] = 0;
                    }
                }
            }
            return;
        case HYPE_NVME_REG_AQA:
            dev->aqa = value;
            /*
             * #202 slice 6c: AQA is where the guest declares its ADMIN queue sizes -- ASQS in bits 11:0,
             * ACQS in 27:16, both ZERO-BASED. Storing the register without acting on it (which is what
             * this did until 6c) leaves hype wrapping at its own maximum and walking off the end of a
             * smaller queue.
             */
            {
                uint32_t asqs = (value & 0xFFFu) + 1u;
                uint32_t acqs = ((value >> 16) & 0xFFFu) + 1u;
                /* Clamp rather than trust: a guest may legally ask for fewer than hype supports, but
                 * never for more than CAP.MQES advertises. */
                dev->sq_entries[0] = (asqs > HYPE_NVME_QUEUE_ENTRIES) ? HYPE_NVME_QUEUE_ENTRIES : asqs;
                dev->cq_entries[0] = (acqs > HYPE_NVME_QUEUE_ENTRIES) ? HYPE_NVME_QUEUE_ENTRIES : acqs;
            }
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
            /*
             * #202 slice 6c: bounded against THIS queue's declared size, not the compile-time maximum.
             * Checking the constant let a guest with a 16-entry queue set a tail of 60 -- in range for
             * hype, far outside the memory the guest actually allocated.
             */
            if (value >= (is_cq ? dev->cq_entries[qid] : dev->sq_entries[qid])) {
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
    /* Wrap where the GUEST's queue ends (6c), not where hype's maximum does. */
    if (dev->cq_tail[qid] >= dev->cq_entries[qid]) {
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

/* ---- slice 3: PRP walking (#202) -------------------------------------------------------------- */

int hype_nvme_prp_init(hype_nvme_prp_iter_t *it, uint64_t prp1, uint64_t prp2, uint64_t total_len,
                       uint32_t page_size) {
    uint64_t first_len;

    if (it == 0 || total_len == 0u || page_size == 0u || (page_size & (page_size - 1u)) != 0u) {
        return -1;
    }
    it->prp1 = prp1;
    it->prp2 = prp2;
    it->page_size = page_size;
    it->remaining = total_len;
    it->first = 1;
    it->list_gpa = 0;
    it->list_left = 0;

    /*
     * PRP1 covers from its offset to the end of its page. Whether PRP2 is a data page or a LIST follows
     * from what is left after that -- decided once, here, so no segment has to guess.
     */
    first_len = (uint64_t)page_size - (prp1 & ((uint64_t)page_size - 1u));
    if (first_len > total_len) {
        first_len = total_len;
    }
    it->using_list = ((total_len - first_len) > (uint64_t)page_size) ? 1 : 0;
    if (it->using_list) {
        it->list_gpa = prp2;
        /* Entries per list page, minus one: the LAST slot chains to the next list rather than
         * describing data. */
        it->list_left = (page_size / 8u) - 1u;
    }
    return 0;
}

int hype_nvme_prp_next(hype_nvme_prp_iter_t *it, hype_nvme_guest_read_fn read_fn, void *ctx,
                       uint64_t *out_gpa, uint32_t *out_len) {
    uint64_t page_mask;
    uint64_t gpa;
    uint64_t seg;

    if (it == 0 || out_gpa == 0 || out_len == 0) {
        return -1;
    }
    if (it->remaining == 0u) {
        return 0;
    }
    page_mask = (uint64_t)it->page_size - 1u;

    if (it->first) {
        it->first = 0;
        gpa = it->prp1;
        seg = (uint64_t)it->page_size - (gpa & page_mask); /* only PRP1 may be offset into its page */
        if (seg > it->remaining) {
            seg = it->remaining;
        }
        it->remaining -= seg;
        *out_gpa = gpa;
        *out_len = (uint32_t)seg;
        return 1;
    }

    if (!it->using_list) {
        /* PRP2 IS the second (and final) data page for a transfer this small. */
        gpa = it->prp2;
        if ((gpa & page_mask) != 0u) {
            return -1; /* continuation PRPs must be page-aligned -- refuse, do not mask */
        }
        seg = it->remaining;
        if (seg > (uint64_t)it->page_size) {
            return -1; /* would mean init mis-decided; fail loudly rather than truncate */
        }
        it->remaining = 0;
        *out_gpa = gpa;
        *out_len = (uint32_t)seg;
        return 1;
    }

    /* Walking a PRP list: read one 8-byte entry at a time from guest memory. */
    if (read_fn == 0) {
        return -1;
    }
    if (it->list_left == 0u) {
        /*
         * The last slot of a list page chains to the next list page. Follow it rather than treating it
         * as data -- doing the latter would write file contents over the guest's own PRP list.
         */
        uint8_t chain[8];
        if (read_fn(ctx, it->list_gpa, 8u, chain) != 0) {
            return -1;
        }
        it->list_gpa = le64(chain);
        if ((it->list_gpa & page_mask) != 0u) {
            return -1;
        }
        it->list_left = (it->page_size / 8u) - 1u;
    }
    {
        uint8_t ent[8];
        if (read_fn(ctx, it->list_gpa, 8u, ent) != 0) {
            return -1;
        }
        gpa = le64(ent);
        if ((gpa & page_mask) != 0u) {
            return -1;
        }
        it->list_gpa += 8u;
        it->list_left--;
        seg = it->remaining;
        if (seg > (uint64_t)it->page_size) {
            seg = (uint64_t)it->page_size;
        }
        it->remaining -= seg;
        *out_gpa = gpa;
        *out_len = (uint32_t)seg;
        return 1;
    }
}

/* ---- slice 4: I/O READ/WRITE (#202) ------------------------------------------------------------ */

uint16_t hype_nvme_exec_io(const hype_nvme_cmd_t *cmd, hype_blk_backend_t *be,
                           uint64_t total_sectors, uint32_t page_size,
                           hype_nvme_guest_read_fn gread, hype_nvme_guest_write_fn gwrite,
                           void *gctx, uint8_t *bounce, uint32_t bounce_len) {
    hype_nvme_prp_iter_t it;
    uint64_t slba;
    uint32_t nlb;
    uint64_t total_bytes;
    uint64_t sectors_done = 0;
    int is_write;

    if (cmd == 0 || be == 0 || bounce == 0 || bounce_len < page_size) {
        return HYPE_NVME_SC_INVALID_FIELD;
    }
    if (cmd->opcode == HYPE_NVME_IO_WRITE) {
        is_write = 1;
    } else if (cmd->opcode == HYPE_NVME_IO_READ) {
        is_write = 0;
    } else {
        return HYPE_NVME_SC_INVALID_OPCODE;
    }

    /* SLBA is 64-bit across CDW10/CDW11 -- CDW10 alone is right until the disk exceeds 2 TiB. */
    slba = (uint64_t)cmd->cdw10 | ((uint64_t)cmd->cdw11 << 32);
    /* NLB is ZERO-BASED: 0 means one block. */
    nlb = (cmd->cdw12 & 0xFFFFu) + 1u;

    /* VALID-3: the range is checked against the backend's REAL size before any access. */
    if (slba > total_sectors || (uint64_t)nlb > total_sectors - slba) {
        return HYPE_NVME_SC_LBA_OUT_OF_RANGE;
    }
    total_bytes = (uint64_t)nlb * HYPE_BLK_SECTOR_SIZE;

    if (hype_nvme_prp_init(&it, cmd->prp1, cmd->prp2, total_bytes, page_size) != 0) {
        return HYPE_NVME_SC_INVALID_FIELD;
    }

    for (;;) {
        uint64_t gpa;
        uint32_t seg;
        uint32_t seg_sectors;
        int rc = hype_nvme_prp_next(&it, gread, gctx, &gpa, &seg);

        if (rc == 0) {
            break; /* transfer complete */
        }
        if (rc < 0) {
            return HYPE_NVME_SC_DATA_XFER_ERROR;
        }
        /*
         * hype transfers whole sectors. A segment that is not a sector multiple would mean the guest
         * pointed PRP1 at a non-sector-aligned offset; REFUSE rather than split across a sector
         * boundary by hand, because a wrong split writes the right bytes to the wrong place and
         * nothing reports it. Real drivers align their buffers, so this refuses only the malformed
         * case.
         */
        if ((seg % HYPE_BLK_SECTOR_SIZE) != 0u || seg > bounce_len) {
            return HYPE_NVME_SC_INVALID_FIELD;
        }
        seg_sectors = seg / HYPE_BLK_SECTOR_SIZE;

        if (is_write) {
            if (gread == 0 || gread(gctx, gpa, seg, bounce) != 0) {
                return HYPE_NVME_SC_DATA_XFER_ERROR;
            }
            if (hype_blk_backend_write(be, slba + sectors_done, seg_sectors, bounce) != 0) {
                return HYPE_NVME_SC_DATA_XFER_ERROR;
            }
        } else {
            if (hype_blk_backend_read(be, slba + sectors_done, seg_sectors, bounce) != 0) {
                return HYPE_NVME_SC_DATA_XFER_ERROR;
            }
            if (gwrite == 0 || gwrite(gctx, gpa, seg, bounce) != 0) {
                return HYPE_NVME_SC_DATA_XFER_ERROR;
            }
        }
        sectors_done += seg_sectors;
    }

    /*
     * The PRP walk and the block count must agree. If they do not, the descriptor described a different
     * amount of memory than the command asked to transfer -- report it rather than declaring success on
     * a partial transfer, which a driver would read as good data.
     */
    if (sectors_done != (uint64_t)nlb) {
        return HYPE_NVME_SC_DATA_XFER_ERROR;
    }
    return HYPE_NVME_SC_SUCCESS;
}

/* ---- slice 5: the command processor (#202) ------------------------------------------------------ */

/* Where queue `qid` lives. Queue 0 is the admin pair (ASQ/ACQ); queue 1 is the I/O pair recorded by
 * CREATE_IO_SQ/CQ. Returning the base rather than indexing an array keeps the admin special case in one
 * place instead of at every use. */
static uint64_t sq_base_of(const hype_nvme_t *dev, unsigned int qid) {
    return (qid == 0u) ? dev->asq : dev->io_sq_base[qid];
}

static uint64_t cq_base_of(const hype_nvme_t *dev, unsigned int qid) {
    return (qid == 0u) ? dev->acq : dev->io_cq_base[qid];
}

/* Executes one ADMIN command, returning its NVMe status. */
static uint16_t exec_admin(hype_nvme_t *dev, const hype_nvme_cmd_t *cmd, const hype_nvme_ctx_t *c) {
    switch (cmd->opcode) {
        case HYPE_NVME_ADMIN_IDENTIFY: {
            uint32_t cns = cmd->cdw10 & 0xFFu;
            if (c->gwrite == 0 || c->bounce == 0 || c->bounce_len < HYPE_NVME_IDENTIFY_BYTES) {
                return HYPE_NVME_SC_INVALID_FIELD;
            }
            if (cns == HYPE_NVME_CNS_CONTROLLER) {
                hype_nvme_identify_controller(c->bounce, c->serial);
            } else if (cns == HYPE_NVME_CNS_NAMESPACE) {
                hype_nvme_identify_namespace(c->bounce, c->total_sectors);
            } else {
                /* An unsupported CNS is a FIELD error, not an opcode error: the driver asked a valid
                 * question hype cannot answer, and it distinguishes the two. */
                return HYPE_NVME_SC_INVALID_FIELD;
            }
            if (c->gwrite(c->gctx, cmd->prp1, HYPE_NVME_IDENTIFY_BYTES, c->bounce) != 0) {
                return HYPE_NVME_SC_DATA_XFER_ERROR;
            }
            return HYPE_NVME_SC_SUCCESS;
        }
        case HYPE_NVME_ADMIN_CREATE_IO_CQ:
            /* CDW10 low 16 = queue id. Only the one I/O pair is modelled, so anything else is refused
             * rather than silently aliased onto it. */
            {
                uint32_t qid = cmd->cdw10 & 0xFFFFu;
                if (qid == 0u || qid >= HYPE_NVME_MAX_QUEUES) {
                    return HYPE_NVME_SC_INVALID_FIELD;
                }
                dev->io_cq_base[qid] = cmd->prp1;
            }
            /* QSIZE is CDW10 bits 31:16, zero-based -- the I/O counterpart of AQA. Ignoring it was the
             * same defect. */
            {
                uint32_t qid2 = cmd->cdw10 & 0xFFFFu;
                uint32_t qs = ((cmd->cdw10 >> 16) & 0xFFFFu) + 1u;
                dev->cq_entries[qid2] = (qs > HYPE_NVME_QUEUE_ENTRIES) ? HYPE_NVME_QUEUE_ENTRIES : qs;
            }
            return HYPE_NVME_SC_SUCCESS;
        case HYPE_NVME_ADMIN_CREATE_IO_SQ:
            {
                uint32_t qid = cmd->cdw10 & 0xFFFFu;
                if (qid == 0u || qid >= HYPE_NVME_MAX_QUEUES) {
                    return HYPE_NVME_SC_INVALID_FIELD;
                }
                dev->io_sq_base[qid] = cmd->prp1;
            }
            {
                uint32_t qid2 = cmd->cdw10 & 0xFFFFu;
                uint32_t qs = ((cmd->cdw10 >> 16) & 0xFFFFu) + 1u;
                dev->sq_entries[qid2] = (qs > HYPE_NVME_QUEUE_ENTRIES) ? HYPE_NVME_QUEUE_ENTRIES : qs;
            }
            return HYPE_NVME_SC_SUCCESS;
        case HYPE_NVME_ADMIN_SET_FEATURES:
        case HYPE_NVME_ADMIN_GET_FEATURES:
            /* Accepted so a driver's setup sequence completes. hype has no feature whose value changes
             * behaviour, so storing them would be state nothing consults. */
            return HYPE_NVME_SC_SUCCESS;
        default:
            return HYPE_NVME_SC_INVALID_OPCODE;
    }
}

int hype_nvme_process_sq(hype_nvme_t *dev, unsigned int qid, const hype_nvme_ctx_t *c) {
    int processed = 0;
    uint64_t sqb, cqb;

    if (dev == 0 || c == 0 || qid >= HYPE_NVME_MAX_QUEUES || c->gread == 0 || c->gwrite == 0) {
        return -1;
    }
    /* A doorbell written before CC.EN is not a command to run: the queues are not valid yet. */
    if ((dev->csts & HYPE_NVME_CSTS_RDY) == 0u) {
        return -1;
    }
    /*
     * #372: a controller that cannot master the bus cannot fetch an SQE, walk a PRP or post a CQE.
     *
     * Returns 0, not -1: -1 here means "malformed", and this is not malformed -- it is a guest
     * asking for something the hardware silently declines. Nothing is consumed and no completion is
     * posted, so the driver waits forever, which is the behaviour being modelled.
     */
    if (dev->bus_master == 0) {
        return 0;
    }
    sqb = sq_base_of(dev, qid);
    cqb = cq_base_of(dev, qid);
    if (sqb == 0u || cqb == 0u) {
        return -1; /* the guest has not told hype where this queue is */
    }

    while (dev->sq_head[qid] != dev->sq_tail[qid]) {
        uint8_t sqe[HYPE_NVME_SQE_BYTES];
        uint8_t cqe[HYPE_NVME_CQE_BYTES];
        hype_nvme_cmd_t cmd;
        uint16_t status;
        uint8_t phase;
        uint64_t cqe_gpa;

        if (c->gread(c->gctx, sqb + (uint64_t)dev->sq_head[qid] * HYPE_NVME_SQE_BYTES,
                     HYPE_NVME_SQE_BYTES, sqe) != 0) {
            /* Cannot even fetch the command; stop rather than advance past it. Advancing would lose the
             * command silently, and the guest would wait forever for a completion. */
            return processed;
        }
        hype_nvme_sqe_decode(sqe, &cmd);

        /*
         * Consume the entry BEFORE executing it. The sq_head reported in the completion must reflect
         * that this slot is free, and doing it after would let a driver see a completion while still
         * believing its queue full.
         */
        dev->sq_head[qid]++;
        if (dev->sq_head[qid] >= dev->sq_entries[qid]) {
            dev->sq_head[qid] = 0;
        }

        if (qid == 0u) {
            status = exec_admin(dev, &cmd, c);
        } else {
            status = hype_nvme_exec_io(&cmd, c->be, c->total_sectors, c->page_size, c->gread,
                                       c->gwrite, c->gctx, c->bounce, c->bounce_len);
        }

        /* Post the completion. The phase comes from the advance, so it is stamped by the one function
         * that owns the wrap rule. */
        cqe_gpa = cqb + (uint64_t)dev->cq_tail[qid] * HYPE_NVME_CQE_BYTES;
        phase = hype_nvme_cq_advance(dev, qid);
        hype_nvme_cqe_build(cqe, cmd.cid, (uint16_t)dev->sq_head[qid], (uint16_t)qid, phase, status);
        if (c->gwrite(c->gctx, cqe_gpa, HYPE_NVME_CQE_BYTES, cqe) != 0) {
            return processed; /* the guest's CQ is unreachable -- nothing useful left to do */
        }
        processed++;
    }
    return processed;
}
