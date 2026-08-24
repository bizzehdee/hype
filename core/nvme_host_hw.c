#include "nvme_host.h"
#include "host_pci_dma.h"
#include "ticket_lock.h" /* #660: fair serialisation around the shared queues/bounce buffer */

/*
 * Hardware shim for the host NVMe driver: real MMIO + DMA against the physical
 * controller. Coverage-exempt (like ahci_host_hw.c / host_pci_hw.c) since it
 * pokes device registers and rings doorbells. The pure command/completion
 * encode-decode it builds on lives in nvme_host.c and is unit-tested.
 *
 * #426: SQ-tail advance and CQ-head/phase-flip-on-wrap now go through
 * core/host_pci_dma.c's shared ring helpers instead of hand-rolled `%
 * Q_ENTRIES` arithmetic -- same math, moved so xHCI/AHCI/future NIC drivers
 * share it instead of re-deriving it. Nothing about queue depth, doorbell
 * offsets or the phase-tag protocol itself changes.
 *
 * Identity-mapped physical == pointer, per hype's flat map. All DMA-visible
 * structures live in hype's own .bss. NVMe transfers are page/PRP based, so a
 * read is bounced through g_bounce (page-aligned, contiguous) and then copied
 * to the caller's buffer -- this makes the driver correct for any caller
 * alignment/size without PRP math on the caller's pointer.
 */

#define Q_ENTRIES 8u                     /* admin + I/O queue depth */
#define BOUNCE_SECTORS 128u              /* 64 KiB, matches iso_stream's bounce */
#define BOUNCE_BYTES (BOUNCE_SECTORS * HYPE_NVME_SECTOR_SIZE)
#define PAGE 4096u

_Static_assert(BOUNCE_SECTORS == HYPE_NVME_WRITEV_MAX_SECTORS,
              "#715: hype_blk_phys_enable_writev()'s cap must match what g_bounce can hold");

static uint8_t g_admin_sq[Q_ENTRIES * HYPE_NVME_SQE_SIZE] __attribute__((aligned(PAGE)));
static uint8_t g_admin_cq[Q_ENTRIES * HYPE_NVME_CQE_SIZE] __attribute__((aligned(PAGE)));
static uint8_t g_io_sq[Q_ENTRIES * HYPE_NVME_SQE_SIZE] __attribute__((aligned(PAGE)));
static uint8_t g_io_cq[Q_ENTRIES * HYPE_NVME_CQE_SIZE] __attribute__((aligned(PAGE)));
static uint8_t g_id_buf[4096] __attribute__((aligned(PAGE)));
static uint8_t g_bounce[BOUNCE_BYTES] __attribute__((aligned(PAGE)));
static uint8_t g_prp_list[PAGE] __attribute__((aligned(PAGE)));

static uint32_t g_dstrd;
static uint16_t g_cid;

/* M10-6a: identity + capacity of the initialised namespace (see accessors). */
static char g_nvme_serial[21];
static char g_nvme_model[41];
static uint64_t g_nvme_total_sectors;
static unsigned g_admin_sq_tail, g_admin_cq_head, g_admin_phase;
static unsigned g_io_sq_tail, g_io_cq_head, g_io_phase;

/* #255: ~5x longer than the original 20M -- on the 2.1 GHz AMD laptop the old
 * window was only ~0.3s of PAUSE loops, marginal for a drive's first medium
 * access out of power-on. Only a FAILING command ever waits this long. */
#define SPIN 100000000u

static inline uint32_t rd32(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint32_t *)(b + off);
}
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(b + off) = v;
}
static inline uint64_t rd64(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint64_t *)(b + off);
}
static inline void wr64(volatile uint8_t *b, uint32_t off, uint64_t v) {
    *(volatile uint64_t *)(b + off) = v;
}
static uint64_t phys(const void *p) { return (uint64_t)(uintptr_t)p; }

/* Submit one entry to a queue, ring its tail doorbell, and poll the paired CQ
 * for the matching phase. Returns 0 on a successful completion, -1 otherwise. */
static int submit_and_poll(volatile uint8_t *bar, uint8_t *sq, uint8_t *cq, unsigned qid,
                           unsigned *sq_tail, unsigned *cq_head, unsigned *phase,
                           const uint8_t sqe[64]) {
    unsigned i;
    unsigned spins = SPIN;
    uint8_t *slot = sq + (*sq_tail) * HYPE_NVME_SQE_SIZE;
    uint8_t *centry;

    for (i = 0; i < HYPE_NVME_SQE_SIZE; i++) {
        slot[i] = sqe[i];
    }
    *sq_tail = hype_dma_ring_advance(*sq_tail, Q_ENTRIES);
    wr32(bar, hype_nvme_doorbell_offset(qid, 0, g_dstrd), *sq_tail);

    centry = cq + (*cq_head) * HYPE_NVME_CQE_SIZE;
    while (spins-- != 0u) {
        /* #229: hype busy-polls the CQ for a completion the HOST emulates
         * (QEMU NVMe). A tight spin can starve QEMU's device thread of a host
         * timeslice, so the CQE intermittently never posts within the window.
         * PAUSE both hints a spin-wait to the CPU and, under KVM pause-loop-
         * exiting, yields a lightweight #VMEXIT so the host can run the device
         * model and post the completion. */
        __asm__ volatile("pause");
        if (hype_nvme_cqe_phase(centry) == (int)(*phase)) {
            int ok = hype_nvme_cqe_success(centry);
            hype_dma_cqueue_advance(cq_head, phase, Q_ENTRIES); /* flips *phase on wrap */
            wr32(bar, hype_nvme_doorbell_offset(qid, 1, g_dstrd), *cq_head);
            if (!ok) {
                extern void hype_debug_print(const char *fmt, ...);
                static int e1 = 0;
                if (e1++ < 4) hype_debug_print("#229dbg nvme: CQ ERROR status (qid=%u)\n", qid);
            }
            return ok ? 0 : -1;
        }
    }
    {
        extern void hype_debug_print(const char *fmt, ...);
        static int t1 = 0;
        if (t1++ < 4) {
            /* Dump the polled CQE's status word + phase bit, plus CC/CSTS and
             * the SQ/CQ doorbell offsets. If the CQE status word is nonzero the
             * controller DID post (phase/head tracking bug); if it's zero the
             * controller never wrote it (doorbell/queue-setup issue). */
            uint16_t sw = (uint16_t)(centry[14] | (centry[15] << 8));
            hype_debug_print("#229dbg TIMEOUT qid=%u sq_tail=%u cq_head=%u want_phase=%u "
                             "cqe_sw=0x%04x cqe_phase=%u CC=0x%08x CSTS=0x%08x dstrd=%u "
                             "sqdb_off=0x%x cqdb_off=0x%x\n",
                             qid, *sq_tail, *cq_head, *phase, (unsigned)sw,
                             (unsigned)(sw & 1u), rd32(bar, HYPE_NVME_REG_CC),
                             rd32(bar, HYPE_NVME_REG_CSTS), g_dstrd,
                             hype_nvme_doorbell_offset(qid, 0, g_dstrd),
                             hype_nvme_doorbell_offset(qid, 1, g_dstrd));
        }
    }
    return -1; /* completion never posted */
}

int hype_nvme_host_init(uint64_t abar_phys) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)abar_phys;
    uint64_t cap = rd64(bar, HYPE_NVME_REG_CAP);
    uint8_t sqe[64];
    uint64_t total_blocks;
    uint32_t block_bytes;
    unsigned spins;
    unsigned i;

    g_dstrd = hype_nvme_cap_dstrd(cap);
    g_cid = 0;
    g_admin_sq_tail = g_admin_cq_head = 0;
    g_io_sq_tail = g_io_cq_head = 0;
    g_admin_phase = g_io_phase = 1; /* CQEs start with phase tag 1 */

    for (i = 0; i < sizeof(g_admin_cq); i++) { g_admin_cq[i] = 0; }
    for (i = 0; i < sizeof(g_io_cq); i++) { g_io_cq[i] = 0; }

    /* Disable, wait not-ready. */
    wr32(bar, HYPE_NVME_REG_CC, rd32(bar, HYPE_NVME_REG_CC) & ~HYPE_NVME_CC_EN);
    spins = SPIN;
    while ((rd32(bar, HYPE_NVME_REG_CSTS) & HYPE_NVME_CSTS_RDY) != 0u) {
        if (spins-- == 0u) { return -1; }
    }

    /* Admin queues: AQA holds (ACQS<<16)|ASQS as 0-based sizes. */
    wr32(bar, HYPE_NVME_REG_AQA, ((Q_ENTRIES - 1u) << 16) | (Q_ENTRIES - 1u));
    wr64(bar, HYPE_NVME_REG_ASQ, phys(g_admin_sq));
    wr64(bar, HYPE_NVME_REG_ACQ, phys(g_admin_cq));

    /* Enable: IOSQES=6 (64 B), IOCQES=4 (16 B), MPS=0 (4 KiB), CSS=0 (NVM), EN=1. */
    wr32(bar, HYPE_NVME_REG_CC, (6u << 16) | (4u << 20) | HYPE_NVME_CC_EN);
    spins = SPIN;
    while ((rd32(bar, HYPE_NVME_REG_CSTS) & HYPE_NVME_CSTS_RDY) == 0u) {
        if (spins-- == 0u) { return -1; }
    }

    /*
     * #255: NEGOTIATE the I/O queue count (Set Features, FID 0x07) BEFORE
     * creating any I/O queue. The spec expects this and every OS driver does
     * it; QEMU forgives skipping it, but the SK hynix 1c5c:1d59 in the AMD
     * laptop accepted Create I/O CQ/SQ without it and then never serviced the
     * I/O queue (CQE never posted, admin path fine -- see the ticket). The
     * completion's DWORD0 reports the ALLOCATED counts; we need 1+1, and a
     * controller allocating zero is a hard init failure, reported loudly.
     */
    {
        unsigned k;
        for (k = 0; k < 64u; k++) { sqe[k] = 0; }
        hype_nvme_build_set_num_queues_sqe(sqe, ++g_cid, 1u, 1u);
        if (submit_and_poll(bar, g_admin_sq, g_admin_cq, 0, &g_admin_sq_tail, &g_admin_cq_head,
                            &g_admin_phase, sqe) != 0) {
            extern void hype_debug_print(const char *fmt, ...);
            hype_debug_print("#255dbg nvme: Set Features (Number of Queues) FAILED\n");
            return -1;
        }
        {
            extern void hype_debug_print(const char *fmt, ...);
            /* DW0 of the completion we just consumed: cq_head already advanced,
             * so look one entry back (with wrap). */
            unsigned prev = (g_admin_cq_head + Q_ENTRIES - 1u) % Q_ENTRIES;
            uint32_t dw0 = hype_nvme_cqe_dw0(g_admin_cq + prev * HYPE_NVME_CQE_SIZE);
            hype_debug_print("#255dbg nvme: queues allocated NSQA=%u NCQA=%u (dw0=0x%08x)\n",
                             (unsigned)(dw0 & 0xFFFFu) + 1u, (unsigned)(dw0 >> 16) + 1u, dw0);
            if ((dw0 & 0xFFFFu) + 1u < 1u || (dw0 >> 16) + 1u < 1u) {
                return -1;
            }
        }
    }

    /* Create the I/O completion queue (qid 1), physically contiguous. */
    {
        unsigned k;
        for (k = 0; k < 64u; k++) { sqe[k] = 0; }
        sqe[0] = HYPE_NVME_ADM_CREATE_IO_CQ;
        sqe[2] = (uint8_t)(++g_cid);
        *(uint32_t *)(sqe + 24) = (uint32_t)phys(g_io_cq);
        *(uint32_t *)(sqe + 28) = (uint32_t)(phys(g_io_cq) >> 32);
        *(uint32_t *)(sqe + 40) = ((Q_ENTRIES - 1u) << 16) | 1u; /* CDW10: qsize|qid */
        *(uint32_t *)(sqe + 44) = 0x1u;                          /* CDW11: PC=1, IEN=0 */
        if (submit_and_poll(bar, g_admin_sq, g_admin_cq, 0, &g_admin_sq_tail, &g_admin_cq_head,
                            &g_admin_phase, sqe) != 0) {
            extern void hype_debug_print(const char *fmt, ...);
            hype_debug_print("#255dbg nvme: Create I/O CQ FAILED\n");
            return -1;
        }
    }
    /* Create the I/O submission queue (qid 1), bound to CQ 1. */
    {
        unsigned k;
        for (k = 0; k < 64u; k++) { sqe[k] = 0; }
        sqe[0] = HYPE_NVME_ADM_CREATE_IO_SQ;
        sqe[2] = (uint8_t)(++g_cid);
        *(uint32_t *)(sqe + 24) = (uint32_t)phys(g_io_sq);
        *(uint32_t *)(sqe + 28) = (uint32_t)(phys(g_io_sq) >> 32);
        *(uint32_t *)(sqe + 40) = ((Q_ENTRIES - 1u) << 16) | 1u; /* CDW10: qsize|qid */
        *(uint32_t *)(sqe + 44) = (1u << 16) | 0x1u;             /* CDW11: CQID=1, PC=1 */
        if (submit_and_poll(bar, g_admin_sq, g_admin_cq, 0, &g_admin_sq_tail, &g_admin_cq_head,
                            &g_admin_phase, sqe) != 0) {
            extern void hype_debug_print(const char *fmt, ...);
            hype_debug_print("#255dbg nvme: Create I/O SQ FAILED\n");
            return -1;
        }
    }
    /* IDENTIFY namespace 1 -> geometry. */
    hype_nvme_build_identify_sqe(sqe, ++g_cid, HYPE_NVME_CNS_NAMESPACE, 1u, phys(g_id_buf));
    if (submit_and_poll(bar, g_admin_sq, g_admin_cq, 0, &g_admin_sq_tail, &g_admin_cq_head,
                        &g_admin_phase, sqe) != 0) {
        return -1;
    }
    if (hype_nvme_parse_identify_ns(g_id_buf, &total_blocks, &block_bytes) != 0) {
        return -1;
    }
    if (block_bytes != HYPE_NVME_SECTOR_SIZE) {
        return -1; /* only 512-byte-block namespaces supported by the 512-sector callers */
    }
    g_nvme_total_sectors = total_blocks;

    /* M10-6a: IDENTIFY CONTROLLER (CNS=1) -> serial/model, for the `physical:`
     * NVMe target guard match. Non-fatal: a controller that refuses it just
     * leaves the identity empty (the guard then denies on serial mismatch). */
    g_nvme_serial[0] = '\0';
    g_nvme_model[0] = '\0';
    hype_nvme_build_identify_sqe(sqe, ++g_cid, HYPE_NVME_CNS_CONTROLLER, 0u, phys(g_id_buf));
    if (submit_and_poll(bar, g_admin_sq, g_admin_cq, 0, &g_admin_sq_tail, &g_admin_cq_head,
                        &g_admin_phase, sqe) == 0) {
        hype_nvme_parse_identify_ctrl(g_id_buf, g_nvme_serial, g_nvme_model);
    }
    return 0;
}

void hype_nvme_host_identity(char serial_out[21], char model_out[41]) {
    unsigned i;
    if (serial_out) {
        for (i = 0; i < sizeof(g_nvme_serial); i++) serial_out[i] = g_nvme_serial[i];
    }
    if (model_out) {
        for (i = 0; i < sizeof(g_nvme_model); i++) model_out[i] = g_nvme_model[i];
    }
}

uint64_t hype_nvme_host_total_sectors(void) {
    return g_nvme_total_sectors;
}

/*
 * #660: NVMe has the identical dual-use shape as AHCI (#343/#658) and USB (#346) -- a physical
 * host disk the BSP (media reads, install-to-physical) and a guest AP (a `physical:` NVMe disk
 * attached to a VM) can both reach -- but had NO serialisation at all around the shared I/O
 * queue (g_io_sq/g_io_cq/g_io_sq_tail/g_io_cq_head/g_io_phase/g_cid) or, worse, the single shared
 * g_bounce/g_prp_list DMA staging buffer every read/write copies through. Two calls in flight at
 * once could interleave into the same bounce buffer or reuse a submission slot before its
 * completion was consumed -- AHCI's #343 failure mode, unmitigated.
 *
 * Same fix as #658 (AHCI): the shared core/ticket_lock.c primitive, held for the WHOLE logical
 * transfer (every submit_and_poll() call the read/write loop makes), not just one command, since
 * the bounce buffer is reused across the loop's own iterations too. Guest AP callers wait
 * unbounded; the BSP gets a bounded claim so it can never freeze behind a stuck guest core.
 */
static volatile unsigned int g_nvme_ticket_next;
static volatile unsigned int g_nvme_ticket_owner;
static volatile unsigned int g_nvme_bsp_apic = 0xFFFFFFFFu;
static volatile unsigned long long g_nvme_bsp_lock_timeouts;
static volatile unsigned long long g_nvme_lock_contended; /* #660 (3): a waiter had to actually wait */

void hype_nvme_host_set_bsp_apic(unsigned int apic_id) { g_nvme_bsp_apic = apic_id; }

unsigned long long hype_nvme_host_bsp_lock_timeouts(void) { return g_nvme_bsp_lock_timeouts; }

unsigned long long hype_nvme_host_lock_contended(void) { return g_nvme_lock_contended; }

static unsigned int nvme_this_apic(void) {
    return (*(volatile uint32_t *)(uintptr_t)0xFEE00020u) >> 24;
}

#define NVME_BSP_LOCK_BUDGET 20000000u

static int nvme_host_lock_bounded(void) {
    unsigned int budget = NVME_BSP_LOCK_BUDGET;

    while (budget-- != 0u) {
        if (hype_ticket_lock_try_claim(&g_nvme_ticket_next, &g_nvme_ticket_owner)) {
            return 0;
        }
        g_nvme_lock_contended++;
        __asm__ volatile("pause");
    }
    g_nvme_bsp_lock_timeouts++;
    return -1;
}

static int nvme_host_lock_or_fail(void) {
    if (nvme_this_apic() == g_nvme_bsp_apic) {
        return nvme_host_lock_bounded();
    }
    if (__atomic_load_n(&g_nvme_ticket_owner, __ATOMIC_ACQUIRE) !=
        __atomic_load_n(&g_nvme_ticket_next, __ATOMIC_ACQUIRE)) {
        g_nvme_lock_contended++;
    }
    hype_ticket_lock_acquire(&g_nvme_ticket_next, &g_nvme_ticket_owner);
    return 0;
}

static void nvme_host_unlock(void) {
    hype_ticket_lock_release(&g_nvme_ticket_owner);
}

static int nvme_host_read_locked(uint64_t abar_phys, uint64_t lba, uint16_t count, void *dst) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)abar_phys;
    uint8_t *out = (uint8_t *)dst;
    uint16_t done = 0;

    while (done < count) {
        uint16_t nsec = (uint16_t)((count - done > BOUNCE_SECTORS) ? BOUNCE_SECTORS
                                                                   : (count - done));
        uint32_t bytes = (uint32_t)nsec * HYPE_NVME_SECTOR_SIZE;
        uint32_t pages = (bytes + PAGE - 1u) / PAGE;
        uint64_t prp1 = phys(g_bounce);
        uint64_t prp2 = 0;
        uint8_t sqe[64];
        uint32_t i;

        if (pages == 2u) {
            prp2 = phys(g_bounce) + PAGE;
        } else if (pages > 2u) {
            for (i = 0; i + 1u < pages; i++) {
                *(uint64_t *)(g_prp_list + i * 8u) = phys(g_bounce) + (uint64_t)(i + 1u) * PAGE;
            }
            prp2 = phys(g_prp_list);
        }

        hype_nvme_build_read_sqe(sqe, ++g_cid, 1u, lba + done, (uint16_t)(nsec - 1u), prp1, prp2);
        if (submit_and_poll(bar, g_io_sq, g_io_cq, 1, &g_io_sq_tail, &g_io_cq_head, &g_io_phase,
                            sqe) != 0) {
            return -1;
        }
        for (i = 0; i < bytes; i++) {
            out[i] = g_bounce[i];
        }
        out += bytes;
        done = (uint16_t)(done + nsec);
    }
    return 0;
}

int hype_nvme_host_read(uint64_t abar_phys, uint64_t lba, uint16_t count, void *dst) {
    int rc;
    if (nvme_host_lock_or_fail() != 0) {
        return -1;
    }
    rc = nvme_host_read_locked(abar_phys, lba, count, dst);
    nvme_host_unlock();
    return rc;
}

/* M10-1c (#197): mirror of hype_nvme_host_read with a WRITE (0x01) SQE and the
 * copy direction reversed -- src is staged into the DMA bounce buffer BEFORE the
 * command is submitted. DESTRUCTIVE; caller must have passed the §6d/phys_guard
 * gate. */
static int nvme_host_write_locked(uint64_t abar_phys, uint64_t lba, uint16_t count,
                                  const void *src) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)abar_phys;
    const uint8_t *in = (const uint8_t *)src;
    uint16_t done = 0;

    while (done < count) {
        uint16_t nsec = (uint16_t)((count - done > BOUNCE_SECTORS) ? BOUNCE_SECTORS
                                                                   : (count - done));
        uint32_t bytes = (uint32_t)nsec * HYPE_NVME_SECTOR_SIZE;
        uint32_t pages = (bytes + PAGE - 1u) / PAGE;
        uint64_t prp1 = phys(g_bounce);
        uint64_t prp2 = 0;
        uint8_t sqe[64];
        uint32_t i;

        for (i = 0; i < bytes; i++) {
            g_bounce[i] = in[i];
        }
        if (pages == 2u) {
            prp2 = phys(g_bounce) + PAGE;
        } else if (pages > 2u) {
            for (i = 0; i + 1u < pages; i++) {
                *(uint64_t *)(g_prp_list + i * 8u) = phys(g_bounce) + (uint64_t)(i + 1u) * PAGE;
            }
            prp2 = phys(g_prp_list);
        }

        hype_nvme_build_write_sqe(sqe, ++g_cid, 1u, lba + done, (uint16_t)(nsec - 1u), prp1, prp2);
        if (submit_and_poll(bar, g_io_sq, g_io_cq, 1, &g_io_sq_tail, &g_io_cq_head, &g_io_phase,
                            sqe) != 0) {
            return -1;
        }
        in += bytes;
        done = (uint16_t)(done + nsec);
    }
    return 0;
}

int hype_nvme_host_write(uint64_t abar_phys, uint64_t lba, uint16_t count, const void *src) {
    int rc;
    if (nvme_host_lock_or_fail() != 0) {
        return -1;
    }
    rc = nvme_host_write_locked(abar_phys, lba, count, src);
    nvme_host_unlock();
    return rc;
}

/*
 * #715: one I/O WRITE command for the whole (already-validated) segment list, instead of one
 * command per segment. Gathers into g_bounce (hype_nvme_gather_segs(), pure) then reuses the
 * exact PRP1/PRP2/PRP-list construction nvme_host_write_locked() already has -- the segments
 * become, from the controller's point of view, indistinguishable from one scalar write of the
 * same total size. Callers bound the total to BOUNCE_SECTORS (HYPE_NVME_WRITEV_MAX_SECTORS in
 * the header), so this never needs the outer per-BOUNCE_SECTORS-chunk loop the scalar path has.
 */
static int nvme_host_writev_locked(uint64_t abar_phys, uint64_t lba, const hype_blk_seg_t *segs,
                                   uint32_t nsegs) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)abar_phys;
    uint64_t total_sectors = 0;
    uint32_t bytes;
    uint32_t pages;
    uint64_t prp1 = phys(g_bounce);
    uint64_t prp2 = 0;
    uint8_t sqe[64];
    uint32_t i;

    for (i = 0; i < nsegs; i++) {
        total_sectors += (uint64_t)segs[i].count;
    }
    if (total_sectors == 0u || total_sectors > BOUNCE_SECTORS) {
        return -1; /* a caller bug -- hype_blk_phys_enable_writev()'s own cap should prevent this */
    }
    bytes = (uint32_t)total_sectors * HYPE_NVME_SECTOR_SIZE;
    if (hype_nvme_gather_segs(segs, nsegs, 0, bytes, g_bounce) != 0) {
        return -1;
    }

    pages = (bytes + PAGE - 1u) / PAGE;
    if (pages == 2u) {
        prp2 = phys(g_bounce) + PAGE;
    } else if (pages > 2u) {
        for (i = 0; i + 1u < pages; i++) {
            *(uint64_t *)(g_prp_list + i * 8u) = phys(g_bounce) + (uint64_t)(i + 1u) * PAGE;
        }
        prp2 = phys(g_prp_list);
    }

    hype_nvme_build_write_sqe(sqe, ++g_cid, 1u, lba, (uint16_t)(total_sectors - 1u), prp1, prp2);
    return submit_and_poll(bar, g_io_sq, g_io_cq, 1, &g_io_sq_tail, &g_io_cq_head, &g_io_phase, sqe);
}

int hype_nvme_host_writev(uint64_t abar_phys, uint64_t lba, const hype_blk_seg_t *segs,
                          uint32_t nsegs) {
    int rc;
    if (nvme_host_lock_or_fail() != 0) {
        return -1;
    }
    rc = nvme_host_writev_locked(abar_phys, lba, segs, nsegs);
    nvme_host_unlock();
    return rc;
}
