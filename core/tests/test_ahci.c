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

    hype_ahci_mmio_write(&ahci, HYPE_AHCI_REG_GHC, 4, 0xA1u);
    hype_ahci_mmio_read(&ahci, HYPE_AHCI_REG_GHC, 4, &value);
    CHECK_HEX("GHC write", 0xA1u, value);

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

int main(void) {
    test_reset_state();
    test_read_write_clb_fb();
    test_pcmd_start_mirrors_running_bits();
    test_is_rw1c();
    test_ci_write_ors_in_bits();
    test_reserved_register_reads_zero_and_ignores_writes();
    test_rejects_misaligned_and_wrong_width();
    test_rejects_out_of_range_offset();
    test_read_every_generic_and_port_register();
    test_write_every_writable_register();
    test_decode_cmd_header();
    test_decode_cmd_header_write_bit();
    test_decode_prdt_entry();
    test_decode_h2d_fis();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
