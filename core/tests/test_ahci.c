#include <stdio.h>
#include "../../devices/ahci.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_reset_state(void) {
    hype_ahci_t ahci;
    hype_ahci_reset(&ahci);

    CHECK_HEX("CAP.S64A set", 1, (ahci.cap & (1u << 31)) != 0);
    CHECK_HEX("CAP.SAM set", 1, (ahci.cap & (1u << 18)) != 0);
    CHECK_HEX("PI has port 0 implemented", 1, ahci.pi & 0x1u);
    CHECK_HEX("VS is 1.3.1", 0x00010301u, ahci.vs);
    CHECK_HEX("PxSIG is ATAPI", HYPE_AHCI_SIG_ATAPI, ahci.p_sig);
    CHECK_HEX("PxSSTS reports device present", 0x123u, ahci.p_ssts);
    CHECK_HEX("PxCMD starts stopped", 0, ahci.p_cmd);
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
    CHECK_HEX("clearing ST/FRE clears CR/FR too", 0, value);
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

int main(void) {
    test_reset_state();
    test_read_write_clb_fb();
    test_pcmd_start_mirrors_running_bits();
    test_ghc_hr_self_clears();
    test_is_rw1c();
    test_ci_write_ors_in_bits();
    test_irq_pending_conditions();
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

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
