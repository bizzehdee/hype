#include <stdio.h>
#include <string.h>
#include "../ahci_host.h"
#include "../../devices/ahci.h"     /* the decoders this encoder must round-trip against */
#include "../../devices/ata_disk.h" /* hype_ata_disk_build_identify -- the parse inverse */

static int failures = 0;

#define CHECK_STR(desc, expected, actual) \
    do { \
        if (strcmp((expected), (actual)) != 0) { \
            printf("FAIL: %s: expected \"%s\", got \"%s\"\n", (desc), (expected), (actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* The host encoder is the exact inverse of devices/ahci.c's decoders, so the
 * strongest test is a round-trip: encode, then decode, and compare. */

static void test_cmd_header_roundtrip(void) {
    uint8_t slot[32];
    hype_ahci_cmd_header_t h;

    hype_ahci_host_build_cmd_header(slot, /*is_write=*/0, /*prdtl=*/1,
                                    /*cmd_table_phys=*/0x1122334455667788ull);
    hype_ahci_decode_cmd_header(slot, &h);
    CHECK_HEX("CFL = 5 dwords (20-byte H2D FIS)", 5u, h.cfl);
    CHECK_HEX("not ATAPI", 0, h.is_atapi);
    CHECK_HEX("read (W=0)", 0, h.is_write);
    CHECK_HEX("PRDTL = 1", 1u, h.prdtl);
    CHECK_HEX("command-table phys round-trips", 0x1122334455667788ull, h.cmd_table_phys);
}

static void test_cmd_header_write_flag(void) {
    uint8_t slot[32];
    hype_ahci_cmd_header_t h;
    hype_ahci_host_build_cmd_header(slot, /*is_write=*/1, /*prdtl=*/4, 0x1000ull);
    hype_ahci_decode_cmd_header(slot, &h);
    CHECK_HEX("write flag set", 1, h.is_write);
    CHECK_HEX("PRDTL = 4", 4u, h.prdtl);
}

static void test_read_dma_ext_roundtrip(void) {
    uint8_t ct[0x80 + 16];
    hype_ahci_h2d_fis_t fis;
    hype_ahci_prdt_entry_t prd;
    int rc = hype_ahci_host_build_read_dma_ext(ct, /*lba=*/0x123456789Aull, /*count=*/8,
                                               /*dst_phys=*/0xDEADBEEF000ull);
    CHECK_HEX("build ok", 0, rc);

    hype_ahci_decode_h2d_fis(ct + HYPE_AHCI_HOST_CT_CFIS_OFF, &fis);
    CHECK_HEX("command = READ DMA EXT (0x25)", 0x25u, fis.command);
    CHECK_HEX("48-bit LBA round-trips", 0x123456789Aull, fis.lba);
    CHECK_HEX("count = 8 sectors", 8u, fis.count);
    CHECK_HEX("device = LBA mode (0x40)", 0x40u, fis.device);
    /* FIS type byte must be a Register H2D FIS (0x27) with the C bit set. */
    CHECK_HEX("FIS type 0x27", 0x27u, ct[0]);
    CHECK_HEX("C bit set", 0x80u, ct[1] & 0x80u);

    hype_ahci_decode_prdt_entry(ct + HYPE_AHCI_HOST_CT_PRDT_OFF, &prd);
    CHECK_HEX("PRDT data base round-trips", 0xDEADBEEF000ull, prd.data_phys);
    CHECK_HEX("PRDT byte count = 8 * 512", 8u * 512u, prd.byte_count);
}

static void test_read_dma_ext_bounds(void) {
    uint8_t ct[0x80 + 16];
    CHECK_HEX("count 0 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_ahci_host_build_read_dma_ext(ct, 0, 0, 0x1000ull));
    /* 8193 sectors = 4 MiB + 512 bytes: exceeds one PRDT entry's 4 MiB cap. */
    CHECK_HEX("count > 4MiB/entry rejected", (unsigned long long)(-1),
              (unsigned long long)hype_ahci_host_build_read_dma_ext(ct, 0, 8193, 0x1000ull));
    CHECK_HEX("count 8192 (exactly 4 MiB) accepted", 0,
              hype_ahci_host_build_read_dma_ext(ct, 0, 8192, 0x1000ull));
}

static void test_identify_cmd_table(void) {
    uint8_t ct[0x80 + 16];
    hype_ahci_h2d_fis_t fis;
    hype_ahci_prdt_entry_t prd;

    hype_ahci_host_build_identify(ct, /*dst_phys=*/0xCAFE1000ull);
    hype_ahci_decode_h2d_fis(ct + HYPE_AHCI_HOST_CT_CFIS_OFF, &fis);
    CHECK_HEX("command = IDENTIFY DEVICE (0xEC)", HYPE_ATA_CMD_IDENTIFY_DEVICE, fis.command);
    CHECK_HEX("IDENTIFY carries no LBA", 0ull, fis.lba);
    CHECK_HEX("IDENTIFY carries no count", 0u, fis.count);
    CHECK_HEX("FIS type 0x27", 0x27u, ct[0]);
    CHECK_HEX("C bit set", 0x80u, ct[1] & 0x80u);

    hype_ahci_decode_prdt_entry(ct + HYPE_AHCI_HOST_CT_PRDT_OFF, &prd);
    CHECK_HEX("PRDT data base round-trips", 0xCAFE1000ull, prd.data_phys);
    CHECK_HEX("PRDT byte count = 512", 512u, prd.byte_count);
}

/* Strongest test of the parser: feed it the guest-side IDENTIFY *builder*'s
 * output (devices/ata_disk.c) and confirm every field round-trips. */
static void test_parse_identify_roundtrip(void) {
    hype_ata_disk_t disk;
    uint8_t id[HYPE_ATA_IDENTIFY_SIZE];
    hype_host_disk_info_t info;

    /* 4 GiB disk => 8388608 sectors, well within 48-bit. */
    hype_ata_disk_reset(&disk, (uint8_t *)0, 8388608ull * 512ull);
    hype_ata_disk_build_identify(&disk, id);
    hype_ahci_host_parse_identify(id, &info);

    CHECK_STR("serial round-trips + trims", "HYPE0000000000000001", info.serial);
    CHECK_STR("model round-trips + trims trailing spaces", "HYPE VIRTUAL DISK", info.model);
    CHECK_HEX("48-bit capacity round-trips", 8388608ull, info.total_sectors);
}

/* When 48-bit addressing is not advertised (word 83 bit 10 clear), the parser
 * must fall back to the 28-bit words-60-61 capacity. */
static void test_parse_identify_lba28_fallback(void) {
    uint8_t id[512];
    hype_host_disk_info_t info;
    unsigned i;

    for (i = 0; i < 512u; i++) {
        id[i] = 0;
    }
    /* word 83 left 0 (no 48-bit); words 100-103 left 0; only 28-bit capacity set. */
    id[120] = 0x00u; /* 0x00100000 = 1,048,576 sectors */
    id[121] = 0x00u;
    id[122] = 0x10u;
    id[123] = 0x00u;
    hype_ahci_host_parse_identify(id, &info);
    CHECK_HEX("28-bit fallback capacity", 0x00100000ull, info.total_sectors);
    CHECK_STR("empty serial trims to nothing", "", info.serial);
}

int main(void) {
    test_cmd_header_roundtrip();
    test_cmd_header_write_flag();
    test_read_dma_ext_roundtrip();
    test_read_dma_ext_bounds();
    test_identify_cmd_table();
    test_parse_identify_roundtrip();
    test_parse_identify_lba28_fallback();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
