#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../devices/usb_msc.h"
#include "../../devices/xhci_dev.h"
#include "../blk_backend.h"

/* #592: unit tests for the USB Mass Storage (BOT/SCSI) device. Two levels: (1) drive the class ops
 * directly (CBW -> SCSI -> data -> CSW); (2) drive it THROUGH the xHCI transfer rings, proving the
 * controller's transfer-ring processing (#591) and the class layer work together. */

static int failures = 0;

#define CHECK_INT(desc, expected, actual)                                                          \
    do {                                                                                           \
        long long _e = (long long)(expected), _a = (long long)(actual);                            \
        if (_e != _a) {                                                                            \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), _e, _a);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_HEX(desc, expected, actual)                                                          \
    do {                                                                                           \
        unsigned long long _e = (unsigned long long)(expected), _a = (unsigned long long)(actual); \
        if (_e != _a) {                                                                            \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), _e, _a);                      \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define DISK_SECTORS 256u
static uint8_t g_disk[DISK_SECTORS * 512u];

static void backend_init(hype_blk_file_t *f, hype_blk_backend_t *be) {
    hype_blk_file_init(f, be, g_disk, sizeof(g_disk));
}

/* Build a 31-byte CBW. */
static void build_cbw(uint8_t *cbw, uint32_t tag, uint32_t dlen, int dir_in, const uint8_t *cdb,
                      uint8_t cdb_len) {
    memset(cbw, 0, 31);
    cbw[0] = 0x55;
    cbw[1] = 0x53;
    cbw[2] = 0x42;
    cbw[3] = 0x43; /* 'USBC' */
    cbw[4] = (uint8_t)tag;
    cbw[5] = (uint8_t)(tag >> 8);
    cbw[6] = (uint8_t)(tag >> 16);
    cbw[7] = (uint8_t)(tag >> 24);
    cbw[8] = (uint8_t)dlen;
    cbw[9] = (uint8_t)(dlen >> 8);
    cbw[10] = (uint8_t)(dlen >> 16);
    cbw[11] = (uint8_t)(dlen >> 24);
    cbw[12] = dir_in ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = cdb_len;
    memcpy(&cbw[15], cdb, cdb_len);
}

static void test_inquiry_removable(void) {
    hype_blk_file_t f;
    hype_blk_backend_t be;
    hype_usb_msc_t msc;
    hype_usb_device_t *usb;
    uint8_t cbw[31], resp[64], csw[13];
    uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
    uint32_t got = 0;
    backend_init(&f, &be);
    usb = hype_usb_msc_init(&msc, &be);

    build_cbw(cbw, 0x11, 36, 1, cdb, 6);
    CHECK_INT("cbw accepted", 0, usb->ops->bulk_out(usb->ctx, cbw, 31));
    CHECK_INT("phase data-in", HYPE_MSC_PHASE_DATA_IN, msc.phase);
    CHECK_INT("inquiry data", 0, usb->ops->bulk_in(usb->ctx, resp, 36, &got));
    CHECK_INT("inquiry len", 36, got);
    CHECK_HEX("peripheral type = block", 0x00, resp[0]);
    CHECK_INT("REMOVABLE bit set", 1, (resp[1] & 0x80) ? 1 : 0);
    CHECK_INT("phase csw", HYPE_MSC_PHASE_CSW, msc.phase);
    CHECK_INT("csw read", 0, usb->ops->bulk_in(usb->ctx, csw, 13, &got));
    CHECK_INT("csw len", 13, got);
    CHECK_HEX("csw sig", 0x53425355u, (uint32_t)csw[0] | ((uint32_t)csw[1] << 8) |
                                          ((uint32_t)csw[2] << 16) | ((uint32_t)csw[3] << 24));
    CHECK_INT("csw status pass", 0, csw[12]);
    CHECK_INT("back to cbw phase", HYPE_MSC_PHASE_CBW, msc.phase);
}

static void test_read_capacity(void) {
    hype_blk_file_t f;
    hype_blk_backend_t be;
    hype_usb_msc_t msc;
    hype_usb_device_t *usb;
    uint8_t cbw[31], resp[8];
    uint8_t cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t got = 0, last_lba, blk;
    backend_init(&f, &be);
    usb = hype_usb_msc_init(&msc, &be);
    build_cbw(cbw, 0x22, 8, 1, cdb, 10);
    usb->ops->bulk_out(usb->ctx, cbw, 31);
    usb->ops->bulk_in(usb->ctx, resp, 8, &got);
    CHECK_INT("capacity len", 8, got);
    last_lba = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) | ((uint32_t)resp[2] << 8) |
               resp[3];
    blk = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) | ((uint32_t)resp[6] << 8) | resp[7];
    CHECK_INT("last LBA = sectors-1", DISK_SECTORS - 1u, last_lba);
    CHECK_INT("block size 512", 512, blk);
}

static void test_read_write_roundtrip(void) {
    hype_blk_file_t f;
    hype_blk_backend_t be;
    hype_usb_msc_t msc;
    hype_usb_device_t *usb;
    uint8_t cbw[31], csw[13];
    uint8_t wbuf[1024], rbuf[1024];
    uint32_t got = 0, i;
    uint8_t wcdb[10] = {0x2A, 0, 0, 0, 0, 5, 0, 0, 2, 0}; /* WRITE(10) LBA 5, 2 blocks */
    uint8_t rcdb[10] = {0x28, 0, 0, 0, 0, 5, 0, 0, 2, 0}; /* READ(10)  LBA 5, 2 blocks */
    backend_init(&f, &be);
    usb = hype_usb_msc_init(&msc, &be);
    for (i = 0; i < sizeof(wbuf); i++) {
        wbuf[i] = (uint8_t)(i * 7u + 1u);
    }
    /* WRITE */
    build_cbw(cbw, 0x33, 1024, 0, wcdb, 10);
    CHECK_INT("write cbw", 0, usb->ops->bulk_out(usb->ctx, cbw, 31));
    CHECK_INT("phase data-out", HYPE_MSC_PHASE_DATA_OUT, msc.phase);
    CHECK_INT("write data", 0, usb->ops->bulk_out(usb->ctx, wbuf, 1024));
    CHECK_INT("phase csw after write", HYPE_MSC_PHASE_CSW, msc.phase);
    usb->ops->bulk_in(usb->ctx, csw, 13, &got);
    CHECK_INT("write csw pass", 0, csw[12]);
    /* READ back */
    build_cbw(cbw, 0x34, 1024, 1, rcdb, 10);
    usb->ops->bulk_out(usb->ctx, cbw, 31);
    memset(rbuf, 0, sizeof(rbuf));
    CHECK_INT("read data", 0, usb->ops->bulk_in(usb->ctx, rbuf, 1024, &got));
    CHECK_INT("read len", 1024, got);
    CHECK_INT("roundtrip match", 0, memcmp(wbuf, rbuf, 1024));
    usb->ops->bulk_in(usb->ctx, csw, 13, &got);
    CHECK_INT("read csw pass", 0, csw[12]);
    /* And the bytes really landed on the backing disk at LBA 5. */
    CHECK_INT("disk has the data", 0, memcmp(&g_disk[5 * 512], wbuf, 1024));
}

static void test_bad_lba_fails(void) {
    hype_blk_file_t f;
    hype_blk_backend_t be;
    hype_usb_msc_t msc;
    hype_usb_device_t *usb;
    uint8_t cbw[31], csw[13];
    /* READ(10) LBA 250, 20 blocks -> past 256-sector disk. */
    uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 250, 0, 0, 20, 0};
    uint32_t got = 0;
    backend_init(&f, &be);
    usb = hype_usb_msc_init(&msc, &be);
    build_cbw(cbw, 0x44, 20u * 512u, 1, cdb, 10);
    usb->ops->bulk_out(usb->ctx, cbw, 31);
    CHECK_INT("bad LBA -> csw phase", HYPE_MSC_PHASE_CSW, msc.phase);
    usb->ops->bulk_in(usb->ctx, csw, 13, &got);
    CHECK_INT("csw status fail", 1, csw[12]);
}

static void test_control_descriptors(void) {
    hype_blk_file_t f;
    hype_blk_backend_t be;
    hype_usb_msc_t msc;
    hype_usb_device_t *usb;
    uint8_t buf[64];
    uint32_t got = 0;
    uint8_t get_dev[8] = {0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0}; /* GET_DESCRIPTOR device, len 18 */
    uint8_t get_cfg[8] = {0x80, 0x06, 0x00, 0x02, 0, 0, 32, 0}; /* config, len 32 */
    uint8_t max_lun[8] = {0xA1, 0xFE, 0x00, 0x00, 0, 0, 1, 0};  /* class Get Max LUN */
    backend_init(&f, &be);
    usb = hype_usb_msc_init(&msc, &be);
    CHECK_INT("get device desc", 0, usb->ops->control(usb->ctx, get_dev, buf, sizeof(buf), &got));
    CHECK_INT("device desc len", 18, got);
    CHECK_HEX("bDescriptorType device", 0x01, buf[1]);
    CHECK_INT("get config desc", 0, usb->ops->control(usb->ctx, get_cfg, buf, sizeof(buf), &got));
    CHECK_INT("config desc len", 32, got);
    CHECK_HEX("interface is MSC/SCSI/BOT class", 0x08, buf[9 + 5]); /* interface bInterfaceClass */
    CHECK_INT("get max lun", 0, usb->ops->control(usb->ctx, max_lun, buf, sizeof(buf), &got));
    CHECK_INT("max lun len", 1, got);
    CHECK_INT("one LUN", 0, buf[0]);
}

/* ---- end-to-end through the xHCI transfer rings ------------------------------------------ */
#define GBASE 0x50000000ull
#define OFF_EVT 0x0000u   /* event ring (64 TRBs) */
#define OFF_ERST 0x1000u
#define OFF_BULK_OUT 0x2000u /* bulk-OUT transfer ring */
#define OFF_BULK_IN 0x3000u  /* bulk-IN transfer ring */
#define OFF_CBW 0x4000u
#define OFF_DATA 0x5000u
#define OFF_CSW 0x6000u
#define IMGSZ 0x8000u

typedef struct {
    uint8_t img[IMGSZ];
    hype_gpa_map_t map;
    hype_xhci_dev_t xh;
    hype_usb_msc_t msc;
    hype_blk_file_t f;
    hype_blk_backend_t be;
} xrig_t;

static void xput32(xrig_t *r, uint32_t off, uint32_t v) {
    r->img[off] = (uint8_t)v;
    r->img[off + 1] = (uint8_t)(v >> 8);
    r->img[off + 2] = (uint8_t)(v >> 16);
    r->img[off + 3] = (uint8_t)(v >> 24);
}
static void xput64(xrig_t *r, uint32_t off, uint64_t v) {
    xput32(r, off, (uint32_t)v);
    xput32(r, off + 4, (uint32_t)(v >> 32));
}
static uint32_t xget32(xrig_t *r, uint32_t off) {
    return (uint32_t)r->img[off] | ((uint32_t)r->img[off + 1] << 8) |
           ((uint32_t)r->img[off + 2] << 16) | ((uint32_t)r->img[off + 3] << 24);
}

/* Put a Normal transfer TRB at ring offset `off`, buffer at bufgpa, length len, cycle 1, IOC. */
static void put_normal(xrig_t *r, uint32_t off, uint64_t bufgpa, uint32_t len) {
    xput64(r, off, bufgpa);
    xput32(r, off + 8, len & HYPE_XHCI_TRB_XFER_LEN_MASK);
    xput32(r, off + 12,
           ((uint32_t)HYPE_XHCI_TRB_NORMAL << HYPE_XHCI_TRB_TYPE_SHIFT) | HYPE_XHCI_TRB_IOC |
               HYPE_XHCI_TRB_CYCLE);
}

static void test_end_to_end_via_xhci(void) {
    xrig_t *r = (xrig_t *)malloc(sizeof(xrig_t));
    hype_usb_device_t *usb;
    uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 3, 0, 0, 1, 0}; /* READ(10) LBA 3, 1 block */
    uint32_t i, evctrl;
    if (r == 0) {
        printf("FAIL: malloc\n");
        failures++;
        return;
    }
    memset(r, 0, sizeof(*r));
    /* Seed the backing disk sector 3 with a known pattern. */
    for (i = 0; i < 512; i++) {
        g_disk[3 * 512 + i] = (uint8_t)(i ^ 0x5A);
    }
    hype_blk_file_init(&r->f, &r->be, g_disk, sizeof(g_disk));
    usb = hype_usb_msc_init(&r->msc, &r->be);
    hype_gpa_map_reset(&r->map);
    hype_gpa_map_add(&r->map, GBASE, (uint64_t)(uintptr_t)r->img, IMGSZ);
    hype_xhci_dev_reset(&r->xh, 1);
    hype_xhci_dev_attach(&r->xh, usb);

    /* Program the event ring. */
    xput64(r, OFF_ERST, GBASE + OFF_EVT);
    xput32(r, OFF_ERST + 8, 64u);
    hype_xhci_dev_mmio_write(&r->xh, HYPE_XHCI_RT_ERSTSZ, 4u, 1u, &r->map);
    hype_xhci_dev_mmio_write(&r->xh, HYPE_XHCI_RT_ERSTBA_LO, 4u, (uint32_t)(GBASE + OFF_ERST),
                             &r->map);
    hype_xhci_dev_mmio_write(&r->xh, HYPE_XHCI_OP_USBCMD, 4u,
                             HYPE_XHCI_USBCMD_RS | HYPE_XHCI_USBCMD_INTE, &r->map);

    /* Manually bring slot 1 up to CONFIGURED with bulk endpoints, bypassing the command ring
     * (that path is covered by test_xhci_dev). Point EP rings at our transfer rings. */
    r->xh.slots[1].state = HYPE_XHCI_SLOT_CONFIGURED;
    r->xh.slots[1].ep_configured[HYPE_XHCI_DCI_BULK_OUT] = 1u;
    r->xh.slots[1].ep_ring[HYPE_XHCI_DCI_BULK_OUT] = GBASE + OFF_BULK_OUT;
    r->xh.slots[1].ep_cycle[HYPE_XHCI_DCI_BULK_OUT] = 1u;
    r->xh.slots[1].ep_configured[HYPE_XHCI_DCI_BULK_IN] = 1u;
    r->xh.slots[1].ep_ring[HYPE_XHCI_DCI_BULK_IN] = GBASE + OFF_BULK_IN;
    r->xh.slots[1].ep_cycle[HYPE_XHCI_DCI_BULK_IN] = 1u;

    /* Build the CBW in guest memory and a Normal TRB on the bulk-OUT ring pointing at it. */
    build_cbw(&r->img[OFF_CBW], 0x77, 512, 1, cdb, 10);
    put_normal(r, OFF_BULK_OUT, GBASE + OFF_CBW, 31u);
    /* Bulk-IN ring: one TRB for the 512-byte data, one for the 13-byte CSW. */
    put_normal(r, OFF_BULK_IN, GBASE + OFF_DATA, 512u);
    put_normal(r, OFF_BULK_IN + HYPE_XHCI_TRB_SIZE, GBASE + OFF_CSW, 13u);

    /* Ring the bulk-OUT doorbell (slot 1, DCI 2) -> CBW consumed, READ dispatched. */
    hype_xhci_dev_doorbell(&r->xh, 1u, HYPE_XHCI_DCI_BULK_OUT, &r->map);
    CHECK_INT("cbw consumed", 1, r->msc.cbws);
    CHECK_INT("phase data-in after cbw", HYPE_MSC_PHASE_DATA_IN, r->msc.phase);
    /* Ring the bulk-IN doorbell -> data TRB then CSW TRB both processed. */
    hype_xhci_dev_doorbell(&r->xh, 1u, HYPE_XHCI_DCI_BULK_IN, &r->map);

    /* The disk sector landed in the guest data buffer. */
    CHECK_INT("read data matches disk", 0, memcmp(&r->img[OFF_DATA], &g_disk[3 * 512], 512));
    /* The CSW landed with pass status. */
    CHECK_HEX("csw sig in guest mem", 0x53425355u, xget32(r, OFF_CSW));
    CHECK_INT("csw status pass", 0, r->img[OFF_CSW + 12]);
    /* Transfer Events were posted (data + CSW, plus the OUT CBW). */
    CHECK_INT("at least 3 transfer events", 1, r->xh.events_posted >= 3u ? 1 : 0);
    /* First event is a Transfer Event type for the bulk-OUT CBW TRB. */
    evctrl = xget32(r, OFF_EVT + 12);
    CHECK_INT("event is transfer event", HYPE_XHCI_TRB_TRANSFER_EVENT,
              (evctrl >> HYPE_XHCI_TRB_TYPE_SHIFT) & 0x3Fu);
    free(r);
}

/* Drive one no-data or data-in SCSI command through the ops, return CSW status. */
static int scsi_status(hype_usb_device_t *usb, const uint8_t *cdb, uint8_t cdb_len, uint32_t dlen,
                       int dir_in, uint8_t *data_out, uint32_t data_max, uint32_t *data_got) {
    uint8_t cbw[31], csw[13];
    uint32_t got = 0;
    build_cbw(cbw, 0x99, dlen, dir_in, cdb, cdb_len);
    usb->ops->bulk_out(usb->ctx, cbw, 31);
    if (dir_in && dlen > 0u && data_out) {
        usb->ops->bulk_in(usb->ctx, data_out, data_max, data_got);
    }
    usb->ops->bulk_in(usb->ctx, csw, 13, &got);
    return csw[12];
}

static void test_misc_scsi(void) {
    hype_blk_file_t f;
    hype_blk_backend_t be;
    hype_usb_msc_t msc;
    hype_usb_device_t *usb;
    uint8_t buf[64];
    uint32_t got = 0;
    uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t modesense[6] = {0x1A, 0, 0x3F, 0, 192, 0};
    uint8_t prevent[6] = {0x1E, 0, 0, 0, 0, 0};
    uint8_t cap16[16] = {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0, 0};
    uint8_t bad[6] = {0xFF, 0, 0, 0, 0, 0};
    backend_init(&f, &be);
    usb = hype_usb_msc_init(&msc, &be);
    CHECK_INT("TEST UNIT READY ok", 0, scsi_status(usb, tur, 6, 0, 0, 0, 0, 0));
    CHECK_INT("PREVENT ALLOW ok", 0, scsi_status(usb, prevent, 6, 0, 0, 0, 0, 0));
    CHECK_INT("REQUEST SENSE ok", 0, scsi_status(usb, sense, 6, 18, 1, buf, sizeof(buf), &got));
    CHECK_INT("sense len", 18, got);
    CHECK_HEX("sense response code", 0x70, buf[0]);
    CHECK_INT("MODE SENSE ok", 0, scsi_status(usb, modesense, 6, 4, 1, buf, sizeof(buf), &got));
    CHECK_INT("mode data length", 3, buf[0]);
    CHECK_INT("READ CAPACITY16 ok", 0, scsi_status(usb, cap16, 16, 32, 1, buf, sizeof(buf), &got));
    CHECK_INT("cap16 len", 32, got);
    CHECK_INT("unknown opcode fails", 1, scsi_status(usb, bad, 6, 0, 0, 0, 0, 0));
    /* The failed command set a sense; REQUEST SENSE now reports ILLEGAL REQUEST (key 5). */
    scsi_status(usb, sense, 6, 18, 1, buf, sizeof(buf), &got);
    CHECK_INT("sense key illegal request", 5, buf[2] & 0x0F);
}

static void test_no_medium(void) {
    hype_usb_msc_t msc;
    hype_usb_device_t *usb;
    uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    usb = hype_usb_msc_init(&msc, 0 /* no backend */);
    /* Descriptors still enumerate. */
    {
        uint8_t buf[32];
        uint32_t got = 0;
        uint8_t get_dev[8] = {0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0};
        CHECK_INT("desc without medium", 0, usb->ops->control(usb->ctx, get_dev, buf, 32, &got));
        CHECK_INT("device desc len", 18, got);
    }
    /* But TEST UNIT READY reports NOT READY. */
    CHECK_INT("TUR not ready", 1, scsi_status(usb, tur, 6, 0, 0, 0, 0, 0));
}

static void test_string_descriptors_and_reset(void) {
    hype_blk_file_t f;
    hype_blk_backend_t be;
    hype_usb_msc_t msc;
    hype_usb_device_t *usb;
    uint8_t buf[64];
    uint32_t got = 0;
    uint8_t s0[8] = {0x80, 0x06, 0x00, 0x03, 0, 0, 64, 0};   /* string 0 (langid) */
    uint8_t s2[8] = {0x80, 0x06, 0x02, 0x03, 0x09, 0x04, 64, 0}; /* string 2 (product) */
    uint8_t bomsr[8] = {0x21, 0xFF, 0x00, 0x00, 0x00, 0x00, 0, 0}; /* class BOMSR */
    backend_init(&f, &be);
    usb = hype_usb_msc_init(&msc, &be);
    CHECK_INT("string0", 0, usb->ops->control(usb->ctx, s0, buf, sizeof(buf), &got));
    CHECK_INT("string0 len", 4, got);
    CHECK_HEX("langid en-US", 0x0409, (uint32_t)buf[2] | ((uint32_t)buf[3] << 8));
    CHECK_INT("string2 product", 0, usb->ops->control(usb->ctx, s2, buf, sizeof(buf), &got));
    CHECK_INT("string desc type", 0x03, buf[1]);
    CHECK_INT("product is unicode 'h'", 'h', buf[2]);
    /* Put the BOT machine mid-flight then reset it. */
    msc.phase = HYPE_MSC_PHASE_DATA_IN;
    CHECK_INT("bomsr ok", 0, usb->ops->control(usb->ctx, bomsr, 0, 0, &got));
    CHECK_INT("phase reset to cbw", HYPE_MSC_PHASE_CBW, msc.phase);
}

int main(void) {
    test_inquiry_removable();
    test_read_capacity();
    test_read_write_roundtrip();
    test_bad_lba_fails();
    test_control_descriptors();
    test_misc_scsi();
    test_no_medium();
    test_string_descriptors_and_reset();
    test_end_to_end_via_xhci();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
