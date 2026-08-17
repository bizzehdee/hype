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
    csw[8] = 1; /* one byte not transferred */
    CHECK_HEX("csw nonzero residue", 0, hype_usb_bot_csw_ok(csw, 0x87654321u));
    csw[8] = 0;
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
        unsigned int n;
        for (n = 0; n < sizeof c; n++) c[n] = 0xA5u;
        hype_scsi_cdb_synchronize_cache10(c);
        CHECK_HEX("sync-cache10 opcode", 0x35u, c[0]);
        for (n = 1; n < sizeof c; n++) {
            CHECK_HEX("sync-cache10 whole-device fields zero", 0u, c[n]);
        }
    }

    {
        uint8_t iq[6];
        hype_scsi_cdb_inquiry(iq, 36);
        CHECK_HEX("inquiry opcode", 0x12u, iq[0]);
        CHECK_HEX("inquiry alloc len", 36u, iq[4]);
    }
}

static void test_inquiry_vpd_cdb(void) {
    uint8_t c[6];
    unsigned int n;
    for (n = 0; n < sizeof c; n++) c[n] = 0xA5u;
    hype_scsi_cdb_inquiry_vpd(c, 0x80u, 4u);
    CHECK_HEX("vpd inquiry opcode", 0x12u, c[0]);
    CHECK_HEX("vpd inquiry EVPD set", 0x01u, c[1]);
    CHECK_HEX("vpd inquiry page code", 0x80u, c[2]);
    CHECK_HEX("vpd inquiry reserved", 0u, c[3]);
    CHECK_HEX("vpd inquiry alloc len", 4u, c[4]);
    CHECK_HEX("vpd inquiry control", 0u, c[5]);
}

static void test_vpd80_serial_parse(void) {
    /* page 0x80, length 10: two leading pad spaces + "HYPE-1234" would be 11;
     * use " HYPE-1234" (leading space pad, SPC-3 right-aligned form). */
    uint8_t ok[14] = {0x00, 0x80, 0x00, 0x0A,
                      ' ', 'H', 'Y', 'P', 'E', '-', '1', '2', '3', '4'};
    char out[16];

    CHECK_HEX("padded serial length", 9, hype_scsi_vpd80_serial(ok, sizeof ok, out, sizeof out));
    CHECK_HEX("padded serial b0", 'H', out[0]);
    CHECK_HEX("padded serial b8", '4', out[8]);
    CHECK_HEX("padded serial NUL", 0, out[9]);

    /* trailing space + NUL padding also trims */
    {
        uint8_t tr[8] = {0x00, 0x80, 0x00, 0x04, 'A', 'B', ' ', 0x00};
        CHECK_HEX("trailing pad trimmed", 2, hype_scsi_vpd80_serial(tr, sizeof tr, out, sizeof out));
        CHECK_HEX("trailing pad b1", 'B', out[1]);
    }

    /* every refusal path: each malformed case must yield NO identity (#323) */
    {
        uint8_t wrongpage[6] = {0x00, 0x83, 0x00, 0x02, 'A', 'B'};
        uint8_t zerolen[4] = {0x00, 0x80, 0x00, 0x00};
        uint8_t overlen[6] = {0x00, 0x80, 0x00, 0x08, 'A', 'B'};
        uint8_t allpad[8] = {0x00, 0x80, 0x00, 0x04, ' ', ' ', ' ', ' '};
        uint8_t binary[6] = {0x00, 0x80, 0x00, 0x02, 0x07, 'B'};
        CHECK_HEX("wrong page refused", -1,
                  hype_scsi_vpd80_serial(wrongpage, sizeof wrongpage, out, sizeof out));
        CHECK_HEX("zero page length refused", -1,
                  hype_scsi_vpd80_serial(zerolen, sizeof zerolen, out, sizeof out));
        CHECK_HEX("truncated page refused", -1,
                  hype_scsi_vpd80_serial(overlen, sizeof overlen, out, sizeof out));
        CHECK_HEX("all-padding serial refused", -1,
                  hype_scsi_vpd80_serial(allpad, sizeof allpad, out, sizeof out));
        CHECK_HEX("non-printable byte refused", -1,
                  hype_scsi_vpd80_serial(binary, sizeof binary, out, sizeof out));
        CHECK_HEX("short response refused", -1, hype_scsi_vpd80_serial(zerolen, 3u, out, sizeof out));
        CHECK_HEX("null response refused", -1, hype_scsi_vpd80_serial(0, 6u, out, sizeof out));
        CHECK_HEX("null output refused", -1,
                  hype_scsi_vpd80_serial(binary, sizeof binary, 0, sizeof out));
        CHECK_HEX("tiny output refused", -1, hype_scsi_vpd80_serial(binary, sizeof binary, out, 1u));
    }

    /* a serial that does not fit is a different serial, not a prefix */
    {
        uint8_t longer[9] = {0x00, 0x80, 0x00, 0x05, 'A', 'B', 'C', 'D', 'E'};
        char tiny[4];
        CHECK_HEX("overflowing serial refused", -1,
                  hype_scsi_vpd80_serial(longer, sizeof longer, tiny, sizeof tiny));
        CHECK_HEX("exact-fit serial accepted", 5,
                  hype_scsi_vpd80_serial(longer, sizeof longer, out, 6u));
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

/*
 * #516: the split CSW view -- a structurally valid command-FAILED status must be
 * distinguishable from transport garbage, and the sense machinery that follows it
 * must produce/parse the right bytes. The 64 GB Cruzer rejected SYNCHRONIZE CACHE
 * with a clean status-1 CSW; the old all-or-nothing csw_ok collapsed that into the
 * same -1 as a torn transport, and the recovery loop never converged.
 */
static void test_csw_split_and_sense(void) {
    uint8_t csw[13];
    uint8_t cdb[6];
    uint8_t sense[18];
    unsigned int key = 9u, asc = 9u, ascq = 9u;
    unsigned int i;

    /* Valid CSW, command failed, full residue. */
    for (i = 0; i < 13u; i++) csw[i] = 0;
    csw[0] = 0x55u; csw[1] = 0x53u; csw[2] = 0x42u; csw[3] = 0x53u; /* 'USBS' */
    csw[4] = 0x2Au; /* tag 0x2A */
    csw[8] = 0x00u; csw[9] = 0x02u; /* residue 0x200 */
    csw[12] = 1u;   /* bCSWStatus: command failed */
    CHECK_HEX("failed CSW is structurally valid", 1, hype_usb_bot_csw_valid(csw, 0x2Au));
    CHECK_HEX("failed CSW is not ok", 0, hype_usb_bot_csw_ok(csw, 0x2Au));
    CHECK_HEX("status extracted", 1u, hype_usb_bot_csw_status(csw));
    CHECK_HEX("residue extracted", 0x200u, hype_usb_bot_csw_residue(csw));
    CHECK_HEX("wrong tag is invalid", 0, hype_usb_bot_csw_valid(csw, 0x2Bu));
    csw[0] = 0x00u;
    CHECK_HEX("bad signature is invalid", 0, hype_usb_bot_csw_valid(csw, 0x2Au));

    /* REQUEST SENSE CDB shape. */
    hype_scsi_cdb_request_sense(cdb, 18u);
    CHECK_HEX("REQUEST SENSE opcode", 0x03u, cdb[0]);
    CHECK_HEX("REQUEST SENSE alloc", 18u, cdb[4]);
    CHECK_HEX("REQUEST SENSE reserved", 0u, cdb[1] | cdb[2] | cdb[3] | cdb[5]);

    /* Fixed-format sense: ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE --
     * the exact bytes the 64 GB Cruzer answers for SYNCHRONIZE CACHE. */
    for (i = 0; i < 18u; i++) sense[i] = 0;
    sense[0] = 0x70u; sense[2] = 0x05u; sense[12] = 0x20u; sense[13] = 0x00u;
    CHECK_HEX("fixed sense parses", 0,
              hype_scsi_parse_fixed_sense(sense, 18u, &key, &asc, &ascq));
    CHECK_HEX("sense key", 0x5u, key);
    CHECK_HEX("sense asc", 0x20u, asc);
    CHECK_HEX("sense ascq", 0x00u, ascq);
    sense[0] = 0x71u; /* deferred: still fixed-format per the 0x7F mask */
    CHECK_HEX("deferred fixed sense parses", 0,
              hype_scsi_parse_fixed_sense(sense, 18u, &key, &asc, &ascq));
    sense[0] = 0x72u; /* descriptor format: not parsed */
    CHECK_HEX("descriptor sense rejected", -1,
              hype_scsi_parse_fixed_sense(sense, 18u, &key, &asc, &ascq));
    CHECK_HEX("short sense rejected", -1,
              hype_scsi_parse_fixed_sense(sense, 13u, &key, &asc, &ascq));
}

int main(void) {
    test_cbw();
    test_csw();
    test_cdbs();
    test_inquiry_vpd_cdb();
    test_vpd80_serial_parse();
    test_read_capacity_parse();
    test_csw_split_and_sense();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
