#include <stdio.h>
#include "../usb_msc.h"

static int failures = 0;
#define CHECK_HEX(desc, expected, actual) \
    do { if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
        printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
               (unsigned long long)(expected), (unsigned long long)(actual)); failures++; } } while (0)

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_cbw(void) {
    uint8_t cdb[10], cbw[31];
    hype_scsi_cdb_read10(cdb, 0x1234, 8);
    hype_usb_bot_cbw(cbw, 0xAABBCCDD, 4096, 1, 0, cdb, 10);
    CHECK_HEX("cbw signature", HYPE_USB_CBW_SIGNATURE, le32(cbw + 0));
    CHECK_HEX("cbw tag", 0xAABBCCDDu, le32(cbw + 4));
    CHECK_HEX("cbw data len", 4096u, le32(cbw + 8));
    CHECK_HEX("cbw flags Data-In", 0x80u, cbw[12]);
    CHECK_HEX("cbw lun 0", 0u, cbw[13]);
    CHECK_HEX("cbw cdb len 10", 10u, cbw[14]);
    CHECK_HEX("cbw cdb[0] READ10", 0x28u, cbw[15]);

    /* Data-Out direction flag */
    hype_usb_bot_cbw(cbw, 1, 512, 0, 0, cdb, 10);
    CHECK_HEX("cbw flags Data-Out", 0x00u, cbw[12]);

    /* over-length CDB is clamped to 16 */
    {
        uint8_t big[20];
        unsigned i;
        for (i = 0; i < 20u; i++) big[i] = (uint8_t)(i + 1);
        hype_usb_bot_cbw(cbw, 2, 0, 0, 3, big, 20);
        CHECK_HEX("cbw cdb len clamped to 16", 16u, cbw[14]);
        CHECK_HEX("cbw lun 3", 3u, cbw[13]);
        CHECK_HEX("cbw last cdb byte (16th)", 16u, cbw[15 + 15]);
    }
}

static void test_csw(void) {
    uint8_t csw[13] = {0};
    csw[0] = 0x55; csw[1] = 0x53; csw[2] = 0x42; csw[3] = 0x53; /* 'USBS' */
    csw[4] = 0x21; csw[5] = 0x43; csw[6] = 0x65; csw[7] = 0x87; /* tag 0x87654321 */
    csw[12] = 0;                                                /* passed */
    CHECK_HEX("csw ok", 1, hype_usb_bot_csw_ok(csw, 0x87654321u));
    CHECK_HEX("csw wrong tag", 0, hype_usb_bot_csw_ok(csw, 0x11111111u));
    csw[12] = 1; /* failed */
    CHECK_HEX("csw failed status", 0, hype_usb_bot_csw_ok(csw, 0x87654321u));
    csw[12] = 0; csw[0] = 0x00; /* bad signature */
    CHECK_HEX("csw bad sig", 0, hype_usb_bot_csw_ok(csw, 0x87654321u));
}

static void test_cdbs(void) {
    uint8_t c[10];
    hype_scsi_cdb_read_capacity10(c);
    CHECK_HEX("read cap opcode", 0x25u, c[0]);

    hype_scsi_cdb_read10(c, 0x00A0B0C0, 4);
    CHECK_HEX("read10 opcode", 0x28u, c[0]);
    CHECK_HEX("read10 lba BE b0", 0x00u, c[2]);
    CHECK_HEX("read10 lba BE b1", 0xA0u, c[3]);
    CHECK_HEX("read10 lba BE b2", 0xB0u, c[4]);
    CHECK_HEX("read10 lba BE b3", 0xC0u, c[5]);
    CHECK_HEX("read10 blocks hi", 0u, c[7]);
    CHECK_HEX("read10 blocks lo", 4u, c[8]);

    hype_scsi_cdb_write10(c, 0x10, 1);
    CHECK_HEX("write10 opcode", 0x2Au, c[0]);
    CHECK_HEX("write10 lba lo", 0x10u, c[5]);

    {
        uint8_t iq[6];
        hype_scsi_cdb_inquiry(iq, 36);
        CHECK_HEX("inquiry opcode", 0x12u, iq[0]);
        CHECK_HEX("inquiry alloc len", 36u, iq[4]);
    }
}

static void test_read_capacity_parse(void) {
    /* last LBA 0x0003FFFF, block size 512 (0x200), both big-endian */
    uint8_t rc[8] = {0x00, 0x03, 0xFF, 0xFF, 0x00, 0x00, 0x02, 0x00};
    uint32_t last = 0, bs = 0;
    hype_scsi_parse_read_capacity10(rc, &last, &bs);
    CHECK_HEX("last lba", 0x0003FFFFu, last);
    CHECK_HEX("block size", 512u, bs);
}

int main(void) {
    test_cbw();
    test_csw();
    test_cdbs();
    test_read_capacity_parse();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
