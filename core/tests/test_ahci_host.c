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

static void test_write_dma_ext_roundtrip(void) {
    uint8_t ct[0x80 + 16];
    hype_ahci_h2d_fis_t fis;
    hype_ahci_prdt_entry_t prd;
    int rc = hype_ahci_host_build_write_dma_ext(ct, /*lba=*/0x55AA55AAull, /*count=*/4,
                                                /*src_phys=*/0x4000ull);
    CHECK_HEX("build write ok", 0, rc);
    hype_ahci_decode_h2d_fis(ct + HYPE_AHCI_HOST_CT_CFIS_OFF, &fis);
    CHECK_HEX("command = WRITE DMA EXT (0x35)", HYPE_ATA_CMD_WRITE_DMA_EXT, fis.command);
    CHECK_HEX("write 48-bit LBA round-trips", 0x55AA55AAull, fis.lba);
    CHECK_HEX("write count = 4", 4u, fis.count);
    hype_ahci_decode_prdt_entry(ct + HYPE_AHCI_HOST_CT_PRDT_OFF, &prd);
    CHECK_HEX("write PRDT src base", 0x4000ull, prd.data_phys);
    CHECK_HEX("write PRDT byte count", 4u * 512u, prd.byte_count);
    /* count bounds shared with the read builder. */
    CHECK_HEX("write count 0 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_ahci_host_build_write_dma_ext(ct, 0, 0, 0x4000ull));
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

/* ---- #325: host-side ATAPI (reading a real optical drive) ---- */

static void test_atapi_header_sets_the_A_bit(void) {
    uint8_t slot[32];
    uint32_t dw0;

    hype_ahci_host_build_cmd_header_atapi(slot, 1u, 0x2000u);
    dw0 = (uint32_t)slot[0] | ((uint32_t)slot[1] << 8) | ((uint32_t)slot[2] << 16) |
          ((uint32_t)slot[3] << 24);
    /* Without bit 5 the HBA never sends the ACMD block and the drive sees a PACKET with no packet. */
    CHECK_HEX("A bit (ATAPI) set", 1u, (dw0 >> 5) & 1u);
    CHECK_HEX("CFL is 5 dwords", 5u, dw0 & 0x1Fu);
    CHECK_HEX("W bit clear -- hype never writes to an optical drive", 0u, (dw0 >> 6) & 1u);
    CHECK_HEX("PRDTL", 1u, dw0 >> 16);
}

static void test_atapi_read10_cdb_and_byte_count(void) {
    static uint8_t ct[256];
    const uint8_t *fis = ct + HYPE_AHCI_HOST_CT_CFIS_OFF;
    const uint8_t *acmd = ct + HYPE_AHCI_HOST_CT_ACMD_OFF;
    const uint8_t *prd = ct + HYPE_AHCI_HOST_CT_PRDT_OFF;
    unsigned i;
    for (i = 0; i < sizeof(ct); i++) ct[i] = 0xAAu;

    CHECK_HEX("build ok", 0, hype_ahci_host_build_atapi_read10(ct, 0x12345u, 2u, 0x40000u));

    CHECK_HEX("FIS type", 0x27u, fis[0]);
    CHECK_HEX("C bit", 0x80u, fis[1]);
    CHECK_HEX("PACKET command", 0xA0u, fis[2]);
    /*
     * For PACKET, LBA mid/high are the BYTE COUNT LIMIT -- not an address. 2 sectors = 4096 bytes,
     * so 0x1000: low byte 0x00 in mid, high byte 0x10 in high. Getting this wrong truncates or
     * overruns the transfer instead of failing visibly.
     */
    CHECK_HEX("byte-count-limit low", 0x00u, fis[5]);
    CHECK_HEX("byte-count-limit high", 0x10u, fis[6]);
    CHECK_HEX("LBA mode device", 0x40u, fis[7]);

    /* The CDB is big-endian, unlike everything else in the FIS. */
    CHECK_HEX("CDB opcode READ(10)", 0x28u, acmd[0]);
    CHECK_HEX("CDB lba[31:24]", 0x00u, acmd[2]);
    CHECK_HEX("CDB lba[23:16]", 0x01u, acmd[3]);
    CHECK_HEX("CDB lba[15:8]", 0x23u, acmd[4]);
    CHECK_HEX("CDB lba[7:0]", 0x45u, acmd[5]);
    CHECK_HEX("CDB len high", 0x00u, acmd[7]);
    CHECK_HEX("CDB len low", 0x02u, acmd[8]);

    /* PRDT: DBC is a byte count MINUS ONE. */
    CHECK_HEX("PRD addr low", 0x40000u, (uint32_t)prd[0] | ((uint32_t)prd[1] << 8) |
                                            ((uint32_t)prd[2] << 16) | ((uint32_t)prd[3] << 24));
    CHECK_HEX("PRD DBC = bytes-1", 4096u - 1u,
              (uint32_t)prd[12] | ((uint32_t)prd[13] << 8) | ((uint32_t)prd[14] << 16) |
                  ((uint32_t)prd[15] << 24));
}

static void test_atapi_read10_refuses_impossible_sizes(void) {
    static uint8_t ct[256];
    const uint8_t *fis = ct + HYPE_AHCI_HOST_CT_CFIS_OFF;
    CHECK_HEX("zero sectors refused", -1, hype_ahci_host_build_atapi_read10(ct, 0, 0, 0x1000u));
    /*
     * #325: the real ceiling is the PACKET byte count limit, not the PRDT.
     *
     * That limit is two bytes of the command FIS, so it tops out at 65535 -- one byte short of 32
     * sectors. Ask for 32 and the limit becomes 0x10000, whose low two bytes are both zero, so the
     * drive is told it may return NO data and the transfer fails. This test used to assert that
     * 2048 sectors (one PRDT entry, 4 MiB) was allowed, which had the same truncation to zero and
     * was simply wrong: a 64 KiB read of an El Torito boot image failed on a real drive while every
     * 2 KiB read succeeded, and that is what exposed it.
     */
    CHECK_HEX("the maximum, 31 sectors, is accepted", 0,
              hype_ahci_host_build_atapi_read10(ct, 0, HYPE_AHCI_HOST_ATAPI_MAX_BLOCKS, 0x1000u));
    CHECK_HEX("byte count limit low  = 0x00 (63488 = 0xF800)", 0x00u, fis[5]);
    CHECK_HEX("byte count limit high = 0xF8 -- crucially NOT zero", 0xF8u, fis[6]);
    CHECK_HEX("32 sectors refused: the limit would truncate to 0", -1,
              hype_ahci_host_build_atapi_read10(ct, 0, 32u, 0x1000u));
    CHECK_HEX("one PRDT's worth refused for the same reason", -1,
              hype_ahci_host_build_atapi_read10(ct, 0, 2048u, 0x1000u));
}

static void test_atapi_lba_conversion_refuses_misalignment(void) {
    uint32_t lba2k = 0xFFFFFFFFu;
    uint16_t cnt2k = 0xFFFFu;

    /* A CD LBA is a QUARTER of the 512-byte LBA at the same byte offset. */
    CHECK_HEX("aligned conversion ok", 0,
              hype_ahci_host_atapi_lba512_to_lba2k(64u, 8u, &lba2k, &cnt2k));
    CHECK_HEX("lba/4", 16u, lba2k);
    CHECK_HEX("count/4", 2u, cnt2k);

    /*
     * Misaligned is REFUSED, not rounded. Serving the containing 2 KiB sector would return the wrong
     * 512-byte slice -- the off-by-4 this conversion exists to prevent, and invisible in a log.
     */
    CHECK_HEX("misaligned LBA refused", -1,
              hype_ahci_host_atapi_lba512_to_lba2k(65u, 8u, &lba2k, &cnt2k));
    CHECK_HEX("misaligned count refused", -1,
              hype_ahci_host_atapi_lba512_to_lba2k(64u, 6u, &lba2k, &cnt2k));
    CHECK_HEX("zero count refused", -1,
              hype_ahci_host_atapi_lba512_to_lba2k(64u, 0u, &lba2k, &cnt2k));
    CHECK_HEX("NULL outputs refused", -1,
              hype_ahci_host_atapi_lba512_to_lba2k(64u, 8u, 0, &cnt2k));
    /* A count that would overflow the 16-bit CDB length field. */
    CHECK_HEX("over-large count refused", -1,
              hype_ahci_host_atapi_lba512_to_lba2k(0u, 0x40000u, &lba2k, &cnt2k));
}


/*
 * #369: the PHY-settle stop/continue rule.
 *
 * The bug this encodes: an empty port and a port whose disk is still negotiating were treated
 * identically, so a controller with five empty sockets paid five full 2,000,000-spin budgets and
 * stalled ~60 s before any guest started. The rule has to shorten the empty case WITHOUT
 * shortening the case the wait exists for.
 */
static void test_settle_stops_when_all_ports_established(void) {
    CHECK_HEX("nothing pending -> stop immediately", 0,
              hype_ahci_host_settle_continue(0u, 0u, 0u));
    CHECK_HEX("nothing pending -> stop even at spin 0 with a device present", 0,
              hype_ahci_host_settle_continue(0u, 0u, 5u));
}

static void test_settle_waits_for_a_negotiating_port(void) {
    /* The real-hardware cold-boot case: DET==1, disk present, link not up yet. It must keep the
     * FULL ceiling -- shortening this is how the AMD laptop's SATA SSD goes missing on some
     * boots, which is the flake the wait was added for. */
    CHECK_HEX("negotiating -> keep waiting at spin 0", 1,
              hype_ahci_host_settle_continue(1u, 1u, 0u));
    CHECK_HEX("negotiating -> keep waiting past the empty budget", 1,
              hype_ahci_host_settle_continue(1u, 1u, HYPE_AHCI_HOST_SETTLE_EMPTY_SPINS));
    CHECK_HEX("negotiating -> keep waiting far past the empty budget", 1,
              hype_ahci_host_settle_continue(6u, 1u, HYPE_AHCI_HOST_SETTLE_EMPTY_SPINS * 10u));
}

static void test_settle_gives_up_on_ports_with_nothing_on_them(void) {
    /* Five empty sockets beside one established disk -- the QEMU ich9-ahci shape that cost 60 s. */
    CHECK_HEX("empty ports still get a real wait", 1,
              hype_ahci_host_settle_continue(5u, 0u, 0u));
    CHECK_HEX("empty ports still waiting just under the budget", 1,
              hype_ahci_host_settle_continue(5u, 0u, HYPE_AHCI_HOST_SETTLE_EMPTY_SPINS - 1u));
    CHECK_HEX("empty ports give up at the budget", 0,
              hype_ahci_host_settle_continue(5u, 0u, HYPE_AHCI_HOST_SETTLE_EMPTY_SPINS));
    CHECK_HEX("empty ports stay given up past the budget", 0,
              hype_ahci_host_settle_continue(5u, 0u, HYPE_AHCI_HOST_SETTLE_EMPTY_SPINS + 1u));
}

static void test_settle_one_negotiating_port_holds_the_whole_controller(void) {
    /* A mixed controller must be governed by the port that CAN still come up, not by the majority
     * that cannot -- otherwise a single slow disk beside empty sockets is written off early. */
    CHECK_HEX("one negotiating among many empty -> keep waiting", 1,
              hype_ahci_host_settle_continue(6u, 1u, HYPE_AHCI_HOST_SETTLE_EMPTY_SPINS * 4u));
}

int main(void) {
    test_settle_stops_when_all_ports_established();
    test_settle_waits_for_a_negotiating_port();
    test_settle_gives_up_on_ports_with_nothing_on_them();
    test_settle_one_negotiating_port_holds_the_whole_controller();
    test_cmd_header_roundtrip();
    test_cmd_header_write_flag();
    test_read_dma_ext_roundtrip();
    test_read_dma_ext_bounds();
    test_write_dma_ext_roundtrip();
    test_identify_cmd_table();
    test_parse_identify_roundtrip();
    test_parse_identify_lba28_fallback();
    test_atapi_header_sets_the_A_bit();
    test_atapi_read10_cdb_and_byte_count();
    test_atapi_read10_refuses_impossible_sizes();
    test_atapi_lba_conversion_refuses_misalignment();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
