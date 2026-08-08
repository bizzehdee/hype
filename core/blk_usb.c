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
/*
 * #362: a TICKET lock, not test-and-set.
 *
 * The original was a plain exchange-and-spin, which has no fairness. Once #360 let a second AP
 * start on the Intel box, three cores contended here: two APs streaming their guests' ISOs off the
 * USB stick, and the BSP flushing the log. Two APs issuing back-to-back reads can hold an unfair
 * lock effectively continuously, and the third contender may never win it. The BSP owns the
 * keyboard, the dashboard AND the log, so starving it takes all three away at once while the
 * guests keep running -- which is exactly what was observed: terminal switching dead, dashboard
 * timer frozen, log stopped, guest I/O still climbing.
 *
 * A ticket lock serves in arrival order, so every waiter is guaranteed to be served after at most
 * the waiters ahead of it. It costs one extra atomic per acquisition, against a USB transfer that
 * is orders of magnitude more expensive.
 */
static volatile unsigned int g_usb_ticket_next;   /* next ticket handed out */
static volatile unsigned int g_usb_ticket_owner;  /* ticket currently served */

/*
 * #362: measurement, kept in tree deliberately.
 *
 * The starvation above is a hypothesis until a counter says otherwise -- and on this project a
 * bounded trace has read as "the event never happened" often enough that counters are the rule.
 * These make the next run decisive whether or not the ticket lock fixes it: if the BSP's wait
 * diverges from the APs', that was the cause; if it does not, the freeze is somewhere else.
 */
static volatile unsigned long long g_usb_lock_acquires;
static volatile unsigned long long g_usb_lock_spins;      /* total spin iterations, all cores */
static volatile unsigned long long g_usb_lock_max_spins;  /* worst single acquisition */
static volatile unsigned int g_usb_lock_max_spin_apic;    /* which core suffered it */

static unsigned int usb_xfer_this_apic(void) {
    return (*(volatile uint32_t *)(uintptr_t)0xFEE00020u) >> 24;
}

static void usb_xfer_lock(void) {
    unsigned int me = __atomic_fetch_add(&g_usb_ticket_next, 1u, __ATOMIC_ACQ_REL);
    unsigned long long spins = 0;
    while (__atomic_load_n(&g_usb_ticket_owner, __ATOMIC_ACQUIRE) != me) {
        __builtin_ia32_pause();
        spins++;
    }
    __atomic_fetch_add(&g_usb_lock_acquires, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_usb_lock_spins, spins, __ATOMIC_RELAXED);
    if (spins > __atomic_load_n(&g_usb_lock_max_spins, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_usb_lock_max_spins, spins, __ATOMIC_RELAXED);
        __atomic_store_n(&g_usb_lock_max_spin_apic, usb_xfer_this_apic(), __ATOMIC_RELAXED);
    }
}

static void usb_xfer_unlock(void) {
    __atomic_fetch_add(&g_usb_ticket_owner, 1u, __ATOMIC_RELEASE);
}

void hype_blk_usb_lock_stats(unsigned long long *acquires, unsigned long long *spins,
                             unsigned long long *max_spins, unsigned int *max_spin_apic) {
    if (acquires != 0) *acquires = __atomic_load_n(&g_usb_lock_acquires, __ATOMIC_RELAXED);
    if (spins != 0) *spins = __atomic_load_n(&g_usb_lock_spins, __ATOMIC_RELAXED);
    if (max_spins != 0) *max_spins = __atomic_load_n(&g_usb_lock_max_spins, __ATOMIC_RELAXED);
    if (max_spin_apic != 0)
        *max_spin_apic = __atomic_load_n(&g_usb_lock_max_spin_apic, __ATOMIC_RELAXED);
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
