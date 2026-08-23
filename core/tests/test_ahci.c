#include <stdio.h>
#include <string.h>
#include "../../devices/ahci.h"
#include "../../arch/x86_64/svm/svm.h" /* hype_svm_vcpu_get_atapi_diag() */
#include "../fatal.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/*
 * #672: process_ahci_ata_command_slot() (arch/x86_64/svm/svm_vcpu.c, shared verbatim by the
 * VMX MMIO handler) translates the guest-programmed command-list pointer (PxCLB/PxCLBU, fully
 * guest-controlled) through the VM's gpa map and used to dereference it with NO null check --
 * unlike its ATAPI sibling process_ahci_command_slot(), which has always had one. A guest
 * pointing PxCLB/PxCLBU outside its own mapped range crashed the host. This pins the fix: the
 * function must refuse (return -1) rather than dereference NULL.
 */
static void test_ata_command_slot_refuses_out_of_range_command_list(void) {
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_gpa_map_t map;
    uint8_t backing[4096];
    int rc;

    hype_ahci_reset(&ahci);
    memset(&disk, 0, sizeof(disk));
    /* One small mapped region, [0, 0x1000). PxCLB/PxCLBU below point well outside it. */
    hype_gpa_map_reset(&map);
    hype_gpa_map_add(&map, 0x0ull, (uint64_t)(uintptr_t)backing, sizeof(backing));
    ahci.p_clb = 0xFFFF0000u;
    ahci.p_clbu = 0u;
    ahci.p_fb = 0u;
    ahci.p_fbu = 0u;

    /*
     * The refusal path logs via hype_debug_print(), which is level-filtered but NOT
     * host-safe below that filter -- it falls through to real UART port I/O
     * (core/serial_hw.c), exempt from unit testing per AGENTS.md because outb/inb only
     * make sense with a real device. Raising the level requirement above DEBUG makes the
     * call a no-op short-circuit, so this test proves the null check without touching
     * hardware. Restored unconditionally so a later test in this binary is unaffected.
     */
    hype_debug_set_level(HYPE_LOG_ERROR);
    rc = process_ahci_ata_command_slot(&ahci, &disk, &map, 0u);
    hype_debug_set_level(HYPE_LOG_DEBUG);
    CHECK_HEX("out-of-range command list is refused, not dereferenced", (unsigned)-1, (unsigned)rc);
}

static void test_reset_state(void) {
    hype_ahci_t ahci;
    hype_ahci_reset(&ahci);

    CHECK_HEX("CAP.S64A set", 1, (ahci.cap & (1u << 31)) != 0);
    CHECK_HEX("CAP.SAM set", 1, (ahci.cap & (1u << 18)) != 0);
    CHECK_HEX("CAP.NCS advertises 32 command slots", 31, (ahci.cap >> 8) & 0x1Fu);
    CHECK_HEX("PI has port 0 implemented", 1, ahci.pi & 0x1u);
    CHECK_HEX("VS is 1.3.1", 0x00010301u, ahci.vs);
    CHECK_HEX("PxSIG is ATAPI", HYPE_AHCI_SIG_ATAPI, ahci.p_sig);
    CHECK_HEX("PxSSTS reports device present", 0x123u, ahci.p_ssts);
    CHECK_HEX("PxCMD starts stopped (HPCP aside)", 0, ahci.p_cmd & ~(uint32_t)HYPE_AHCI_PXCMD_HPCP);
}

static void test_read_write_clb_fb(void) {
    hype_ahci_t ahci;
    uint32_t value;
    hype_ahci_reset(&ahci);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CLB, 4, 0x1000u);
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CLBU, 4, 0x2u);
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_FB, 4, 0x3000u);
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_FBU, 4, 0x4u);

    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CLB, 4, &value);
    CHECK_HEX("PxCLB round trip", 0x1000u, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CLBU, 4, &value);
    CHECK_HEX("PxCLBU round trip", 0x2u, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_FB, 4, &value);
    CHECK_HEX("PxFB round trip", 0x3000u, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_FBU, 4, &value);
    CHECK_HEX("PxFBU round trip", 0x4u, value);
}

static void test_pcmd_start_mirrors_running_bits(void) {
    hype_ahci_t ahci;
    uint32_t value;
    hype_ahci_reset(&ahci);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD, 4,
                          HYPE_AHCI_PCMD_ST | HYPE_AHCI_PCMD_FRE);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD, 4, &value);
    CHECK_HEX("ST set", 1, (value & HYPE_AHCI_PCMD_ST) != 0);
    CHECK_HEX("CR mirrors ST immediately", 1, (value & HYPE_AHCI_PCMD_CR) != 0);
    CHECK_HEX("FRE set", 1, (value & HYPE_AHCI_PCMD_FRE) != 0);
    CHECK_HEX("FR mirrors FRE immediately", 1, (value & HYPE_AHCI_PCMD_FR) != 0);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD, 4, 0);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD, 4, &value);
    CHECK_HEX("clearing ST/FRE clears CR/FR too (HPCP aside)", 0, value & ~(uint32_t)HYPE_AHCI_PXCMD_HPCP);
}

static void test_ghc_hr_self_clears(void) {
    hype_ahci_t ahci;
    uint32_t value;
    hype_ahci_reset(&ahci);

    /* A real AHCI driver (EDK2 AhciReset) sets GHC.AE, then sets GHC.HR
     * and polls GHC until HR reads back clear. HR is self-clearing on
     * real hardware, so the model must report it clear immediately (else
     * the driver spins until timeout and abandons the controller). AE
     * and every other bit written alongside must be preserved. */
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_GHC, 4, HYPE_AHCI_GHC_AE);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_GHC, 4, &value);
    CHECK_HEX("AE stored", HYPE_AHCI_GHC_AE, value);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_GHC, 4, HYPE_AHCI_GHC_AE | HYPE_AHCI_GHC_HR);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_GHC, 4, &value);
    CHECK_HEX("HR reads back clear immediately", 0, (value & HYPE_AHCI_GHC_HR));
    CHECK_HEX("AE preserved across the HR write", HYPE_AHCI_GHC_AE, (value & HYPE_AHCI_GHC_AE));
}

static void test_is_rw1c(void) {
    hype_ahci_t ahci;
    uint32_t value;
    hype_ahci_reset(&ahci);
    ahci.is = 0x3u;
    ahci.p_is = 0x5u;
    ahci.p_serr = 0x7u;
    ahci.p_sntf = 0x9u;

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_IS, 4, 0x1u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_IS, 4, &value);
    CHECK_HEX("IS bit 0 cleared, bit 1 untouched", 0x2u, value);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IS, 4, 0x4u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IS, 4, &value);
    CHECK_HEX("PxIS bit 2 cleared, bit 0 untouched", 0x1u, value);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SERR, 4, 0x1u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SERR, 4, &value);
    CHECK_HEX("PxSERR bit 0 cleared, bit 1/2 untouched", 0x6u, value);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SNTF, 4, 0x8u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SNTF, 4, &value);
    CHECK_HEX("PxSNTF bit 3 cleared, bit 0 untouched", 0x1u, value);
}

static void test_ci_write_ors_in_bits(void) {
    hype_ahci_t ahci;
    uint32_t value;
    hype_ahci_reset(&ahci);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI, 4, 0x1u);
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI, 4, 0x4u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI, 4, &value);
    CHECK_HEX("PxCI accumulates issued slots", 0x5u, value);
}

static void test_irq_pending_conditions(void) {
    hype_ahci_t ahci;
    hype_ahci_reset(&ahci);

    /* Reset state: no interrupt asserted. */
    CHECK_HEX("irq idle after reset", 0, hype_ahci_irq_pending(&ahci));

    /* A completion status bit alone (PxIS) is not enough -- GHC.IE and
     * PxIE must both be enabled for the HBA to assert its line. */
    ahci.p_is = HYPE_AHCI_PIS_DHRS;
    CHECK_HEX("PxIS alone: no irq (GHC.IE+PxIE clear)", 0, hype_ahci_irq_pending(&ahci));

    ahci.p_ie = HYPE_AHCI_PIS_DHRS;
    CHECK_HEX("PxIS&PxIE but GHC.IE clear: no irq", 0, hype_ahci_irq_pending(&ahci));

    ahci.ghc |= HYPE_AHCI_GHC_IE;
    CHECK_HEX("GHC.IE + (PxIS&PxIE): irq asserts", 1, hype_ahci_irq_pending(&ahci));

    /* A status bit the driver did not enable must not assert. */
    ahci.p_is = HYPE_AHCI_PIS_PSS;
    ahci.p_ie = HYPE_AHCI_PIS_DHRS;
    CHECK_HEX("PxIS bit not in PxIE: no irq", 0, hype_ahci_irq_pending(&ahci));

    /* Clearing PxIS (RW1C, what a driver's ISR does) deasserts the
     * level-sensitive line. */
    ahci.p_is = HYPE_AHCI_PIS_DHRS;
    ahci.p_ie = HYPE_AHCI_PIS_DHRS;
    CHECK_HEX("re-armed: irq asserts", 1, hype_ahci_irq_pending(&ahci));
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IS, 4, HYPE_AHCI_PIS_DHRS);
    CHECK_HEX("PxIS cleared: irq deasserts", 0, hype_ahci_irq_pending(&ahci));
}

/*
 * #512 regression. Interrupt-condition edges are counted by the model, so an MSI sender that
 * missed a fall-and-rise between two of its polls (a second vCPU's ISR clearing PxIS and a new
 * completion re-posting it) still sees the count move. One increment per 0->1 of irq_pending.
 */
static void test_irq_events_counts_model_edges(void) {
    hype_ahci_t ahci;
    hype_ahci_reset(&ahci);

    CHECK_HEX("no events after reset", 0, (unsigned)ahci.irq_events);

    /* Completion posted while interrupts are fully off: no edge. */
    hype_ahci_set_pis(&ahci, HYPE_AHCI_PIS_DHRS);
    CHECK_HEX("completion with IE off is no edge", 0, (unsigned)ahci.irq_events);

    /* Enabling PxIE then GHC.IE over the pending bit: the edge lands on the
     * write that completes the condition. */
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IE, 4, HYPE_AHCI_PIS_DHRS);
    CHECK_HEX("PxIE alone (GHC.IE clear) is no edge", 0, (unsigned)ahci.irq_events);
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_GHC, 4, HYPE_AHCI_GHC_IE);
    CHECK_HEX("GHC.IE completing the condition is an edge", 1, (unsigned)ahci.irq_events);

    /* A second completion while the line is already high coalesces. */
    hype_ahci_set_pis(&ahci, HYPE_AHCI_PIS_PSS);
    CHECK_HEX("completion while asserted is no new edge", 1, (unsigned)ahci.irq_events);

    /* The ISR clears PxIS (RW1C); the NEXT completion is a new edge -- the
     * exact fall-and-rise the sampled-level MSI latch used to swallow. */
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IS, 4, 0xFFFFFFFFu);
    CHECK_HEX("service is not an edge", 1, (unsigned)ahci.irq_events);
    hype_ahci_set_pis(&ahci, HYPE_AHCI_PIS_DHRS);
    CHECK_HEX("completion after service is a new edge", 2, (unsigned)ahci.irq_events);
}

static void test_reserved_register_reads_zero_and_ignores_writes(void) {
    hype_ahci_t ahci;
    uint32_t value;
    hype_ahci_reset(&ahci);

    CHECK_HEX("write to reserved-but-in-range offset succeeds silently", 0,
              hype_ahci_mmio_write(&ahci, 0x2Cu, 4, 0xFFFFFFFFu));
    CHECK_HEX("read of reserved-but-in-range offset returns 0", 0,
              hype_ahci_mmio_read(&ahci, 0x2Cu, 4, &value));
    CHECK_HEX("value is 0", 0, value);
}

static void test_rejects_misaligned_and_wrong_width(void) {
    hype_ahci_t ahci;
    uint32_t value;
    hype_ahci_reset(&ahci);

    if (hype_ahci_mmio_read(&ahci, 0x01u, 4, &value) == 0) {
        printf("FAIL: misaligned read should be rejected\n");
        failures++;
    }
    if (hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_CAP, 1, &value) == 0) {
        printf("FAIL: 1-byte read should be rejected\n");
        failures++;
    }
    if (hype_ahci_mmio_write(&ahci, 0x02u, 4, 0) == 0) {
        printf("FAIL: misaligned write should be rejected\n");
        failures++;
    }
}

static void test_rejects_out_of_range_offset(void) {
    hype_ahci_t ahci;
    uint32_t value;
    hype_ahci_reset(&ahci);

    if (hype_ahci_mmio_read(&ahci, HYPE_AHCI_MMIO_SIZE, 4, &value) == 0) {
        printf("FAIL: out-of-range read should be rejected\n");
        failures++;
    }
    if (hype_ahci_mmio_write(&ahci, HYPE_AHCI_MMIO_SIZE, 4, 0) == 0) {
        printf("FAIL: out-of-range write should be rejected\n");
        failures++;
    }
}

static void test_read_every_generic_and_port_register(void) {
    hype_ahci_t ahci;
    uint32_t value;

    hype_ahci_reset(&ahci);
    ahci.ghc = 0x11;
    ahci.ccc_ctl = 0x12;
    ahci.ccc_ports = 0x13;
    ahci.em_loc = 0x14;
    ahci.em_ctl = 0x15;
    ahci.cap2 = 0x16;
    ahci.bohc = 0x17;
    ahci.p_ie = 0x21;
    ahci.p_sctl = 0x22;
    ahci.p_sact = 0x23;

    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_CAP, 4, &value);
    CHECK_HEX("CAP read", ahci.cap, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_GHC, 4, &value);
    CHECK_HEX("GHC read", 0x11, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_PI, 4, &value);
    CHECK_HEX("PI read", ahci.pi, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_VS, 4, &value);
    CHECK_HEX("VS read", ahci.vs, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_CCC_CTL, 4, &value);
    CHECK_HEX("CCC_CTL read", 0x12, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_CCC_PORTS, 4, &value);
    CHECK_HEX("CCC_PORTS read", 0x13, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_EM_LOC, 4, &value);
    CHECK_HEX("EM_LOC read", 0x14, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_EM_CTL, 4, &value);
    CHECK_HEX("EM_CTL read", 0x15, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_CAP2, 4, &value);
    CHECK_HEX("CAP2 read", 0x16, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_BOHC, 4, &value);
    CHECK_HEX("BOHC read", 0x17, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IE, 4, &value);
    CHECK_HEX("PxIE read", 0x21, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_TFD, 4, &value);
    CHECK_HEX("PxTFD read", ahci.p_tfd, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SIG, 4, &value);
    CHECK_HEX("PxSIG read", HYPE_AHCI_SIG_ATAPI, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SSTS, 4, &value);
    CHECK_HEX("PxSSTS read", ahci.p_ssts, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SCTL, 4, &value);
    CHECK_HEX("PxSCTL read", 0x22, value);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SACT, 4, &value);
    CHECK_HEX("PxSACT read", 0x23, value);
}

static void test_write_every_writable_register(void) {
    hype_ahci_t ahci;
    uint32_t value;

    hype_ahci_reset(&ahci);

    /* GHC.HR (bit 0) is self-clearing (HBA reset completes instantly in
     * the model), so 0xA1 written reads back as 0xA0 -- every other bit
     * stored, HR reported clear. See test_ghc_hr_self_clears(). */
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_GHC, 4, 0xA1u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_GHC, 4, &value);
    CHECK_HEX("GHC write (HR self-clears)", 0xA0u, value);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_CCC_CTL, 4, 0xA2u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_CCC_CTL, 4, &value);
    CHECK_HEX("CCC_CTL write", 0xA2u, value);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_EM_CTL, 4, 0xA3u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_EM_CTL, 4, &value);
    CHECK_HEX("EM_CTL write", 0xA3u, value);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_BOHC, 4, 0xA4u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_BOHC, 4, &value);
    CHECK_HEX("BOHC write", 0xA4u, value);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IE, 4, 0xA5u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IE, 4, &value);
    CHECK_HEX("PxIE write", 0xA5u, value);

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SCTL, 4, 0xA6u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SCTL, 4, &value);
    CHECK_HEX("PxSCTL write", 0xA6u, value);

    /* Clearing only FRE (leaving ST set) should drop FR but keep CR --
     * exercises the CMD write handler's else-branches independently. */
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD, 4,
                          HYPE_AHCI_PCMD_ST | HYPE_AHCI_PCMD_FRE);
    hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD, 4, HYPE_AHCI_PCMD_ST);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD, 4, &value);
    CHECK_HEX("CR still set (ST still set)", 1, (value & HYPE_AHCI_PCMD_CR) != 0);
    CHECK_HEX("FR cleared (FRE cleared)", 0, (value & HYPE_AHCI_PCMD_FR) != 0);
}

static void test_decode_cmd_header(void) {
    /* CFL=5, ATAPI(bit5)=1, WRITE(bit6)=0, PRDTL=3 -> opts =
     * 0x00030025 (PRDTL<<16 | 0x25). CTBA=0x2000, CTBAU=0x1. */
    uint8_t raw[32] = {0};
    hype_ahci_cmd_header_t hdr;

    raw[0] = 0x25;
    raw[1] = 0x00;
    raw[2] = 0x03; /* PRDTL low byte at bit16 -> byte 2 */
    raw[3] = 0x00;
    raw[8] = 0x00;
    raw[9] = 0x20;
    raw[10] = 0x00;
    raw[11] = 0x00; /* CTBA = 0x00002000 */
    raw[12] = 0x01;
    raw[13] = 0x00;
    raw[14] = 0x00;
    raw[15] = 0x00; /* CTBAU = 0x00000001 */

    hype_ahci_decode_cmd_header(raw, &hdr);

    CHECK_HEX("cfl", 5, hdr.cfl);
    CHECK_HEX("is_atapi", 1, hdr.is_atapi);
    CHECK_HEX("is_write", 0, hdr.is_write);
    CHECK_HEX("prdtl", 3, hdr.prdtl);
    CHECK_HEX("cmd_table_phys", 0x0000000100002000ULL, hdr.cmd_table_phys);
}

static void test_decode_cmd_header_write_bit(void) {
    uint8_t raw[32] = {0};
    hype_ahci_cmd_header_t hdr;
    raw[0] = 0x40 | 0x05; /* WRITE bit (0x40) | CFL=5 */

    hype_ahci_decode_cmd_header(raw, &hdr);

    CHECK_HEX("is_write set", 1, hdr.is_write);
    CHECK_HEX("is_atapi clear", 0, hdr.is_atapi);
}

static void test_decode_prdt_entry(void) {
    uint8_t raw[16] = {0};
    hype_ahci_prdt_entry_t prd;

    raw[0] = 0x00;
    raw[1] = 0x10;
    raw[2] = 0x00;
    raw[3] = 0x00; /* DBA = 0x00001000 */
    raw[4] = 0x02;
    raw[5] = 0x00;
    raw[6] = 0x00;
    raw[7] = 0x00; /* DBAU = 0x00000002 */
    /* DBC field (bits 21:0) = 2047 -> actual byte count = 2048; I bit (bit31) set */
    raw[12] = 0xFF;
    raw[13] = 0x07;
    raw[14] = 0x00;
    raw[15] = 0x80;

    hype_ahci_decode_prdt_entry(raw, &prd);

    CHECK_HEX("data_phys", 0x0000000200001000ULL, prd.data_phys);
    CHECK_HEX("byte_count is DBC+1", 2048u, prd.byte_count);
}

static void test_decode_h2d_fis(void) {
    uint8_t raw[20];
    hype_ahci_h2d_fis_t fis;
    unsigned i;

    for (i = 0; i < 20; i++) {
        raw[i] = 0;
    }
    raw[0] = 0x27;               /* FIS type: Register H2D */
    raw[1] = 0x80;               /* C bit set */
    raw[2] = 0x25; /* READ DMA EXT (devices/ata_disk.h's own HYPE_ATA_CMD_READ_DMA_EXT) */
    raw[4] = 0x11;               /* LBA[7:0] */
    raw[5] = 0x22;               /* LBA[15:8] */
    raw[6] = 0x33;               /* LBA[23:16] */
    raw[7] = 0x40;               /* Device register */
    raw[8] = 0x44;               /* LBA[31:24] */
    raw[9] = 0x55;               /* LBA[39:32] */
    raw[10] = 0x66;              /* LBA[47:40] */
    raw[12] = 0x34;              /* Count low */
    raw[13] = 0x12;              /* Count high */

    hype_ahci_decode_h2d_fis(raw, &fis);

    CHECK_HEX("command decoded", 0x25u, fis.command);
    CHECK_HEX("48-bit LBA decoded from both the low and expanded byte groups",
              0x0000665544332211ULL, fis.lba);
    CHECK_HEX("device register decoded", 0x40u, fis.device);
    CHECK_HEX("16-bit count decoded", 0x1234u, fis.count);
}

/* --- #309: software reset --- */

/* A Register H2D FIS: type 0x27, C bit per `c_bit`, command 0x00, Control byte `control`. */
static void build_control_fis(uint8_t fis[20], int c_bit, uint8_t control) {
    unsigned int i;
    for (i = 0; i < 20u; i++) {
        fis[i] = 0;
    }
    fis[0] = 0x27u;
    fis[1] = c_bit ? HYPE_AHCI_FIS_H2D_FLAG_C : 0u;
    fis[15] = control;
}

static void test_control_write_is_told_apart_from_a_command(void) {
    uint8_t fis[20];

    build_control_fis(fis, 0, HYPE_AHCI_ATA_CONTROL_SRST);
    CHECK_HEX("C clear is a Control write", 1, hype_ahci_h2d_is_control_write(fis));
    build_control_fis(fis, 1, 0);
    CHECK_HEX("C set is a command", 0, hype_ahci_h2d_is_control_write(fis));
}

static void test_soft_reset_sequence(void) {
    hype_ahci_t ahci;

    hype_ahci_reset(&ahci);
    CHECK_HEX("no reset in progress after power-on", 0, ahci.srst_asserted);

    /* First half: SRST asserted. The device goes BSY and posts NOTHING -- the driver is not
     * waiting on a FIS yet, and posting one here would report a completed reset. */
    ahci.p_ci = 0x5u; /* slots 0 and 2 outstanding */
    CHECK_HEX("SRST posts no FIS", 0, hype_ahci_soft_reset(&ahci, HYPE_AHCI_ATA_CONTROL_SRST, 0u));
    CHECK_HEX("reset recorded", 1, ahci.srst_asserted);
    CHECK_HEX("device is BSY", 0x80u, ahci.p_tfd);
    CHECK_HEX("slot 0 completed", 0x4u, ahci.p_ci);

    /* Second half: SRST released. NOW the signature FIS is delivered. */
    CHECK_HEX("release posts a FIS", 1, hype_ahci_soft_reset(&ahci, 0u, 2u));
    CHECK_HEX("reset no longer in progress", 0, ahci.srst_asserted);
    CHECK_HEX("device ready, not busy", 0x50u, ahci.p_tfd);
    CHECK_HEX("slot 2 completed", 0u, ahci.p_ci);
}

static void test_release_without_an_asserted_reset_posts_nothing(void) {
    /*
     * Drivers write the Control register for reasons other than reset (nIEN), so this is
     * accepted and the slot completed -- but announcing a reset that never started would tell
     * the driver the device had just re-identified itself.
     */
    hype_ahci_t ahci;

    hype_ahci_reset(&ahci);
    ahci.p_ci = 0x1u;
    CHECK_HEX("no FIS posted", 0, hype_ahci_soft_reset(&ahci, 0u, 0u));
    CHECK_HEX("slot still completed", 0u, ahci.p_ci);
    CHECK_HEX("task file untouched", 0x50u, ahci.p_tfd);
}

static void test_soft_reset_clears_serr(void) {
    hype_ahci_t ahci;

    hype_ahci_reset(&ahci);
    ahci.p_serr = 0xDEADBEEFu;
    (void)hype_ahci_soft_reset(&ahci, HYPE_AHCI_ATA_CONTROL_SRST, 0u);
    CHECK_HEX("SERR survives the assert half", 0xDEADBEEFu, ahci.p_serr);
    (void)hype_ahci_soft_reset(&ahci, 0u, 0u);
    CHECK_HEX("and is cleared by the completed reset", 0u, ahci.p_serr);
}

static void test_signature_fis_carries_the_atapi_signature(void) {
    /*
     * The signature is the whole point of the completion FIS: it is how the driver learns
     * whether it reset a packet device or a disk. PxSIG packs the four registers as
     * (LBA_HIGH<<24)|(LBA_MID<<16)|(LBA_LOW<<8)|COUNT, so 0xEB140101 must land as
     * LBA high 0xEB / mid 0x14 / low 0x01 / count 1.
     */
    uint8_t fis[20];
    unsigned int i;

    hype_ahci_build_signature_fis(fis, 0x50u, 0u, HYPE_AHCI_SIG_ATAPI);
    CHECK_HEX("D2H Register FIS", 0x34u, fis[0]);
    CHECK_HEX("interrupt bit", 0x40u, fis[1]);
    CHECK_HEX("status", 0x50u, fis[2]);
    CHECK_HEX("error", 0u, fis[3]);
    CHECK_HEX("LBA low", 0x01u, fis[4]);
    CHECK_HEX("LBA mid", 0x14u, fis[5]);
    CHECK_HEX("LBA high", 0xEBu, fis[6]);
    CHECK_HEX("sector count", 0x01u, fis[12]);
    /* Everything else must be zeroed, not left holding whatever was on the stack. */
    for (i = 7u; i < 12u; i++) {
        CHECK_HEX("reserved byte zeroed", 0u, fis[i]);
    }
    CHECK_HEX("count high zeroed", 0u, fis[13]);
}

static void test_signature_fis_carries_the_plain_disk_signature(void) {
    /* #262's second HBA presents a disk; a driver that reset it must not be told ATAPI. */
    uint8_t fis[20];

    hype_ahci_build_signature_fis(fis, 0x50u, 0u, HYPE_AHCI_SIG_ATA);
    CHECK_HEX("LBA low", 0x01u, fis[4]);
    CHECK_HEX("LBA mid zero", 0u, fis[5]);
    CHECK_HEX("LBA high zero", 0u, fis[6]);
    CHECK_HEX("sector count", 0x01u, fis[12]);
}

static void test_d2h_fis_reports_the_result_registers(void) {
    /*
     * #314: the D2H Register FIS is what a driver reads to learn a command's outcome. The
     * completion paths pass flags 0 (hype sets PxIS itself), so the I bit must NOT appear
     * unasked -- that would tell a driver the device requested an interrupt hype never
     * modelled through this FIS.
     */
    uint8_t fis[20];
    unsigned int i;

    for (i = 0; i < 20u; i++) {
        fis[i] = 0xAAu; /* poison: the builder must zero what it does not set */
    }
    hype_ahci_build_d2h_fis(fis, 0u, 0x50u, 0u);
    CHECK_HEX("D2H Register FIS type", HYPE_AHCI_FIS_TYPE_D2H_REGISTER, fis[0]);
    CHECK_HEX("no interrupt bit when not asked", 0u, fis[1]);
    CHECK_HEX("status", 0x50u, fis[2]);
    CHECK_HEX("error", 0u, fis[3]);
    for (i = 4u; i < 20u; i++) {
        CHECK_HEX("trailing byte zeroed", 0u, fis[i]);
    }
}

static void test_d2h_fis_passes_flags_through_and_reports_an_error(void) {
    /* The signature path asks for I; an error completion must carry a non-zero Error byte. */
    uint8_t fis[20];

    hype_ahci_build_d2h_fis(fis, (uint8_t)HYPE_AHCI_FIS_D2H_FLAG_I, 0x51u, 0x04u);
    CHECK_HEX("interrupt bit passed through", HYPE_AHCI_FIS_D2H_FLAG_I, fis[1]);
    CHECK_HEX("error status", 0x51u, fis[2]);
    CHECK_HEX("error register", 0x04u, fis[3]);
}

static void test_pio_setup_fis_carries_e_status_and_transfer_count(void) {
    /*
     * #314: FreeBSD ends a PIO-in transaction from THIS FIS's E_Status (byte 15) and 16-bit
     * Transfer Count (bytes 16-17), not from the PxIS.PSS bit EDK2 waits on. A 512-byte
     * IDENTIFY must therefore report 0x0200 split little-endian across two bytes -- getting
     * the byte order wrong here reads as a 2-byte transfer and is exactly the class of bug
     * that made ATAPI_IDENTIFY time out on a command hype had completed correctly.
     */
    uint8_t fis[20];
    unsigned int i;

    for (i = 0; i < 20u; i++) {
        fis[i] = 0xAAu;
    }
    hype_ahci_build_pio_setup_fis(fis, 0x50u, 0u, 512u);
    CHECK_HEX("PIO Setup FIS type", HYPE_AHCI_FIS_TYPE_PIO_SETUP, fis[0]);
    CHECK_HEX("I + D flags", HYPE_AHCI_FIS_D2H_FLAG_I | HYPE_AHCI_FIS_PIO_FLAG_D, fis[1]);
    CHECK_HEX("status at the start of the transfer", 0x50u, fis[2]);
    CHECK_HEX("error", 0u, fis[3]);
    CHECK_HEX("E_Status at the end of the transfer", 0x50u, fis[15]);
    CHECK_HEX("transfer count low", 0x00u, fis[16]);
    CHECK_HEX("transfer count high", 0x02u, fis[17]);
    /* The register block between Error and E_Status must be zeroed, not poison. */
    for (i = 4u; i < 15u; i++) {
        CHECK_HEX("register byte zeroed", 0u, fis[i]);
    }
    CHECK_HEX("trailing reserved zeroed", 0u, fis[18]);
    CHECK_HEX("trailing reserved zeroed", 0u, fis[19]);
}

static void test_pio_setup_fis_truncates_transfer_count_to_16_bits(void) {
    /*
     * The Transfer Count field is 16 bits wide. A caller handing over a larger byte count
     * must not have the excess spill into the reserved bytes past it.
     */
    uint8_t fis[20];

    hype_ahci_build_pio_setup_fis(fis, 0x50u, 0u, 0x1234ABCDu);
    CHECK_HEX("transfer count low", 0xCDu, fis[16]);
    CHECK_HEX("transfer count high", 0xABu, fis[17]);
    CHECK_HEX("no spill past the 16-bit field", 0u, fis[18]);
    CHECK_HEX("no spill past the 16-bit field", 0u, fis[19]);
}

static void test_signature_fis_is_a_d2h_fis(void) {
    /*
     * #314 folded three hand-rolled FIS builders into one definition. Pin that the signature
     * FIS really is the D2H builder plus the signature registers, so the two cannot drift
     * apart again -- drift between two such copies is what this ticket was filed for.
     */
    uint8_t sig_fis[20];
    uint8_t d2h_fis[20];
    unsigned int i;

    hype_ahci_build_signature_fis(sig_fis, 0x50u, 0u, HYPE_AHCI_SIG_ATAPI);
    hype_ahci_build_d2h_fis(d2h_fis, (uint8_t)HYPE_AHCI_FIS_D2H_FLAG_I, 0x50u, 0u);
    for (i = 0; i < 4u; i++) {
        CHECK_HEX("header byte matches the plain D2H builder", d2h_fis[i], sig_fis[i]);
    }
}

static void test_global_is_reflects_the_port_level(void) {
    /*
     * #311: IS.IPS is a level reflection of (PxIS & PxIE), not a latch. FreeBSD's ahci_intr()
     * reads the global IS to decide which port fired and dispatches to nobody if the bit is clear,
     * so a latch that cannot come back after an RW1C clear silently strands every completion.
     */
    hype_ahci_t ahci;
    uint32_t v = 0;

    hype_ahci_reset(&ahci);
    CHECK_HEX("no port interrupt at reset", 0u, (hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_IS, 4u, &v),
                                                v));

    /* A completion sets PxIS; with PxIE enabling it, the global bit must read set. */
    ahci.p_is = 0x2u;
    ahci.p_ie = 0x2u;
    (void)hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_IS, 4u, &v);
    CHECK_HEX("port bit reads set while the port has a pending interrupt", HYPE_AHCI_IS_PORT0,
              v & HYPE_AHCI_IS_PORT0);

    /* The guest clears IS (RW1C). While the PORT condition persists, the bit must come BACK. */
    (void)hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_IS, 4u, 0xFFFFFFFFu);
    (void)hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_IS, 4u, &v);
    CHECK_HEX("and comes back after an RW1C clear, because the port is still asserting",
              HYPE_AHCI_IS_PORT0, v & HYPE_AHCI_IS_PORT0);

    /* Once the guest acknowledges PxIS the condition is gone and the bit must read clear. */
    (void)hype_ahci_mmio_write(&ahci, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IS, 4u, 0x2u);
    (void)hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_IS, 4u, &v);
    CHECK_HEX("clear once PxIS is acknowledged", 0u, v & HYPE_AHCI_IS_PORT0);

    /* A bit PxIE does not enable must not raise the global bit -- same gate as the line. */
    ahci.p_is = 0x8u;
    ahci.p_ie = 0x2u;
    (void)hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_IS, 4u, &v);
    CHECK_HEX("a PxIE-disabled status bit does not assert IS", 0u, v & HYPE_AHCI_IS_PORT0);
    CHECK_HEX("and the line agrees", 0, hype_ahci_irq_pending(&ahci));
}

/* #489: AHCI hot-plug. */
static void test_hotplug_capable_at_reset(void) {
    hype_ahci_t a;
    hype_ahci_reset(&a);
    CHECK_HEX("port advertises HPCP at reset", HYPE_AHCI_PXCMD_HPCP,
              a.p_cmd & HYPE_AHCI_PXCMD_HPCP);
    /* A guest p_cmd write (ST|FRE) must not clear HPCP. */
    (void)hype_ahci_mmio_write(&a, HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD, 4,
                               HYPE_AHCI_PCMD_ST | HYPE_AHCI_PCMD_FRE);
    CHECK_HEX("HPCP survives a p_cmd write", HYPE_AHCI_PXCMD_HPCP,
              a.p_cmd & HYPE_AHCI_PXCMD_HPCP);
}

static void test_hotplug_attach_raises_connect_change(void) {
    hype_ahci_t a;
    unsigned long long edges_before;
    hype_ahci_reset(&a);
    /* Start from an empty port and enable interrupts so the attach raises an edge. */
    a.p_ssts = 0;
    a.ghc = HYPE_AHCI_GHC_IE;
    a.p_ie = HYPE_AHCI_PXIS_PRCS | HYPE_AHCI_PXIS_PCS;
    a.p_is = 0;
    edges_before = a.irq_events;
    hype_ahci_hotplug_attach(&a, HYPE_AHCI_SIG_ATA);
    CHECK_HEX("DET=3 after attach", HYPE_AHCI_SSTS_PRESENT, a.p_ssts);
    CHECK_HEX("signature set", HYPE_AHCI_SIG_ATA, a.p_sig);
    CHECK_HEX("PhyRdy change in PxIS", HYPE_AHCI_PXIS_PRCS, a.p_is & HYPE_AHCI_PXIS_PRCS);
    CHECK_HEX("connect change in PxIS", HYPE_AHCI_PXIS_PCS, a.p_is & HYPE_AHCI_PXIS_PCS);
    CHECK_HEX("PhyRdy diag in PxSERR", HYPE_AHCI_PXSERR_DIAG_N, a.p_serr & HYPE_AHCI_PXSERR_DIAG_N);
    CHECK_HEX("irq pending after attach", 1, hype_ahci_irq_pending(&a) != 0);
    CHECK_HEX("one MSI edge counted", 1, (a.irq_events == edges_before + 1ull));
}

static void test_hotplug_detach_clears_det(void) {
    hype_ahci_t a;
    hype_ahci_reset(&a);
    a.ghc = HYPE_AHCI_GHC_IE;
    a.p_ie = HYPE_AHCI_PXIS_PRCS | HYPE_AHCI_PXIS_PCS;
    a.p_is = 0;
    hype_ahci_hotplug_detach(&a);
    CHECK_HEX("DET=0 after detach", 0, a.p_ssts & 0xFu);
    CHECK_HEX("change interrupt raised", HYPE_AHCI_PXIS_PRCS, a.p_is & HYPE_AHCI_PXIS_PRCS);
    CHECK_HEX("irq pending after detach", 1, hype_ahci_irq_pending(&a) != 0);
}

/*
 * #694: process_ahci_ata_command_slot()/ahci_backend_rw_prdt()/complete_ahci_soft_reset()
 * (moved to arch/x86_64/vmm_device_ops.c) full-function coverage. #672's regression test
 * above only exercised the command-list bounds check; this rig drives every command branch,
 * both storage paths (disk->media RAM and a real hype_blk_backend_t), and the PRDT
 * aligned/straddling/refused/backend-failure cases -- mirroring test_ahci_dma.c's rig for
 * process_ahci_command_slot()'s own ATAPI side.
 */

#define ATA_RIG_BASE 0x9000000000ull

typedef struct {
    uint8_t cmd_list[32];
    uint8_t cmd_table[0x80 + 64]; /* room for up to 4 PRD entries */
    uint8_t data[8192];
    uint8_t rx_fis[0x40 + 20];
    hype_gpa_map_t map;
} ata_rig_t;

static uint64_t ata_rig_gpa(const ata_rig_t *r, const void *host_ptr) {
    return ATA_RIG_BASE + (uint64_t)((const uint8_t *)host_ptr - (const uint8_t *)r);
}

static void ata_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Builds slot 0: command header (opts carries prdtl, ATAPI bit clear) -> command
 * table (Register H2D FIS, C bit set: a command, not a Control-register write)
 * carrying `cmd`/`lba`/`count` -> `nprd` PRD entries at prd_gpa[i]/prd_len[i]. */
static void ata_build_slot0(ata_rig_t *r, uint8_t cmd, uint64_t lba, uint16_t count,
                            const uint64_t *prd_gpa, const uint32_t *prd_len, unsigned nprd) {
    uint32_t opts;
    unsigned i;

    memset(r->cmd_list, 0, sizeof(r->cmd_list));
    opts = 5u | ((uint32_t)nprd << 16);
    ata_put32(r->cmd_list + 0, opts);
    ata_put32(r->cmd_list + 8, (uint32_t)ata_rig_gpa(r, r->cmd_table));
    ata_put32(r->cmd_list + 12, (uint32_t)(ata_rig_gpa(r, r->cmd_table) >> 32));

    memset(r->cmd_table, 0, sizeof(r->cmd_table));
    r->cmd_table[0] = 0x27u;
    r->cmd_table[1] = HYPE_AHCI_FIS_H2D_FLAG_C;
    r->cmd_table[2] = cmd;
    r->cmd_table[4] = (uint8_t)(lba & 0xFFu);
    r->cmd_table[5] = (uint8_t)((lba >> 8) & 0xFFu);
    r->cmd_table[6] = (uint8_t)((lba >> 16) & 0xFFu);
    r->cmd_table[8] = (uint8_t)((lba >> 24) & 0xFFu);
    r->cmd_table[9] = (uint8_t)((lba >> 32) & 0xFFu);
    r->cmd_table[10] = (uint8_t)((lba >> 40) & 0xFFu);
    r->cmd_table[12] = (uint8_t)(count & 0xFFu);
    r->cmd_table[13] = (uint8_t)((count >> 8) & 0xFFu);

    for (i = 0; i < nprd; i++) {
        uint8_t *p = r->cmd_table + 0x80 + i * 16u;
        ata_put32(p + 0, (uint32_t)prd_gpa[i]);
        ata_put32(p + 4, (uint32_t)(prd_gpa[i] >> 32));
        ata_put32(p + 12, prd_len[i] - 1u); /* DBC = byte_count - 1 */
    }
}

static void ata_setup_rig(ata_rig_t *r, hype_ahci_t *ahci) {
    memset(r, 0, sizeof(*r));
    hype_gpa_map_reset(&r->map);
    hype_gpa_map_add(&r->map, ATA_RIG_BASE, (uint64_t)(uintptr_t)r, sizeof(*r));

    hype_ahci_reset(ahci); /* bus_master defaults enabled */
    ahci->p_clb = (uint32_t)ata_rig_gpa(r, r->cmd_list);
    ahci->p_clbu = (uint32_t)(ata_rig_gpa(r, r->cmd_list) >> 32);
    ahci->p_fb = (uint32_t)ata_rig_gpa(r, r->rx_fis);
    ahci->p_fbu = (uint32_t)(ata_rig_gpa(r, r->rx_fis) >> 32);
}

static void test_ata_identify_device_completes(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint8_t media[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, media, sizeof(media));
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_IDENTIFY_DEVICE, 0, 0, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("IDENTIFY completes", 0, rc);
    CHECK_HEX("status DRDY|DSC", (unsigned)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_DSC),
              ahci.p_tfd & 0xFFu);
    CHECK_HEX("PSS latched (PIO data-in)", HYPE_AHCI_PIS_PSS, ahci.p_is & HYPE_AHCI_PIS_PSS);
    CHECK_HEX("DHRS latched too", HYPE_AHCI_PIS_DHRS, ahci.p_is & HYPE_AHCI_PIS_DHRS);
    CHECK_HEX("slot completed", 0u, ahci.p_ci);
}

static void test_ata_read_dma_ram_media(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint8_t media[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(media, 0xAB, sizeof(media));
    hype_ata_disk_reset(&disk, media, sizeof(media));
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 1u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("READ DMA completes", 0, rc);
    CHECK_HEX("media content delivered to the PRD buffer", 0xABu, r.data[0]);
    CHECK_HEX("status DRDY|DSC, no error", (unsigned)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_DSC),
              ahci.p_tfd & 0xFFu);
}

static void test_ata_write_dma_ram_media(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint8_t media[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(media, 0, sizeof(media));
    hype_ata_disk_reset(&disk, media, sizeof(media));
    memset(r.data, 0xCDu, 512u);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_WRITE_DMA_EXT, 0u, 1u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("WRITE DMA completes", 0, rc);
    CHECK_HEX("PRD content landed in media", 0xCDu, media[0]);
}

static void test_ata_read_dma_backend_aligned(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_file_t f;
    hype_blk_backend_t be;
    uint8_t img[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 1024u; /* two whole sectors: the aligned fast path */
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(img, 0x5Au, sizeof(img));
    hype_blk_file_init(&f, &be, img, sizeof(img));
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 2u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("backend-aligned READ DMA completes", 0, rc);
    CHECK_HEX("backend content delivered", 0x5Au, r.data[0]);
    CHECK_HEX("delivered through the whole aligned span", 0x5Au, r.data[1023]);
}

static void test_ata_write_dma_backend_straddling_prd(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_file_t f;
    hype_blk_backend_t be;
    uint8_t img[4096];
    uint64_t prd_gpa[2];
    uint32_t prd_len[2];
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(img, 0, sizeof(img));
    hype_blk_file_init(&f, &be, img, sizeof(img));
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);

    /* One 512-byte sector split across two PRDs (200 + 312 bytes): neither PRD
     * alone is a whole sector, forcing ahci_backend_rw_prdt's staging path. */
    memset(r.data, 0x11u, 200u);
    memset(r.data + 200, 0x22u, 312u);
    prd_gpa[0] = ata_rig_gpa(&r, r.data);
    prd_gpa[1] = ata_rig_gpa(&r, r.data + 200);
    prd_len[0] = 200u;
    prd_len[1] = 312u;
    ata_build_slot0(&r, HYPE_ATA_CMD_WRITE_DMA_EXT, 0u, 1u, prd_gpa, prd_len, 2);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("straddling-PRD WRITE DMA completes", 0, rc);
    CHECK_HEX("first half landed", 0x11u, img[0]);
    CHECK_HEX("second half landed", 0x22u, img[200]);
    CHECK_HEX("last byte of the sector landed", 0x22u, img[511]);
}

static void test_ata_short_inside_a_sector(void) {
    /* A PRDT that runs out mid-sector on the straddling path: ahci_backend_rw_prdt
     * must report the short byte count rather than reading/writing past its PRDT. */
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_file_t f;
    hype_blk_backend_t be;
    uint8_t img[4096];
    uint64_t prd_gpa[1];
    uint32_t prd_len[1];
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(img, 0, sizeof(img));
    hype_blk_file_init(&f, &be, img, sizeof(img));
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);

    /* Only 100 bytes of PRD offered for a 1-sector (512-byte) write -- the PRDT
     * is exhausted mid-sector, so the write never lands. */
    memset(r.data, 0x33u, 100u);
    prd_gpa[0] = ata_rig_gpa(&r, r.data);
    prd_len[0] = 100u;
    ata_build_slot0(&r, HYPE_ATA_CMD_WRITE_DMA_EXT, 0u, 1u, prd_gpa, prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("short transfer still completes the slot", 0, rc);
}

static void test_ata_flush_cache_completes(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, 0, 0);
    ata_build_slot0(&r, HYPE_ATA_CMD_FLUSH_CACHE_EXT, 0u, 0u, 0, 0, 0);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("FLUSH completes", 0, rc);
    CHECK_HEX("status DRDY|DSC, no error", (unsigned)(HYPE_ATA_STATUS_DRDY | HYPE_ATA_STATUS_DSC),
              ahci.p_tfd & 0xFFu);
    CHECK_HEX("DHRS only, no PSS (no data phase)", 0u, ahci.p_is & HYPE_AHCI_PIS_PSS);
}

static void test_ata_set_features_completes(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, 0, 0);
    ata_build_slot0(&r, HYPE_ATA_CMD_SET_FEATURES, 0u, 0u, 0, 0, 0);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("SET FEATURES completes", 0, rc);
    CHECK_HEX("DHRS latched", HYPE_AHCI_PIS_DHRS, ahci.p_is & HYPE_AHCI_PIS_DHRS);
}

static void test_ata_unmodelled_command_aborts(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, 0, 0);
    ata_build_slot0(&r, 0x00u /* unmodelled */, 0u, 0u, 0, 0, 0);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("unmodelled command still completes the slot", 0, rc);
    CHECK_HEX("status ERR", HYPE_ATA_STATUS_ERR, ahci.p_tfd & HYPE_ATA_STATUS_ERR);
    CHECK_HEX("error register ABRT", 0x04u, (ahci.p_tfd >> 8) & 0xFFu);
}

static void test_ata_read_dma_out_of_range_lba_reports_idnf(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint8_t media[512]; /* one sector total */
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, media, sizeof(media));
    prd_gpa = ata_rig_gpa(&r, r.data);
    /* LBA 5 is well past this 1-sector disk. */
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 5u, 1u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("out-of-range LBA still completes the slot", 0, rc);
    CHECK_HEX("status ERR", HYPE_ATA_STATUS_ERR, ahci.p_tfd & HYPE_ATA_STATUS_ERR);
    CHECK_HEX("error register IDNF", 0x10u, (ahci.p_tfd >> 8) & 0xFFu);
}

static void test_ata_atapi_header_falls_through(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint32_t opts;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, 0, 0);
    ata_build_slot0(&r, HYPE_ATA_CMD_IDENTIFY_DEVICE, 0u, 0u, 0, 0, 0);
    opts = 5u | (1u << 5); /* ATAPI bit set: not this handler's command */
    ata_put32(r.cmd_list + 0, opts);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("an ATAPI command header is left for the ATAPI handler", (unsigned)-1, (unsigned)rc);
}

static void test_ata_bus_master_refused_returns_zero(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ahci_set_bus_master(&ahci, 0);
    hype_ata_disk_reset(&disk, 0, 0);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 1u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("no bus master: ignored, not an error", 0, rc);
    CHECK_HEX("slot is left outstanding", 0u, ahci.p_ci);
}

static void test_ata_out_of_range_command_table_refused(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, 0, 0);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_IDENTIFY_DEVICE, 0u, 0u, &prd_gpa, &prd_len, 1);
    /* Point the command header at a command table well outside the rig's mapped range. */
    ata_put32(r.cmd_list + 8, (uint32_t)(ATA_RIG_BASE + 0x10000000ull));
    ata_put32(r.cmd_list + 12, (uint32_t)((ATA_RIG_BASE + 0x10000000ull) >> 32));

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("out-of-range command table is refused", (unsigned)-1, (unsigned)rc);
}

static void test_ata_control_write_triggers_soft_reset(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    int rc;

    ata_setup_rig(&r, &ahci);
    ahci.p_ie = HYPE_AHCI_PIS_DHRS;
    hype_ata_disk_reset(&disk, 0, 0);
    ata_build_slot0(&r, 0u, 0u, 0u, 0, 0, 0);
    r.cmd_table[1] = 0u; /* C bit clear: a Control-register write, not a command */
    r.cmd_table[15] = HYPE_AHCI_ATA_CONTROL_SRST;

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("SRST assert posts no FIS", 0, rc);
    CHECK_HEX("device busy while reset is asserted", 0x80u, ahci.p_tfd & 0xFFu);

    r.cmd_table[15] = 0u; /* release */
    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("SRST release completes", 0, rc);
    CHECK_HEX("device ready after release", 0x50u, ahci.p_tfd & 0xFFu);
    CHECK_HEX("global IS.PORT0 latched (PxIE armed for DHRS)", HYPE_AHCI_IS_PORT0,
              ahci.is & HYPE_AHCI_IS_PORT0);
    CHECK_HEX("signature FIS posted into the RX FIS area", HYPE_AHCI_FIS_TYPE_D2H_REGISTER,
              r.rx_fis[0x40]);
}

static void test_ata_prd_data_pointer_refused_read(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint8_t media[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, media, sizeof(media));
    prd_gpa = ATA_RIG_BASE + 0x20000000ull; /* outside the rig's mapped range */
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 1u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("refused PRD data pointer still completes the slot with an error", 0, rc);
    CHECK_HEX("status ERR", HYPE_ATA_STATUS_ERR, ahci.p_tfd & HYPE_ATA_STATUS_ERR);
}

static void test_ata_prd_data_pointer_refused_write(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint8_t media[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, media, sizeof(media));
    prd_gpa = ATA_RIG_BASE + 0x20000000ull;
    ata_build_slot0(&r, HYPE_ATA_CMD_WRITE_DMA_EXT, 0u, 1u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("refused PRD data pointer still completes the slot with an error", 0, rc);
    CHECK_HEX("status ERR", HYPE_ATA_STATUS_ERR, ahci.p_tfd & HYPE_ATA_STATUS_ERR);
}

static void test_ata_backend_translation_refused(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_file_t f;
    hype_blk_backend_t be;
    uint8_t img[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 1024u; /* aligned span, so ahci_backend_rw_prdt's fast path runs */
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(img, 0, sizeof(img));
    hype_blk_file_init(&f, &be, img, sizeof(img));
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);
    prd_gpa = ATA_RIG_BASE + 0x20000000ull; /* outside the rig's mapped range */
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 2u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("a refused backend DMA translation aborts the command", (unsigned)-1, (unsigned)rc);
}

static void test_ata_backend_write_failure(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_backend_t be;
    uint64_t prd_gpa;
    uint32_t prd_len = 1024u;
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(&be, 0, sizeof(be));
    be.read = 0;
    be.write = 0; /* read-only backend: any write fails */
    be.writev = 0;
    be.ctx = 0;
    be.total_sectors = 8;
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_WRITE_DMA_EXT, 0u, 2u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("a backend write failure still completes the slot with an error", 0, rc);
    CHECK_HEX("status ERR", HYPE_ATA_STATUS_ERR, ahci.p_tfd & HYPE_ATA_STATUS_ERR);
    CHECK_HEX("error register IDNF", 0x10u, (ahci.p_tfd >> 8) & 0xFFu);
}

static int ata_fail_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx; (void)lba; (void)count; (void)buf;
    return -1;
}

static void test_ata_backend_read_failure(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_backend_t be;
    uint64_t prd_gpa;
    uint32_t prd_len = 1024u;
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(&be, 0, sizeof(be));
    be.read = ata_fail_read;
    be.write = 0;
    be.writev = 0;
    be.ctx = 0;
    be.total_sectors = 8;
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 2u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("a backend read failure still completes the slot with an error", 0, rc);
    CHECK_HEX("status ERR", HYPE_ATA_STATUS_ERR, ahci.p_tfd & HYPE_ATA_STATUS_ERR);
}

static void test_ata_out_of_range_rx_fis_refused_at_completion(void) {
    /* complete_ahci_command_slot() re-checks rx_fis_phys itself (#677) -- a valid
     * command list/table with a bad PxFB/PxFBU is refused only at the very end. */
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, 0, 0);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_FLUSH_CACHE_EXT, 0u, 0u, &prd_gpa, &prd_len, 0);
    ahci.p_fb = 0xFFFF0000u;
    ahci.p_fbu = 0u;

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("out-of-range RX FIS is refused at completion", (unsigned)-1, (unsigned)rc);
}

static void test_ata_soft_reset_release_out_of_range_rx_fis(void) {
    /* complete_ahci_soft_reset()'s own rx_fis check, on the release half. */
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, 0, 0);
    ata_build_slot0(&r, 0u, 0u, 0u, 0, 0, 0);
    r.cmd_table[1] = 0u;
    r.cmd_table[15] = HYPE_AHCI_ATA_CONTROL_SRST;
    (void)process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u); /* assert half: no FIS yet */

    ahci.p_fb = 0xFFFF0000u;
    ahci.p_fbu = 0u;
    r.cmd_table[15] = 0u; /* release */
    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("out-of-range RX FIS on soft-reset release is refused", (unsigned)-1, (unsigned)rc);
}

static void test_ata_identify_device_raises_port0_interrupt(void) {
    /* Exercises the "latch the global IS.PORT0 bit" branch both completion helpers share --
     * only taken when the guest has already enabled PxIE for the bit(s) just posted. */
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint8_t media[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    ahci.p_ie = HYPE_AHCI_PIS_PSS | HYPE_AHCI_PIS_DHRS;
    hype_ata_disk_reset(&disk, media, sizeof(media));
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_IDENTIFY_DEVICE, 0u, 0u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("IDENTIFY completes", 0, rc);
    CHECK_HEX("global IS.PORT0 latched", HYPE_AHCI_IS_PORT0, ahci.is & HYPE_AHCI_IS_PORT0);
}

static void test_ata_backend_aligned_two_prds(void) {
    /* Two PRDs, each an exact whole number of sectors: after the first is fully
     * consumed, ahci_backend_rw_prdt's outer loop advances to the second PRD
     * (the idx++/continue branch) rather than the straddling stage path. */
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_file_t f;
    hype_blk_backend_t be;
    uint8_t img[4096];
    uint64_t prd_gpa[2];
    uint32_t prd_len[2];
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(img, 0x66u, sizeof(img));
    hype_blk_file_init(&f, &be, img, sizeof(img));
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);

    prd_gpa[0] = ata_rig_gpa(&r, r.data);
    prd_gpa[1] = ata_rig_gpa(&r, r.data + 512);
    prd_len[0] = 512u;
    prd_len[1] = 512u;
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 2u, prd_gpa, prd_len, 2);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("two-aligned-PRD READ DMA completes", 0, rc);
    CHECK_HEX("second PRD received backend content too", 0x66u, r.data[512]);
}

static void test_ata_backend_aligned_prd_larger_than_request(void) {
    /* A single PRD larger than the whole request: the aligned fast path must cap
     * its span to what remains rather than reading/writing past total_bytes. */
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_file_t f;
    hype_blk_backend_t be;
    uint8_t img[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 2048u; /* PRD offers 4 sectors */
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(img, 0x77u, sizeof(img));
    hype_blk_file_init(&f, &be, img, sizeof(img));
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 1u, &prd_gpa, &prd_len, 1); /* request only 1 sector */

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("an oversized PRD is capped to the request", 0, rc);
    CHECK_HEX("only the requested sector was delivered", 0x77u, r.data[0]);
}

static void test_ata_read_dma_backend_straddling_prd(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_file_t f;
    hype_blk_backend_t be;
    uint8_t img[4096];
    uint64_t prd_gpa[2];
    uint32_t prd_len[2];
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(img, 0x99u, sizeof(img));
    hype_blk_file_init(&f, &be, img, sizeof(img));
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);

    prd_gpa[0] = ata_rig_gpa(&r, r.data);
    prd_gpa[1] = ata_rig_gpa(&r, r.data + 200);
    prd_len[0] = 200u;
    prd_len[1] = 312u;
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 1u, prd_gpa, prd_len, 2);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("straddling-PRD READ DMA completes", 0, rc);
    CHECK_HEX("scattered content delivered", 0x99u, r.data[0]);
    CHECK_HEX("scattered content delivered past the straddle point", 0x99u, r.data[511]);
}

static void test_ata_write_dma_backend_straddling_failure(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_backend_t be;
    uint64_t prd_gpa[2];
    uint32_t prd_len[2];
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(&be, 0, sizeof(be));
    be.read = 0;
    be.write = 0; /* read-only: the staged write at the end of the sector fails */
    be.writev = 0;
    be.ctx = 0;
    be.total_sectors = 8;
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);

    prd_gpa[0] = ata_rig_gpa(&r, r.data);
    prd_gpa[1] = ata_rig_gpa(&r, r.data + 200);
    prd_len[0] = 200u;
    prd_len[1] = 312u;
    ata_build_slot0(&r, HYPE_ATA_CMD_WRITE_DMA_EXT, 0u, 1u, prd_gpa, prd_len, 2);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("a staged backend write failure still completes the slot with an error", 0, rc);
    CHECK_HEX("status ERR", HYPE_ATA_STATUS_ERR, ahci.p_tfd & HYPE_ATA_STATUS_ERR);
}

static void test_ata_write_dma_backend_straddling_translation_refused(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_file_t f;
    hype_blk_backend_t be;
    uint8_t img[4096];
    uint64_t prd_gpa[2];
    uint32_t prd_len[2];
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(img, 0, sizeof(img));
    hype_blk_file_init(&f, &be, img, sizeof(img));
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);

    prd_gpa[0] = ata_rig_gpa(&r, r.data);
    prd_gpa[1] = ATA_RIG_BASE + 0x20000000ull; /* second PRD outside the mapped rig */
    prd_len[0] = 200u;
    prd_len[1] = 312u;
    ata_build_slot0(&r, HYPE_ATA_CMD_WRITE_DMA_EXT, 0u, 1u, prd_gpa, prd_len, 2);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("a refused translation mid-straddle aborts the command", (unsigned)-1, (unsigned)rc);
}

static void test_ata_short_prdt_on_aligned_path(void) {
    /* The PRDT is exhausted (one aligned 1-sector PRD) before a 2-sector request is
     * satisfied: ahci_backend_rw_prdt's aligned loop must report the short count. */
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_file_t f;
    hype_blk_backend_t be;
    uint8_t img[4096];
    uint64_t prd_gpa;
    uint32_t prd_len = 512u; /* only 1 of the 2 requested sectors */
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(img, 0, sizeof(img));
    hype_blk_file_init(&f, &be, img, sizeof(img));
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_WRITE_DMA_EXT, 0u, 2u, &prd_gpa, &prd_len, 1);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("a short PRDT on the aligned path still completes the slot", 0, rc);
}

static void test_ata_not_a_register_h2d_fis_refused(void) {
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    uint64_t prd_gpa;
    uint32_t prd_len = 512u;
    int rc;

    ata_setup_rig(&r, &ahci);
    hype_ata_disk_reset(&disk, 0, 0);
    prd_gpa = ata_rig_gpa(&r, r.data);
    ata_build_slot0(&r, HYPE_ATA_CMD_IDENTIFY_DEVICE, 0u, 0u, &prd_gpa, &prd_len, 1);
    r.cmd_table[0] = 0x00u; /* not a Register H2D FIS at all */

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("a command table without a Register H2D FIS is refused", (unsigned)-1, (unsigned)rc);
}

static int ata_fail_read_once(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx; (void)lba; (void)count; (void)buf;
    return -1;
}

static void test_ata_read_dma_backend_straddling_failure(void) {
    /* The staged-read half of ahci_backend_rw_prdt's straddle path fetches the sector
     * from the backend before scattering it into the PRDs; a backend read failure there
     * must abort with an error, same as the aligned path's own failure. */
    ata_rig_t r;
    hype_ahci_t ahci;
    hype_ata_disk_t disk;
    hype_blk_backend_t be;
    uint64_t prd_gpa[2];
    uint32_t prd_len[2];
    int rc;

    ata_setup_rig(&r, &ahci);
    memset(&be, 0, sizeof(be));
    be.read = ata_fail_read_once;
    be.write = 0;
    be.writev = 0;
    be.ctx = 0;
    be.total_sectors = 8;
    hype_ata_disk_reset(&disk, 0, 0);
    hype_ata_disk_set_backend(&disk, &be);

    prd_gpa[0] = ata_rig_gpa(&r, r.data);
    prd_gpa[1] = ata_rig_gpa(&r, r.data + 200);
    prd_len[0] = 200u;
    prd_len[1] = 312u;
    ata_build_slot0(&r, HYPE_ATA_CMD_READ_DMA_EXT, 0u, 1u, prd_gpa, prd_len, 2);

    rc = process_ahci_ata_command_slot(&ahci, &disk, &r.map, 0u);
    CHECK_HEX("a staged backend read failure still completes the slot with an error", 0, rc);
    CHECK_HEX("status ERR", HYPE_ATA_STATUS_ERR, ahci.p_tfd & HYPE_ATA_STATUS_ERR);
}

static void test_get_atapi_diag_reads_back_zero_counters(void) {
    unsigned long long xfers = 99, sx = 99, rb = 99, db = 99, ob = 99;
    /*
     * Direct smoke test of the accessor moved alongside process_ahci_command_slot()
     * (arch/x86_64/vmm_device_ops.c). This binary never calls the ATAPI path (only
     * process_ahci_ata_command_slot(), above), so the shared counters stay at 0 --
     * and a NULL out-pointer must be tolerated (every other test binary linking this
     * module passes a subset of NULLs).
     */
    hype_svm_vcpu_get_atapi_diag(&xfers, &sx, &rb, &db, &ob);
    CHECK_HEX("xfers stays 0 (ATAPI path never ran in this binary)", 0, xfers);
    CHECK_HEX("short_xfers stays 0", 0, sx);
    CHECK_HEX("req_bytes stays 0", 0, rb);
    CHECK_HEX("done_bytes stays 0", 0, db);
    CHECK_HEX("owed_bytes stays 0", 0, ob);
    hype_svm_vcpu_get_atapi_diag(0, 0, 0, 0, 0); /* NULL out-pointers tolerated */
}

int main(void) {
    test_reset_state();
    test_read_write_clb_fb();
    test_pcmd_start_mirrors_running_bits();
    test_ghc_hr_self_clears();
    test_is_rw1c();
    test_ci_write_ors_in_bits();
    test_irq_pending_conditions();
    test_irq_events_counts_model_edges();
    test_reserved_register_reads_zero_and_ignores_writes();
    test_rejects_misaligned_and_wrong_width();
    test_rejects_out_of_range_offset();
    test_read_every_generic_and_port_register();
    test_write_every_writable_register();
    test_decode_cmd_header();
    test_decode_cmd_header_write_bit();
    test_decode_prdt_entry();
    test_decode_h2d_fis();

    test_control_write_is_told_apart_from_a_command();
    test_soft_reset_sequence();
    test_release_without_an_asserted_reset_posts_nothing();
    test_soft_reset_clears_serr();
    test_signature_fis_carries_the_atapi_signature();
    test_signature_fis_carries_the_plain_disk_signature();
    test_d2h_fis_reports_the_result_registers();
    test_d2h_fis_passes_flags_through_and_reports_an_error();
    test_pio_setup_fis_carries_e_status_and_transfer_count();
    test_pio_setup_fis_truncates_transfer_count_to_16_bits();
    test_signature_fis_is_a_d2h_fis();
    test_global_is_reflects_the_port_level();
    test_hotplug_capable_at_reset();
    test_hotplug_attach_raises_connect_change();
    test_hotplug_detach_clears_det();
    test_ata_command_slot_refuses_out_of_range_command_list();

    /*
     * process_ahci_ata_command_slot() unconditionally traces its first 12 commands
     * (the #262 ATACMD line) regardless of outcome, unlike process_ahci_command_slot()'s
     * refusal-only logging above -- so every call below needs the host-unsafe DEBUG
     * sink suppressed, not just the ones expected to refuse.
     */
    hype_debug_set_level(HYPE_LOG_ERROR);
    test_ata_identify_device_completes();
    test_ata_read_dma_ram_media();
    test_ata_write_dma_ram_media();
    test_ata_read_dma_backend_aligned();
    test_ata_write_dma_backend_straddling_prd();
    test_ata_short_inside_a_sector();
    test_ata_flush_cache_completes();
    test_ata_set_features_completes();
    test_ata_unmodelled_command_aborts();
    test_ata_read_dma_out_of_range_lba_reports_idnf();
    test_ata_atapi_header_falls_through();
    test_ata_bus_master_refused_returns_zero();
    test_ata_out_of_range_command_table_refused();
    test_ata_control_write_triggers_soft_reset();
    test_ata_prd_data_pointer_refused_read();
    test_ata_prd_data_pointer_refused_write();
    test_ata_backend_translation_refused();
    test_ata_backend_write_failure();
    test_ata_backend_read_failure();
    test_ata_out_of_range_rx_fis_refused_at_completion();
    test_ata_soft_reset_release_out_of_range_rx_fis();
    test_ata_identify_device_raises_port0_interrupt();
    test_ata_backend_aligned_two_prds();
    test_ata_backend_aligned_prd_larger_than_request();
    test_ata_read_dma_backend_straddling_prd();
    test_ata_write_dma_backend_straddling_failure();
    test_ata_write_dma_backend_straddling_translation_refused();
    test_ata_short_prdt_on_aligned_path();
    test_ata_not_a_register_h2d_fis_refused();
    test_ata_read_dma_backend_straddling_failure();
    test_get_atapi_diag_reads_back_zero_counters();
    hype_debug_set_level(HYPE_LOG_DEBUG);

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
