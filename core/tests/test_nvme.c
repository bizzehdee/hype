#include <stdio.h>
#include <string.h>
#include "../../devices/nvme.h"

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

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
