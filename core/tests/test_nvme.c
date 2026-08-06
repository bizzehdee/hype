#include <stdio.h>
#include <string.h>
#include "../../devices/nvme.h"
#include "../blk_backend.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual)                                                          \
    do {                                                                                           \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) {                       \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc),                              \
                   (unsigned long long)(expected), (unsigned long long)(actual));                  \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/*
 * #202/#335: the phase tag is the whole reason this file exists before any queue plumbing does.
 *
 * OVMF's NvmExpressDxe does not use interrupts -- it polls for a completion entry whose phase differs
 * from what it expects. So a phase-tag bug is COMPLETELY SILENT: the driver spins to its timeout with
 * no error, no fault and nothing in any log. Both directions are fatal and neither is observable:
 * toggling per entry hides half the completions; never toggling hides everything after the first wrap.
 */
static void test_phase_starts_at_one(void) {
    hype_nvme_t d;
    hype_nvme_reset(&d);
    /*
     * A driver zeroes its completion queue and waits for phase != 0. If the controller stamped its
     * first completion with phase 0 it would be indistinguishable from "nothing posted yet", and the
     * driver would poll forever.
     */
    CHECK_HEX("first completion is stamped phase 1", 1u, hype_nvme_cq_advance(&d, 0u));
}

static void test_phase_toggles_only_on_wrap(void) {
    hype_nvme_t d;
    unsigned int i;
    unsigned ones = 0, zeros = 0;

    hype_nvme_reset(&d);
    /* One full lap: every entry must carry phase 1. */
    for (i = 0; i < HYPE_NVME_QUEUE_ENTRIES; i++) {
        uint8_t p = hype_nvme_cq_advance(&d, 0u);
        if (p == 1u) ones++; else zeros++;
    }
    CHECK_HEX("a full lap is all phase 1", HYPE_NVME_QUEUE_ENTRIES, ones);
    CHECK_HEX("...with no phase 0 in it", 0u, zeros);
    CHECK_HEX("and the producer wrapped back to 0", 0u, d.cq_tail[0]);

    /* Second lap: every entry must now carry phase 0. */
    ones = 0; zeros = 0;
    for (i = 0; i < HYPE_NVME_QUEUE_ENTRIES; i++) {
        uint8_t p = hype_nvme_cq_advance(&d, 0u);
        if (p == 1u) ones++; else zeros++;
    }
    CHECK_HEX("the second lap is all phase 0", HYPE_NVME_QUEUE_ENTRIES, zeros);
    CHECK_HEX("...with no phase 1 in it", 0u, ones);

    /* Third lap back to 1 -- proves it toggles rather than latching. */
    CHECK_HEX("the third lap returns to phase 1", 1u, hype_nvme_cq_advance(&d, 0u));
}

static void test_phase_is_per_queue(void) {
    hype_nvme_t d;
    unsigned int i;

    hype_nvme_reset(&d);
    /* Wrap queue 0 only. Queue 1 must be untouched: completion queues wrap independently, and sharing
     * a phase across them would make one queue's wrap corrupt the other's polling. */
    for (i = 0; i < HYPE_NVME_QUEUE_ENTRIES; i++) {
        (void)hype_nvme_cq_advance(&d, 0u);
    }
    CHECK_HEX("queue 0 flipped to phase 0", 0u, d.cq_phase[0]);
    CHECK_HEX("queue 1 is still phase 1", 1u, d.cq_phase[1]);
    CHECK_HEX("queue 1's producer never moved", 0u, d.cq_tail[1]);
    CHECK_HEX("an out-of-range qid is refused safely", 1u,
              hype_nvme_cq_advance(&d, HYPE_NVME_MAX_QUEUES));
}

static void test_enable_handshake_and_reset(void) {
    hype_nvme_t d;
    unsigned int i;

    hype_nvme_reset(&d);
    CHECK_HEX("not ready at power-on", 0u, hype_nvme_mmio_read32(&d, HYPE_NVME_REG_CSTS) & 1u);

    /* The driver sets CC.EN and polls CSTS.RDY. */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_CC, HYPE_NVME_CC_EN);
    CHECK_HEX("CC.EN makes the controller ready", 1u,
              hype_nvme_mmio_read32(&d, HYPE_NVME_REG_CSTS) & 1u);

    /* Move some indices, then reset via CC.EN=0. */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 5u);
    for (i = 0; i < HYPE_NVME_QUEUE_ENTRIES + 3u; i++) {
        (void)hype_nvme_cq_advance(&d, 0u);
    }
    CHECK_HEX("indices moved", 5u, d.sq_tail[0]);

    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_CC, 0u);
    CHECK_HEX("clearing CC.EN drops ready", 0u, hype_nvme_mmio_read32(&d, HYPE_NVME_REG_CSTS) & 1u);
    /* A reset must drop every index AND restore phase 1: a driver that re-creates its queues polls a
     * freshly zeroed one against phase 1 again, so leaving phase inverted would hide every completion
     * after the reset. */
    CHECK_HEX("reset clears the SQ tail", 0u, d.sq_tail[0]);
    CHECK_HEX("reset clears the CQ producer", 0u, d.cq_tail[0]);
    CHECK_HEX("reset restores phase 1", 1u, d.cq_phase[0]);
}

static void test_capabilities_advertise_what_hype_actually_does(void) {
    hype_nvme_t d;
    uint32_t cap_lo, cap_hi;

    hype_nvme_reset(&d);
    cap_lo = hype_nvme_mmio_read32(&d, HYPE_NVME_REG_CAP);
    cap_hi = hype_nvme_mmio_read32(&d, HYPE_NVME_REG_CAP + 4u);

    /* MQES is entries MINUS ONE -- an off-by-one here either wastes an entry or lets the driver index
     * one past the queue. */
    CHECK_HEX("CAP.MQES is entries-1", HYPE_NVME_QUEUE_ENTRIES - 1u, cap_lo & 0xFFFFu);
    CHECK_HEX("CAP.CQR set: hype requires contiguous queues", 1u, (cap_lo >> 16) & 1u);
    CHECK_HEX("CAP.DSTRD 0 => 4-byte doorbell stride", 0u, cap_hi & 0xFu);
    CHECK_HEX("version reads 1.4.0", 0x00010400u, hype_nvme_mmio_read32(&d, HYPE_NVME_REG_VS));
}

static void test_doorbell_decode_refuses_rather_than_clamps(void) {
    unsigned int qid = 99u;
    int is_cq = -1;

    /* SQ0, CQ0, SQ1, CQ1 at 4-byte stride. */
    CHECK_HEX("SQ0 decodes", 0, hype_nvme_doorbell_decode(0x1000u, &qid, &is_cq));
    CHECK_HEX("  qid 0", 0u, qid);
    CHECK_HEX("  is a submission queue", 0, is_cq);
    CHECK_HEX("CQ0 decodes", 0, hype_nvme_doorbell_decode(0x1004u, &qid, &is_cq));
    CHECK_HEX("  is a completion queue", 1, is_cq);
    CHECK_HEX("SQ1 decodes", 0, hype_nvme_doorbell_decode(0x1008u, &qid, &is_cq));
    CHECK_HEX("  qid 1", 1u, qid);

    /*
     * VALID-3: refuse, never clamp. A misaligned offset rounded down would move a NEIGHBOURING queue's
     * index, and a doorbell past the modelled set would alias onto queue 0 -- both let a guest steer an
     * index it does not own.
     */
    CHECK_HEX("a misaligned doorbell is refused", -1, hype_nvme_doorbell_decode(0x1002u, &qid, &is_cq));
    CHECK_HEX("a doorbell past the last queue is refused", -1,
              hype_nvme_doorbell_decode(0x1010u, &qid, &is_cq));
    CHECK_HEX("an offset below the doorbell window is refused", -1,
              hype_nvme_doorbell_decode(0x0FFCu, &qid, &is_cq));
    CHECK_HEX("NULL outputs refused", -1, hype_nvme_doorbell_decode(0x1000u, 0, &is_cq));
}

static void test_out_of_range_doorbell_value_is_refused(void) {
    hype_nvme_t d;
    hype_nvme_reset(&d);

    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 7u);
    CHECK_HEX("an in-range tail is accepted", 7u, d.sq_tail[0]);
    /* An index at or past the queue size would walk off the end of guest-supplied queue memory. */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, HYPE_NVME_QUEUE_ENTRIES);
    CHECK_HEX("an out-of-range tail is REFUSED, leaving the old value", 7u, d.sq_tail[0]);
}

static void test_queue_bases_are_64bit(void) {
    hype_nvme_t d;
    hype_nvme_reset(&d);

    /* ASQ/ACQ are 64-bit and written as two dwords; a driver may write them in either order, and
     * mixing them up silently points the controller at the wrong guest memory. */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_ASQ + 4u, 0x1234u);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_ASQ, 0xF000u);
    CHECK_HEX("ASQ low", 0xF000u, hype_nvme_mmio_read32(&d, HYPE_NVME_REG_ASQ));
    CHECK_HEX("ASQ high", 0x1234u, hype_nvme_mmio_read32(&d, HYPE_NVME_REG_ASQ + 4u));
    CHECK_HEX("ASQ reassembled", 0x12340000F000ull, d.asq);

    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_ACQ, 0xE000u);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_ACQ + 4u, 0x9u);
    CHECK_HEX("ACQ reassembled", 0x90000E000ull, d.acq);
}

static void test_unmodelled_registers_are_inert(void) {
    hype_nvme_t d;
    hype_nvme_reset(&d);

    /* A driver probing optional features must not be able to take the VM down. */
    hype_nvme_mmio_write32(&d, 0x0800u, 0xDEADBEEFu);
    CHECK_HEX("an unmodelled write is ignored", 0u, hype_nvme_mmio_read32(&d, 0x0800u));
    /* INTMS/INTMC: accepted and ignored -- #335 showed the firmware never reads them. */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_INTMS, 0xFFFFFFFFu);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_INTMC, 0xFFFFFFFFu);
    CHECK_HEX("the controller is still ready-capable after mask writes", 0u, d.csts & 2u);
}

static void test_remaining_register_paths(void) {
    hype_nvme_t d;
    unsigned int qid = 0;
    hype_nvme_reset(&d);

    /* AQA round-trips: the admin queue sizes the driver asks for must survive, or the controller and
     * driver disagree about where the admin queues end. */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_AQA, 0x003F003Fu);
    CHECK_HEX("AQA round-trips", 0x003F003Fu, hype_nvme_mmio_read32(&d, HYPE_NVME_REG_AQA));

    /* CC round-trips including the fields above EN, which the driver sets in one write. */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_CC, 0x00460001u);
    CHECK_HEX("CC round-trips whole", 0x00460001u, hype_nvme_mmio_read32(&d, HYPE_NVME_REG_CC));

    /* Reading a write-only/unmodelled register yields 0 rather than stale state. */
    CHECK_HEX("INTMS reads 0", 0u, hype_nvme_mmio_read32(&d, HYPE_NVME_REG_INTMS));
    CHECK_HEX("a doorbell offset read yields 0", 0u,
              hype_nvme_mmio_read32(&d, HYPE_NVME_REG_DOORBELL_BASE));

    /* The other NULL-output arm. */
    CHECK_HEX("NULL is_cq refused", -1, hype_nvme_doorbell_decode(0x1000u, &qid, 0));

    /* A CQ doorbell moves the consumer, not the producer -- confusing the two would make the
     * controller believe entries the driver has not read are free to overwrite. */
    hype_nvme_mmio_write32(&d, 0x1004u, 9u);
    CHECK_HEX("CQ doorbell moved the head", 9u, d.cq_head[0]);
    CHECK_HEX("...and left the producer alone", 0u, d.cq_tail[0]);
}


/* ---- slice 2: admin commands ---- */

static void test_sqe_decode(void) {
    uint8_t sqe[HYPE_NVME_SQE_BYTES];
    hype_nvme_cmd_t c;
    unsigned i;

    for (i = 0; i < sizeof(sqe); i++) sqe[i] = 0;
    /* CDW0: opcode 0x06 (IDENTIFY) in 7:0, CID 0xBEEF in 31:16. */
    sqe[0] = 0x06u; sqe[2] = 0xEFu; sqe[3] = 0xBEu;
    sqe[4] = 0x01u;                                  /* NSID = 1 */
    sqe[24] = 0x00u; sqe[25] = 0x20u;                /* PRP1 = 0x2000 */
    sqe[32] = 0x00u; sqe[33] = 0x30u;                /* PRP2 = 0x3000 */
    sqe[40] = 0x01u;                                 /* CDW10: CNS = 1 */

    hype_nvme_sqe_decode(sqe, &c);
    CHECK_HEX("opcode", 0x06u, c.opcode);
    /* The CID must survive exactly: a driver matches completions to commands by it, so a mangled CID
     * makes a SUCCESSFUL command look like a timeout. */
    CHECK_HEX("cid", 0xBEEFu, c.cid);
    CHECK_HEX("nsid", 1u, c.nsid);
    CHECK_HEX("prp1", 0x2000u, c.prp1);
    CHECK_HEX("prp2", 0x3000u, c.prp2);
    CHECK_HEX("cdw10", 1u, c.cdw10);
}

static void test_cqe_status_does_not_collide_with_phase(void) {
    uint8_t cqe[HYPE_NVME_CQE_BYTES];
    uint16_t sf;

    /* Success with phase 1. */
    hype_nvme_cqe_build(cqe, 0x1234u, 7u, 0u, 1u, HYPE_NVME_SC_SUCCESS);
    CHECK_HEX("cid echoed", 0x1234u, (uint16_t)(cqe[12] | (cqe[13] << 8)));
    CHECK_HEX("sq_head reported", 7u, (uint16_t)(cqe[8] | (cqe[9] << 8)));
    sf = (uint16_t)(cqe[14] | (cqe[15] << 8));
    CHECK_HEX("phase bit set", 1u, sf & 1u);
    CHECK_HEX("status code is 0 for success", 0u, (sf >> 1) & 0xFFu);

    /*
     * The trap: SC lives at bits 8:1, so it is SHIFTED LEFT BY ONE past the phase bit. Forget the
     * shift and the status overwrites the phase -- and a polling driver then never observes the
     * completion at all, with no error anywhere.
     */
    hype_nvme_cqe_build(cqe, 1u, 0u, 0u, 0u, HYPE_NVME_SC_INVALID_OPCODE);
    sf = (uint16_t)(cqe[14] | (cqe[15] << 8));
    CHECK_HEX("phase 0 preserved alongside a status", 0u, sf & 1u);
    CHECK_HEX("status survives the shift", HYPE_NVME_SC_INVALID_OPCODE, (sf >> 1) & 0xFFu);

    hype_nvme_cqe_build(cqe, 1u, 0u, 0u, 1u, HYPE_NVME_SC_LBA_OUT_OF_RANGE);
    sf = (uint16_t)(cqe[14] | (cqe[15] << 8));
    CHECK_HEX("phase 1 alongside a large status", 1u, sf & 1u);
    CHECK_HEX("0x80 status survives", HYPE_NVME_SC_LBA_OUT_OF_RANGE, (sf >> 1) & 0xFFu);
}

static void test_identify_controller(void) {
    static uint8_t buf[HYPE_NVME_IDENTIFY_BYTES];
    unsigned i;

    hype_nvme_identify_controller(buf, "HYPE-NVME-0001");
    /* SPACE padded, not NUL: these are fixed-width ASCII fields, and a NUL-padded serial appears as
     * garbage in a guest's device listing. */
    CHECK_HEX("SN starts with 'H'", (unsigned)'H', buf[4]);
    CHECK_HEX("SN is space padded, not NUL", (unsigned)' ', buf[4 + 19]);
    CHECK_HEX("model is space padded too", (unsigned)' ', buf[24 + 39]);
    CHECK_HEX("exactly one namespace", 1u,
              (unsigned)(buf[516] | (buf[517] << 8) | (buf[518] << 16) | (buf[519] << 24)));
    /* SQES/CQES as powers of two -- must match the queue arithmetic or entry N is at the wrong place. */
    CHECK_HEX("SQES advertises 2^6 = 64 bytes", 0x66u, buf[512]);
    CHECK_HEX("CQES advertises 2^4 = 16 bytes", 0x44u, buf[513]);
    CHECK_HEX("MDTS 0 = unlimited", 0u, buf[77]);
    /* Nothing should have been left uninitialised beyond the fields set. */
    for (i = 600; i < 700; i++) {
        if (buf[i] != 0) { CHECK_HEX("tail of identify is zeroed", 0u, buf[i]); break; }
    }
}

static void test_identify_namespace_block_size_is_a_power_of_two(void) {
    static uint8_t buf[HYPE_NVME_IDENTIFY_BYTES];
    uint64_t nsze;
    unsigned k;

    hype_nvme_identify_namespace(buf, 1048576ull); /* 512 MiB of 512-byte blocks */
    nsze = 0;
    for (k = 0; k < 8u; k++) {
        nsze |= (uint64_t)buf[k] << (8u * k);
    }
    CHECK_HEX("NSZE is the block count", 1048576ull, nsze);
    CHECK_HEX("NCAP matches", 1048576ull, (uint64_t)buf[8] | ((uint64_t)buf[9] << 8) |
                                              ((uint64_t)buf[10] << 16) | ((uint64_t)buf[11] << 24));
    CHECK_HEX("fully provisioned (NUSE == NSZE)", (unsigned)buf[16], (unsigned)buf[0]);
    /*
     * THE field to get right: LBADS is a POWER OF TWO. 9 => 512 bytes. A wrong value makes every guest
     * LBA land at the wrong byte offset, and the guest reads and writes there without complaint.
     */
    CHECK_HEX("LBAF0.LBADS is 9 (2^9 = 512 bytes)", 9u, buf[130]);
    CHECK_HEX("format 0 selected", 0u, buf[26]);
    CHECK_HEX("one LBA format described", 0u, buf[25]);
}


/* ---- slice 3: PRP walking ---- */

/* A fake guest memory holding PRP lists. Entry i of the list at LIST_BASE describes page i. */
#define PRP_PAGE 4096u
#define LIST_BASE 0x70000ull
#define LIST2_BASE 0x80000ull
static uint64_t g_list[512];
static uint64_t g_list2[512];
static int g_list_read_fails = 0;

static int fake_guest_read(void *ctx, uint64_t gpa, uint32_t len, void *dst) {
    (void)ctx;
    if (g_list_read_fails) {
        return -1;
    }
    if (len != 8u) {
        return -1;
    }
    if (gpa >= LIST_BASE && gpa < LIST_BASE + sizeof(g_list)) {
        memcpy(dst, (const uint8_t *)g_list + (gpa - LIST_BASE), 8);
        return 0;
    }
    if (gpa >= LIST2_BASE && gpa < LIST2_BASE + sizeof(g_list2)) {
        memcpy(dst, (const uint8_t *)g_list2 + (gpa - LIST2_BASE), 8);
        return 0;
    }
    return -1;
}

static void test_prp_single_page_with_offset(void) {
    hype_nvme_prp_iter_t it;
    uint64_t gpa; uint32_t len;

    /* PRP1 may be offset into its page; the first segment runs to the END of that page only. */
    CHECK_HEX("init", 0, hype_nvme_prp_init(&it, 0x1000u + 512u, 0, 1024u, PRP_PAGE));
    CHECK_HEX("one segment", 1, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
    CHECK_HEX("  at the offset address", 0x1200u, gpa);
    CHECK_HEX("  covering the whole request", 1024u, len);
    CHECK_HEX("then done", 0, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
}

static void test_prp_first_segment_stops_at_the_page_end(void) {
    hype_nvme_prp_iter_t it;
    uint64_t gpa; uint32_t len;

    /* Offset 3584 in the page leaves 512 bytes; the rest must come from PRP2, NOT by running past the
     * end of PRP1's page -- which is the mistake that would read the guest's next page. */
    CHECK_HEX("init", 0, hype_nvme_prp_init(&it, 0x2000u + 3584u, 0x9000u, 1024u, PRP_PAGE));
    CHECK_HEX("seg0", 1, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
    CHECK_HEX("  stops at the page boundary", 512u, len);
    CHECK_HEX("seg1", 1, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
    CHECK_HEX("  comes from PRP2", 0x9000u, gpa);
    CHECK_HEX("  with the remainder", 512u, len);
    CHECK_HEX("done", 0, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
}

static void test_prp2_is_a_data_page_when_the_transfer_is_small(void) {
    hype_nvme_prp_iter_t it;
    uint64_t gpa; uint32_t len;

    /* Exactly two pages: PRP2 IS the second data page, not a list. Misreading it as a list would read
     * addresses out of the guest's data. */
    CHECK_HEX("init", 0, hype_nvme_prp_init(&it, 0x3000u, 0x4000u, 2u * PRP_PAGE, PRP_PAGE));
    CHECK_HEX("seg0", 1, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
    CHECK_HEX("  full page", PRP_PAGE, len);
    CHECK_HEX("seg1", 1, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
    CHECK_HEX("  is PRP2 itself", 0x4000u, gpa);
    CHECK_HEX("  full page", PRP_PAGE, len);
    CHECK_HEX("done", 0, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
}

static void test_prp2_is_a_list_when_the_transfer_is_larger(void) {
    hype_nvme_prp_iter_t it;
    uint64_t gpa; uint32_t len;
    unsigned i;

    for (i = 0; i < 512u; i++) {
        g_list[i] = 0x100000ull + (uint64_t)i * PRP_PAGE;
    }
    /* Three pages: PRP1 + two entries from the LIST. Treating PRP2 as data here would write file
     * contents over the guest's PRP list. */
    CHECK_HEX("init", 0, hype_nvme_prp_init(&it, 0x3000u, LIST_BASE, 3u * PRP_PAGE, PRP_PAGE));
    CHECK_HEX("seg0 is PRP1", 1, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
    CHECK_HEX("  PRP1 address", 0x3000u, gpa);
    CHECK_HEX("seg1 from the list", 1, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
    CHECK_HEX("  list entry 0", 0x100000ull, gpa);
    CHECK_HEX("seg2 from the list", 1, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
    CHECK_HEX("  list entry 1", 0x101000ull, gpa);
    CHECK_HEX("done", 0, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
}

static void test_prp_list_chains_at_the_last_slot(void) {
    hype_nvme_prp_iter_t it;
    uint64_t gpa; uint32_t len;
    unsigned i, segs = 0;
    unsigned entries_per_page = PRP_PAGE / 8u;   /* 512 */
    /* One list page describes entries_per_page-1 data pages, then chains. */
    uint64_t total = (uint64_t)PRP_PAGE * (uint64_t)(entries_per_page + 4u);

    for (i = 0; i < entries_per_page; i++) {
        g_list[i] = 0x200000ull + (uint64_t)i * PRP_PAGE;
        g_list2[i] = 0x900000ull + (uint64_t)i * PRP_PAGE;
    }
    /* The LAST slot of list 1 chains to list 2 rather than describing data. */
    g_list[entries_per_page - 1u] = LIST2_BASE;

    CHECK_HEX("init", 0, hype_nvme_prp_init(&it, 0x3000u, LIST_BASE, total, PRP_PAGE));
    while (hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len) == 1) {
        segs++;
        if (segs > entries_per_page + 16u) {
            break; /* runaway guard */
        }
    }
    /* PRP1 plus every page the two lists describe: the chain must have been followed, otherwise the
     * walk stops early and the tail of the transfer silently never happens. */
    CHECK_HEX("the whole transfer was covered", 0u, (unsigned)(it.remaining != 0));
    CHECK_HEX("segments = 1 (PRP1) + the rest", entries_per_page + 4u, segs);
}

static void test_prp_refuses_malformed_descriptors(void) {
    hype_nvme_prp_iter_t it;
    uint64_t gpa; uint32_t len;

    CHECK_HEX("zero length refused", -1, hype_nvme_prp_init(&it, 0x1000u, 0, 0u, PRP_PAGE));
    CHECK_HEX("non-power-of-two page refused", -1,
              hype_nvme_prp_init(&it, 0x1000u, 0, 512u, 3000u));
    CHECK_HEX("NULL iterator refused", -1, hype_nvme_prp_init(0, 0x1000u, 0, 512u, PRP_PAGE));

    /* A MISALIGNED continuation PRP is refused, not masked: the spec requires alignment, so a
     * misaligned entry means the list is not what hype thinks it is, and continuing would scatter the
     * transfer across addresses the guest never nominated. */
    CHECK_HEX("init", 0, hype_nvme_prp_init(&it, 0x3000u, 0x4001u, 2u * PRP_PAGE, PRP_PAGE));
    (void)hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len);
    CHECK_HEX("misaligned PRP2 refused", -1, hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));

    /* A failed guest read of a list entry must propagate, not silently end the transfer. */
    g_list[0] = 0x100000ull;
    CHECK_HEX("init list", 0, hype_nvme_prp_init(&it, 0x3000u, LIST_BASE, 3u * PRP_PAGE, PRP_PAGE));
    (void)hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len);
    g_list_read_fails = 1;
    CHECK_HEX("a failed list read is an error", -1,
              hype_nvme_prp_next(&it, fake_guest_read, 0, &gpa, &len));
    g_list_read_fails = 0;

    /* Walking a list with no reader is an error rather than a wild dereference. */
    CHECK_HEX("init list", 0, hype_nvme_prp_init(&it, 0x3000u, LIST_BASE, 3u * PRP_PAGE, PRP_PAGE));
    (void)hype_nvme_prp_next(&it, 0, 0, &gpa, &len);
    CHECK_HEX("no reader refused", -1, hype_nvme_prp_next(&it, 0, 0, &gpa, &len));
}


/* ---- slice 4: I/O READ/WRITE over a blk_backend ---- */

/* A tiny in-memory backend plus a fake guest RAM, so data actually round-trips. */
#define DISK_SECTORS 64u
static uint8_t g_disk[DISK_SECTORS * 512u];
static uint8_t g_gram[64u * 1024u];
#define GRAM_BASE 0x400000ull

static int disk_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if (lba + count > DISK_SECTORS) return -1;
    memcpy(buf, g_disk + lba * 512u, (size_t)count * 512u);
    return 0;
}
static int disk_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx;
    if (lba + count > DISK_SECTORS) return -1;
    memcpy(g_disk + lba * 512u, buf, (size_t)count * 512u);
    return 0;
}
static int gram_read(void *ctx, uint64_t gpa, uint32_t len, void *dst) {
    (void)ctx;
    /* Serve PRP lists too, so a large transfer can be exercised. */
    if (gpa >= LIST_BASE && gpa < LIST_BASE + sizeof(g_list)) {
        return fake_guest_read(ctx, gpa, len, dst);
    }
    if (gpa < GRAM_BASE || gpa + len > GRAM_BASE + sizeof(g_gram)) return -1;
    memcpy(dst, g_gram + (gpa - GRAM_BASE), len);
    return 0;
}
static int gram_write(void *ctx, uint64_t gpa, uint32_t len, const void *src) {
    (void)ctx;
    if (gpa < GRAM_BASE || gpa + len > GRAM_BASE + sizeof(g_gram)) return -1;
    memcpy(g_gram + (gpa - GRAM_BASE), src, len);
    return 0;
}

static void io_backend(hype_blk_backend_t *be) {
    be->read = disk_read;
    be->write = disk_write;
    be->ctx = 0;
    be->total_sectors = DISK_SECTORS;
}

static void mk_io_cmd(hype_nvme_cmd_t *c, uint8_t op, uint64_t slba, uint32_t blocks, uint64_t prp1,
                      uint64_t prp2) {
    memset(c, 0, sizeof(*c));
    c->opcode = op;
    c->cid = 1u;
    c->prp1 = prp1;
    c->prp2 = prp2;
    c->cdw10 = (uint32_t)slba;
    c->cdw11 = (uint32_t)(slba >> 32);
    c->cdw12 = blocks - 1u; /* NLB is ZERO-BASED -- the caller passes a real count */
}

static void test_io_read_single_block_nlb_is_zero_based(void) {
    hype_blk_backend_t be;
    hype_nvme_cmd_t c;
    static uint8_t bounce[4096];
    unsigned i;

    io_backend(&be);
    for (i = 0; i < 512u; i++) g_disk[i] = (uint8_t)(i & 0xFFu);
    memset(g_gram, 0, sizeof(g_gram));

    /* ONE block. NLB arrives as 0; read literally, this transfers nothing -- and a driver whose read
     * "succeeded" but moved no data sees corrupt content rather than an error. */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 1u, GRAM_BASE, 0);
    CHECK_HEX("single-block read succeeds", HYPE_NVME_SC_SUCCESS,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    CHECK_HEX("first byte landed", 0u, g_gram[0]);
    CHECK_HEX("last byte of the sector landed", 511u & 0xFFu, g_gram[511]);
    CHECK_HEX("and NOTHING beyond one sector was written", 0u, g_gram[512]);
}

static void test_io_read_write_roundtrip_multi_sector(void) {
    hype_blk_backend_t be;
    hype_nvme_cmd_t c;
    static uint8_t bounce[4096];
    unsigned i;

    io_backend(&be);
    memset(g_disk, 0, sizeof(g_disk));
    for (i = 0; i < 2048u; i++) g_gram[i] = (uint8_t)(0xA0u + (i % 7u));

    /* WRITE 4 blocks from guest RAM at LBA 8. */
    mk_io_cmd(&c, HYPE_NVME_IO_WRITE, 8u, 4u, GRAM_BASE, 0);
    CHECK_HEX("write succeeds", HYPE_NVME_SC_SUCCESS,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    CHECK_HEX("landed at the right LBA", 0xA0u, g_disk[8u * 512u]);
    CHECK_HEX("and not at LBA 7", 0u, g_disk[7u * 512u]);

    /* READ it back to a different guest address and compare. */
    memset(g_gram + 4096u, 0, 2048u);
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 8u, 4u, GRAM_BASE + 4096u, 0);
    CHECK_HEX("read back succeeds", HYPE_NVME_SC_SUCCESS,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    CHECK_HEX("round-trip is byte-exact", 0, memcmp(g_gram, g_gram + 4096u, 2048u));
}

static void test_io_uses_the_full_64bit_slba(void) {
    hype_blk_backend_t be;
    hype_nvme_cmd_t c;
    static uint8_t bounce[4096];

    io_backend(&be);
    /* CDW11 non-zero puts SLBA far past the disk. If only CDW10 were read, this would look like LBA 0
     * and quietly transfer the WRONG SECTORS -- correct on any disk under 2 TiB, then wrapping. */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 1u, GRAM_BASE, 0);
    c.cdw11 = 1u; /* SLBA = 2^32 */
    CHECK_HEX("a 64-bit SLBA past the end is refused", HYPE_NVME_SC_LBA_OUT_OF_RANGE,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
}

static void test_io_bounds_and_error_paths(void) {
    hype_blk_backend_t be;
    hype_nvme_cmd_t c;
    static uint8_t bounce[4096];

    io_backend(&be);

    /* VALID-3: the range is checked against the backend's REAL size. */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, DISK_SECTORS, 1u, GRAM_BASE, 0);
    CHECK_HEX("starting past the end is refused", HYPE_NVME_SC_LBA_OUT_OF_RANGE,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    /* Straddling the end: the dangerous one, since the first sectors ARE valid. */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, DISK_SECTORS - 2u, 4u, GRAM_BASE, 0);
    CHECK_HEX("straddling the end is refused", HYPE_NVME_SC_LBA_OUT_OF_RANGE,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    /* The last sector exactly IS allowed -- an off-by-one here would make the final block unreachable. */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, DISK_SECTORS - 1u, 1u, GRAM_BASE, 0);
    CHECK_HEX("the final sector is readable", HYPE_NVME_SC_SUCCESS,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));

    /* A guest address outside mapped RAM must fail the command, not be written anyway. */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 1u, 0xDEAD0000ull, 0);
    CHECK_HEX("an unmapped PRP fails the transfer", HYPE_NVME_SC_DATA_XFER_ERROR,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));

    /*
     * A non-sector-multiple segment is REFUSED rather than hand-split: a wrong split writes the right
     * bytes to the wrong place and nothing reports it.
     *
     * Note what it takes to PRODUCE one, because my first attempt at this test did not: with a small
     * transfer the first segment is clamped by the bytes REMAINING (always a sector multiple), so a
     * misaligned PRP1 offset alone is harmless. The partial-sector split only appears when the PAGE
     * BOUNDARY is what limits the segment -- offset 100 with 8192 bytes to move gives 3996.
     */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 16u, GRAM_BASE + 100u, GRAM_BASE + 8192u);
    CHECK_HEX("a page-boundary partial-sector split is refused", HYPE_NVME_SC_INVALID_FIELD,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    /* ...while a misaligned offset that does NOT straddle a sector is fine. */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 2u, GRAM_BASE + 100u, 0);
    CHECK_HEX("a harmless misaligned offset still works", HYPE_NVME_SC_SUCCESS,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));

    /* Unknown opcode, and the guard rails. */
    mk_io_cmd(&c, 0x7Fu, 0, 1u, GRAM_BASE, 0);
    CHECK_HEX("an unknown opcode is reported", HYPE_NVME_SC_INVALID_OPCODE,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 1u, GRAM_BASE, 0);
    CHECK_HEX("a bounce smaller than a page is refused", HYPE_NVME_SC_INVALID_FIELD,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce, 64u));
    CHECK_HEX("a NULL backend is refused", HYPE_NVME_SC_INVALID_FIELD,
              hype_nvme_exec_io(&c, 0, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
}

static void test_io_spanning_two_prp_pages(void) {
    hype_blk_backend_t be;
    hype_nvme_cmd_t c;
    static uint8_t bounce[4096];
    unsigned i;

    io_backend(&be);
    for (i = 0; i < 16u * 512u; i++) g_disk[i] = (uint8_t)(i * 3u);
    memset(g_gram, 0, sizeof(g_gram));

    /* 16 blocks = 8192 bytes = exactly two pages, so PRP2 is the second DATA page. Both halves must
     * land, and at the right guest addresses. */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 16u, GRAM_BASE, GRAM_BASE + 4096u);
    CHECK_HEX("two-page read succeeds", HYPE_NVME_SC_SUCCESS,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    CHECK_HEX("first page byte 0", (unsigned)(uint8_t)0u, g_gram[0]);
    CHECK_HEX("second page starts at disk offset 4096", (unsigned)(uint8_t)(4096u * 3u),
              g_gram[4096]);
    CHECK_HEX("last byte of the transfer", (unsigned)(uint8_t)((8192u - 1u) * 3u), g_gram[8191]);
}


static int g_disk_write_fails = 0;
static int disk_write_maybe_fails(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    if (g_disk_write_fails) return -1;
    return disk_write(ctx, lba, count, buf);
}
static int g_disk_read_fails = 0;
static int disk_read_maybe_fails(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    if (g_disk_read_fails) return -1;
    return disk_read(ctx, lba, count, buf);
}

static void test_io_backend_and_callback_failures_are_reported(void) {
    hype_blk_backend_t be;
    hype_nvme_cmd_t c;
    static uint8_t bounce[4096];

    io_backend(&be);
    be.write = disk_write_maybe_fails;
    be.read = disk_read_maybe_fails;

    /*
     * A backend that fails mid-transfer must FAIL THE COMMAND. Reporting success on a partial write is
     * the worst option: the guest believes its data is durable when some of it never landed.
     */
    g_disk_write_fails = 1;
    mk_io_cmd(&c, HYPE_NVME_IO_WRITE, 0, 2u, GRAM_BASE, 0);
    CHECK_HEX("a failing backend write is reported", HYPE_NVME_SC_DATA_XFER_ERROR,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    g_disk_write_fails = 0;

    g_disk_read_fails = 1;
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 2u, GRAM_BASE, 0);
    CHECK_HEX("a failing backend read is reported", HYPE_NVME_SC_DATA_XFER_ERROR,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    g_disk_read_fails = 0;

    /* A WRITE whose guest source is unreadable must not write stale bounce contents to the disk. */
    mk_io_cmd(&c, HYPE_NVME_IO_WRITE, 0, 2u, 0xDEAD0000ull, 0);
    CHECK_HEX("an unmapped write source is reported", HYPE_NVME_SC_DATA_XFER_ERROR,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));

    /* Missing callbacks: fail the command rather than dereference nothing. */
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 1u, GRAM_BASE, 0);
    CHECK_HEX("a read with no guest-write callback fails", HYPE_NVME_SC_DATA_XFER_ERROR,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, 0, 0, bounce,
                                sizeof(bounce)));
    mk_io_cmd(&c, HYPE_NVME_IO_WRITE, 0, 1u, GRAM_BASE, 0);
    CHECK_HEX("a write with no guest-read callback fails", HYPE_NVME_SC_DATA_XFER_ERROR,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, 0, gram_write, 0, bounce,
                                sizeof(bounce)));

    /* A NULL command and a bounce of zero length. */
    CHECK_HEX("NULL command refused", HYPE_NVME_SC_INVALID_FIELD,
              hype_nvme_exec_io(0, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 1u, GRAM_BASE, 0);
    CHECK_HEX("NULL bounce refused", HYPE_NVME_SC_INVALID_FIELD,
              hype_nvme_exec_io(&c, &be, DISK_SECTORS, 4096u, gram_read, gram_write, 0, 0,
                                sizeof(bounce)));
}

static void test_io_zero_size_disk_refuses_everything(void) {
    hype_blk_backend_t be;
    hype_nvme_cmd_t c;
    static uint8_t bounce[4096];

    io_backend(&be);
    mk_io_cmd(&c, HYPE_NVME_IO_READ, 0, 1u, GRAM_BASE, 0);
    /* total_sectors 0: every LBA is out of range, including 0. An off-by-one that allowed LBA 0 here
     * would read a backend that has nothing. */
    CHECK_HEX("a zero-sector namespace refuses LBA 0", HYPE_NVME_SC_LBA_OUT_OF_RANGE,
              hype_nvme_exec_io(&c, &be, 0u, 4096u, gram_read, gram_write, 0, bounce,
                                sizeof(bounce)));
}


/* ---- slice 5: the command processor ---- */

#define ASQ_BASE (GRAM_BASE + 0x8000ull)
#define ACQ_BASE (GRAM_BASE + 0x9000ull)
#define IOSQ_BASE (GRAM_BASE + 0xA000ull)
#define IOCQ_BASE (GRAM_BASE + 0xB000ull)

static void put_sqe(uint64_t base, unsigned slot, uint8_t op, uint16_t cid, uint32_t nsid,
                    uint64_t prp1, uint32_t cdw10, uint32_t cdw11, uint32_t cdw12) {
    uint8_t *p = g_gram + (base - GRAM_BASE) + (size_t)slot * HYPE_NVME_SQE_BYTES;
    memset(p, 0, HYPE_NVME_SQE_BYTES);
    p[0] = op;
    p[2] = (uint8_t)(cid & 0xFFu); p[3] = (uint8_t)(cid >> 8);
    p[4] = (uint8_t)(nsid & 0xFFu);
    p[24] = (uint8_t)(prp1 & 0xFFu); p[25] = (uint8_t)((prp1 >> 8) & 0xFFu);
    p[26] = (uint8_t)((prp1 >> 16) & 0xFFu); p[27] = (uint8_t)((prp1 >> 24) & 0xFFu);
    p[40] = (uint8_t)(cdw10 & 0xFFu); p[41] = (uint8_t)((cdw10 >> 8) & 0xFFu);
    p[44] = (uint8_t)(cdw11 & 0xFFu);
    p[48] = (uint8_t)(cdw12 & 0xFFu); p[49] = (uint8_t)((cdw12 >> 8) & 0xFFu);
}

static uint16_t cqe_status_at(uint64_t base, unsigned slot) {
    const uint8_t *p = g_gram + (base - GRAM_BASE) + (size_t)slot * HYPE_NVME_CQE_BYTES;
    return (uint16_t)((p[14] | (p[15] << 8)) >> 1) & 0xFFu;
}
static uint8_t cqe_phase_at(uint64_t base, unsigned slot) {
    const uint8_t *p = g_gram + (base - GRAM_BASE) + (size_t)slot * HYPE_NVME_CQE_BYTES;
    return (uint8_t)(p[14] & 1u);
}
static uint16_t cqe_cid_at(uint64_t base, unsigned slot) {
    const uint8_t *p = g_gram + (base - GRAM_BASE) + (size_t)slot * HYPE_NVME_CQE_BYTES;
    return (uint16_t)(p[12] | (p[13] << 8));
}

static void enable_with_admin_queues(hype_nvme_t *d) {
    hype_nvme_reset(d);
    memset(g_gram, 0, sizeof(g_gram));
    hype_nvme_mmio_write32(d, HYPE_NVME_REG_ASQ, (uint32_t)ASQ_BASE);
    hype_nvme_mmio_write32(d, HYPE_NVME_REG_ASQ + 4u, (uint32_t)(ASQ_BASE >> 32));
    hype_nvme_mmio_write32(d, HYPE_NVME_REG_ACQ, (uint32_t)ACQ_BASE);
    hype_nvme_mmio_write32(d, HYPE_NVME_REG_ACQ + 4u, (uint32_t)(ACQ_BASE >> 32));
    hype_nvme_mmio_write32(d, HYPE_NVME_REG_CC, HYPE_NVME_CC_EN);
}

static void fill_ctx(hype_nvme_ctx_t *c, hype_blk_backend_t *be, uint8_t *bounce, uint32_t blen) {
    memset(c, 0, sizeof(*c));
    c->be = be;
    c->total_sectors = DISK_SECTORS;
    c->page_size = 4096u;
    c->gread = gram_read;
    c->gwrite = gram_write;
    c->gctx = 0;
    c->bounce = bounce;
    c->bounce_len = blen;
    c->serial = "HYPE-NVME-TEST";
}

static void test_admin_identify_round_trip(void) {
    hype_nvme_t d;
    hype_blk_backend_t be;
    hype_nvme_ctx_t c;
    static uint8_t bounce[8192];
    uint64_t dest = GRAM_BASE + 0x2000ull;

    io_backend(&be);
    enable_with_admin_queues(&d);
    fill_ctx(&c, &be, bounce, sizeof(bounce));

    put_sqe(ASQ_BASE, 0, HYPE_NVME_ADMIN_IDENTIFY, 0x55u, 0, dest, HYPE_NVME_CNS_CONTROLLER, 0, 0);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 1u); /* SQ0 tail = 1 */

    CHECK_HEX("one command processed", 1, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("completion says success", HYPE_NVME_SC_SUCCESS, cqe_status_at(ACQ_BASE, 0));
    /* The CID must be echoed or the driver cannot match the completion to its command. */
    CHECK_HEX("completion echoes the CID", 0x55u, cqe_cid_at(ACQ_BASE, 0));
    CHECK_HEX("first completion carries phase 1", 1u, cqe_phase_at(ACQ_BASE, 0));
    /* The IDENTIFY payload actually landed in guest memory. */
    CHECK_HEX("identify SN reached the guest buffer", (unsigned)'H',
              g_gram[(dest - GRAM_BASE) + 4u]);
    CHECK_HEX("and the controller reports one namespace", 1u,
              g_gram[(dest - GRAM_BASE) + 516u]);
}

static void test_io_command_end_to_end_through_the_processor(void) {
    hype_nvme_t d;
    hype_blk_backend_t be;
    hype_nvme_ctx_t c;
    static uint8_t bounce[8192];
    uint64_t dest = GRAM_BASE + 0x3000ull;
    unsigned i;

    io_backend(&be);
    enable_with_admin_queues(&d);
    fill_ctx(&c, &be, bounce, sizeof(bounce));
    for (i = 0; i < 512u; i++) g_disk[512u + i] = (uint8_t)(i ^ 0x5Au);

    /* The guest must create its I/O queues before using them; without that the processor refuses. */
    CHECK_HEX("an I/O queue with no base is refused", -1, hype_nvme_process_sq(&d, 1u, &c));

    put_sqe(ASQ_BASE, 0, HYPE_NVME_ADMIN_CREATE_IO_CQ, 1u, 0, IOCQ_BASE, 1u, 0, 0);
    put_sqe(ASQ_BASE, 1, HYPE_NVME_ADMIN_CREATE_IO_SQ, 2u, 0, IOSQ_BASE, 1u, 0, 0);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 2u);
    CHECK_HEX("two admin commands processed", 2, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("CQ created", HYPE_NVME_SC_SUCCESS, cqe_status_at(ACQ_BASE, 0));
    CHECK_HEX("SQ created", HYPE_NVME_SC_SUCCESS, cqe_status_at(ACQ_BASE, 1));

    /* Now a real READ of LBA 1 through the I/O queue. */
    put_sqe(IOSQ_BASE, 0, HYPE_NVME_IO_READ, 0x99u, 1u, dest, 1u, 0, 0 /* NLB 0 => 1 block */);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE + 8u, 1u); /* SQ1 tail = 1 */
    CHECK_HEX("the I/O command was processed", 1, hype_nvme_process_sq(&d, 1u, &c));
    CHECK_HEX("it succeeded", HYPE_NVME_SC_SUCCESS, cqe_status_at(IOCQ_BASE, 0));
    CHECK_HEX("data reached the guest", (unsigned)(uint8_t)(0u ^ 0x5Au), g_gram[dest - GRAM_BASE]);
    CHECK_HEX("...to the end of the sector", (unsigned)(uint8_t)(511u ^ 0x5Au),
              g_gram[(dest - GRAM_BASE) + 511u]);
}

static void test_failing_command_still_gets_a_completion(void) {
    hype_nvme_t d;
    hype_blk_backend_t be;
    hype_nvme_ctx_t c;
    static uint8_t bounce[8192];

    io_backend(&be);
    enable_with_admin_queues(&d);
    fill_ctx(&c, &be, bounce, sizeof(bounce));

    /*
     * The rule that matters most here: a command that FAILS must still be completed. A driver that
     * receives no completion waits for its timeout and cannot distinguish a failure from a hang, so
     * silence is strictly worse than an error status.
     */
    put_sqe(ASQ_BASE, 0, 0xEEu, 0x77u, 0, GRAM_BASE, 0, 0, 0); /* nonsense opcode */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 1u);
    CHECK_HEX("the bad command was processed", 1, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("and completed with INVALID_OPCODE", HYPE_NVME_SC_INVALID_OPCODE,
              cqe_status_at(ACQ_BASE, 0));
    CHECK_HEX("with its CID echoed", 0x77u, cqe_cid_at(ACQ_BASE, 0));
    CHECK_HEX("and a valid phase bit", 1u, cqe_phase_at(ACQ_BASE, 0));
}

static void test_processor_consumes_each_entry_exactly_once(void) {
    hype_nvme_t d;
    hype_blk_backend_t be;
    hype_nvme_ctx_t c;
    static uint8_t bounce[8192];
    unsigned i;

    io_backend(&be);
    enable_with_admin_queues(&d);
    fill_ctx(&c, &be, bounce, sizeof(bounce));

    for (i = 0; i < 5u; i++) {
        put_sqe(ASQ_BASE, i, HYPE_NVME_ADMIN_SET_FEATURES, (uint16_t)(0x100u + i), 0, GRAM_BASE, 0, 0, 0);
    }
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 5u);
    CHECK_HEX("all five processed", 5, hype_nvme_process_sq(&d, 0u, &c));
    /* Draining again must do NOTHING: re-running consumed commands is how a controller repeats a write. */
    CHECK_HEX("a second drain does nothing", 0, hype_nvme_process_sq(&d, 0u, &c));
    /* Completions land in order with the right CIDs. */
    for (i = 0; i < 5u; i++) {
        CHECK_HEX("completion CID in order", 0x100u + i, cqe_cid_at(ACQ_BASE, i));
    }
    CHECK_HEX("sq_head advanced to the tail", 5u, d.sq_head[0]);
}

static void test_processor_refuses_before_enable(void) {
    hype_nvme_t d;
    hype_blk_backend_t be;
    hype_nvme_ctx_t c;
    static uint8_t bounce[8192];

    io_backend(&be);
    hype_nvme_reset(&d);
    memset(g_gram, 0, sizeof(g_gram));
    fill_ctx(&c, &be, bounce, sizeof(bounce));

    /* A doorbell written before CC.EN is not a command to run: the queues are not valid yet, so their
     * contents are whatever the guest memory happened to contain. */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 1u);
    CHECK_HEX("no processing before CC.EN", -1, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("a bad qid is refused", -1, hype_nvme_process_sq(&d, HYPE_NVME_MAX_QUEUES, &c));
    CHECK_HEX("NULL ctx refused", -1, hype_nvme_process_sq(&d, 0u, 0));
}


static int g_gram_fail_at_cqe = 0;
static uint64_t g_gram_fail_gpa = 0;
static int gram_write_selective(void *ctx, uint64_t gpa, uint32_t len, const void *src) {
    if (g_gram_fail_at_cqe && gpa == g_gram_fail_gpa) return -1;
    return gram_write(ctx, gpa, len, src);
}
static int g_gram_read_fail_gpa_set = 0;
static uint64_t g_gram_read_fail_gpa = 0;
static int gram_read_selective(void *ctx, uint64_t gpa, uint32_t len, void *dst) {
    if (g_gram_read_fail_gpa_set && gpa == g_gram_read_fail_gpa) return -1;
    return gram_read(ctx, gpa, len, dst);
}

static void test_admin_refusals(void) {
    hype_nvme_t d;
    hype_blk_backend_t be;
    hype_nvme_ctx_t c;
    static uint8_t bounce[8192];

    io_backend(&be);
    enable_with_admin_queues(&d);
    fill_ctx(&c, &be, bounce, sizeof(bounce));

    /* An unsupported CNS is a FIELD error, not an OPCODE error -- the driver asked a valid question
     * hype cannot answer, and it distinguishes the two. */
    put_sqe(ASQ_BASE, 0, HYPE_NVME_ADMIN_IDENTIFY, 1u, 0, GRAM_BASE + 0x2000ull, 0x0Du, 0, 0);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 1u);
    CHECK_HEX("processed", 1, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("an unknown CNS is a FIELD error", HYPE_NVME_SC_INVALID_FIELD,
              cqe_status_at(ACQ_BASE, 0));

    /* Only one I/O queue pair is modelled; another id is refused rather than aliased onto it. */
    put_sqe(ASQ_BASE, 1, HYPE_NVME_ADMIN_CREATE_IO_CQ, 2u, 0, IOCQ_BASE, 7u, 0, 0);
    put_sqe(ASQ_BASE, 2, HYPE_NVME_ADMIN_CREATE_IO_SQ, 3u, 0, IOSQ_BASE, 7u, 0, 0);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 3u);
    CHECK_HEX("processed", 2, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("CQ id 7 refused", HYPE_NVME_SC_INVALID_FIELD, cqe_status_at(ACQ_BASE, 1));
    CHECK_HEX("SQ id 7 refused", HYPE_NVME_SC_INVALID_FIELD, cqe_status_at(ACQ_BASE, 2));

    /* IDENTIFY with a bounce too small to hold the payload. */
    fill_ctx(&c, &be, bounce, 64u);
    put_sqe(ASQ_BASE, 3, HYPE_NVME_ADMIN_IDENTIFY, 4u, 0, GRAM_BASE + 0x2000ull,
            HYPE_NVME_CNS_CONTROLLER, 0, 0);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 4u);
    CHECK_HEX("processed", 1, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("a too-small bounce is a FIELD error", HYPE_NVME_SC_INVALID_FIELD,
              cqe_status_at(ACQ_BASE, 3));

    /* IDENTIFY whose destination is unmapped. */
    fill_ctx(&c, &be, bounce, sizeof(bounce));
    put_sqe(ASQ_BASE, 4, HYPE_NVME_ADMIN_IDENTIFY, 5u, 0, 0xDEAD0000ull, HYPE_NVME_CNS_NAMESPACE, 0, 0);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 5u);
    CHECK_HEX("processed", 1, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("an unmapped identify destination is a transfer error", HYPE_NVME_SC_DATA_XFER_ERROR,
              cqe_status_at(ACQ_BASE, 4));
}

static void test_processor_stops_rather_than_losing_a_command(void) {
    hype_nvme_t d;
    hype_blk_backend_t be;
    hype_nvme_ctx_t c;
    static uint8_t bounce[8192];

    io_backend(&be);
    enable_with_admin_queues(&d);
    fill_ctx(&c, &be, bounce, sizeof(bounce));

    /*
     * If the SQE itself cannot be fetched, STOP without advancing. Advancing would drop the command
     * silently and the guest would wait forever for a completion that can never come.
     */
    c.gread = gram_read_selective;
    g_gram_read_fail_gpa_set = 1;
    g_gram_read_fail_gpa = ASQ_BASE;
    put_sqe(ASQ_BASE, 0, HYPE_NVME_ADMIN_SET_FEATURES, 1u, 0, GRAM_BASE, 0, 0, 0);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 1u);
    CHECK_HEX("nothing processed when the SQE is unreadable", 0, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("and the head did NOT advance past it", 0u, d.sq_head[0]);
    g_gram_read_fail_gpa_set = 0;

    /* If the completion cannot be posted, report what was done rather than looping. */
    fill_ctx(&c, &be, bounce, sizeof(bounce));
    c.gwrite = gram_write_selective;
    g_gram_fail_at_cqe = 1;
    g_gram_fail_gpa = ACQ_BASE;
    CHECK_HEX("an unpostable completion stops the drain", 0, hype_nvme_process_sq(&d, 0u, &c));
    g_gram_fail_at_cqe = 0;
}

static void test_processor_needs_queue_bases(void) {
    hype_nvme_t d;
    hype_blk_backend_t be;
    hype_nvme_ctx_t c;
    static uint8_t bounce[8192];

    io_backend(&be);
    hype_nvme_reset(&d);
    memset(g_gram, 0, sizeof(g_gram));
    fill_ctx(&c, &be, bounce, sizeof(bounce));
    /* Enabled, but ASQ/ACQ never written: the guest has not said where the queues are. */
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_CC, HYPE_NVME_CC_EN);
    hype_nvme_mmio_write32(&d, HYPE_NVME_REG_DOORBELL_BASE, 1u);
    CHECK_HEX("no admin queue base means refuse", -1, hype_nvme_process_sq(&d, 0u, &c));

    /* Missing callbacks. */
    enable_with_admin_queues(&d);
    fill_ctx(&c, &be, bounce, sizeof(bounce));
    c.gread = 0;
    CHECK_HEX("no gread refused", -1, hype_nvme_process_sq(&d, 0u, &c));
    fill_ctx(&c, &be, bounce, sizeof(bounce));
    c.gwrite = 0;
    CHECK_HEX("no gwrite refused", -1, hype_nvme_process_sq(&d, 0u, &c));
    CHECK_HEX("NULL dev refused", -1, hype_nvme_process_sq(0, 0u, &c));
}


int main(void) {
    test_phase_starts_at_one();
    test_phase_toggles_only_on_wrap();
    test_phase_is_per_queue();
    test_enable_handshake_and_reset();
    test_capabilities_advertise_what_hype_actually_does();
    test_doorbell_decode_refuses_rather_than_clamps();
    test_out_of_range_doorbell_value_is_refused();
    test_queue_bases_are_64bit();
    test_unmodelled_registers_are_inert();
    test_remaining_register_paths();
    test_sqe_decode();
    test_cqe_status_does_not_collide_with_phase();
    test_identify_controller();
    test_identify_namespace_block_size_is_a_power_of_two();
    test_prp_single_page_with_offset();
    test_prp_first_segment_stops_at_the_page_end();
    test_prp2_is_a_data_page_when_the_transfer_is_small();
    test_prp2_is_a_list_when_the_transfer_is_larger();
    test_prp_list_chains_at_the_last_slot();
    test_prp_refuses_malformed_descriptors();
    test_io_read_single_block_nlb_is_zero_based();
    test_io_read_write_roundtrip_multi_sector();
    test_io_uses_the_full_64bit_slba();
    test_io_bounds_and_error_paths();
    test_io_spanning_two_prp_pages();
    test_io_backend_and_callback_failures_are_reported();
    test_io_zero_size_disk_refuses_everything();
    test_admin_identify_round_trip();
    test_io_command_end_to_end_through_the_processor();
    test_failing_command_still_gets_a_completion();
    test_processor_consumes_each_entry_exactly_once();
    test_processor_refuses_before_enable();
    test_admin_refusals();
    test_processor_stops_rather_than_losing_a_command();
    test_processor_needs_queue_bases();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
