#include <stdio.h>
#include "../ahci_host.h"
#include "../../devices/ahci.h" /* the decoders this encoder must round-trip against */

static int failures = 0;

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

int main(void) {
    test_cmd_header_roundtrip();
    test_cmd_header_write_flag();
    test_read_dma_ext_roundtrip();
    test_read_dma_ext_bounds();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
