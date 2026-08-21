#ifndef HYPE_DEVICES_USB_PASSTHRU_H
#define HYPE_DEVICES_USB_PASSTHRU_H

#include <stdint.h>
#include "xhci_dev.h"

/*
 * #595 (USB-PASSTHRU-1): a guest-facing MEDIATED passthrough of a real host USB device.
 *
 * The device is claimed on the host via the #241 inventory and driven by core/xhci.c. This model
 * presents it to a guest through the emulated guest xHCI (#591) with its REAL VID/PID and REAL
 * descriptors, so the guest binds its own class driver. hype forwards every transfer between the
 * guest's emulated controller and the host device -- the guest NEVER DMAs to real hardware (the
 * plan.md §6j hard rule: no IOMMU in v1, so guest-initiated DMA to a real device is forbidden).
 *
 * Forwarding goes through a host-ops vtable rather than calling core/xhci.c directly, for two
 * reasons: the datapath differs per host controller and belongs behind core/xhci.c's own
 * abstraction, and a mock host lets the mediation LOGIC (which control requests are served locally
 * vs forwarded, how bulk maps to the host endpoints, descriptor pass-through) be unit-tested
 * without real hardware. The concrete binding to core/xhci.c + the real-hardware validation are
 * the HW-gated half; this is the device model.
 */

/*
 * The host side of a claimed USB device. control() forwards a control transfer to the device's
 * EP0; bulk() forwards a bulk transfer to endpoint address `ep_addr` (bit 7 = direction, 1 = IN).
 * Both return 0 on success (and set the byte count moved via their out-parameter), -1 to STALL.
 */
typedef struct hype_usb_host_ops {
    int (*control)(void *ctx, const uint8_t setup[8], uint8_t *data, uint32_t data_max,
                   uint32_t *data_len);
    int (*bulk)(void *ctx, uint8_t ep_addr, uint8_t *data, uint32_t len, uint32_t *actual);
} hype_usb_host_ops_t;

typedef struct {
    const hype_usb_host_ops_t *host;
    void *host_ctx;
    hype_usb_device_t usb; /* the emulated guest xHCI attaches through this */
    uint8_t bulk_in_ep;    /* host endpoint address for guest bulk-IN (0 = not yet known) */
    uint8_t bulk_out_ep;   /* host endpoint address for guest bulk-OUT */
    uint32_t controls_forwarded;
    uint32_t bulks_forwarded;
} hype_usb_passthru_t;

/*
 * Initialise a passthrough device over the host ops. bulk_in_ep / bulk_out_ep are the host device's
 * bulk endpoint addresses (from its config descriptor); pass 0 if the device is control-only.
 * Returns the embedded hype_usb_device_t to hand to hype_xhci_dev_attach().
 */
hype_usb_device_t *hype_usb_passthru_init(hype_usb_passthru_t *pt, const hype_usb_host_ops_t *host,
                                          void *host_ctx, uint8_t bulk_in_ep, uint8_t bulk_out_ep);

#endif /* HYPE_DEVICES_USB_PASSTHRU_H */
