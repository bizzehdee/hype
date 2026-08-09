#include "blk_usb.h"

/* hype_xhci_msc_read/_write bounce through a 4 KiB page, so at most 8 x 512B
 * sectors per xHCI call. blk_backend hands us larger runs; sub-chunk them. */
/* #365: 128 x 512B = 64 KiB, matching XPAGE, so a media fill is ONE SCSI command
 * instead of sixteen. See the XPAGE comment in core/xhci_hw.c. */
#define USB_MAX_SECTORS 128u

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
static volatile unsigned int g_usb_lock_holder_apic = 0xFFFFFFFFu; /* core inside the transfer */

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

/*
 * #363: the BSP must never block here forever.
 *
 * The operator's console -- dashboard, keyboard, terminal switching -- and hype's own logging all
 * run on the BSP, and all three stopped dead mid-run while the guests carried on. The BSP had
 * completed one last log flush and then gone silent: it was queued behind this lock, held by a
 * guest core that was no longer making progress.
 *
 * USBLOCK could not show this, and I closed #362 on the strength of it. Every counter below is
 * updated AFTER the spin loop exits, so a core that never acquires the lock is invisible to them
 * -- the statistics describe only the waiters that eventually succeeded. A permanent block is
 * exactly the case the instrument cannot report.
 *
 * So: guest media reads still wait indefinitely, because returning short data to a guest is not an
 * option. The BSP gets a bounded wait and fails instead. Everything the BSP does over USB is
 * optional relative to correctness -- a skipped log flush costs a few seconds of log, a skipped
 * HID poll costs 8 ms of key latency -- whereas a blocked BSP costs the operator the entire
 * machine, which is what happened.
 */
static void usb_xfer_lock(void);

#define USB_BSP_LOCK_BUDGET 20000000u

static volatile unsigned int g_usb_bsp_apic = 0xFFFFFFFFu;
static volatile unsigned long long g_usb_bsp_lock_timeouts;

void hype_blk_usb_set_bsp_apic(unsigned int apic_id) { g_usb_bsp_apic = apic_id; }

unsigned long long hype_blk_usb_bsp_lock_timeouts(void) { return g_usb_bsp_lock_timeouts; }

static int usb_xfer_lock_bounded(void) {
    unsigned int me = __atomic_fetch_add(&g_usb_ticket_next, 1u, __ATOMIC_ACQ_REL);
    unsigned long long spins = 0;
    unsigned int budget = USB_BSP_LOCK_BUDGET;
    while (__atomic_load_n(&g_usb_ticket_owner, __ATOMIC_ACQUIRE) != me) {
        if (budget-- == 0u) {
            /*
             * Give the ticket back by advancing the owner past it. Safe only because the holder
             * is the one that advances it on release and we are the next in line -- any waiter
             * behind us simply moves up. Without this the queue would be permanently gapped.
             */
            __atomic_fetch_add(&g_usb_ticket_owner, 1u, __ATOMIC_RELEASE);
            g_usb_bsp_lock_timeouts++;
            return -1;
        }
        __builtin_ia32_pause();
        spins++;
    }
    __atomic_store_n(&g_usb_lock_holder_apic, usb_xfer_this_apic(), __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_usb_lock_acquires, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_usb_lock_spins, spins, __ATOMIC_RELAXED);
    return 0;
}

static int usb_xfer_lock_or_fail(void) {
    if (usb_xfer_this_apic() == g_usb_bsp_apic) {
        return usb_xfer_lock_bounded();
    }
    usb_xfer_lock();
    return 0;
}

static void usb_xfer_lock(void) {
    unsigned int me = __atomic_fetch_add(&g_usb_ticket_next, 1u, __ATOMIC_ACQ_REL);
    unsigned long long spins = 0;
    while (__atomic_load_n(&g_usb_ticket_owner, __ATOMIC_ACQUIRE) != me) {
        __builtin_ia32_pause();
        spins++;
    }
    __atomic_store_n(&g_usb_lock_holder_apic, usb_xfer_this_apic(), __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_usb_lock_acquires, 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_usb_lock_spins, spins, __ATOMIC_RELAXED);
    if (spins > __atomic_load_n(&g_usb_lock_max_spins, __ATOMIC_RELAXED)) {
        __atomic_store_n(&g_usb_lock_max_spins, spins, __ATOMIC_RELAXED);
        __atomic_store_n(&g_usb_lock_max_spin_apic, usb_xfer_this_apic(), __ATOMIC_RELAXED);
    }
}

static void usb_xfer_unlock(void) {
    __atomic_store_n(&g_usb_lock_holder_apic, 0xFFFFFFFFu, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_usb_ticket_owner, 1u, __ATOMIC_RELEASE);
}

/*
 * #368: the live queue, not the run totals.
 *
 * The existing counters are cumulative, so they say the USB path is heavily contended over a
 * whole run but cannot say whether anything was contended AT the instant something else stalled.
 * (next - owner) is the number of cores queued right now, and holder is the core inside the
 * transfer. Sampled during the stall, those distinguish "everyone is waiting on USB" from
 * "the USB path is idle and the stall is something else entirely" -- which is exactly the
 * question the run totals cannot answer.
 */
void hype_blk_usb_queue_stats(unsigned int *waiters, unsigned int *holder_apic) {
    unsigned int next = __atomic_load_n(&g_usb_ticket_next, __ATOMIC_RELAXED);
    unsigned int owner = __atomic_load_n(&g_usb_ticket_owner, __ATOMIC_RELAXED);
    if (waiters != 0) *waiters = next - owner;
    if (holder_apic != 0) *holder_apic = __atomic_load_n(&g_usb_lock_holder_apic, __ATOMIC_RELAXED);
}

void hype_blk_usb_lock_stats(unsigned long long *acquires, unsigned long long *spins,
                             unsigned long long *max_spins, unsigned int *max_spin_apic) {
    if (acquires != 0) *acquires = __atomic_load_n(&g_usb_lock_acquires, __ATOMIC_RELAXED);
    if (spins != 0) *spins = __atomic_load_n(&g_usb_lock_spins, __ATOMIC_RELAXED);
    if (max_spins != 0) *max_spins = __atomic_load_n(&g_usb_lock_max_spins, __ATOMIC_RELAXED);
    if (max_spin_apic != 0)
        *max_spin_apic = __atomic_load_n(&g_usb_lock_max_spin_apic, __ATOMIC_RELAXED);
}

/*
 * #365: where does a USB read's time actually go?
 *
 * A 2 KB guest read costs 6.73 ms, of which only ~1% is wire time at USB 2.0 rates. The rest is
 * fixed per-transaction cost -- but "fixed cost" could be the device's own latency OR hype's
 * polling inside hype_xhci_msc_read(). Those call for completely different fixes: read-ahead
 * amortises device latency, while cutting poll overhead speeds up every path including the ones
 * read-ahead cannot help.
 *
 * So time the transfer itself, separately from the lock wait already counted above. Split by
 * whether the request was one chunk or several, because a multi-chunk read pays the per-chunk cost
 * repeatedly and that is exactly what a larger USB_MAX_SECTORS would change.
 */
static volatile unsigned long long g_usb_xfer_tsc;
static volatile unsigned long long g_usb_xfer_calls;
static volatile unsigned long long g_usb_xfer_chunks;
static volatile unsigned long long g_usb_xfer_sectors;
static volatile unsigned long long g_usb_xfer_max_tsc;

static inline unsigned long long usb_rdtsc(void) {
    unsigned int lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

void hype_blk_usb_xfer_stats(unsigned long long *tsc, unsigned long long *calls,
                             unsigned long long *chunks, unsigned long long *sectors,
                             unsigned long long *max_tsc) {
    if (tsc != 0) *tsc = g_usb_xfer_tsc;
    if (calls != 0) *calls = g_usb_xfer_calls;
    if (chunks != 0) *chunks = g_usb_xfer_chunks;
    if (sectors != 0) *sectors = g_usb_xfer_sectors;
    if (max_tsc != 0) *max_tsc = g_usb_xfer_max_tsc;
}

static int usb_read(void *hw, uint64_t lba, uint32_t count, void *buf) {
    hype_blk_usb_t *u = (hype_blk_usb_t *)hw;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    unsigned long long t0;
    if (usb_xfer_lock_or_fail() != 0) return -1;
    t0 = usb_rdtsc(); /* after the lock, so contention is not counted as device time */
    while (done < count) {
        uint32_t n = (count - done > USB_MAX_SECTORS) ? USB_MAX_SECTORS : (count - done);
        if (hype_xhci_msc_read(u->ctrl, u->slot, &u->msc, (uint32_t)(lba + done), n,
                               u->block_size, p + (uint64_t)done * u->block_size) != 0) {
            usb_xfer_unlock();
            return -1;
        }
        done += n;
        g_usb_xfer_chunks++;
    }
    {
        unsigned long long dt = usb_rdtsc() - t0;
        g_usb_xfer_tsc += dt;
        g_usb_xfer_calls++;
        g_usb_xfer_sectors += count;
        if (dt > g_usb_xfer_max_tsc) g_usb_xfer_max_tsc = dt;
    }
    usb_xfer_unlock();
    return 0;
}

static int usb_write(void *hw, uint64_t lba, uint32_t count, const void *buf) {
    hype_blk_usb_t *u = (hype_blk_usb_t *)hw;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t done = 0;
    /*
     * #363: bounded for the BSP here TOO, and this is the path that mattered.
     *
     * The previous attempt bounded only usb_read(), which was the wrong half: guests READ their
     * discs, while the BSP's dominant use of the controller is WRITING the log. So the BSP still
     * blocked indefinitely, bsp_usb_timeouts stayed 0, and the console still died -- the fix
     * measured as ineffective because it was applied to the path the BSP barely uses.
     */
    if (usb_xfer_lock_or_fail() != 0) return -1;
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
