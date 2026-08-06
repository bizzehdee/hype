#include "blk_usb.h"

/* hype_xhci_msc_read/_write bounce through a 4 KiB page, so at most 8 x 512B
 * sectors per xHCI call. blk_backend hands us larger runs; sub-chunk them. */
#define USB_MAX_SECTORS 8u

/*
 * #346: ONE xHCI transfer at a time, across cores. The guest's media reads run on an AP (the
 * vCPU's core) while the BSP flushes \HYPEFULL.LOG through the SAME controller, ring and
 * MSC endpoints -- with no lock, two cores interleave TRBs/doorbells/completions and the ring
 * state corrupts. Observed on real hardware as BOTH symptoms at once, starting exactly at
 * guest dispatch: OVMF's boot-image reads return garbage ("failed to load Boot0002 -- Not
 * found" with byte-perfect media on disk) and every later log write dies silently. The scan
 * phase (BSP only) hammers the same primitive 50k+ times without fault -- concurrency, not
 * the transfer itself, is the variable. Same class as #229 (AP physical I/O) and #237/#338
 * (shared state, two cores); same cure as #338's logbuf: a test-and-set spinlock around the
 * whole synchronous command, held across the chunk loop so a logical transfer is atomic.
 */
static volatile int g_usb_xfer_lock;

static void usb_xfer_lock(void) {
    while (__atomic_exchange_n(&g_usb_xfer_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        __builtin_ia32_pause();
    }
}

static void usb_xfer_unlock(void) {
    __atomic_store_n(&g_usb_xfer_lock, 0, __ATOMIC_RELEASE);
}

static int usb_read(void *hw, uint64_t lba, uint32_t count, void *buf) {
    hype_blk_usb_t *u = (hype_blk_usb_t *)hw;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    usb_xfer_lock();
    while (done < count) {
        uint32_t n = (count - done > USB_MAX_SECTORS) ? USB_MAX_SECTORS : (count - done);
        if (hype_xhci_msc_read(u->ctrl, u->slot, &u->msc, (uint32_t)(lba + done), n,
                               u->block_size, p + (uint64_t)done * u->block_size) != 0) {
            usb_xfer_unlock();
            return -1;
        }
        done += n;
    }
    usb_xfer_unlock();
    return 0;
}

static int usb_write(void *hw, uint64_t lba, uint32_t count, const void *buf) {
    hype_blk_usb_t *u = (hype_blk_usb_t *)hw;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    usb_xfer_lock();
    while (done < count) {
        uint32_t n = (count - done > USB_MAX_SECTORS) ? USB_MAX_SECTORS : (count - done);
        if (hype_xhci_msc_write(u->ctrl, u->slot, &u->msc, (uint32_t)(lba + done), n,
                                u->block_size, p + (uint64_t)done * u->block_size) != 0) {
            usb_xfer_unlock();
            return -1;
        }
        done += n;
    }
    usb_xfer_unlock();
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
