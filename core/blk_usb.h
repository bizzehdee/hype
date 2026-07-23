#ifndef HYPE_CORE_BLK_USB_H
#define HYPE_CORE_BLK_USB_H

#include <stdint.h>
#include "blk_backend.h"
#include "blk_phys.h"
#include "xhci.h"

/*
 * USB-4 (#216): bind a USB Mass Storage device (already Enable-Slot'd,
 * Address'd, configured + bulk-endpoint'd -- see xhci.h) to a hype_blk_backend,
 * the same interface the AHCI/NVMe physical backends use. Reads/writes go
 * through hype_xhci_msc_read/_write; because those bounce through a single 4 KiB
 * page, this adapter sub-chunks each blk_backend request into <=8-sector pieces.
 * Coverage-exempt shim (real xHCI I/O), like blk_phys_hw.c.
 */

typedef struct {
    hype_xhci_ctrl_t *ctrl;
    unsigned int slot;
    hype_xhci_msc_eps_t msc;   /* owned copy; caller's need not outlive this */
    unsigned int block_size;   /* logical block size (512 supported) */
} hype_blk_usb_t;

/*
 * Wires `be` (via the physical-backend chunker `p`) to the USB MSC device on
 * `slot`, with `total_sectors` the READ CAPACITY count. `hw` (caller-allocated)
 * must outlive the backend. Writable (be->write set) -- USB is removable, so no
 * §6d physical-write guard applies. Requires block_size == 512.
 */
void hype_blk_usb_init(hype_blk_usb_t *hw, hype_blk_phys_t *p, hype_blk_backend_t *be,
                       hype_xhci_ctrl_t *ctrl, unsigned int slot,
                       const hype_xhci_msc_eps_t *msc, unsigned int block_size,
                       uint64_t total_sectors);

#endif /* HYPE_CORE_BLK_USB_H */
