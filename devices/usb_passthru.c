#include "usb_passthru.h"

/*
 * #595: the mediated-passthrough device model. Every guest transfer is forwarded to the real host
 * device through the host-ops vtable. See usb_passthru.h. The only requests served LOCALLY are the
 * ones the emulated controller (not the device) owns: SET_ADDRESS (the guest xHCI assigns the
 * address itself, so forwarding it would set a second, conflicting address on the real device).
 * Everything else -- GET_DESCRIPTOR (real VID/PID + real descriptors), SET_CONFIGURATION,
 * class-specific requests -- goes to the device so the guest drives the real thing.
 */

static int pt_control(void *ctx, const uint8_t setup[8], uint8_t *data, uint32_t data_max,
                      uint32_t *data_len) {
    hype_usb_passthru_t *pt = (hype_usb_passthru_t *)ctx;
    uint8_t bmRequestType = setup[0];
    uint8_t bRequest = setup[1];
    if (data_len) {
        *data_len = 0;
    }
    if (pt == 0 || pt->host == 0 || pt->host->control == 0) {
        return -1;
    }
    /* SET_ADDRESS (standard, bRequest 5): swallowed. The emulated xHCI already addressed the slot;
     * the real device keeps whatever address hype's host driver gave it during enumeration, and
     * the two address spaces are independent. Forwarding it would reassign the real device. */
    if ((bmRequestType & 0x60u) == 0x00u && bRequest == 0x05u) {
        return 0;
    }
    pt->controls_forwarded++;
    return pt->host->control(pt->host_ctx, setup, data, data_max, data_len);
}

static int pt_bulk_out(void *ctx, const uint8_t *data, uint32_t len) {
    hype_usb_passthru_t *pt = (hype_usb_passthru_t *)ctx;
    uint32_t actual = 0;
    if (pt == 0 || pt->host == 0 || pt->host->bulk == 0 || pt->bulk_out_ep == 0u) {
        return -1;
    }
    pt->bulks_forwarded++;
    /* Cast away const for the shared host-ops signature; the OUT direction never writes `data`. */
    return pt->host->bulk(pt->host_ctx, pt->bulk_out_ep, (uint8_t *)(uintptr_t)data, len, &actual);
}

static int pt_bulk_in(void *ctx, uint8_t *data, uint32_t max, uint32_t *len) {
    hype_usb_passthru_t *pt = (hype_usb_passthru_t *)ctx;
    uint32_t actual = 0;
    int rc;
    if (len) {
        *len = 0;
    }
    if (pt == 0 || pt->host == 0 || pt->host->bulk == 0 || pt->bulk_in_ep == 0u) {
        return -1;
    }
    pt->bulks_forwarded++;
    rc = pt->host->bulk(pt->host_ctx, pt->bulk_in_ep, data, max, &actual);
    if (rc == 0 && len) {
        *len = actual;
    }
    return rc;
}

static const hype_usb_device_ops_t k_passthru_ops = {
    .control = pt_control,
    .bulk_out = pt_bulk_out,
    .bulk_in = pt_bulk_in,
};

hype_usb_device_t *hype_usb_passthru_init(hype_usb_passthru_t *pt, const hype_usb_host_ops_t *host,
                                          void *host_ctx, uint8_t bulk_in_ep, uint8_t bulk_out_ep) {
    uint32_t i;
    if (pt == 0) {
        return 0;
    }
    for (i = 0; i < sizeof(*pt); i++) {
        ((uint8_t *)pt)[i] = 0;
    }
    pt->host = host;
    pt->host_ctx = host_ctx;
    pt->bulk_in_ep = bulk_in_ep;
    pt->bulk_out_ep = bulk_out_ep;
    pt->usb.ops = &k_passthru_ops;
    pt->usb.ctx = pt;
    return &pt->usb;
}
