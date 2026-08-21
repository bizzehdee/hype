#ifndef HYPE_DEVICES_USB_MSC_H
#define HYPE_DEVICES_USB_MSC_H

#include <stdint.h>
#include "xhci_dev.h"
#include "../core/blk_backend.h"

/*
 * #592 (USB-GUEST-2): a USB Mass Storage class device -- Bulk-Only Transport (BOT) carrying SCSI
 * commands -- that plugs into the guest-facing xHCI controller (#591). It answers the standard USB
 * control requests with descriptors that mark it a REMOVABLE mass-storage device (the point of
 * #446: the guest sees removable USB media, not a fixed disk), and it answers SCSI over the bulk
 * endpoints. Block I/O goes through the shared hype_blk_backend_t vtable, so file-backed and
 * physical-backed both work with no class-layer change.
 *
 * The BOT state machine is: bulk-OUT a 31-byte CBW -> optional data phase (bulk-IN or bulk-OUT) ->
 * bulk-IN a 13-byte CSW. All of it is pure and unit-testable: drive the ops directly, or through
 * the xHCI transfer rings.
 */

/* BOT phase. */
typedef enum {
    HYPE_MSC_PHASE_CBW = 0, /* awaiting a Command Block Wrapper */
    HYPE_MSC_PHASE_DATA_IN, /* sending SCSI data to the host */
    HYPE_MSC_PHASE_DATA_OUT,/* receiving SCSI data from the host */
    HYPE_MSC_PHASE_CSW      /* awaiting the host to collect the Command Status Wrapper */
} hype_msc_phase_t;

/* Small SCSI responses (INQUIRY/READ CAPACITY/REQUEST SENSE/MODE SENSE) fit here. */
#define HYPE_MSC_RESP_MAX 64u
#define HYPE_MSC_SECTOR 512u

typedef struct {
    const hype_blk_backend_t *be;
    hype_usb_device_t usb; /* the xHCI attaches through this (usb.ops/usb.ctx point back here) */

    hype_msc_phase_t phase;

    /* Current CBW. */
    uint32_t tag;
    uint32_t data_len;   /* dCBWDataTransferLength -- host's expectation for the data phase */
    uint8_t cbw_dir_in;  /* 1 if the data phase is device-to-host */
    uint8_t csw_status;   /* 0 pass, 1 fail, 2 phase error -- decided at CBW time */

    /* Active data-in source. */
    uint8_t resp[HYPE_MSC_RESP_MAX];
    uint32_t resp_len;    /* bytes still to send from resp[] */
    uint32_t resp_off;    /* next byte in resp[] */
    int from_disk;        /* data phase streams from the backend rather than resp[] */
    uint64_t disk_lba;    /* next sector for the disk data phase */
    uint32_t disk_remaining; /* bytes left in the disk data phase */
    uint32_t residue;     /* dCSWDataResidue accumulator */

    /* Fixed-format sense data returned by REQUEST SENSE. */
    uint8_t sense_key;
    uint8_t sense_asc;
    uint8_t sense_ascq;

    uint8_t bounce[HYPE_MSC_SECTOR]; /* for sub-sector alignment on the data phase */

    /* Counters. */
    uint32_t cbws;
    uint32_t csws;
} hype_usb_msc_t;

/*
 * Initialise the device over `be`. Returns a pointer to the embedded hype_usb_device_t to hand to
 * hype_xhci_dev_attach(). `be` may be NULL (no medium); descriptors still enumerate, and SCSI
 * commands report NOT READY.
 */
hype_usb_device_t *hype_usb_msc_init(hype_usb_msc_t *msc, const hype_blk_backend_t *be);

#endif /* HYPE_DEVICES_USB_MSC_H */
