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

/*
 * #362: USB transfer-lock contention counters. `max_spin_apic` names the core that waited longest,
 * which is what distinguishes "the BSP is being starved" from "everyone waits a bit".
 */
void hype_blk_usb_lock_stats(unsigned long long *acquires, unsigned long long *spins,
                             unsigned long long *max_spins, unsigned int *max_spin_apic);

/*
 * #365: time spent INSIDE USB transfers, excluding the lock wait. Distinguishes device latency
 * (fix: read-ahead) from hype's own polling (fix: the xHCI path itself) -- different problems with
 * different answers, and the per-command cost alone cannot tell them apart.
 */
void hype_blk_usb_xfer_stats(unsigned long long *tsc, unsigned long long *calls,
                             unsigned long long *chunks, unsigned long long *sectors,
                             unsigned long long *max_tsc);

/*
 * #363: tell blk_usb which core is the BSP, so its USB waits can be BOUNDED while guest media
 * reads stay blocking. A blocked BSP costs the operator the dashboard, the keyboard and the log
 * all at once; a skipped log flush or HID poll costs almost nothing.
 */
void hype_blk_usb_set_bsp_apic(unsigned int apic_id);

/* How many times the BSP gave up waiting. Non-zero means a guest core held the controller longer
 * than the budget -- the condition that used to hang the console silently. */
unsigned long long hype_blk_usb_bsp_lock_timeouts(void);

/*
 * #368: the LIVE queue depth and current holder, as opposed to the cumulative counters above.
 * `waiters` is how many cores are queued for the USB path right now; `holder_apic` is the core
 * inside the transfer, or 0xFFFFFFFF if nobody holds it. Sampled while something else is
 * stalling, these say whether the stall coincides with USB contention or not -- a question the
 * run totals cannot answer, because they are heavily contended over any whole run.
 */
void hype_blk_usb_queue_stats(unsigned int *waiters, unsigned int *holder_apic);

#endif /* HYPE_CORE_BLK_USB_H */