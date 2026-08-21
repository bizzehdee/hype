#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../devices/usb_passthru.h"
#include "../../devices/xhci_dev.h"

/* #595: unit tests for the mediated USB passthrough device model. A MOCK host device stands in for
 * a real one driven by core/xhci.c: it returns a distinctive device descriptor (so we can prove
 * the guest sees the REAL VID/PID) and echoes bulk data. We drive the model both directly and
 * through the emulated xHCI transfer rings. */

static int failures = 0;
#define CHECK_INT(desc, expected, actual)                                                          \
    do {                                                                                           \
        long long _e = (long long)(expected), _a = (long long)(actual);                            \
        if (_e != _a) { printf("FAIL: %s: expected %lld, got %lld\n", (desc), _e, _a); failures++; } \
    } while (0)
#define CHECK_HEX(desc, expected, actual)                                                          \
    do {                                                                                           \
        unsigned long long _e = (unsigned long long)(expected), _a = (unsigned long long)(actual); \
        if (_e != _a) { printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), _e, _a); failures++; } \
    } while (0)

/* ---- mock host device ------------------------------------------------------------------- */
#define MOCK_VID 0x0951u /* Kingston, a real vendor -- proves real VID/PID pass-through */
#define MOCK_PID 0x1666u

static int g_mock_set_address_seen;
static uint8_t g_mock_last_bulk_ep;

static const uint8_t mock_devdesc[18] = {
    18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
    (uint8_t)MOCK_VID, (uint8_t)(MOCK_VID >> 8), (uint8_t)MOCK_PID, (uint8_t)(MOCK_PID >> 8),
    0x00, 0x01, 1, 2, 3, 1
};

static int mock_control(void *ctx, const uint8_t setup[8], uint8_t *data, uint32_t data_max,
                        uint32_t *data_len) {
    uint8_t bmReq = setup[0], bReq = setup[1];
    uint16_t wValue = (uint16_t)(setup[2] | ((uint16_t)setup[3] << 8));
    uint16_t wLength = (uint16_t)(setup[6] | ((uint16_t)setup[7] << 8));
    (void)ctx;
    if (data_len) *data_len = 0;
    /* The mock should NEVER be asked to SET_ADDRESS -- the passthrough model must swallow it. */
    if ((bmReq & 0x60u) == 0x00u && bReq == 0x05u) {
        g_mock_set_address_seen = 1;
        return 0;
    }
    if ((bmReq & 0x60u) == 0x00u && bReq == 0x06u && (wValue >> 8) == 0x01u) {
        uint32_t n = sizeof(mock_devdesc);
        if (n > wLength) n = wLength;
        if (n > data_max) n = data_max;
        if (data) memcpy(data, mock_devdesc, n);
        if (data_len) *data_len = n;
        return 0;
    }
    if ((bmReq & 0x60u) == 0x00u && bReq == 0x09u) {
        return 0; /* SET_CONFIGURATION */
    }
    return -1; /* STALL everything else */
}

static int mock_bulk(void *ctx, uint8_t ep_addr, uint8_t *data, uint32_t len, uint32_t *actual) {
    (void)ctx;
    g_mock_last_bulk_ep = ep_addr;
    if (ep_addr & 0x80u) {
        /* IN: fill with a recognizable pattern. */
        uint32_t i;
        for (i = 0; i < len; i++) data[i] = (uint8_t)(0xC0u + (i & 0x3Fu));
    }
    if (actual) *actual = len;
    return 0;
}

static const hype_usb_host_ops_t k_mock_host = { .control = mock_control, .bulk = mock_bulk };

/* ---- direct-ops tests -------------------------------------------------------------------- */
static void test_descriptor_passthrough(void) {
    hype_usb_passthru_t pt;
    hype_usb_device_t *usb;
    uint8_t buf[18];
    uint32_t got = 0;
    uint8_t get_dev[8] = {0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0};
    g_mock_set_address_seen = 0;
    usb = hype_usb_passthru_init(&pt, &k_mock_host, 0, 0x81u, 0x01u);
    CHECK_INT("control forwarded ok", 0, usb->ops->control(usb->ctx, get_dev, buf, sizeof(buf), &got));
    CHECK_INT("desc len", 18, got);
    CHECK_HEX("real VID passed through", MOCK_VID, (uint32_t)buf[8] | ((uint32_t)buf[9] << 8));
    CHECK_HEX("real PID passed through", MOCK_PID, (uint32_t)buf[10] | ((uint32_t)buf[11] << 8));
    CHECK_INT("one control forwarded", 1, pt.controls_forwarded);
}

static void test_set_address_swallowed(void) {
    hype_usb_passthru_t pt;
    hype_usb_device_t *usb;
    uint32_t got = 0;
    uint8_t set_addr[8] = {0x00, 0x05, 0x07, 0x00, 0, 0, 0, 0}; /* SET_ADDRESS 7 */
    g_mock_set_address_seen = 0;
    usb = hype_usb_passthru_init(&pt, &k_mock_host, 0, 0x81u, 0x01u);
    CHECK_INT("set_address accepted", 0, usb->ops->control(usb->ctx, set_addr, 0, 0, &got));
    CHECK_INT("host never saw SET_ADDRESS", 0, g_mock_set_address_seen);
    CHECK_INT("not counted as forwarded", 0, pt.controls_forwarded);
}

static void test_bulk_forwarding(void) {
    hype_usb_passthru_t pt;
    hype_usb_device_t *usb;
    uint8_t buf[64];
    uint32_t got = 0;
    usb = hype_usb_passthru_init(&pt, &k_mock_host, 0, 0x82u, 0x03u);
    /* Bulk IN routes to the host's IN endpoint and returns the mock's pattern. */
    memset(buf, 0, sizeof(buf));
    CHECK_INT("bulk in ok", 0, usb->ops->bulk_in(usb->ctx, buf, 64, &got));
    CHECK_INT("bulk in len", 64, got);
    CHECK_HEX("used host IN endpoint 0x82", 0x82, g_mock_last_bulk_ep);
    CHECK_HEX("mock pattern byte0", 0xC0, buf[0]);
    /* Bulk OUT routes to the host's OUT endpoint. */
    CHECK_INT("bulk out ok", 0, usb->ops->bulk_out(usb->ctx, buf, 64));
    CHECK_HEX("used host OUT endpoint 0x03", 0x03, g_mock_last_bulk_ep);
    CHECK_INT("two bulks forwarded", 2, pt.bulks_forwarded);
}

/* ---- end-to-end through the emulated xHCI ------------------------------------------------ */
#define GBASE 0x60000000ull
#define OFF_EVT 0x0000u
#define OFF_ERST 0x1000u
#define OFF_EP0 0x2000u
#define OFF_SETUP_DATA 0x3000u
#define IMGSZ 0x4000u

typedef struct {
    uint8_t img[IMGSZ];
    hype_gpa_map_t map;
    hype_xhci_dev_t xh;
    hype_usb_passthru_t pt;
} erig_t;

static void eput32(erig_t *r, uint32_t off, uint32_t v) {
    r->img[off] = (uint8_t)v; r->img[off+1] = (uint8_t)(v>>8);
    r->img[off+2] = (uint8_t)(v>>16); r->img[off+3] = (uint8_t)(v>>24);
}
static void eput64(erig_t *r, uint32_t off, uint64_t v) { eput32(r, off, (uint32_t)v); eput32(r, off+4, (uint32_t)(v>>32)); }

static void test_control_via_xhci_rings(void) {
    erig_t *r = (erig_t *)malloc(sizeof(erig_t));
    hype_usb_device_t *usb;
    uint32_t evctrl;
    if (!r) { printf("FAIL: malloc\n"); failures++; return; }
    memset(r, 0, sizeof(*r));
    usb = hype_usb_passthru_init(&r->pt, &k_mock_host, 0, 0x81u, 0x01u);
    hype_gpa_map_reset(&r->map);
    hype_gpa_map_add(&r->map, GBASE, (uint64_t)(uintptr_t)r->img, IMGSZ);
    hype_xhci_dev_reset(&r->xh, 1);
    hype_xhci_dev_attach(&r->xh, usb);
    eput64(r, OFF_ERST, GBASE + OFF_EVT);
    eput32(r, OFF_ERST + 8, 64u);
    hype_xhci_dev_mmio_write(&r->xh, HYPE_GXHCI_RT_ERSTSZ, 4u, 1u, &r->map);
    hype_xhci_dev_mmio_write(&r->xh, HYPE_GXHCI_RT_ERSTBA_LO, 4u, (uint32_t)(GBASE+OFF_ERST), &r->map);
    hype_xhci_dev_mmio_write(&r->xh, HYPE_GXHCI_OP_USBCMD, 4u, HYPE_GXHCI_USBCMD_RS, &r->map);
    /* Bring EP0 up manually and point it at our control ring. */
    r->xh.slots[1].state = HYPE_GXHCI_SLOT_ADDRESSED;
    r->xh.slots[1].ep_configured[HYPE_GXHCI_DCI_CONTROL] = 1u;
    r->xh.slots[1].ep_ring[HYPE_GXHCI_DCI_CONTROL] = GBASE + OFF_EP0;
    r->xh.slots[1].ep_cycle[HYPE_GXHCI_DCI_CONTROL] = 1u;

    /* Setup Stage: GET_DESCRIPTOR device, 18 bytes (immediate data in the TRB). */
    {
        uint8_t setup[8] = {0x80, 0x06, 0x00, 0x01, 0, 0, 18, 0};
        eput32(r, OFF_EP0 + 0, (uint32_t)setup[0] | ((uint32_t)setup[1]<<8) | ((uint32_t)setup[2]<<16) | ((uint32_t)setup[3]<<24));
        eput32(r, OFF_EP0 + 4, (uint32_t)setup[4] | ((uint32_t)setup[5]<<8) | ((uint32_t)setup[6]<<16) | ((uint32_t)setup[7]<<24));
        eput32(r, OFF_EP0 + 8, 8u);
        eput32(r, OFF_EP0 + 12, ((uint32_t)HYPE_GXHCI_TRB_SETUP_STAGE << HYPE_GXHCI_TRB_TYPE_SHIFT) | HYPE_GXHCI_TRB_IDT | HYPE_GXHCI_TRB_CYCLE);
    }
    /* Data Stage: IN, buffer at OFF_SETUP_DATA, 18 bytes. */
    eput64(r, OFF_EP0 + 16, GBASE + OFF_SETUP_DATA);
    eput32(r, OFF_EP0 + 24, 18u);
    eput32(r, OFF_EP0 + 28, ((uint32_t)HYPE_GXHCI_TRB_DATA_STAGE << HYPE_GXHCI_TRB_TYPE_SHIFT) | HYPE_GXHCI_TRB_DATA_DIR_IN | HYPE_GXHCI_TRB_IOC | HYPE_GXHCI_TRB_CYCLE);
    /* Status Stage. */
    eput32(r, OFF_EP0 + 44, ((uint32_t)HYPE_GXHCI_TRB_STATUS_STAGE << HYPE_GXHCI_TRB_TYPE_SHIFT) | HYPE_GXHCI_TRB_IOC | HYPE_GXHCI_TRB_CYCLE);

    hype_xhci_dev_doorbell(&r->xh, 1u, HYPE_GXHCI_DCI_CONTROL, &r->map);

    /* The real device descriptor (with the mock's VID/PID) landed in the guest data buffer. */
    CHECK_HEX("VID via xhci rings", MOCK_VID, (uint32_t)r->img[OFF_SETUP_DATA+8] | ((uint32_t)r->img[OFF_SETUP_DATA+9]<<8));
    CHECK_HEX("PID via xhci rings", MOCK_PID, (uint32_t)r->img[OFF_SETUP_DATA+10] | ((uint32_t)r->img[OFF_SETUP_DATA+11]<<8));
    CHECK_INT("transfers processed", 1, r->xh.transfers_processed >= 1 ? 1 : 0);
    evctrl = (uint32_t)r->img[OFF_EVT+12] | ((uint32_t)r->img[OFF_EVT+13]<<8) | ((uint32_t)r->img[OFF_EVT+14]<<16) | ((uint32_t)r->img[OFF_EVT+15]<<24);
    CHECK_INT("event is transfer event", HYPE_GXHCI_TRB_TRANSFER_EVENT, (evctrl >> HYPE_GXHCI_TRB_TYPE_SHIFT) & 0x3Fu);
    free(r);
}

int main(void) {
    test_descriptor_passthrough();
    test_set_address_swallowed();
    test_bulk_forwarding();
    test_control_via_xhci_rings();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
