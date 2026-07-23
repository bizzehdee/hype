#include "blk_usb.h"

/* hype_xhci_msc_read/_write bounce through a 4 KiB page, so at most 8 x 512B
 * sectors per xHCI call. blk_backend hands us larger runs; sub-chunk them. */
#define USB_MAX_SECTORS 8u

static int usb_read(void *hw, uint64_t lba, uint32_t count, void *buf) {
    hype_blk_usb_t *u = (hype_blk_usb_t *)hw;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = (count - done > USB_MAX_SECTORS) ? USB_MAX_SECTORS : (count - done);
        if (hype_xhci_msc_read(u->ctrl, u->slot, &u->msc, (uint32_t)(lba + done), n,
                               u->block_size, p + (uint64_t)done * u->block_size) != 0) {
            return -1;
        }
        done += n;
    }
    return 0;
}

static int usb_write(void *hw, uint64_t lba, uint32_t count, const void *buf) {
    hype_blk_usb_t *u = (hype_blk_usb_t *)hw;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t n = (count - done > USB_MAX_SECTORS) ? USB_MAX_SECTORS : (count - done);
        if (hype_xhci_msc_write(u->ctrl, u->slot, &u->msc, (uint32_t)(lba + done), n,
                                u->block_size, p + (uint64_t)done * u->block_size) != 0) {
            return -1;
        }
        done += n;
    }
    return 0;
}

void hype_blk_usb_init(hype_blk_usb_t *hw, hype_blk_phys_t *p, hype_blk_backend_t *be,
                       hype_xhci_ctrl_t *ctrl, unsigned int slot,
                       const hype_xhci_msc_eps_t *msc, unsigned int block_size,
                       uint64_t total_sectors) {
    hw->ctrl = ctrl;
    hw->slot = slot;
    hw->msc = *msc;
    hw->block_size = block_size;
    hype_blk_phys_init(p, be, usb_read, usb_write, hw, total_sectors);
}
