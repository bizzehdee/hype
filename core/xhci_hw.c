#include "xhci.h"
#include "int_in_queue.h"
#include "usb_msc.h"
#include "fatal.h" /* hype_debug_print -- hub-descent diagnostics (real HW visibility) */
#include "host_pci_dma.h" /* #426: shared ring-advance math (see ring_enqueue below) */

/*
 * Hardware shim for the xHCI host driver: real MMIO bring-up + port reset.
 * Coverage-exempt (like nvme_host_hw.c / ahci_host_hw.c) -- it pokes device
 * registers and spins on status bits. The pure register/TRB/ring model it
 * builds on lives in xhci.c and is unit-tested. Identity-mapped physical ==
 * pointer, per hype's flat map; all DMA-visible structures live in hype's .bss.
 * Post-ExitBootServices only.
 */

/*
 * #365: bytes per SCSI command.
 *
 * Was 4096. Bulk-Only Transport is CBW -> DATA -> CSW, three SEQUENTIAL dependent transfers, and
 * USB 2.0 schedules bulk traffic per 125 us microframe -- so one SCSI command costs at least three
 * microframes (375 us) no matter how small it is. Measured on hardware: 551 us per command, just
 * above that floor, with only ~62 us of actual wire time. The wait is the bus schedule, not hype's
 * polling, so polling faster gains nothing.
 *
 * The lever is bytes per command. At 4 KiB a 64 KiB media fill needed 16 commands (~8.8 ms); at
 * 64 KiB it needs one (~0.55 ms). A single xHCI Normal TRB carries up to 64 KiB (the TRB Transfer
 * Length field is 17 bits), so this stays one TRB per transfer and adds no ring complexity.
 *
 * Note this is a bounce BUFFER size, not a cache: nothing is retained between transfers, so it
 * introduces no state shared between guests -- deliberately, given #343.
 */
#define XPAGE 4096u

/*
 * #365: bytes per SCSI command -- the DATA bounce buffer only, NOT the rings.
 *
 * XPAGE stays a page: it sizes the DCBAA, the command/event rings and the scratchpad pages, none
 * of which want to be bigger (and scratch_pages[MAX_SCRATCH][XPAGE] would balloon).
 *
 * The data path is different. Bulk-Only Transport is CBW -> DATA -> CSW, three SEQUENTIAL
 * dependent transfers, and USB 2.0 schedules bulk traffic per 125 us microframe -- so one SCSI
 * command costs at least three microframes (375 us) however small it is. Measured on hardware:
 * 551 us per command, just above that floor, of which only ~62 us is wire time. The wait is the
 * bus schedule, not hype's polling, so polling faster gains nothing.
 *
 * The lever is bytes per command. At 4 KiB a 64 KiB media fill took 16 commands (~8.8 ms); at
 * 64 KiB it takes one (~0.55 ms). A single xHCI Normal TRB carries up to 64 KiB (TRB Transfer
 * Length is 17 bits), so this remains one TRB per transfer.
 *
 * A bounce BUFFER, not a cache: nothing is retained between transfers, so no state is shared
 * between guests. That is deliberate given #343.
 */
#define XDATA 65536u
/*
 * Every ring -- command, event, transfer -- lives in its own 4 KiB page, which is 256 TRBs.
 * Only 16 of them were ever used.
 *
 * That mattered most on the EVENT ring. xHCI 4.9.4: when the Event Ring fills, the xHC posts
 * an Event Ring Full Error and STOPS Command and Transfer Ring processing until software
 * advances ERDP. hype drains at the 125 Hz input tick, so the window between drains is 8 ms,
 * and with HYPE_INT_IN_DEPTH transfers outstanding on each of the seven interrupt-IN
 * endpoints the desktop polls, up to 28 completions can land in one window -- against 16
 * slots. (Not the 56 an events-per-millisecond estimate gives: an interrupt IN endpoint with
 * no data NAKs, and a NAK posts no event, so an idle keyboard produces nothing. The bound is
 * the queue depth, not the poll rate. 28 still exceeds 16.)
 *
 * At 256 the ring can no longer fill between drains, and that is a bound rather than a
 * hope: an endpoint with HYPE_INT_IN_DEPTH TRBs outstanding cannot produce a further event
 * until hype retires one and re-arms, so the whole controller can post at most
 * DEPTH * (interrupt-IN endpoints) transfer events per window -- 28 on the desktop -- plus
 * a port-status event per port and whatever synchronous command hype is waiting on. That
 * is why the 125 Hz drain cadence is left alone: it was only ever unsafe against 16.
 *
 * It also mattered on the TRANSFER rings, where a 16-TRB ring reuses each physical TRB
 * address every 15 submissions. TRB address is the only identity a completion carries, so a
 * short ring makes stale and current completions alias each other -- which is the ambiguity
 * bulk_xfer's parked_drop_exact exists to paper over. 256 pushes the reuse distance to 255.
 *
 * The memory was already allocated. This costs nothing.
 */
#define RING_TRBS 256u
#define MAX_SCRATCH 64u
#define DEVPOOL 8u
#define SPIN 20000000u
/* #759: one look at the event ring, for pollers rather than waiters. See int_in_poll. */
#define XHCI_POLL_PEEK 1u
/* #764: polls an interrupt-IN endpoint may sit armed and silent before hype re-arms it. */
#define HYPE_INT_IN_SILENT_MAX 4000u
/*
 * The silence-triggered revive is OFF by default.
 *
 * Boots 8-26 (no revive path) had zero command-ring wedges. Boots 27-39 (revive path present)
 * had ten in thirteen, and in every one the command that never completed was the revive's
 * Stop Endpoint -- the first line after each `#266 command TIMEOUT` is `REVIVE FAILED ...
 * Stop Endpoint did not complete`. Boot 38 wedged ctrl1, boot 39 ctrl2, so it is the command,
 * not a controller. 174 of the 264 revives issued targeted endpoints with zero reports so far:
 * hub status-change endpoints and an unused mouse endpoint, idle rather than deaf. Silence
 * cannot tell those apart, and the false positive is not free -- it is a Stop Endpoint, and
 * that is what stops the ring.
 *
 * The condition the revive guarded against (boot 18, a lost first completion) had its causes
 * fixed by #761/#766/#773, and HYPE_INT_IN_DEPTH outstanding transfers make a single lost
 * completion cost one report. Build with -DHYPE_INT_IN_SILENCE_REVIVE=1 to A/B the old
 * behaviour on hardware; the halted-endpoint recovery path is unaffected either way.
 */
#ifndef HYPE_INT_IN_SILENCE_REVIVE
#define HYPE_INT_IN_SILENCE_REVIVE 0
#endif

/*
 * #782 (decision 75): the command-ring wedge, ON DEMAND.
 *
 * The real wedge has shown up in three boots out of eleven, and every observation cost a
 * cold boot. -DHYPE_781_WEDGE_MS=<n> makes the first command submitted on controller
 * HYPE_781_WEDGE_CTRL (the enumeration log's 1-based number) more than n ms after its
 * bring-up behave exactly as boot 35's did: the TRB is enqueued, the doorbell is never rung,
 * no event arrives, the Command Abort is issued and CRR reads as still set for the whole
 * five-second bound. Every later command on that controller fails the same way until the
 * controller is reset, after which the injection is spent and the controller works -- which
 * is what lets the recovery be seen recovering. Default OFF: nothing below compiles in.
 */
#ifndef HYPE_781_WEDGE_MS
#define HYPE_781_WEDGE_MS 0u
#endif
#ifndef HYPE_781_WEDGE_CTRL
#define HYPE_781_WEDGE_CTRL 2u
#endif
/* #764 capture: how often to compare the controller's dequeue against ours while armed. */
/* #775: polls an endpoint may be armed with NOTHING ever reported before it is reset.
 * ~16 s at 125 Hz -- far longer than any device takes to say something for the first time. */
#define HYPE_INT_IN_DEAF_POLLS 2000u
/* #764: consecutive checks, all showing the same stuck pair, before it is reported. */
#define HYPE_INT_IN_DIVERGE_RUNS 8u

/* #266: one second, chosen against the measured distribution (mean 493,692 spins,
 * worst observed ~36,000,000 spins). Generous by design -- being early is what
 * caused the bug. */
#define HYPE_XHCI_EVENT_TIMEOUT_US 1000000u

/*
 * #299: every controller-owned DMA structure and ring cursor, in ONE block, with a small
 * fixed pool of them (HYPE_XHCI_MAX_CTRL, see xhci.h for why it is bounded and why these
 * cannot be shared).
 *
 * Note slot_dev/dev_used live here too: xHCI slot IDs are assigned PER CONTROLLER, so a
 * shared slot table would have controller B's slot 1 overwrite controller A's.
 */
typedef struct {
    int in_use;

    /* Controller-wide structures. */
    uint8_t dcbaa[XPAGE] __attribute__((aligned(XPAGE)));       /* device context base addr array */
    uint8_t cmd_ring[XPAGE] __attribute__((aligned(XPAGE)));    /* command ring */
    uint8_t evt_ring[XPAGE] __attribute__((aligned(XPAGE)));    /* event ring segment 0 */
    uint8_t erst[64] __attribute__((aligned(64)));              /* event ring segment table */
    uint8_t scratch_arr[XPAGE] __attribute__((aligned(XPAGE))); /* scratchpad buffer array */
    uint8_t scratch_pages[MAX_SCRATCH][XPAGE] __attribute__((aligned(XPAGE)));

    unsigned int cmd_enq; /* command-ring enqueue index */
    unsigned int cmd_cyc; /* command-ring producer cycle bit */
    unsigned int evt_deq; /* event-ring dequeue index */
    unsigned int evt_cyc; /* event-ring consumer cycle bit */
    /*
     * Host Controller Events, and how many of those said Event Ring Full. Non-zero
     * ring_full_events is direct proof the controller stopped rather than software losing
     * completions -- the one question no counter could previously answer.
     */
    unsigned long long hc_events;
    unsigned long long ring_full_events;
    uint32_t opreg; /* operational-register base, so USBSTS is readable where events are drained */
    /*
     * Command-ring health (boot 31, 2026-08-30).
     *
     * A command that never completes used to be a bare `return -1`: no abort, no register
     * read, no counter. On that run the ring stopped answering at t=9.9 min and hype went on
     * enqueuing into it for 74 minutes -- 614 timeouts, one per attempt, each blocking the
     * input tick for a full second. Nothing in the log said the ring itself had stopped,
     * because nothing looked.
     *
     * cmd_dead latches once recovery has been tried and failed the allowed number of times,
     * so the endless retry becomes one reported death instead of a silent grind.
     */
    unsigned long long cmd_timeouts;
    /*
     * Commands abandoned because the 64-event guard ran out before the completion arrived --
     * "things arrived, none of them ours", which #266 named as the case that needs a
     * different fix from "nothing arrived" and which then returned -1 with no message and no
     * counter. Boot 34 needed exactly this distinction: every interrupt-IN endpoint on one
     * controller went deaf, every revive failed, and cmd_timeouts stayed at 0.
     */
    unsigned long long cmd_guard_exhausted;
    unsigned long long cmd_recoveries;
    unsigned long long cmd_recovery_fails;
    int cmd_dead;
    /*
     * #785: set alongside cmd_dead wherever software has run out of moves -- the abort did
     * not clear CRR, or the controller raised HSE/HCE. The owner of this controller (boot/
     * main.c, which knows what is on it) reads it and decides whether a controller reset is
     * allowed; xhci_hw.c only knows that nothing short of one will help.
     */
    int reset_wanted;
    uint64_t up_tsc;        /* TSC at the end of the last bring-up: the #782 injection's clock */
    int wedged;             /* #782: the injected wedge is live on this controller */
    int wedge_spent;        /* #782: it has fired once; a reset makes the controller honest */

    /* #734: the interrupt-IN rings moved to the per-ENDPOINT pool below (g_iin_hw). One
     * set per controller could not carry a keyboard and a mouse at once -- see
     * HYPE_XHCI_INT_IN_MAX in core/xhci.h. */

    /* Device pool: hub descent (#231 pt5b) needs several devices addressed at once
     * (a hub plus the device behind it), so each addressed device owns its own
     * Device Context + EP0 ring + ring cursor, keyed by its slot id. The Input
     * Context is shared -- the controller only reads it transiently during Address
     * Device / Configure Endpoint. */
    uint8_t input_ctx[XPAGE] __attribute__((aligned(XPAGE)));
    uint8_t dev_ctx[DEVPOOL][XPAGE] __attribute__((aligned(XPAGE)));
    uint8_t ep0_ring[DEVPOOL][XPAGE] __attribute__((aligned(XPAGE)));
    uint8_t xfer_buf[XPAGE] __attribute__((aligned(XPAGE)));
    unsigned int ep0_enq[DEVPOOL];
    unsigned int ep0_cyc[DEVPOOL];
    unsigned int dev_used[DEVPOOL]; /* 1 if this pool slot is in use */
    unsigned int slot_dev[256];     /* slot id -> pool index + 1 (0 = none) */
    uint8_t slot_seen[256];         /* #734: slot id has been Enable-Slot'd at least once */
    /*
     * #744: root ports whose status changed since anyone last looked, one bit per port,
     * and a count of the events behind them. Set from the event-ring dequeue below, which
     * is the only place events leave the ring, so no caller can miss one by not looking.
     */
    uint32_t port_changed;
    unsigned long long port_events;

    /* #387: the MSC bulk rings + BOT state moved to the per-claimed-device pool below --
     * per-CONTROLLER they were the reason a second stick could not be brought up at all. */

    /*
     * #266 defect 1: completions that arrive for another endpoint are parked here rather
     * than discarded. See core/xhci.h for why discarding was the bug.
     *
     * #299: per-controller. Attribution was already safe when this was shared, because a
     * parked event can only be claimed by the exact (slot, dci, TRB pointer) it names and
     * the TRB pointer is unique across ring blocks. What was NOT safe is capacity: one
     * controller's late events could fill a fixed table and starve the other's, which on
     * the controller carrying hype's log is a stall in the datapath the log depends on.
     */
    hype_xhci_parked_t parked;
    /* #516: the last failed command's sense (0xFF = none captured), and whether this device
     * rejected SYNCHRONIZE CACHE as an unknown opcode -- cacheless flash sticks legitimately
     * do (the 64 GB Cruzer answers key=5 asc=0x20), and the sync is then a no-op forever,
     * exactly how Linux's sd driver treats it. */
    unsigned int last_sense_key;
    unsigned int last_sense_asc;
    int sync_cache_unsupported;
} xhci_hw_t;

static xhci_hw_t g_hw[HYPE_XHCI_MAX_CTRL];

/*
 * #387 (plan.md §10 decision 31): per-CLAIMED-DEVICE bulk transfer state. The bulk ring pair and
 * BOT wrappers used to live in xhci_hw_t -- one set per controller -- which made "bring up a
 * second stick" structurally impossible: configuring its endpoints would have re-pointed the
 * first stick's rings out from under the log sink. Each claimed MSC now owns its own rings,
 * wrappers and bounce; the controller-wide TRANSFER LOCK is unchanged, deliberately -- one
 * transfer at a time per controller is the concurrency contract the #343/#377 corruption work
 * proved. Per-device rings remove the bring-up limit, not the serialisation.
 */
typedef struct {
    int used;
    unsigned int ctrl; /* index into g_hw */
    unsigned int slot;
    uint8_t bulk_in_ring[XPAGE] __attribute__((aligned(XPAGE)));
    uint8_t bulk_out_ring[XPAGE] __attribute__((aligned(XPAGE)));
    unsigned int bin_enq, bin_cyc, bout_enq, bout_cyc;
    uint8_t cbw[64] __attribute__((aligned(64)));
    uint8_t csw[64] __attribute__((aligned(64)));
    uint8_t data[XDATA] __attribute__((aligned(4096)));
    uint32_t bot_tag;
} xhci_msc_hw_t;

static xhci_msc_hw_t g_msc_hw[HYPE_XHCI_MSC_MAX];

/*
 * #734: per-INTERRUPT-IN-ENDPOINT transfer state -- one block per endpoint hype polls for
 * itself, keyed by (controller, slot, dci) in g_iin_key.
 *
 * Kept out of xhci_hw_t for the same reason the MSC rings were: an endpoint's ring,
 * report buffer and armed flag are per-endpoint state, and sharing one set per controller
 * meant configuring the second HID re-pointed the first one's ring and, worse, the single
 * armed flag serialised two independent pollers into one -- an idle device holding it
 * silenced the other endpoint permanently.
 *
 * An interrupt endpoint still needs exactly ONE transfer queued at a time, re-armed after
 * each completion; `armed` is that, now per endpoint. Enqueuing on every poll call --
 * which the dispatch loop makes thousands of times a second -- fills the ring in a
 * fraction of a second and the device goes silent (the original #217 QEMU finding).
 */
/*
 * #775: how many transfers this endpoint keeps outstanding at once.
 *
 * ONE was the whole problem. An interrupt endpoint is re-armed when a completion is claimed,
 * so if that completion never arrives there is nothing queued behind it: `armed` stays set
 * and the endpoint is deaf for the rest of the boot. Boot 18 lost a keyboard that way --
 * polls=5779, reports=0, errors=0, on a context byte-identical to the boot where it worked.
 *
 * With several queued, a lost completion costs ONE report and the next transfer carries on.
 * That is what real host drivers do, and it needs no detector -- which matters, because
 * every attempt to DETECT this has misfired: hype cannot read the controller's dequeue
 * pointer on a running endpoint (#764), and "has never reported" is indistinguishable from
 * an idle mouse (the first cut of #775 reset one).
 *
 * Each outstanding transfer needs its own buffer, or two completions land on top of each
 * other.
 */
typedef struct {
    uint8_t ring[XPAGE] __attribute__((aligned(XPAGE)));
    uint8_t report[HYPE_INT_IN_DEPTH][64] __attribute__((aligned(64)));
    unsigned int enq;
    unsigned int cyc;
    unsigned int mps;        /* the endpoint's wMaxPacketSize -- the TRB length to arm */
    unsigned int recoveries; /* consecutive halt recoveries; reset by a good report */
    unsigned int backoff;    /* polls to skip before the next recovery attempt */
    /*
     * Transfer and report-buffer ownership, in core/int_in_queue.c.
     *
     * It lives outside this file because this file is excluded from the unit-test build
     * (core/tests/run.sh skips *_hw.c), and that is precisely why two ownership bugs got
     * this far: a single completion slot against four outstanding transfers, and then a
     * report buffer handed back to the controller while a queued completion still held it.
     * Neither was reachable from a test. Both are now.
     */
    hype_iiq_t q;
    /*
     * #764: polls this endpoint has been armed without a completion.
     *
     * `armed` is a latch: it clears only when a completion is claimed, so ANY completion
     * that goes missing leaves the endpoint armed forever and it is never re-armed. #761
     * fixed the known way to lose one (three event waits that dropped foreign transfer
     * events), but "the endpoint is now permanently deaf" is too severe a consequence to
     * leave resting on having found every such path.
     *
     * Boot 11 showed the shape again on real hardware -- the keyboard sat at reports=8
     * while polls climbed past 9,000, with errors=0. So: re-arm after a long silence, and
     * COUNT it, so a recovered endpoint is visibly different from one that never broke.
     */
    unsigned int silent_polls;
    unsigned long long rearms;
    /*
     * The revive trigger. `revive_after` starts at HYPE_INT_IN_SILENT_MAX and DOUBLES each
     * time a revive fails to bring the endpoint back, so a keyboard nobody is touching
     * settles into revives that are rare rather than one every thirty seconds for ever,
     * while a genuinely deaf one is caught promptly the first time.
     */
    unsigned int revive_after;
    unsigned long long revives;
    /*
     * Revives that were ATTEMPTED and failed. Boot 31 is the reason this exists: an endpoint
     * deaf for 74 minutes reported revives=0 against 416,531 polls, which reads as "the
     * revive never fired". It fired every 4,000 polls the whole time -- int_in_revive()
     * returned -1 because its Stop Endpoint command timed out, and that path incremented
     * nothing and printed nothing. A recovery that cannot succeed must still be visible.
     */
    unsigned long long revive_fails;
    /*
     * Transfers retired with a STOPPED completion code -- the completion a Stop Endpoint
     * generates for whatever was still outstanding. Expected, not an error; counted because
     * hype used to treat these as transfer failures and "recover" a stopped endpoint.
     */
    unsigned long long stopped_ccs;
    /*
     * #764 capture: what this endpoint was doing when it diverged.
     *
     * The existing report fires after HYPE_INT_IN_SILENT_MAX polls, which on a 125 Hz tick
     * is half a minute after the fact -- long enough that the ring state it prints is no
     * longer the state that went wrong. These record the last few completions actually
     * CLAIMED, so the moment of divergence can be read against what led to it.
     */
    uint64_t claim_trb[8];
    uint8_t claim_cc[8];
    unsigned int claim_n;
    int diverged;            /* reported once; the first sighting is the useful one */
    unsigned long long reports;
    /*
     * #764: where this endpoint's completions went. A completion can reach its owner three
     * ways and be lost one, and only counting them apart says which happened:
     *   deliv      handed straight to this block by another poll (#761)
     *   own        dequeued by this endpoint's own poll
     *   to_park    seen for this endpoint but NOT matching its outstanding TRB, so parked --
     *              and the parked table holds 8 entries and evicts round-robin, so this is
     *              the count that can turn into a permanently deaf endpoint
     */
    unsigned long long deliv, own, to_park;
    /*
     * #764: handed INTO this block by another poll, counted at the moment of handover.
     * `deliv` counts handovers the owner went on to CLAIM. The difference between the two
     * is a completion that arrived, was filed correctly, and was then never picked up --
     * which no other counter can distinguish from one that never arrived at all.
     */
    unsigned long long handed;
    /*
     * #775: how many times this endpoint has been armed, and whether it has EVER reported.
     *
     * An interrupt endpoint carries one transfer at a time, so if its first completion goes
     * missing the endpoint is armed for ever and deaf for the rest of the boot. Boot 18 had
     * exactly that: polls=5779, reports=0, errors=0, on an endpoint whose context was
     * byte-identical to the boot where the same keyboard worked.
     *
     * "Never reported at all" is unambiguous in a way that silence is not -- a quiet keyboard
     * and a dead one look the same for a few seconds, but not after thousands of polls with
     * nothing ever having arrived.
     */
    unsigned long long arms;
    unsigned int never_recovered;
    /* #764: a divergence only counts if it PERSISTS. See the check in the poll. */
    uint64_t last_deq;
    unsigned int diverge_run;
} xhci_int_in_hw_t;

static xhci_int_in_hw_t g_iin_hw[HYPE_XHCI_INT_IN_MAX];
static hype_xhci_int_in_key_t g_iin_key[HYPE_XHCI_INT_IN_MAX];

/* The block for (c, slot, dci); claims one on first sight when `alloc` is set. */
static xhci_int_in_hw_t *iin_hw_for(const hype_xhci_ctrl_t *c, unsigned int slot,
                                    unsigned int dci, int alloc) {
    int i = hype_xhci_int_in_index(g_iin_key, HYPE_XHCI_INT_IN_MAX, c->hw_slot, slot, dci,
                                   alloc);
    return (i < 0) ? (xhci_int_in_hw_t *)0 : &g_iin_hw[i];
}

/* The claimed-device block for (c, slot); allocates on first sight when `alloc` is set. */
static xhci_msc_hw_t *msc_hw_for(const hype_xhci_ctrl_t *c, unsigned int slot, int alloc) {
    unsigned int i, free_i = HYPE_XHCI_MSC_MAX;
    for (i = 0; i < HYPE_XHCI_MSC_MAX; i++) {
        if (g_msc_hw[i].used && g_msc_hw[i].ctrl == c->hw_slot && g_msc_hw[i].slot == slot) {
            return &g_msc_hw[i];
        }
        if (!g_msc_hw[i].used && free_i == HYPE_XHCI_MSC_MAX) {
            free_i = i;
        }
    }
    if (!alloc || free_i == HYPE_XHCI_MSC_MAX) {
        return 0;
    }
    g_msc_hw[free_i].used = 1;
    g_msc_hw[free_i].ctrl = c->hw_slot;
    g_msc_hw[free_i].slot = slot;
    g_msc_hw[free_i].bot_tag = 0;
    return &g_msc_hw[free_i];
}

/* This controller's block. hw_slot is set by hype_xhci_host_init() and is only
 * meaningful while c->inited; it is clamped so a caller that hands over an
 * uninitialised hype_xhci_ctrl_t cannot index outside the pool. */
static xhci_hw_t *HW(const hype_xhci_ctrl_t *c) {
    unsigned int i = c->hw_slot;
    if (i >= HYPE_XHCI_MAX_CTRL) {
        i = 0;
    }
    return &g_hw[i];
}

static int dev_alloc(xhci_hw_t *hw, unsigned int slot) {
    unsigned int i;
    for (i = 0; i < DEVPOOL; i++) {
        if (!hw->dev_used[i]) {
            hw->dev_used[i] = 1;
            hw->slot_dev[slot & 0xFFu] = i + 1u;
            return (int)i;
        }
    }
    return -1;
}
static int dev_index(const xhci_hw_t *hw, unsigned int slot) {
    unsigned int v = hw->slot_dev[slot & 0xFFu];
    return v ? (int)(v - 1u) : -1;
}
static void dev_free(xhci_hw_t *hw, unsigned int slot) {
    int i = dev_index(hw, slot);
    if (i >= 0) hw->dev_used[i] = 0;
    hw->slot_dev[slot & 0xFFu] = 0;
}

static inline uint8_t  rd8(volatile uint8_t *b, uint32_t o)  { return *(volatile uint8_t *)(b + o); }
static inline uint32_t rd32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void     wr32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void     wr64(volatile uint8_t *b, uint32_t o, uint64_t v) { *(volatile uint64_t *)(b + o) = v; }
static uint64_t phys(const void *p) { return (uint64_t)(uintptr_t)p; }

static void zero(uint8_t *p, unsigned n) { unsigned i; for (i = 0; i < n; i++) p[i] = 0; }

static void put_le64(uint8_t *p, uint64_t v) {
    unsigned i;
    for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put_le32(uint8_t *p, uint32_t v) {
    unsigned i;
    for (i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* Bounded busy-wait: spin until (reg & mask) == want, else -1. */
static int wait_bits(volatile uint8_t *bar, uint32_t off, uint32_t mask, uint32_t want) {
    unsigned s = SPIN;
    while (s-- != 0u) {
        if ((rd32(bar, off) & mask) == want) return 0;
    }
    return -1;
}
static void short_delay(void) { volatile unsigned s = 200000u; while (s-- != 0u) { } }

/* Real-time millisecond delay for USB timing (post-reset settle, the 2 ms
 * SET_ADDRESS recovery, etc.). Uses the host TSC when its frequency has been
 * provided (hype_xhci_set_tsc_hz, called from boot once TSC is calibrated);
 * falls back to a coarse busy-spin otherwise. USB timing only needs a floor, so
 * over-delaying is harmless -- correctness over precision. */
static uint64_t g_tsc_hz;
void hype_xhci_set_tsc_hz(uint64_t hz) { g_tsc_hz = hz; }

/*
 * Connect-detect settle after a host-controller reset, and the per-port budget
 * for waiting on CCS. USB 2.0's attach debounce (TATTDB) is 100 ms and a
 * SuperSpeed port adds link training, so 150 ms is the floor with margin. Any
 * port that still reports nothing after its own 150 ms window genuinely has
 * nothing attached. Applies to every controller equally -- where a device is
 * plugged in must not matter, and a reset blinds every port the same way.
 */
#define HYPE_XHCI_CONNECT_DEBOUNCE_MS 150u
#define HYPE_XHCI_PORT_CCS_WAIT_MS 150u
static inline uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
static void delay_ms(unsigned int ms) {
    if (g_tsc_hz != 0u) {
        uint64_t end = rdtsc_now() + (g_tsc_hz / 1000ull) * (uint64_t)ms;
        while (rdtsc_now() < end) { }
    } else {
        unsigned int k;
        for (k = 0; k < ms; k++) short_delay(); /* coarse fallback */
    }
}

/* #266: foreign transfer events discarded, cumulative. Aggregate across controllers on
 * purpose -- it is a diagnostic counter, not per-controller state. */
static unsigned int g_bulk_foreign_seen;

static void ring_state_reset(xhci_hw_t *hw) {
    hw->cmd_enq = 0; hw->cmd_cyc = 1; hw->evt_deq = 0; hw->evt_cyc = 1;
}

/* Volatile read of dword `dw` of TRB `idx` in a ring (controller DMAs into it). */
static uint32_t trb_dw(const uint8_t *ring, unsigned int idx, unsigned int dw) {
    return *(volatile uint32_t *)(ring + idx * HYPE_XHCI_TRB_BYTES + dw * 4u);
}

/* Enqueue a fully-built TRB (cycle already = *cyc) onto any producer ring,
 * handling the Link-TRB wrap + producer-cycle toggle.
 *
 * #426: the wrap/cycle-toggle DECISION now goes through core/host_pci_dma.c's
 * hype_dma_link_ring_advance() -- the same index arithmetic this function
 * always did (`*enq == RING_TRBS - 1u` after incrementing is exactly
 * `old_enq + 1 >= capacity - 1` for capacity == RING_TRBS), just shared with
 * other producer rings instead of re-derived per driver. The Link-TRB MEMORY
 * WRITE itself stays here: it is real MMIO-visible DMA memory, ring-specific
 * (needs `ring`'s own base address for the link target), and out of scope for
 * a pure-logic helper. Nothing about when the wrap happens or which cycle
 * value the Link TRB is stamped with changes. */
static void ring_enqueue(uint8_t *ring, unsigned int *enq, unsigned int *cyc,
                         const uint32_t trb[4]) {
    uint8_t *slot = ring + (*enq) * HYPE_XHCI_TRB_BYTES;
    uint32_t cyc_before_wrap = *cyc; /* the Link TRB must match the cycle the wrap is leaving */
    put_le32(slot + 0, trb[0]);
    put_le32(slot + 4, trb[1]);
    put_le32(slot + 8, trb[2]);
    put_le32(slot + 12, trb[3]);
    hype_dma_link_ring_advance(enq, cyc, RING_TRBS);
    if (*enq == 0u) { /* wrapped: the advance reset us to slot 0, install the Link TRB it skipped */
        uint32_t link[4];
        hype_xhci_trb_link(link, phys(ring), cyc_before_wrap);
        put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
        put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
        put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
        put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
    }
}

static void cmd_enqueue(xhci_hw_t *hw, const uint32_t trb[4]) {
    ring_enqueue(hw->cmd_ring, &hw->cmd_enq, &hw->cmd_cyc, trb);
}

/* Poll the event ring for the next valid event (cycle == consumer cycle),
 * copy it to out[4], advance the dequeue pointer + ERDP. -1 on timeout. */
/*
 * #266 measurement. The failures on this controller all report "no event arrived":
 * next_event polls its whole budget and the cycle bit never flips. That is a stall,
 * not a mis-ordering -- but "the completion is very late" and "the completion never
 * comes" demand completely different fixes (waiting strategy vs a missed doorbell or
 * ERDP/interrupter bug), and the two are indistinguishable from a fixed-budget poll.
 *
 * So: the budget is a parameter, and how many spins each event actually took is
 * recorded. A caller that times out can then re-poll with a far larger budget and
 * settle the question, and the normal-case statistics say how far the outliers
 * really are from typical.
 */
static unsigned int g_evt_spin_max;      /* worst spin count for an event that DID arrive */
static unsigned long long g_evt_spin_sum; /* for a mean; unsigned long long: SPIN is 2e7 */
static unsigned long long g_evt_count;

/*
 * #266 RESOLVED by measurement. A 35-minute real-hardware run answered it: of every
 * bulk timeout, 4 of 4 were events that DID arrive (all cc=1, i.e. the transfers had
 * SUCCEEDED) and 0 were genuinely absent. So nine BOT recoveries were triggered on
 * transfers that were about to complete, and the datapath was being torn down for no
 * reason.
 *
 * The numbers say why. Mean arrival was 493,692 spins over 8,215 events; the late
 * ones needed up to 15,950,630 spins ON TOP of the 20,000,000 budget -- about 36M
 * total, roughly 70x the mean. The budget sat squarely in the middle of the tail.
 *
 * A spin count was the wrong unit to begin with: it means different amounts of time
 * on different CPUs, at different frequencies, with different cache behaviour, so it
 * can be generous on one machine and far too short on another. The budget is now a
 * real deadline in microseconds, with the spin count kept only as a fallback for
 * before the TSC frequency is known.
 */
/*
 * #808: called periodically while this loop spins waiting for a completion.
 *
 * HYPE_XHCI_EVENT_TIMEOUT_US is ONE SECOND, deliberately generous (#266: "being early is what
 * caused the bug"). A completion that never arrives therefore holds this loop for up to a second
 * -- and boot AMD-L0 run 13 measured exactly that: gap_recent=1237713us, a 1.24 s window with no
 * keyboard drain, while the i8042 buffers ONE byte.
 *
 * This is where the yield has to be, and run 13 is what proved it. The previous attempt put it in
 * core/blk_usb.c's chunk loop, one layer up. It ran -- pumps=5019 on that same run -- and did not
 * help, because the time is spent INSIDE one hype_xhci_msc_write() call, below that loop. A
 * counter for "did the fix run" is what made the difference between "does not help" and "was
 * never wired", which are opposite findings.
 *
 * Injected: this is core/ and the keyboard is arch/x86_64/cpu/. Called from a spin that may hold
 * the USB transfer lock, so the callback must not touch USB -- the keyboard pump touches ports
 * 0x60/0x64 only.
 */
static void (*g_xhci_spin_yield)(void);

void hype_xhci_set_spin_yield(void (*yield)(void)) { g_xhci_spin_yield = yield; }

static int next_event_budget(xhci_hw_t *hw, volatile uint8_t *bar, uint32_t rtsoff,
                             uint32_t out[4], unsigned int budget, unsigned int *spins_used) {
    unsigned int spins = budget;
    unsigned int yield_ctr = 0u;
    while (spins-- != 0u) {
        /*
         * Not every spin: this loop's measured mean is ~493,692 iterations (#266) and the
         * callback reads an I/O port, so calling it per spin would replace one stall with
         * another. Every 4,096 keeps the drain well inside the i8042's ~1 ms byte time while
         * adding nothing measurable to a loop that usually exits early.
         */
        if ((++yield_ctr & 0xFFFu) == 0u && g_xhci_spin_yield != 0) {
            g_xhci_spin_yield();
        }
        uint32_t d3 = trb_dw(hw->evt_ring, hw->evt_deq, 3);
        if ((int)(d3 & 1u) == (int)hw->evt_cyc) {
            out[0] = trb_dw(hw->evt_ring, hw->evt_deq, 0);
            out[1] = trb_dw(hw->evt_ring, hw->evt_deq, 1);
            out[2] = trb_dw(hw->evt_ring, hw->evt_deq, 2);
            out[3] = d3;
            hype_dma_cqueue_advance(&hw->evt_deq, &hw->evt_cyc, RING_TRBS);
            /* ERDP = address of the new dequeue slot, with EHB (bit3) written 1 to clear. */
            wr64(bar, hype_xhci_ir0_offset(rtsoff, HYPE_XHCI_IR_ERDP),
                 (phys(hw->evt_ring) + (uint64_t)hw->evt_deq * HYPE_XHCI_TRB_BYTES) | (1u << 3));
            /*
             * A Host Controller Event is the controller telling software it has stopped.
             * xHCI 4.9.4: when the Event Ring fills, the xHC posts Event Ring Full Error
             * and halts Command and Transfer Ring processing until ERDP advances -- so
             * every endpoint goes silent at once, with no error on any transfer, which is
             * indistinguishable from the software-side loss this release fixes unless it
             * is named. hype had no handler and no counter for it.
             */
            if (hype_xhci_trb_type(out) == HYPE_XHCI_TRB_HOST_CONTROLLER) {
                unsigned int hc_cc = hype_xhci_event_cc(out);
                hw->hc_events++;
                if (hc_cc == HYPE_XHCI_CC_EVENT_RING_FULL) hw->ring_full_events++;
                if (hw->hc_events <= 8ull) {
                    hype_debug_print("host-xhci: HOST CONTROLLER EVENT cc=%u%s -- deq=%u "
                                     "cyc=%u usbsts=0x%08x. The controller stopped; this is "
                                     "not a transfer failure\n",
                                     hc_cc,
                                     (hc_cc == HYPE_XHCI_CC_EVENT_RING_FULL)
                                         ? " (EVENT RING FULL)" : "",
                                     hw->evt_deq, hw->evt_cyc,
                                     rd32(bar, hw->opreg + HYPE_XHCI_OP_USBSTS));
                }
            }
            /*
             * #744: a Port Status Change Event is recorded HERE and not handed back.
             *
             * This is the only place an event leaves the ring, so recording it here is the
             * only way no caller can miss one by not looking -- and every caller that does
             * look is waiting for a transfer or a command completion and has no idea what
             * to do with a port event. cmd_submit_wait() documented them as noise to be
             * skipped, spending its 64-event guard to do it; now they never reach it.
             *
             * DW0 bits 31:24 are the Port ID, 1-based. The bitmap covers ports 1..31, which
             * is every port an xHCI can have on one controller in practice; a port number
             * outside that is counted but not flagged, rather than shifting out of range.
             */
            if (((d3 >> 10) & 0x3Fu) == (uint32_t)HYPE_XHCI_TRB_PORT_STATUS) {
                unsigned int pid = (out[0] >> 24) & 0xFFu;
                hw->port_events++;
                /*
                 * #760: say so HERE, where it happens.
                 *
                 * Boot 9 could not answer "did the controller even raise an event when the
                 * operator re-plugged", because the only place that number appeared was a
                 * DIAG line behind a 30-second gate, and it printed once -- before the
                 * operator touched anything. A hot-plug is a rare, operator-driven event;
                 * one line each is not noise, and its ABSENCE is the finding.
                 */
                hype_debug_print("host-xhci: PORT EVENT port=%u (event #%llu on this "
                                 "controller) [#760]\n", pid,
                                 (unsigned long long)hw->port_events);
                if (pid >= 1u && pid <= 31u) {
                    hw->port_changed |= (1u << pid);
                }
                continue; /* consumed: keep waiting for what the caller actually asked for */
            }
            {
                unsigned int used = budget - spins - 1u;
                if (spins_used != 0) *spins_used = used;
                if (used > g_evt_spin_max) g_evt_spin_max = used;
                g_evt_spin_sum += (unsigned long long)used;
                g_evt_count++;
            }
            return 0;
        }
    }
    if (spins_used != 0) *spins_used = budget;
    return -1;
}

/*
 * Wait up to `timeout_us` of real time. Falls back to the spin budget when the TSC
 * frequency has not been supplied yet (early bring-up), which is the same
 * correctness-over-precision trade delay_ms() above already makes.
 */
static int next_event_timed(xhci_hw_t *hw, volatile uint8_t *bar, uint32_t rtsoff,
                            uint32_t out[4], unsigned int timeout_us, unsigned int *spins_used) {
    uint64_t end;

    if (g_tsc_hz == 0u) {
        return next_event_budget(hw, bar, rtsoff, out, SPIN, spins_used);
    }
    end = rdtsc_now() + (g_tsc_hz / 1000000ull) * (uint64_t)timeout_us;
    for (;;) {
        if (next_event_budget(hw, bar, rtsoff, out, SPIN / 64u, spins_used) == 0) {
            return 0;
        }
        if (rdtsc_now() >= end) {
            return -1;
        }
    }
}

static int next_event(xhci_hw_t *hw, volatile uint8_t *bar, uint32_t rtsoff, uint32_t out[4]) {
    /* One second. Far beyond any healthy completion -- the measured mean is under a
     * millisecond of equivalent work -- and still bounded, so a genuinely dead device
     * still fails rather than hanging hype. */
    return next_event_timed(hw, bar, rtsoff, out, HYPE_XHCI_EVENT_TIMEOUT_US, 0);
}

/*
 * Enqueue a command TRB, ring the command doorbell (DB[0]), and consume events
 * until a Command Completion Event.
 *
 * #254 v2 -- DELIBERATELY LENIENT, and this is measured, not stylistic. v1
 * required the completion's TRB pointer to equal the command we just enqueued.
 * That broke enumeration on real hardware: this controller (1022:15e0) loses
 * Address Device completions on EVERY boot, and the pre-#254 code only worked
 * because try 2's wait consumed try 1's LATE completion. Strict matching
 * discarded it, so both tries timed out, no USB disk was found, and the log
 * sink never opened -- 4 builds without the strict check produced a log on this
 * laptop, 2 with it produced none.
 *
 * Commands are strictly serialised here (one outstanding at a time) and none of
 * them move data to the medium, so accepting a late completion costs at most a
 * mis-attributed status -- whereas the corruption this ticket exists to fix came
 * from the BULK path, which stays strict and now has BOT reset recovery. A
 * mismatched pointer is logged so the behaviour stays visible.
 */
/*
 * #761: route a Transfer Event that is not the one this caller is waiting for.
 *
 * An interrupt-IN endpoint owns its block, so its completion has a home; anything else
 * goes to the parked table for the MSC datapath (#266). What must NEVER happen is the
 * third option -- dropping it. `armed` clears only on a CLAIMED completion, so a dropped
 * int-in completion leaves the endpoint armed forever, never re-armed, and permanently
 * deaf. One lost event is not one lost report.
 */
static int int_in_deliver(xhci_int_in_hw_t *iin, uint64_t trb, uint32_t cc,
                          unsigned int residue);

static void route_foreign_event(hype_xhci_ctrl_t *c, xhci_hw_t *hw, const uint32_t evt[4]) {
    unsigned int oslot = hype_xhci_event_slot_id(evt);
    unsigned int odci = hype_xhci_event_ep_id(evt);
    xhci_int_in_hw_t *other;

    if (hype_xhci_trb_type(evt) != HYPE_XHCI_TRB_TRANSFER_EVENT) {
        return; /* command completions and the rest are the caller's business */
    }
    other = iin_hw_for(c, oslot, odci, 0);
    /*
     * #766: the TRB pointer must match too, not just (slot, dci).
     *
     * The parked table this replaced matched on all three and said why: "the parked event
     * can only ever be claimed by the exact (slot, dci, trb) it names". Dropping the TRB
     * check let a stale or duplicate event for the same endpoint be claimed as the CURRENT
     * transfer -- `armed` clears, hype re-arms and advances `enq`, and the controller never
     * consumed the TRB that was actually outstanding.
     *
     * The pointers then drift apart. Measured on hardware: four endpoints with the
     * controller's dequeue sitting at the RING BASE while hype had enqueued ten TRBs past
     * it, all four permanently silent -- the controller waiting on a TRB whose cycle bit
     * says not-ready, hype waiting on a completion that can never come. Every interrupt-IN
     * endpoint on that controller went deaf at once, which is why re-plugging the keyboard
     * three times changed nothing: hype could not see the unplug either.
     */
    if (other != (xhci_int_in_hw_t *)0) {
        /*
         * Retire against it HERE. An interrupt-IN endpoint owns its completions outright,
         * so none of them has any business in the shared parked table -- that table evicts
         * round-robin, and an evicted int-in completion leaves the endpoint armed for ever
         * and permanently deaf. int_in_deliver retires the matching transfer immediately
         * and queues its report, so the head is never stale when the next event arrives.
         */
        if (int_in_deliver(other, hype_xhci_event_trb_ptr(evt), hype_xhci_event_cc(evt),
                           hype_xhci_event_xfer_residue(evt))) {
            other->handed++;
            return;
        }
        /*
         * Matched nothing this endpoint holds outstanding: a stale or duplicate event for a
         * TRB address the ring has since reused. Dropping it is correct (#766 -- letting it
         * stand in for the current transfer is what drove the enqueue pointer ten TRBs past
         * the controller's dequeue), and it is counted in `lost` by int_in_deliver.
         * Parking it would only expose the MSC datapath's table to eviction for nothing.
         */
        other->to_park++;
        return;
    }
    hype_xhci_parked_put(&hw->parked, oslot, odci, hype_xhci_event_trb_ptr(evt),
                         hype_xhci_event_cc(evt), hype_xhci_event_xfer_residue(evt));
}

/*
 * Command-ring recovery (boot 31, 2026-08-30).
 *
 * A command TRB that never completes leaves the ring in a state software cannot reason
 * about: the controller may still be chewing on it, or may have stopped the ring entirely.
 * Enqueuing the next command behind it and ringing the doorbell again -- which is what hype
 * did -- cannot work in either case, and it is not harmless: every attempt costs a full
 * one-second event wait on the input tick. Boot 31 spent 614 of them, 74 minutes, with every
 * interrupt-IN endpoint on the controller deaf because reviving one needs two commands.
 *
 * xHCI 4.6.1.2 gives the recovery: set CRCR.CA to abort the command in flight, wait for
 * CRCR.CRR to clear, then re-point the ring (the pointer field is only writable while CRR is
 * clear) and start again from its base.
 *
 * USBSTS is read FIRST because it decides whether recovery is even the right move. HSE or
 * HCE means the controller itself has failed, and no amount of command-ring surgery brings
 * that back -- only a controller reset would, and that tears down every addressed device
 * including the boot stick this log is being written to. So that case is named and latched
 * rather than papered over.
 */
/*
 * xHCI 4.6.1.2's own bound for a Command Abort to take effect is FIVE SECONDS. Boot 34 waited
 * 200 ms, found CRR still set, and declared the ring unrecoverable -- which may well have been
 * true, but 200 ms is not the spec's answer and the run could not tell the two apart. It is a
 * long time to hold the input tick, and it is spent only on a path whose alternative is every
 * device on that controller staying deaf for the rest of the run.
 */
#define XHCI_CMD_ABORT_WAIT_MS 5000u
#define XHCI_CMD_MAX_RECOVERIES 4u

static void cmd_ring_rebuild(xhci_hw_t *hw) {
    uint32_t link[4];

    zero(hw->cmd_ring, XPAGE);
    hype_xhci_trb_link(link, phys(hw->cmd_ring), 1);
    put_le32(hw->cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
    put_le32(hw->cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
    put_le32(hw->cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
    put_le32(hw->cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
    hw->cmd_enq = 0;
    hw->cmd_cyc = 1;
}

static int cmd_ring_recover(hype_xhci_ctrl_t *c) {
    xhci_hw_t *hw = HW(c);
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    uint32_t sts = rd32(bar, hw->opreg + HYPE_XHCI_OP_USBSTS);
    uint32_t crcr;
    uint32_t drop[4];
    unsigned int ms;

    if (hw->cmd_dead) {
        return -1; /* already reported; do not print the same death once per second */
    }
    if ((sts & (HYPE_XHCI_USBSTS_HSE | HYPE_XHCI_USBSTS_HCE)) != 0u) {
        hw->cmd_dead = 1;
        hw->cmd_recovery_fails++;
        hw->reset_wanted = 1; /* xHCI 4.24.1: HCE clears only with HCRST */
        hype_debug_print("host-xhci: the CONTROLLER has failed, not just a command: "
                         "usbsts=0x%08x (%s%s). Aborting the command ring cannot fix this; "
                         "every device on this controller is now unreachable [#775]\n",
                         sts,
                         (sts & HYPE_XHCI_USBSTS_HSE) ? "HSE " : "",
                         (sts & HYPE_XHCI_USBSTS_HCE) ? "HCE" : "");
        return -1;
    }
    if (hw->cmd_recoveries >= (unsigned long long)XHCI_CMD_MAX_RECOVERIES) {
        hw->cmd_dead = 1;
        hw->cmd_recovery_fails++;
        hw->reset_wanted = 1;
        hype_debug_print("host-xhci: command ring recovered %u times and stopped answering "
                         "again -- giving up on it. usbsts=0x%08x crcr=0x%08x [#775]\n",
                         (unsigned)XHCI_CMD_MAX_RECOVERIES, sts,
                         rd32(bar, hw->opreg + HYPE_XHCI_OP_CRCR));
        return -1;
    }

    crcr = rd32(bar, hw->opreg + HYPE_XHCI_OP_CRCR);
    if (hw->wedged) crcr |= HYPE_XHCI_CRCR_CRR; /* #782: the injected wedge never lets go */
    hype_debug_print("host-xhci: ctrl%u command ring stopped answering -- aborting it. "
                     "usbsts=0x%08x crcr=0x%08x (CRR=%u) after %llu timeout(s) [#775]\n",
                     c->log_id, sts, crcr, (crcr & HYPE_XHCI_CRCR_CRR) ? 1u : 0u,
                     (unsigned long long)hw->cmd_timeouts);

    /*
     * A SIXTY-FOUR BIT write. CRCR is one 64-bit register (xHCI 5.4.5), and boot 34 wrote only
     * its low dword: the abort was issued, and 200 ms later CRR was still set with USBSTS
     * reporting a perfectly healthy controller (0x00000010, PCD alone -- no HCH, no HSE, no
     * HCE). A partial write is the first thing to rule out before concluding the ring cannot
     * be restarted at all.
     *
     * Only CS/CA are meaningful while the ring is running; the pointer field is not writable
     * then, and RCS reads as 0, so this writes the abort bit and nothing else.
     */
    wr64(bar, hw->opreg + HYPE_XHCI_OP_CRCR, (uint64_t)HYPE_XHCI_CRCR_CA);
    for (ms = 0; ms < XHCI_CMD_ABORT_WAIT_MS; ms++) {
        crcr = rd32(bar, hw->opreg + HYPE_XHCI_OP_CRCR);
        if (hw->wedged) crcr |= HYPE_XHCI_CRCR_CRR;
        if ((crcr & HYPE_XHCI_CRCR_CRR) == 0u) {
            break;
        }
        delay_ms(1);
    }
    if ((crcr & HYPE_XHCI_CRCR_CRR) != 0u) {
        hw->cmd_dead = 1;
        hw->cmd_recovery_fails++;
        /*
         * Past this point software has run out of moves that leave the controller's devices
         * addressed. The remaining option is a controller RESET, which re-enumerates
         * everything on it (#785) -- asked for here, granted or refused by the owner, which
         * knows whether this controller carries the boot medium or the log (#783).
         */
        hw->reset_wanted = 1;
        hype_debug_print("host-xhci: command ring still RUNNING %u ms after Command Abort "
                         "(crcr=0x%08x usbsts=0x%08x) -- it cannot be restarted from software. "
                         "Every device on this controller stays unreachable; asking for a "
                         "controller reset [#775 #785]\n",
                         XHCI_CMD_ABORT_WAIT_MS, crcr,
                         rd32(bar, hw->opreg + HYPE_XHCI_OP_USBSTS));
        return -1;
    }

    /*
     * The abort posts a Command Ring Stopped (or Command Aborted) completion, and any
     * transfer events the controller had queued behind it are still on the ring. Drain what
     * is there and route the transfer events to their endpoints -- dropping one is how an
     * interrupt-IN endpoint goes permanently deaf (#761).
     */
    for (ms = 0; ms < 64u; ms++) {
        if (next_event_timed(hw, bar, c->rtsoff, drop, 2000u, 0) != 0) {
            break;
        }
        route_foreign_event(c, hw, drop);
    }

    cmd_ring_rebuild(hw);
    wr64(bar, hw->opreg + HYPE_XHCI_OP_CRCR, phys(hw->cmd_ring) | HYPE_XHCI_CRCR_RCS);
    hw->cmd_recoveries++;
    hype_debug_print("host-xhci: command ring restarted at its base (recovery %llu) [#775]\n",
                     hw->cmd_recoveries);
    return 0;
}

static int cmd_submit_wait(hype_xhci_ctrl_t *c, uint32_t cmd[4], uint32_t evt[4]) {
    xhci_hw_t *hw = HW(c);
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    unsigned int guard = 64u; /* bound the number of skipped (e.g. port-change) events */
    uint64_t my_trb = phys(hw->cmd_ring) + (uint64_t)hw->cmd_enq * HYPE_XHCI_TRB_BYTES;

    /*
     * Once the ring is known dead, fail IMMEDIATELY. Every attempt otherwise costs a
     * one-second event wait, and the caller is usually the 125 Hz input tick: boot 31 spent
     * 614 seconds of the run inside this function waiting for a controller that had stopped
     * listening. Failing fast keeps the tick alive and the dashboard responsive while the
     * rest of the machine carries on.
     */
    if (hw->cmd_dead) {
        return -1;
    }
    cmd_enqueue(hw, cmd);
#if HYPE_781_WEDGE_MS
    if (!hw->wedged && !hw->wedge_spent && c->log_id == HYPE_781_WEDGE_CTRL &&
        g_tsc_hz != 0ull &&
        rdtsc_now() - hw->up_tsc >= (g_tsc_hz / 1000ull) * (uint64_t)HYPE_781_WEDGE_MS) {
        hw->wedged = 1;
        hw->wedge_spent = 1;
        hype_debug_print("host-xhci: #782 INJECTED WEDGE on ctrl%u -- this and every later "
                         "command on it will see no completion, and the abort will not take "
                         "(-DHYPE_781_WEDGE_MS=%u)\n", c->log_id, (unsigned)HYPE_781_WEDGE_MS);
    }
#endif
    /* A wedged ring does not get its doorbell rung: the TRB sits there unanswered, which is
     * exactly what the controller does to it in the real case. */
    if (!hw->wedged) {
        wr32(bar, hype_xhci_doorbell_offset(c->dboff, 0), 0u); /* command doorbell, target 0 */
    }
    while (guard-- != 0u) {
        if (hw->wedged || next_event(hw, bar, c->rtsoff, evt) != 0) {
            /* #266: distinguish "nothing arrived" from "things arrived, none ours". */
            hw->cmd_timeouts++;
            if (hw->cmd_timeouts <= 8ull || !hw->cmd_dead) {
                hype_debug_print("host-xhci: #266 command TIMEOUT waiting for trb=0x%llx "
                                 "(no event arrived, timeout %llu)\n",
                                 (unsigned long long)my_trb,
                                 (unsigned long long)hw->cmd_timeouts);
            }
            /*
             * Do not simply return. The command is still enqueued and the ring may have
             * stopped; leaving it there means every later command lands behind a wedge and
             * times out too, which is boot 31 exactly. Abort and restart the ring so the
             * NEXT command has a chance.
             */
            (void)cmd_ring_recover(c);
            return -1;
        }
        if (hype_xhci_trb_type(evt) == HYPE_XHCI_TRB_CMD_COMPLETION) {
            if (hype_xhci_event_trb_ptr(evt) != my_trb) {
                static int l1 = 0;
                if (l1++ < 4) {
                    hype_debug_print("host-xhci: #254 late/foreign command completion accepted "
                                     "(trb=0x%llx wanted 0x%llx) -- this controller delivers "
                                     "command events late\n",
                                     (unsigned long long)hype_xhci_event_trb_ptr(evt),
                                     (unsigned long long)my_trb);
                }
            }
            return 0;
        }
        /*
         * #761: NOT "skip it". A Transfer Event that arrives while a command is in flight
         * used to be dropped here, and for an interrupt-IN endpoint that is permanent --
         * the endpoint stays armed, is never re-armed, and never reports again. A hub's
         * status-change endpoint is armed during enumeration, when commands are flying,
         * so it lost its first completion essentially every boot: measured as one transfer
         * event for the hub's slot in a whole run, then silence, with HUBPOLL reports=0
         * against 18,400 polls.
         */
        route_foreign_event(c, hw, evt);
    }
    /*
     * The guard ran out: 64 events came off the ring and none was our completion. That is a
     * different failure from a timeout and wants a different fix -- more budget, or a command
     * whose completion is genuinely never generated -- so it is counted and named separately
     * rather than sharing the timeout's silence.
     */
    hw->cmd_guard_exhausted++;
    if (hw->cmd_guard_exhausted <= 8ull) {
        hype_debug_print("host-xhci: command GUARD EXHAUSTED for trb=0x%llx -- 64 events came "
                         "off the ring and none was its completion (exhaustion %llu) [#775]\n",
                         (unsigned long long)my_trb,
                         (unsigned long long)hw->cmd_guard_exhausted);
    }
    return -1;
}

int hype_xhci_enable_slot(hype_xhci_ctrl_t *c, unsigned int *out_slot) {
    xhci_hw_t *hw = HW(c);
    uint32_t cmd[4], evt[4];
    if (!c->inited) return -1;
    hype_xhci_trb_enable_slot(cmd, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) {
        hype_debug_print("host-xhci:     Enable Slot: no command completion event (timeout)\n");
        return -1;
    }
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) {
        hype_debug_print("host-xhci:     Enable Slot: completion code %u\n",
                         hype_xhci_event_cc(evt));
        return -1;
    }
    if (out_slot) *out_slot = hype_xhci_event_slot_id(evt);
    /*
     * #734: say when a slot id is handed out for the SECOND time.
     *
     * Across the 2026-08-27 10:42 and 11:23 boots, every claimed HID that failed with
     * cc=4 sat on a slot id previously enabled for another device and then disabled, and
     * every one that worked sat on a fresh id -- four device-instances, no exceptions.
     * That pattern was inferred from duplicate slot numbers in the inventory; this makes
     * it a fact the log states outright, so the next boot either confirms or kills it.
     */
    {
        unsigned int sid = hype_xhci_event_slot_id(evt) & 0xFFu;
        if (hw->slot_seen[sid]) {
            hype_debug_print("host-xhci:     Enable Slot: %u -- RECYCLED, this id was "
                             "enabled and disabled before [#734]\n", sid);
        }
        hw->slot_seen[sid] = 1u;
    }
    return 0;
}

/* Write an 8-dword context into `base` at byte offset `off`. */
static void write_ctx(uint8_t *base, unsigned int off, const uint32_t c[8]) {
    unsigned int i;
    for (i = 0; i < 8u; i++) put_le32(base + off + i * 4u, c[i]);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * #734: dump the OUTPUT device context the controller wrote back for one slot.
 *
 * The 2026-08-27 10:42 boot left a working device and a broken one on the SAME hub, at
 * the same speed, with the same interval: the mouse reported 548 times with 0 errors
 * while the keyboard took cc=4 on every single transfer, recovered cleanly 8 times, and
 * failed again each time. Everything hype BUILDS for the two is the same shape, so the
 * question is what the CONTROLLER ended up holding -- EP State, and whether the speed,
 * TT and Interval fields survived Configure Endpoint. This prints that, once per
 * endpoint, so the two can be diffed inside one boot.
 */
/*
 * #755: the Context Entries the controller currently holds for this slot.
 *
 * Slot Context dword 0 bits 31:27, read from the OUTPUT context -- the controller's own
 * view, which is the only authority on what is configured right now.
 *
 * Needed because Configure Endpoint REPLACES the Slot Context wholesale when A0 is set.
 * Building one from the endpoint being added sets Context Entries to that endpoint's DCI,
 * and since the field means "index of the last valid Endpoint Context", a second endpoint
 * with a LOWER DCI silently invalidates the higher one that is already configured.
 *
 * Returns 0 when the slot has no device context, so a caller's max() falls through to its
 * own DCI and behaves exactly as before.
 */
/*
 * #764: the controller's TR Dequeue Pointer for one endpoint, from the OUTPUT context.
 *
 * Endpoint Context DW2/DW3 hold it, with the low 4 bits carrying DCS and reserved bits --
 * masked off so the comparison is against a TRB address. Returns -1 when the slot has no
 * device context, so the caller leaves the endpoint alone.
 */
static int int_in_ctx_dequeue(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int dci,
                              uint64_t *out) {
    xhci_hw_t *hw = HW(c);
    unsigned int cs = c->ctx_size;
    const uint8_t *dctx;
    int di = dev_index(hw, slot);

    if (di < 0 || out == (uint64_t *)0) return -1;
    dctx = hw->dev_ctx[di];
    *out = ((uint64_t)get_le32(dctx + dci * cs + 8u) |
            ((uint64_t)get_le32(dctx + dci * cs + 12u) << 32)) & ~0xFull;
    return 0;
}

static int int_in_poll_body(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int ep_addr,
                            uint8_t *out, unsigned int len);

/*
 * #775: does this event complete the OLDEST outstanding transfer on this endpoint?
 *
 * An interrupt ring completes in order, so only the head can be next. Checking the TRB
 * pointer against it keeps #766's strictness -- a stale or duplicate event still cannot be
 * claimed as something it is not -- while allowing several to be in flight.
 */


/*
 * Take a completion for THIS endpoint, wherever it was dequeued, and retire against it NOW.
 *
 * This is the fix for the permanent deafness. Retirement used to happen at poll time, so
 * between one completion being routed and the endpoint's next poll the head was stale, and
 * any further completion for the same endpoint could not match it -- it went to the shared
 * parked table, which evicts, and an evicted interrupt-IN completion never comes back.
 *
 * Matching is against the whole outstanding set rather than the head alone, which keeps
 * #766's strictness (a TRB this endpoint does not hold outstanding still retires nothing)
 * while surviving a completion observed later than the one behind it. An interrupt ring
 * completes in order, so a match at index k means 0..k-1 completed too and they are retired
 * with it, their reports queued rather than discarded.
 *
 * Returns 1 if the completion belonged here and was queued, 0 if it matched nothing.
 */

/* #764 capture: note a completion this endpoint actually claimed. */
/*
 * Take a completion for THIS endpoint, wherever it was dequeued, and retire against it NOW.
 *
 * The whole state machine is in core/int_in_queue.c so it can be unit-tested; this is the
 * bridge from an xHCI Transfer Event to it. Retiring HERE rather than at poll time is the
 * fix: the head used to stay stale until the endpoint was next polled, so a second
 * completion arriving in the same drain pass matched nothing and went to a table that
 * evicts -- and an evicted interrupt-IN completion never comes back.
 */
static int int_in_deliver(xhci_int_in_hw_t *iin, uint64_t trb, uint32_t cc,
                          unsigned int residue) {
    if (!hype_iiq_deliver(&iin->q, trb, cc, residue)) {
        return 0;
    }
    iin->silent_polls = 0;
    iin->revive_after = HYPE_INT_IN_SILENT_MAX; /* it works: be prompt again next time */
    return 1;
}

static void int_in_note_claim(xhci_int_in_hw_t *iin, uint64_t trb, uint32_t cc) {
    unsigned int i = iin->claim_n % 8u;
    iin->claim_trb[i] = trb;
    iin->claim_cc[i] = (uint8_t)cc;
    iin->claim_n++;
    iin->reports++;
}

/*
 * #764 capture: the ring state at the moment this endpoint stopped tracking the controller.
 *
 * Printed ONCE per endpoint, the first time the controller's dequeue pointer and hype's
 * outstanding TRB disagree. Everything needed to reconstruct what happened is on one line:
 * where each side thinks it is, how hype's enqueue cursor sits, and the last completions
 * hype claimed -- so a claim from a TRB that was never armed is visible rather than inferred.
 */
static unsigned int out_ctx_entries(hype_xhci_ctrl_t *c, unsigned int slot) {
    xhci_hw_t *hw = HW(c);
    int di = dev_index(hw, slot);
    if (di < 0) return 0u;
    return (get_le32(hw->dev_ctx[di] + 0u) >> 27) & 0x1Fu;
}

static void dump_out_ctx(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int dci,
                         const char *what) {
    xhci_hw_t *hw = HW(c);
    unsigned int cs = c->ctx_size;
    const uint8_t *dctx;
    uint32_t s0, s1, s2, e0, e1, e4;
    int di = dev_index(hw, slot);

    if (di < 0) {
        hype_debug_print("host-xhci: CTXDUMP %s slot=%u -- no device context [#734]\n", what, slot);
        return;
    }
    dctx = hw->dev_ctx[di];
    s0 = get_le32(dctx + 0u);
    s1 = get_le32(dctx + 4u);
    s2 = get_le32(dctx + 8u);
    e0 = get_le32(dctx + dci * cs + 0u);
    e1 = get_le32(dctx + dci * cs + 4u);
    e4 = get_le32(dctx + dci * cs + 16u);
    hype_debug_print("host-xhci: CTXDUMP %s slot=%u dci=%u | SLOT route=0x%05x speed=%u "
                     "entries=%u mtt=%u hub=%u rootport=%u ttslot=%u ttport=%u ttt=%u "
                     "| EP state=%u interval=%u mult=%u maxburst=%u mps=%u cerr=%u "
                     "type=%u esit=%u [#734]\n",
                     what, slot, dci,
                     s0 & 0xFFFFFu, (s0 >> 20) & 0xFu, (s0 >> 27) & 0x1Fu,
                     (s0 >> 25) & 1u, (s0 >> 26) & 1u, (s1 >> 16) & 0xFFu,
                     s2 & 0xFFu, (s2 >> 8) & 0xFFu, (s2 >> 16) & 0x3u,
                     e0 & 0x7u, (e0 >> 16) & 0xFFu, (e0 >> 8) & 0x3u, (e1 >> 15) & 0x1u,
                     (e1 >> 16) & 0xFFFFu, (e1 >> 1) & 0x3u, (e1 >> 3) & 0x7u,
                     (e4 >> 16) & 0xFFFFu);
}

static void ep0_enqueue(xhci_hw_t *hw, unsigned int di, const uint32_t trb[4]) {
    ring_enqueue(hw->ep0_ring[di], &hw->ep0_enq[di], &hw->ep0_cyc[di], trb);
}

int hype_xhci_address_device(hype_xhci_ctrl_t *c, unsigned int slot,
                             const hype_xhci_devpath_t *path) {
    xhci_hw_t *hw = HW(c);
    unsigned int cs = c->ctx_size;
    uint32_t ctx[8];
    uint32_t cmd[4], evt[4];
    int di;
    uint8_t *dctx, *ep0;

    if (!c->inited || slot == 0u) return -1;
    di = dev_index(hw, slot);
    if (di < 0) di = dev_alloc(hw, slot);
    if (di < 0) return -1; /* device pool exhausted */
    dctx = hw->dev_ctx[di];
    ep0 = hw->ep0_ring[di];

    /* Fresh Input/Device contexts + this device's EP0 transfer ring. */
    zero(hw->input_ctx, XPAGE);
    zero(dctx, XPAGE);
    zero(ep0, XPAGE);
    {
        uint32_t link[4];
        hype_xhci_trb_link(link, phys(ep0), 1);
        put_le32(ep0 + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
        put_le32(ep0 + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
        put_le32(ep0 + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
        put_le32(ep0 + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
    }
    hw->ep0_enq[di] = 0;
    hw->ep0_cyc[di] = 1;

    /* Input Control Context (offset 0): add the slot + EP0 contexts. */
    hype_xhci_input_ctrl_ctx(ctx, HYPE_XHCI_ADD_SLOT | HYPE_XHCI_ADD_EP0, 0);
    write_ctx(hw->input_ctx, 0, ctx);
    /* Slot Context (offset 1*ctx_size): full topology, 1 valid context entry (EP0). */
    hype_xhci_slot_ctx(ctx, path->route, path->speed, 1, path->root_port,
                       path->tt_hub_slot, path->tt_port);
    write_ctx(hw->input_ctx, cs, ctx);
    /* EP0 Context (offset 2*ctx_size). */
    hype_xhci_ep0_ctx(ctx, hype_xhci_default_mps(path->speed), phys(ep0), 1);
    write_ctx(hw->input_ctx, 2u * cs, ctx);

    /* DCBAA[slot] -> this device's output Device Context. */
    put_le64(hw->dcbaa + slot * 8u, phys(dctx));

    /* Real HW (esp. High-Speed devices) intermittently NAKs the SET_ADDRESS the
     * controller issues during Address Device -> a USB Transaction Error (cc 4)
     * or a slow/absent completion. USB enumeration is expected to retry, so
     * attempt it a few times with a settle delay between tries. */
    {
        unsigned int attempt;
        int ok = 0;
        for (attempt = 0; attempt < 3u && !ok; attempt++) {
            unsigned int cc;
            if (attempt != 0u) delay_ms(10); /* let the device settle before re-issue */
            hype_xhci_trb_address_device(cmd, phys(hw->input_ctx), slot, 0, (int)hw->cmd_cyc);
            if (cmd_submit_wait(c, cmd, evt) != 0) {
                hype_debug_print("host-xhci:     Address Device slot %u try %u: no completion "
                                 "event (timeout)\n", slot, attempt + 1u);
                continue;
            }
            cc = hype_xhci_event_cc(evt);
            if (cc == HYPE_XHCI_CC_SUCCESS) { ok = 1; break; }
            /* cc: 5=TRB Error, 11=Context State, 17=Parameter Error, 4=USB Transaction Error. */
            hype_debug_print("host-xhci:     Address Device slot %u try %u: completion code %u "
                             "(ctx_size=%uB speed=%u route=0x%05x)\n", slot, attempt + 1u, cc,
                             c->ctx_size, path->speed, path->route);
        }
        if (!ok) { dev_free(hw, slot); return -1; }
    }
    /* USB 2.0 §9.2.6.3: a device needs up to 2 ms of recovery after SET_ADDRESS
     * before it will respond at its new address -- without this the immediately
     * following GET_DESCRIPTOR fails (seen on real HW). Give it a wide margin. */
    delay_ms(10);
    return 0;
}

/*
 * A control transfer on EP0: Setup [+ Data] + Status, doorbell DCI 1, wait the
 * Transfer Event. dir_in selects IN(1)/OUT(0) for a data stage; len==0 = no data
 * (status defaults IN). For IN the received bytes are copied to buf; for OUT the
 * buf bytes are staged before the transfer. Returns 0 on success/short-packet.
 */
static int control_transfer(hype_xhci_ctrl_t *c, unsigned int slot, uint8_t bm_req, uint8_t b_req,
                            uint16_t wvalue, uint16_t windex, void *buf, unsigned int len,
                            int dir_in) {
    xhci_hw_t *hw = HW(c);
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    uint32_t t[4], evt[4];
    unsigned int guard = 64u;
    unsigned int trt, status_dir_in, i;
    int di;

    if (!c->inited || slot == 0u || len > XPAGE) return -1;
    di = dev_index(hw, slot);
    if (di < 0) return -1;
    trt = (len == 0u) ? HYPE_XHCI_TRT_NO_DATA : (dir_in ? HYPE_XHCI_TRT_IN : HYPE_XHCI_TRT_OUT);

    if (len && !dir_in && buf) {
        for (i = 0; i < len; i++) hw->xfer_buf[i] = ((const uint8_t *)buf)[i];
    } else {
        zero(hw->xfer_buf, XPAGE);
    }

    {
        /* #254: remember this transfer's own TRB addresses so the wait below
         * can tell OUR events from stale ones (same reasoning as bulk_xfer). */
        uint64_t ring_base = phys(hw->ep0_ring[di]);
        uint64_t setup_trb, data_trb = 0, status_trb;

        setup_trb = ring_base + (uint64_t)hw->ep0_enq[di] * HYPE_XHCI_TRB_BYTES;
        hype_xhci_trb_setup_stage(t, bm_req, b_req, wvalue, windex, (uint16_t)len, trt,
                                  (int)hw->ep0_cyc[di]);
        ep0_enqueue(hw, di, t);
        if (len) {
            data_trb = ring_base + (uint64_t)hw->ep0_enq[di] * HYPE_XHCI_TRB_BYTES;
            hype_xhci_trb_data_stage(t, phys(hw->xfer_buf), len, dir_in, (int)hw->ep0_cyc[di]);
            ep0_enqueue(hw, di, t);
        }
        /* Status stage direction is opposite the data direction (IN if no data). */
        status_dir_in = (len && dir_in) ? 0u : 1u;
        status_trb = ring_base + (uint64_t)hw->ep0_enq[di] * HYPE_XHCI_TRB_BYTES;
        hype_xhci_trb_status_stage(t, (int)status_dir_in, 1, (int)hw->ep0_cyc[di]);
        ep0_enqueue(hw, di, t);

        wr32(bar, hype_xhci_doorbell_offset(c->dboff, slot), 1u); /* DCI 1 = EP0 */

        while (guard-- != 0u) {
            uint64_t p;
            if (next_event(hw, bar, c->rtsoff, evt) != 0) return -1;
            if (hype_xhci_trb_type(evt) != HYPE_XHCI_TRB_TRANSFER_EVENT) continue;
            if (hype_xhci_event_slot_id(evt) != slot || hype_xhci_event_ep_id(evt) != 1u) {
                /*
                 * #761: route it, do not drop it. Control transfers run constantly during
                 * enumeration -- descriptor reads, SET_CONFIGURATION, SET_PROTOCOL -- which
                 * is exactly when an interrupt-IN endpoint armed moments earlier delivers
                 * its first completion. Dropping it leaves that endpoint armed forever and
                 * never re-armed: permanently deaf, from one lost event.
                 */
                route_foreign_event(c, hw, evt);
                continue;
            }
            p = hype_xhci_event_trb_ptr(evt);
            if (p != setup_trb && p != data_trb && p != status_trb) {
                /*
                 * #254 v2: do NOT discard. Same reasoning as cmd_submit_wait --
                 * EP0 transfers carry no medium data, and this controller
                 * delivers events late, so discarding an unrecognised EP0 event
                 * only strands enumeration. Treat it as this transfer's
                 * completion (it is on our slot's EP0) and log it once.
                 */
                static int l2 = 0;
                if (l2++ < 4) {
                    hype_debug_print("host-xhci: #254 late/foreign EP0 event accepted "
                                     "(trb=0x%llx) -- controller delivers events late\n",
                                     (unsigned long long)p);
                }
                p = status_trb; /* fall through as if the status stage completed */
            }
            {
                unsigned int cc = hype_xhci_event_cc(evt);
                if (cc != HYPE_XHCI_CC_SUCCESS && cc != HYPE_XHCI_CC_SHORT_PACKET) return -1;
            }
            if (p != status_trb) {
                continue; /* setup/data stage completed fine: wait for status */
            }
            if (len && dir_in && buf) {
                for (i = 0; i < len; i++) ((uint8_t *)buf)[i] = hw->xfer_buf[i];
            }
            return 0;
        }
    }
    return -1;
}

int hype_xhci_get_device_descriptor(hype_xhci_ctrl_t *c, unsigned int slot, uint8_t *buf18) {
    /* GET_DESCRIPTOR(DEVICE, index 0), 18 bytes, IN. */
    return control_transfer(c, slot, 0x80, 6, 0x0100, 0, buf18, 18u, 1);
}

int hype_xhci_get_config_descriptor(hype_xhci_ctrl_t *c, unsigned int slot, uint8_t *buf,
                                    unsigned int maxlen, unsigned int *out_len) {
    uint8_t hdr[9];
    unsigned int total;

    /* First read the 9-byte config header to learn wTotalLength. */
    if (control_transfer(c, slot, 0x80, 6, 0x0200, 0, hdr, 9u, 1) != 0) return -1;
    total = (unsigned int)hdr[2] | ((unsigned int)hdr[3] << 8);
    if (total < 9u) return -1;
    if (total > maxlen) total = maxlen;
    /* Re-read the full config (config + interface + endpoint descriptors). */
    if (control_transfer(c, slot, 0x80, 6, 0x0200, 0, buf, total, 1) != 0) return -1;
    if (out_len) *out_len = total;
    return 0;
}

int hype_xhci_get_string_descriptor(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int index,
                                    uint16_t langid, uint8_t *buf, unsigned int maxlen) {
    /* GET_DESCRIPTOR(STRING, index): wValue = type|index, wIndex = LANGID.
     * bLength (byte 0) bounds the useful bytes; a short reply is normal. */
    if (maxlen < 2u || maxlen > 255u) return -1;
    return control_transfer(c, slot, 0x80, 6,
                            (uint16_t)(((unsigned int)HYPE_USB_DESC_STRING << 8) | index), langid,
                            buf, maxlen, 1);
}

int hype_xhci_set_configuration(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int config_value) {
    /* SET_CONFIGURATION: bmRequestType=0x00 (OUT/standard/device), bRequest=9,
     * wValue=config, no data stage. */
    return control_transfer(c, slot, 0x00, 9, (uint16_t)config_value, 0, 0, 0u, 0);
}

int hype_xhci_hid_set_boot_protocol(hype_xhci_ctrl_t *c, unsigned int slot,
                                    unsigned int interface_num) {
    /* SET_PROTOCOL: bmRequestType=0x21 (OUT/class/interface), bRequest=0x0B,
     * wValue=0 (Boot), wIndex=interface, no data stage. */
    return control_transfer(c, slot, 0x21, 0x0B, 0, (uint16_t)interface_num, 0, 0u, 0);
}

/* Stamp a Link TRB (toggle-cycle, cycle=1) at the end of a fresh transfer ring. */
static void ring_init_link(uint8_t *ring) {
    uint32_t link[4];
    hype_xhci_trb_link(link, phys(ring), 1);
    put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
    put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
    put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
    put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
}

int hype_xhci_configure_bulk_endpoints(hype_xhci_ctrl_t *c, unsigned int slot,
                                       const hype_xhci_devpath_t *path,
                                       const hype_xhci_msc_eps_t *msc) {
    xhci_hw_t *hw = HW(c);
    unsigned int cs = c->ctx_size;
    unsigned int dci_in = hype_xhci_ep_dci(msc->bulk_in_ep);
    unsigned int dci_out = hype_xhci_ep_dci(msc->bulk_out_ep);
    unsigned int max_dci = (dci_in > dci_out) ? dci_in : dci_out;
    uint32_t ctx[8], cmd[4], evt[4];

    xhci_msc_hw_t *m;

    if (!c->inited || slot == 0u) return -1;
    /* #387: this device's OWN rings -- allocated here, the datapath's front door. */
    m = msc_hw_for(c, slot, 1);
    if (m == 0) {
        hype_debug_print("host-xhci: no claimed-MSC block free (cap %u) -- slot %u not brought "
                         "up [#387]\n", HYPE_XHCI_MSC_MAX, slot);
        return -1;
    }

    /* Fresh bulk transfer rings. */
    zero(m->bulk_in_ring, XPAGE);
    zero(m->bulk_out_ring, XPAGE);
    ring_init_link(m->bulk_in_ring);
    ring_init_link(m->bulk_out_ring);
    m->bin_enq = 0; m->bin_cyc = 1;
    m->bout_enq = 0; m->bout_cyc = 1;

    /* Input Context: add the Slot + both bulk endpoint contexts. The Slot
     * Context must re-provide the device's full topology (route/root/TT). */
    zero(hw->input_ctx, XPAGE);
    hype_xhci_input_ctrl_ctx(ctx, HYPE_XHCI_ADD_SLOT | (1u << dci_in) | (1u << dci_out), 0);
    write_ctx(hw->input_ctx, 0, ctx);
    hype_xhci_slot_ctx(ctx, path->route, path->speed, max_dci, path->root_port,
                       path->tt_hub_slot, path->tt_port); /* context entries = highest DCI */
    write_ctx(hw->input_ctx, cs, ctx);
    hype_xhci_ep_ctx(ctx, HYPE_XHCI_EP_TYPE_BULK_IN, msc->bulk_in_mps, phys(m->bulk_in_ring), 1);
    write_ctx(hw->input_ctx, (1u + dci_in) * cs, ctx);
    hype_xhci_ep_ctx(ctx, HYPE_XHCI_EP_TYPE_BULK_OUT, msc->bulk_out_mps, phys(m->bulk_out_ring), 1);
    write_ctx(hw->input_ctx, (1u + dci_out) * cs, ctx);

    hype_xhci_trb_configure_endpoint(cmd, phys(hw->input_ctx), slot, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) return -1;
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) return -1;
    return 0;
}

/* One bulk transfer: enqueue a Normal TRB on `ring`, ring the slot doorbell for
 * `dci`, and wait its Transfer Event. Returns 0 on success/short-packet. */
/*
 * USB-5 (#217): configure the HID keyboard's interrupt-IN endpoint.
 *
 * Structurally the bulk sibling above, with EP_TYPE_INT_IN and its own ring. The
 * Slot Context has to re-provide the device's full topology (route/root/TT) for the
 * same reason it does there -- a Configure Endpoint command replaces it.
 */
int hype_xhci_configure_int_in_endpoint(hype_xhci_ctrl_t *c, unsigned int slot,
                                       const hype_xhci_devpath_t *path, unsigned int ep_addr,
                                       unsigned int mps, unsigned int interval) {
    xhci_hw_t *hw = HW(c);
    unsigned int cs = c->ctx_size;
    unsigned int dci = hype_xhci_ep_dci(ep_addr);
    xhci_int_in_hw_t *iin;
    uint32_t ctx[8], cmd[4], evt[4];

    if (!c->inited || slot == 0u || path == (const hype_xhci_devpath_t *)0) return -1;
    iin = iin_hw_for(c, slot, dci, 1);
    if (iin == (xhci_int_in_hw_t *)0) {
        hype_debug_print("host-xhci: no interrupt-IN block free for slot=%u ep=0x%02x "
                         "(pool of %u) [#734]\n", slot, ep_addr,
                         (unsigned)HYPE_XHCI_INT_IN_MAX);
        return -1;
    }
    if (mps == 0u || mps > sizeof(iin->report)) return -1;

    zero(iin->ring, XPAGE);
    ring_init_link(iin->ring);
    iin->enq = 0; iin->cyc = 1; hype_iiq_reset(&iin->q);
    iin->revive_after = HYPE_INT_IN_SILENT_MAX; iin->revives = 0;
    iin->revive_fails = 0; iin->stopped_ccs = 0;
    iin->mps = mps; iin->recoveries = 0; iin->backoff = 0;
    iin->silent_polls = 0; iin->rearms = 0;
    iin->claim_n = 0; iin->diverged = 0; iin->reports = 0;
    iin->deliv = 0; iin->own = 0; iin->to_park = 0; iin->handed = 0;
    iin->last_deq = 0; iin->diverge_run = 0;
    iin->arms = 0; iin->never_recovered = 0;

    zero(hw->input_ctx, XPAGE);
    hype_xhci_input_ctrl_ctx(ctx, HYPE_XHCI_ADD_SLOT | (1u << dci), 0);
    write_ctx(hw->input_ctx, 0, ctx);
    /*
     * #755: never LOWER Context Entries. A composite device -- a wireless receiver
     * presenting a keyboard interface and a mouse interface -- is claimed twice on one
     * slot, and the second claim used to rebuild the Slot Context with its own DCI, so
     * claiming ep 0x81 (DCI 3) after ep 0x82 (DCI 5) invalidated the endpoint just
     * configured. Observed on hardware: entries=5 then entries=3 on slot 5.
     */
    {
        unsigned int have = out_ctx_entries(c, slot);
        hype_xhci_slot_ctx(ctx, path->route, path->speed, (have > dci) ? have : dci,
                           path->root_port, path->tt_hub_slot, path->tt_port);
    }
    write_ctx(hw->input_ctx, cs, ctx);
    /* #217: the Interval is what gives the controller a schedule to poll on. Without
     * it the endpoint configures cleanly and never reports. */
    hype_xhci_ep_ctx_interval(ctx, HYPE_XHCI_EP_TYPE_INT_IN, mps, phys(iin->ring), 1,
                              hype_xhci_interval_encode(path->speed, interval));
    write_ctx(hw->input_ctx, (1u + dci) * cs, ctx);

    hype_xhci_trb_configure_endpoint(cmd, phys(hw->input_ctx), slot, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) return -1;
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) return -1;
    dump_out_ctx(c, slot, dci, "int-in");
    if (path->tt_hub_slot) {
        dump_out_ctx(c, path->tt_hub_slot, 1u, "its-TT-hub");
    }
    /*
     * #772: make the controller's dequeue pointer agree with the ring we just reset.
     *
     * Configure Endpoint takes the TR Dequeue Pointer from the input context, and hype had
     * been trusting that. On a RE-configure -- a device re-claimed after a hot-plug, on a
     * recycled slot id, reusing this same block and therefore the same ring addresses --
     * the controller can be left pointing where it was, while hype resets its own cursor to
     * index 0. Neither side is wrong on its own terms and neither can make progress: hype
     * arms TRB 0, the controller waits at TRB 1 for a cycle bit that will never be set
     * there, nothing completes, and no event is ever generated.
     *
     * That is not a lost completion. It is a completion that never happens, which is why
     * every routing counter reads zero when it is caught:
     *
     *   #764 DIVERGED slot=3 ep=3 | controller deq=+0x10 hype trb=+0x0 enq=1 cyc=1 armed=1
     *                             | reports=0 | deliv=0 own=0 to_park=0
     *
     * Reproduced in the #767 rig on a hot-plugged keyboard: enumerated, claimed, and deaf
     * from its first poll.
     *
     * Read it back and force it if it differs. A Set TR Dequeue on an endpoint hype has
     * just configured is cheap and happens once per claim.
     */
    {
        uint64_t want = phys(iin->ring), have = 0;
        if (int_in_ctx_dequeue(c, slot, dci, &have) == 0 && have != want) {
            hype_debug_print("host-xhci: #772 slot=%u ep=%u dequeue is 0x%llx after Configure "
                             "Endpoint, not the ring base 0x%llx -- forcing it, or this "
                             "endpoint would never report\n", slot, dci,
                             (unsigned long long)have, (unsigned long long)want);
            hype_xhci_trb_set_tr_dequeue(cmd, want | 1u /* DCS=1 */, slot, dci, (int)hw->cmd_cyc);
            if (cmd_submit_wait(c, cmd, evt) != 0 ||
                hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) {
                hype_debug_print("host-xhci: #772 slot=%u ep=%u Set TR Dequeue FAILED -- the "
                                 "endpoint is configured but will not report\n", slot, dci);
                return -1;
            }
        }
    }
    return 0;
}

/*
 * #737: tell the controller that an addressed slot is a HUB.
 *
 * Address Device builds a plain function's Slot Context. A hub needs three more
 * fields -- Hub, Number of Ports, TT Think Time -- and they are only evaluated by a
 * Configure Endpoint command with A0 set (xHCI 4.6.6). Until this runs, every child
 * addressed through the hub names it as its Transaction Translator by slot id while
 * that slot still says Hub=0, which is what the controller rejects.
 *
 * A0 only: no endpoint contexts are added or dropped.
 *
 * #755: Context Entries is carried over from the output context rather than forced to 1.
 * It used to be hardcoded, with the comment "hype never configures a hub's own
 * status-change endpoint" -- true when this was written, false since #746 armed exactly
 * that endpoint. Forcing 1 after it is configured would invalidate it, and a hub whose
 * status-change endpoint has quietly gone away is a hub that can no longer hot-plug.
 */
int hype_xhci_configure_hub_slot(hype_xhci_ctrl_t *c, unsigned int slot,
                                 const hype_xhci_devpath_t *path, unsigned int nbr_ports,
                                 unsigned int ttt) {
    xhci_hw_t *hw = HW(c);
    unsigned int cs = c->ctx_size;
    uint32_t ctx[8], cmd[4], evt[4];

    if (!c->inited || slot == 0u || path == (const hype_xhci_devpath_t *)0) return -1;

    zero(hw->input_ctx, XPAGE);
    hype_xhci_input_ctrl_ctx(ctx, HYPE_XHCI_ADD_SLOT, 0);
    write_ctx(hw->input_ctx, 0, ctx);
    {
        unsigned int have = out_ctx_entries(c, slot); /* #755: >=1, never lower */
        hype_xhci_slot_ctx(ctx, path->route, path->speed, (have > 1u) ? have : 1u,
                           path->root_port, path->tt_hub_slot, path->tt_port);
    }
    hype_xhci_slot_ctx_set_hub(ctx, nbr_ports, ttt, 0u);
    write_ctx(hw->input_ctx, cs, ctx);

    hype_xhci_trb_configure_endpoint(cmd, phys(hw->input_ctx), slot, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) return -1;
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) return -1;
    return 0;
}

/*
 * #746: the hubs this walk registered, so their status-change endpoints can be polled.
 *
 * A hub's downstream ports do NOT raise xHCI Port Status Change Events -- those are root
 * ports only. The hub reports its own port changes on an interrupt-IN endpoint that hype
 * never configured, because until hot-plug nothing needed it (hype_xhci_configure_hub_slot()
 * still sets Context Entries to 1 for exactly that reason). Without this, #744 and #745
 * cover root ports and miss the operator's actual topology, where the keyboard is behind a
 * 2.0 hub.
 */
/* Defined with the rest of the hub descent further down; declared here because the
 * status-change poller above it is the first user. */
#define HUB_FEAT_PORT_RESET        4u
#define HUB_FEAT_PORT_POWER        8u
#define HUB_FEAT_C_PORT_CONNECTION 16u
#define HUB_FEAT_C_PORT_ENABLE     17u
#define HUB_FEAT_C_PORT_SUSPEND    18u
#define HUB_FEAT_C_PORT_OVERCURRENT 19u
#define HUB_FEAT_C_PORT_RESET      20u
static int hub_set_port_feature(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int feat,
                                unsigned int port);
static int hub_clear_port_feature(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int feat,
                                  unsigned int port);
static int hub_get_port_status(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int port,
                               uint8_t st[4]);
static unsigned int hub_port_speed(const uint8_t st[4]);

typedef struct {
    unsigned int used;
    unsigned int ctrl;      /* c->hw_slot */
    unsigned int slot;
    unsigned int nports;
    unsigned int ep;        /* the status-change endpoint address, 0 if it could not be set up */
    unsigned int bitmap_len;
    /* #746: what a child arriving here needs. Kept at walk time because recomputing it
     * later would mean re-walking the topology to find this hub's own route and TT. */
    hype_xhci_devpath_t path;
    unsigned int tier;
    int ss;                 /* a SuperSpeed hub: its downstream ports are all SS */
    /*
     * #770: ports hype has given up on, one bit per port (1-based, so bit 1 is port 1).
     *
     * A hub reports a port while any of its change bits is set, and a device whose link
     * keeps flapping sets them again as fast as they are cleared. Boot 14 logged
     * `hub slot 6 port 1 changed` 4,610 times -- 68 bytes apart, a tight loop -- and it
     * dominated an 864 KB log.
     *
     * #763 caps the ENUMERATION retries, which stopped the Address Device storm, but the
     * reporting itself carries on: take_change still spends a GET_PORT_STATUS and a
     * ClearPortFeature per report, from the guest dispatch loop, forever. An ignored port
     * still has its change bits cleared -- leaving them set would make the hub report the
     * whole bitmap every time -- but it is not handed back to the caller.
     */
    uint32_t ignore_ports;
} xhci_hub_reg_t;

static xhci_hub_reg_t g_hubs[HYPE_XHCI_HUB_MAX];
/* #746: how often the hub status endpoints were polled, and what came back. Without this,
 * "hype is not polling" and "the hub is not reporting" look identical from the log. */
static unsigned long long g_hub_polls, g_hub_reports, g_hub_errs;

void hype_xhci_hub_poll_stats(unsigned long long *polls, unsigned long long *reports,
                              unsigned long long *errs) {
    if (polls) *polls = g_hub_polls;
    if (reports) *reports = g_hub_reports;
    if (errs) *errs = g_hub_errs;
}

int hype_xhci_configure_hub_int_in(hype_xhci_ctrl_t *c, unsigned int slot,
                                   const hype_xhci_devpath_t *path, unsigned int nbr_ports,
                                   unsigned int ttt, unsigned int ep_addr, unsigned int mps,
                                   unsigned int interval) {
    xhci_hw_t *hw = HW(c);
    unsigned int cs = c->ctx_size;
    unsigned int dci = hype_xhci_ep_dci(ep_addr);
    xhci_int_in_hw_t *iin;
    uint32_t ctx[8], cmd[4], evt[4];

    if (!c->inited || slot == 0u || path == (const hype_xhci_devpath_t *)0) return -1;
    iin = iin_hw_for(c, slot, dci, 1);
    if (iin == (xhci_int_in_hw_t *)0) {
        hype_debug_print("host-xhci: no interrupt-IN block free for hub slot=%u ep=0x%02x "
                         "(pool of %u) -- that hub's ports cannot hot-plug [#746]\n",
                         slot, ep_addr, (unsigned)HYPE_XHCI_INT_IN_MAX);
        return -1;
    }
    /* A hub's status-change endpoint is one bit per port plus one for the hub, so its
     * mps is tiny -- but honour whatever the descriptor said, clamped to the block. */
    if (mps == 0u) mps = 1u;
    if (mps > sizeof(iin->report)) mps = (unsigned int)sizeof(iin->report);

    zero(iin->ring, XPAGE);
    ring_init_link(iin->ring);
    iin->enq = 0; iin->cyc = 1; hype_iiq_reset(&iin->q);
    iin->revive_after = HYPE_INT_IN_SILENT_MAX; iin->revives = 0;
    iin->revive_fails = 0; iin->stopped_ccs = 0;
    iin->mps = mps; iin->recoveries = 0; iin->backoff = 0;
    iin->silent_polls = 0; iin->rearms = 0;
    iin->claim_n = 0; iin->diverged = 0; iin->reports = 0;
    iin->deliv = 0; iin->own = 0; iin->to_park = 0; iin->handed = 0;
    iin->last_deq = 0; iin->diverge_run = 0;
    iin->arms = 0; iin->never_recovered = 0;

    zero(hw->input_ctx, XPAGE);
    hype_xhci_input_ctrl_ctx(ctx, HYPE_XHCI_ADD_SLOT | (1u << dci), 0);
    write_ctx(hw->input_ctx, 0, ctx);
    /* #755: never lower Context Entries -- see out_ctx_entries(). */
    {
        unsigned int have = out_ctx_entries(c, slot);
        hype_xhci_slot_ctx(ctx, path->route, path->speed, (have > dci) ? have : dci,
                           path->root_port, path->tt_hub_slot, path->tt_port);
    }
    /* THE POINT OF THIS FUNCTION: still a hub afterwards. */
    hype_xhci_slot_ctx_set_hub(ctx, nbr_ports, ttt, 0u);
    write_ctx(hw->input_ctx, cs, ctx);
    hype_xhci_ep_ctx_interval(ctx, HYPE_XHCI_EP_TYPE_INT_IN, mps, phys(iin->ring), 1,
                              hype_xhci_interval_encode(path->speed, interval));
    write_ctx(hw->input_ctx, (1u + dci) * cs, ctx);

    hype_xhci_trb_configure_endpoint(cmd, phys(hw->input_ctx), slot, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) return -1;
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) return -1;
    dump_out_ctx(c, slot, dci, "hub-status");
    return 0;
}

static void hub_register(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int nports,
                         unsigned int ep, unsigned int bitmap_len,
                         const hype_xhci_devpath_t *path, unsigned int tier, int ss) {
    unsigned int i;

    for (i = 0; i < HYPE_XHCI_HUB_MAX; i++) {
        if (g_hubs[i].used && g_hubs[i].ctrl == c->hw_slot && g_hubs[i].slot == slot) {
            break; /* re-walked: update in place rather than duplicating */
        }
    }
    if (i == HYPE_XHCI_HUB_MAX) {
        for (i = 0; i < HYPE_XHCI_HUB_MAX; i++) {
            if (!g_hubs[i].used) break;
        }
    }
    if (i == HYPE_XHCI_HUB_MAX) {
        hype_debug_print("host-xhci: hub slot %u not registered -- only %u hubs fit; its ports "
                         "cannot hot-plug [#746]\n", slot, (unsigned)HYPE_XHCI_HUB_MAX);
        return;
    }
    g_hubs[i].used = 1u;
    g_hubs[i].ctrl = c->hw_slot;
    g_hubs[i].slot = slot;
    g_hubs[i].nports = nports;
    g_hubs[i].ep = ep;
    g_hubs[i].bitmap_len = bitmap_len;
    g_hubs[i].path = *path;
    g_hubs[i].tier = tier;
    g_hubs[i].ss = ss;
}

static int hub_reg_find(unsigned int ctrl, unsigned int slot) {
    unsigned int i;
    for (i = 0; i < HYPE_XHCI_HUB_MAX; i++) {
        if (g_hubs[i].used && g_hubs[i].ctrl == ctrl && g_hubs[i].slot == slot) {
            return (int)i;
        }
    }
    return -1;
}

int hype_xhci_hub_child_route(hype_xhci_ctrl_t *c, unsigned int hub_slot, unsigned int port,
                              unsigned int *out_route) {
    int i;
    if (c == (hype_xhci_ctrl_t *)0 || out_route == (unsigned int *)0) return -1;
    i = hub_reg_find(c->hw_slot, hub_slot);
    if (i < 0) return -1;
    *out_route = hype_xhci_route_append(g_hubs[i].path.route, g_hubs[i].tier, port);
    return 0;
}

int hype_xhci_hub_root_port(hype_xhci_ctrl_t *c, unsigned int hub_slot, unsigned int *out_port) {
    int i;
    if (c == (hype_xhci_ctrl_t *)0 || out_port == (unsigned int *)0) return -1;
    i = hub_reg_find(c->hw_slot, hub_slot);
    if (i < 0) return -1;
    *out_port = g_hubs[i].path.root_port;
    return 0;
}

int hype_xhci_hub_child_path(hype_xhci_ctrl_t *c, unsigned int hub_slot, unsigned int port,
                             hype_xhci_devpath_t *out_hub, hype_xhci_devpath_t *out_child,
                             unsigned int *out_speed) {
    uint8_t st[4];
    unsigned int guard, child_speed;
    int i;

    if (c == (hype_xhci_ctrl_t *)0 || out_child == (hype_xhci_devpath_t *)0) return -1;
    i = hub_reg_find(c->hw_slot, hub_slot);
    if (i < 0) return -1;

    /*
     * Reset the port and wait for the reset-complete change bit, exactly as the walk does.
     * A device that has just been plugged in is not addressable until its port is reset --
     * this is not optional tidiness, it is the bus protocol.
     */
    hub_set_port_feature(c, hub_slot, HUB_FEAT_PORT_POWER, port);
    /*
     * #746: CONNECT DEBOUNCE. USB 2.0 7.1.7.3 requires 100ms between a connect being
     * detected and the port being reset, for the electrical connection to settle.
     *
     * The boot walk never needed it -- by the time it runs, everything plugged in has been
     * attached for seconds. A device plugged in at runtime is reset within one 8ms input
     * tick of arriving, and without this the reset lands on a port that has not settled:
     * PORT_ENABLE comes back clear and hype decides nothing arrived.
     */
    delay_ms(100);
    if (hub_get_port_status(c, hub_slot, port, st) != 0) return -1;
    if (!(st[0] & 0x01u)) {
        hype_debug_print("host-xhci: hub slot %u port %u -- gone again before the reset "
                         "(status 0x%02x%02x) [#746]\n", hub_slot, port,
                         (unsigned)st[1], (unsigned)st[0]);
        return -1;
    }
    if (hub_set_port_feature(c, hub_slot, HUB_FEAT_PORT_RESET, port) != 0) return -1;
    /*
     * Wait for the reset in REAL MILLISECONDS, not in spins.
     *
     * This used to be 20 iterations of short_delay() -- a 200,000-step volatile decrement,
     * roughly 120 us -- plus one control transfer each, so about 13 ms of budget in total.
     * USB 2.0 11.5.1.5 has a hub drive reset for 10 ms MINIMUM and allows longer, so 13 ms
     * is not a timeout, it is a coin flip.
     *
     * Boot 21 measured the losing side: 159 failed resets on one hub port, every one of them
     * reporting status 0x0511 -- PORT_RESET still SET, so the hub had not finished. hype was
     * declaring failure on a port that was still resetting normally.
     *
     * 200 ms in 10 ms steps, which is what the port is actually allowed to take. It exits as
     * soon as C_PORT_RESET appears, so a healthy port still costs one step.
     */
    for (guard = 0; guard < 20u; guard++) {
        delay_ms(10);
        if (hub_get_port_status(c, hub_slot, port, st) != 0) break;
        if (st[2] & 0x10u) break; /* C_PORT_RESET */
        if (!(st[0] & 0x10u)) break; /* PORT_RESET cleared without the change bit */
    }
    hub_clear_port_feature(c, hub_slot, HUB_FEAT_C_PORT_RESET, port);
    hub_clear_port_feature(c, hub_slot, HUB_FEAT_C_PORT_CONNECTION, port);
    if (hub_get_port_status(c, hub_slot, port, st) != 0) return -1;
    if (!(st[0] & 0x02u)) {
        hype_debug_print("host-xhci: hub slot %u port %u -- reset did not enable the port "
                         "(status 0x%02x%02x) [#746]\n", hub_slot, port,
                         (unsigned)st[1], (unsigned)st[0]);
        return -1;
    }
    delay_ms(15); /* USB reset recovery before Address Device */

    /* #739: everything on a SuperSpeed hub's downstream ports is SuperSpeed; its 2.0 half
     * is a separate xHCI device, so reading the 2.0 speed bits here would call it full. */
    child_speed = g_hubs[i].ss ? HYPE_USB_SPEED_SUPER : hub_port_speed(st);

    out_child->root_port = g_hubs[i].path.root_port;
    out_child->route = hype_xhci_route_append(g_hubs[i].path.route, g_hubs[i].tier, port);
    out_child->speed = child_speed;
    /* #218: the same TT selection the walk makes -- a LS/FS child of a HS hub is reached
     * through THAT hub's translator, and one under an already-translated hub inherits it. */
    if (hype_xhci_tt_required(g_hubs[i].path.speed, child_speed)) {
        out_child->tt_hub_slot = hub_slot;
        out_child->tt_port = port;
    } else if ((child_speed == HYPE_USB_SPEED_LOW || child_speed == HYPE_USB_SPEED_FULL) &&
               g_hubs[i].path.tt_hub_slot) {
        out_child->tt_hub_slot = g_hubs[i].path.tt_hub_slot;
        out_child->tt_port = g_hubs[i].path.tt_port;
    } else {
        out_child->tt_hub_slot = 0u;
        out_child->tt_port = 0u;
    }
    if (out_hub) *out_hub = g_hubs[i].path;
    if (out_speed) *out_speed = child_speed;
    return 0;
}

/*
 * Read the hub port status for the device at `route`, over a CONTROL transfer.
 *
 * This is the discriminator that silence could never provide. An interrupt-IN endpoint that
 * has gone quiet is either a device with nothing to say or a device hype can no longer hear,
 * and no amount of watching the endpoint separates them -- that ambiguity is why #764 was
 * withdrawn and why a later "went deaf" detector fired on a keyboard nobody was typing on.
 *
 * The PORT tells you directly, and it answers over ep0, so it still answers when the
 * endpoint is dead:
 *
 *   PORT_SUSPEND (bit 2) set    the device put itself to sleep. The operator's Keychron
 *                               advertises Remote Wakeup (bmAttributes 0xa0) and has a
 *                               documented idle power-saving state, and hype has no resume
 *                               path at all -- so it would stay asleep for ever.
 *   PORT_SUSPEND clear          the device is awake and connected, and hype is not hearing
 *                               an endpoint that is running. That is the real deafness.
 *
 * Behind-hub devices only. A root-port device reports its link state in PORTSC PLS instead,
 * which is a different register and a different decode; every device this is aimed at sits
 * behind a hub.
 *
 * Returns 0 and fills st[] plus the hub slot and port on success.
 */
int hype_xhci_port_status_for_route(hype_xhci_ctrl_t *c, unsigned int root_port,
                                    unsigned int route, uint8_t st[4],
                                    unsigned int *out_hub_slot, unsigned int *out_port) {
    unsigned int i, port;

    if (c == (hype_xhci_ctrl_t *)0 || !c->inited || st == (uint8_t *)0) return -1;
    for (i = 0; i < HYPE_XHCI_HUB_MAX; i++) {
        if (!g_hubs[i].used || g_hubs[i].ctrl != c->hw_slot) continue;
        /*
         * ROOT PORT TOO. A route string is only unique WITHIN a root port -- two tier-1 hubs
         * on different root ports both give their port 2 the route 0x00002. Matching on
         * route alone found the wrong hub in boot 24 and read an empty port on it, reporting
         * connected=0 for two devices that were reporting normally at the time.
         */
        if (g_hubs[i].path.root_port != root_port) continue;
        for (port = 1u; port <= g_hubs[i].nports; port++) {
            unsigned int r = 0;
            if (hype_xhci_hub_child_route(c, g_hubs[i].slot, port, &r) != 0) continue;
            if (r != route) continue;
            if (hub_get_port_status(c, g_hubs[i].slot, port, st) != 0) return -1;
            if (out_hub_slot) *out_hub_slot = g_hubs[i].slot;
            if (out_port) *out_port = port;
            return 0;
        }
    }
    return -1;
}

unsigned int hype_xhci_hub_count(void) {
    unsigned int i, n = 0;
    for (i = 0; i < HYPE_XHCI_HUB_MAX; i++) {
        if (g_hubs[i].used) n++;
    }
    return n;
}

int hype_xhci_hub_at(unsigned int i, unsigned int *out_ctrl, unsigned int *out_slot,
                     unsigned int *out_nports, unsigned int *out_ep) {
    if (i >= HYPE_XHCI_HUB_MAX || !g_hubs[i].used) {
        return -1;
    }
    if (out_ctrl) *out_ctrl = g_hubs[i].ctrl;
    if (out_slot) *out_slot = g_hubs[i].slot;
    if (out_nports) *out_nports = g_hubs[i].nports;
    if (out_ep) *out_ep = g_hubs[i].ep;
    return 0;
}

void hype_xhci_hub_forget_ctrl(unsigned int ctrl) {
    unsigned int i;
    for (i = 0; i < HYPE_XHCI_HUB_MAX; i++) {
        if (g_hubs[i].used && g_hubs[i].ctrl == ctrl) {
            g_hubs[i].used = 0u;
        }
    }
}

int hype_xhci_hub_take_change(hype_xhci_ctrl_t *c, unsigned int i, unsigned int *out_port,
                              int *out_connected) {
    uint8_t bitmap[8];
    uint8_t st[4];
    unsigned int port;
    unsigned int len;
    int r;

    if (c == (hype_xhci_ctrl_t *)0 || out_port == (unsigned int *)0 ||
        out_connected == (int *)0 || i >= HYPE_XHCI_HUB_MAX || !g_hubs[i].used ||
        g_hubs[i].ctrl != c->hw_slot || g_hubs[i].ep == 0u) {
        return -1;
    }
    *out_port = 0;
    *out_connected = 0;
    len = g_hubs[i].bitmap_len;
    if (len == 0u || len > sizeof(bitmap)) len = 1u;

    g_hub_polls++;
    r = hype_xhci_int_in_poll(c, g_hubs[i].slot, g_hubs[i].ep, bitmap, len);
    if (r < 0) g_hub_errs++;
    if (r <= 0) {
        return r; /* 0 = nothing changed, the normal case; -1 = a transfer error */
    }
    g_hub_reports++;
    {
        /* The raw bitmap, the first few times. A hub that reports a change hype then fails
         * to act on, and a hub that reports nothing at all, look identical without it --
         * which is exactly where the first version of this went wrong. */
        static unsigned int bm_reported = 0;
        if (bm_reported++ < 8u) {
            hype_debug_print("host-xhci: hub slot %u status bitmap 0x%02x (nports=%u) [#746]\n",
                             g_hubs[i].slot, (unsigned)bitmap[0], g_hubs[i].nports);
        }
    }
    /*
     * The hub's status-change bitmap: bit 0 is the hub itself, bit N is downstream port N
     * (USB 2.0 11.12.4). One port per call -- the endpoint re-arms on the next poll and
     * the hub re-reports anything still outstanding, so draining the rest costs 8ms and
     * keeps this bounded, which matters because it runs from the guest dispatch loop.
     */
    for (port = 1u; port <= g_hubs[i].nports && port < len * 8u; port++) {
        if (!(bitmap[port >> 3] & (1u << (port & 7u)))) {
            continue;
        }
        if (hub_get_port_status(c, g_hubs[i].slot, port, st) != 0) {
            return -1;
        }
        /*
         * #758, on the hub path this time. A port that has just changed is still settling,
         * and one read can catch it mid-transition -- rig 746 read a keyboard's UNPLUG as
         * "something is attached", cleared the change bits in the same breath, and the
         * departure was gone for good. The stale inventory entry then made the re-plug look
         * like a device already present, so it was never re-enumerated either: one bad read
         * cost both halves of the cycle.
         *
         * Read again and require agreement on the connect bit. Disagreement means do not
         * clear and do not classify -- the hub will report the port again and the next pass
         * gets a settled answer. The root-port path does the same, for the same reason.
         */
        {
            uint8_t st2[4];
            if (hub_get_port_status(c, g_hubs[i].slot, port, st2) != 0) {
                return -1;
            }
            if (((st[0] ^ st2[0]) & 0x01u) != 0u) {
                return 0; /* unsettled: leave the change standing and come back to it */
            }
            st[0] = st2[0]; st[1] = st2[1]; st[2] = st2[2]; st[3] = st2[3];
        }
        /*
         * Clear EVERY change bit that is set, not just C_PORT_CONNECTION -- the hub twin
         * of #744's PORTSC write-1-to-clear.
         *
         * #762: a hub reports a port while ANY bit of wPortChange is set (USB 2.0
         * §11.12.4), and one physical unplug sets several: losing the connection also
         * disables and un-suspends the port, each with its own change bit. Clearing only
         * the connection bit left the others standing, so the hub re-reported the same
         * port forever -- measured at 5,504 reports of an unchanging bitmap 0x14 from a
         * single unplug, with the poll never returning to idle.
         *
         * wPortChange is the second half of the 4-byte port status, bit per feature from
         * C_PORT_CONNECTION (§11.24.2.7.2), so the feature selector is 16 + bit.
         */
        {
            unsigned int chg = (unsigned int)st[2] | ((unsigned int)st[3] << 8);
            unsigned int b;
            for (b = 0; b < 5u; b++) {
                if (chg & (1u << b)) {
                    (void)hub_clear_port_feature(c, g_hubs[i].slot,
                                                 HUB_FEAT_C_PORT_CONNECTION + b, port);
                }
            }
            /* Always clear the connection bit, even if the hub reported none set: the
             * port is in this bitmap for a reason, and leaving it uncleared is the
             * failure this exists to prevent. */
            if ((chg & 0x1Fu) == 0u) {
                (void)hub_clear_port_feature(c, g_hubs[i].slot,
                                             HUB_FEAT_C_PORT_CONNECTION, port);
            }
        }
        /* #770: cleared above, but not reported -- hype has given up on this one. */
        if (g_hubs[i].ignore_ports & (1u << (port & 31u))) {
            continue;
        }
        *out_port = port;
        *out_connected = (st[0] & 0x01u) ? 1 : 0;
        return 1;
    }
    return 0;
}

/*
 * Poll for one HID report. Returns 1 when a report was copied out, 0 when none has
 * arrived yet, -1 on a transfer error.
 *
 * The three-way return is the whole point. A keyboard is idle almost all the time, so
 * "nothing yet" is the NORMAL case and must be distinguishable from a fault -- the
 * bulk path collapses both into -1, which is right for a disk read that must succeed
 * and wrong here: treating idle as an error would disable the keyboard the moment
 * nobody typed, and treating a fault as idle would hide a dead endpoint forever.
 *
 * Deliberately a SHORT budget. This is called from the guest dispatch loop, so a long
 * spin waiting for a keystroke would stall the guest -- the cost of missing a report
 * is that it is picked up on the next pass a moment later.
 */
/*
 * #734: name the completion code of a failed report transfer, the first few times.
 *
 * The per-endpoint DIAG counters say only "errors=1", and a Stall, a USB Transaction
 * Error and a Babble need different fixes -- one bare count cannot tell them apart, and
 * an input device that fails once per boot gives exactly one chance to read it.
 */
static void int_in_report_error(unsigned int slot, unsigned int dci, uint64_t trb,
                                uint32_t cc) {
    static unsigned int reported = 0;

    if (reported++ < 8u) {
        hype_debug_print("host-xhci: interrupt-IN transfer FAILED slot=%u ep=%u trb=0x%llx "
                         "cc=%u [#734]\n", slot, dci, (unsigned long long)trb, cc);
    }
}

static int ep_recover(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int dci, uint8_t *ring,
                      unsigned int *enq, unsigned int *cyc);
static int ep_recover_halted(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int dci,
                             uint8_t *ring, unsigned int *enq, unsigned int *cyc);

/*
 * #734: an error completion on an interrupt endpoint HALTS it (xHCI 4.10.2.1) -- the
 * controller stops running that ring and ignores its doorbell until software issues
 * Reset Endpoint and Set TR Dequeue Pointer (xHCI 4.6.8/4.6.10). Clearing `armed` and
 * ringing again, which is all this path used to do, arms into a dead endpoint forever.
 *
 * That is exactly what the 2026-08-27 09:46 boot recorded: ONE error each on the
 * keyboard (cc=4, USB Transaction Error) and the mouse (cc=3, Babble Detected), then
 * `errors=1` frozen while `polls` climbed past 300000 and `reports` stayed at 0. One
 * error per boot is not a device that is failing; it is a device that got one chance.
 *
 * Rate-limited, because recovery is three synchronous commands and this runs from the
 * guest dispatch loop: an endpoint failing every 1ms must not be reset thousands of
 * times a second. After INT_IN_MAX_RECOVERIES back-to-back failures it BACKS OFF rather
 * than dying -- the 10:42 boot's keyboard is exactly the case that must not be written
 * off forever, since a device that starts answering later is still the operator's only
 * keyboard. The counters reset on the next good report.
 */
#define INT_IN_MAX_RECOVERIES 2u
#define INT_IN_BACKOFF_POLLS  200000u

/*
 * Revive an endpoint that is RUNNING, attached, awake -- and not delivering.
 *
 * This is the fix the last eighteen boots were narrowing down to, and it is only defensible
 * now because boot 25 removed every alternative. At the moment two devices went permanently
 * silent, their hub ports read connected=1 enabled=1 SUSPENDED=0 -- attached and awake, not
 * asleep -- with the endpoint armed, the doorbell rung, no error, no Host Controller Event,
 * and nothing lost in software. And boot 23 showed what fixes it: a hot-plug, which tears the
 * endpoint down and builds it again. This does the same thing without the hand.
 *
 * Stop Endpoint then Set TR Dequeue Pointer, per xHCI 4.6.9 and 4.6.10. NOT Reset Endpoint:
 * that is for a HALTED endpoint and returns Context State Error against a running one, which
 * is the mistake ep_recover_halted() exists to avoid. The ring restarts at index 0 with
 * cycle 1 and the queue state is cleared, so the next poll re-arms from scratch.
 *
 * Triggered on silence, which cannot distinguish a deaf endpoint from an idle one -- that
 * ambiguity is why #764's detector was withdrawn. The difference is what happens on a false
 * positive: a detector REPORTED a fault that was not there, while reviving an idle endpoint
 * costs two commands and loses nothing, because an idle endpoint has no reports in flight.
 * The backoff below keeps that cost bounded for a keyboard nobody is using.
 */
/*
 * Boot 34: which of the three ways a revive fails.
 *
 * int_in_revive() had three bare `return -1`s, so an endpoint that could not be rebuilt said
 * only "revive_fail is going up" -- and the run showed both interrupt-IN devices on one
 * controller permanently deaf with revive_fail climbing and cmd_timeouts at ZERO, which rules
 * out the boot-31 mechanism and leaves nothing to distinguish the remaining three. A recovery
 * that cannot work must say what stopped it, or the next run measures the same nothing.
 *
 * `cc` is the completion code where the controller returned one (a Set TR Dequeue against an
 * endpoint that is not Stopped answers Context State Error, 19), and 0 where the step never
 * completed at all.
 */
static void revive_report_failure(unsigned int ctrl, unsigned int slot, unsigned int dci,
                                  const char *what, uint32_t cc) {
    static unsigned int reported = 0;

    if (reported++ < 12u) {
        if (cc != 0u) {
            hype_debug_print("host-xhci: REVIVE FAILED ctrl%u slot=%u ep=%u -- %s (cc=%u) "
                             "[#775]\n", ctrl, slot, dci, what, cc);
        } else {
            hype_debug_print("host-xhci: REVIVE FAILED ctrl%u slot=%u ep=%u -- %s [#775]\n",
                             ctrl, slot, dci, what);
        }
    }
}

static int int_in_revive(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int dci,
                         xhci_int_in_hw_t *iin) {
    xhci_hw_t *hw = HW(c);
    uint32_t cmd[4], evt[4];
    unsigned int i;

    hype_xhci_trb_stop_endpoint(cmd, slot, dci, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) {
        revive_report_failure(c->log_id, slot, dci, "Stop Endpoint did not complete", 0u);
        return -1;
    }

    for (i = 0; i < XPAGE; i++) { iin->ring[i] = 0; }
    ring_init_link(iin->ring);
    iin->enq = 0;
    iin->cyc = 1;
    hype_xhci_trb_set_tr_dequeue(cmd, phys(iin->ring) | 1u /* DCS=1 */, slot, dci,
                                 (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) {
        revive_report_failure(c->log_id, slot, dci, "Set TR Dequeue did not complete", 0u);
        return -1;
    }
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) {
        revive_report_failure(c->log_id, slot, dci, "Set TR Dequeue was REFUSED",
                              hype_xhci_event_cc(evt));
        return -1;
    }

    /* Everything the old ring owned is gone with it: outstanding TRBs name addresses the
     * controller will never complete now, and queued completions belong to a ring that no
     * longer exists. The next poll re-arms from an empty queue. */
    hype_iiq_reset(&iin->q);
    iin->revives++;
    return 0;
}

static void int_in_recover(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int dci,
                           xhci_int_in_hw_t *iin) {
    static unsigned int backoff_reported = 0;
    static unsigned int recover_reported = 0;

    hype_iiq_reset(&iin->q);
    iin->silent_polls = 0;
    if (iin->recoveries >= INT_IN_MAX_RECOVERIES) {
        iin->backoff = INT_IN_BACKOFF_POLLS;
        if (backoff_reported++ < 4u) {
            hype_debug_print("host-xhci: interrupt-IN slot=%u ep=%u failed %u recoveries in a "
                             "row -- backing off for %u polls [#734]\n", slot, dci,
                             (unsigned)INT_IN_MAX_RECOVERIES, (unsigned)INT_IN_BACKOFF_POLLS);
        }
        iin->recoveries = 0;
        return;
    }
    iin->recoveries++;
    if (recover_reported++ < 8u) {
        hype_debug_print("host-xhci: interrupt-IN slot=%u ep=%u halted -- recovering (%u/%u) "
                         "[#734]\n", slot, dci, iin->recoveries, (unsigned)INT_IN_MAX_RECOVERIES);
    }
    (void)ep_recover_halted(c, slot, dci, iin->ring, &iin->enq, &iin->cyc);
}

int hype_xhci_int_in_poll(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int ep_addr,
                          uint8_t *out, unsigned int len) {
    /*
     * #764 capture: the operator reports a DASHBOARD FREEZE at the moment the keyboard
     * stops. A freeze is something blocking, and the input tick's only blocking calls are
     * in here -- endpoint recovery issues commands and waits on them. Timing the whole poll
     * says whether the stall is here at all, and which endpoint owns it, which no counter
     * in the periodic DIAG can.
     */
    uint64_t t_enter = rdtsc_now();
    int poll_rc = int_in_poll_body(c, slot, ep_addr, out, len);
    if (g_tsc_hz != 0u) {
        uint64_t took = rdtsc_now() - t_enter;
        if (took > g_tsc_hz / 50u) { /* 20 ms -- an order above any healthy poll */
            static unsigned int slow_n = 0;
            if (slow_n++ < 32u) {
                hype_debug_print("host-xhci: #764 SLOW POLL slot=%u ep=0x%02x took %llu ms "
                                 "(rc=%d) -- this is what a dashboard freeze is made of\n",
                                 slot, ep_addr,
                                 (unsigned long long)((took * 1000ull) / g_tsc_hz), poll_rc);
            }
        }
    }
    return poll_rc;
}

/*
 * #773: arm one transfer on this endpoint and ring its doorbell.
 *
 * Extracted so a claim can re-arm IMMEDIATELY instead of waiting for the next poll.
 *
 * An interrupt endpoint carries one transfer at a time, and hype used to leave it unarmed
 * from the moment a report was claimed until the next tick came round -- up to 8 ms at
 * 125 Hz. A HID boot report is a STATE SNAPSHOT, not an event log, so the device has
 * nowhere to record what happened while nothing was armed: a key pressed and released
 * inside that window is simply absent from the next report, and hype's diff never sees it.
 *
 * That is the operator's "the faster i typed, the more it missed". It is not the tick being
 * slow -- the tick was measured at 122-136 Hz, which is what it asks for. It is the endpoint
 * being deaf for most of each interval between ticks.
 */
static void int_in_arm(hype_xhci_ctrl_t *c, xhci_int_in_hw_t *iin, unsigned int slot,
                       unsigned int dci, unsigned int len) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    unsigned int xfer = (iin->mps != 0u && iin->mps <= sizeof(iin->report[0])) ? iin->mps : len;
    uint64_t my_trb = phys(iin->ring) + (uint64_t)iin->enq * HYPE_XHCI_TRB_BYTES;
    unsigned int buf;
    uint32_t t[4];
    unsigned int i;

    /*
     * Take a buffer nothing still owns. A completion sitting in the queue is still holding
     * the buffer its report was DMA'd into, so a buffer is NOT free merely because its
     * transfer retired -- handing that one to the controller would zero it and DMA over the
     * report before the poll copied it out.
     */
    buf = hype_iiq_reserve(&iin->q);
    if (buf >= HYPE_INT_IN_DEPTH) {
        return; /* every buffer is spoken for -- arm again once a report has been read */
    }
    for (i = 0; i < sizeof(iin->report[0]); i++) iin->report[buf][i] = 0;
    hype_xhci_trb_normal(t, phys(iin->report[buf]), xfer, (int)iin->cyc);
    ring_enqueue(iin->ring, &iin->enq, &iin->cyc, t);
    wr32(bar, hype_xhci_doorbell_offset(c->dboff, slot), dci);
    if (!hype_iiq_commit(&iin->q, buf, my_trb, xfer)) {
        return; /* queue full: the TRB is enqueued but unowned -- cannot happen, see check */
    }
    iin->arms++;
}

/* #775: top the queue back up to HYPE_INT_IN_DEPTH. */
static void int_in_fill(hype_xhci_ctrl_t *c, xhci_int_in_hw_t *iin, unsigned int slot,
                        unsigned int dci, unsigned int len) {
    while (iin->q.inflight < HYPE_INT_IN_DEPTH) {
        unsigned int before = iin->q.inflight;
        int_in_arm(c, iin, slot, dci, len);
        if (iin->q.inflight == before) break; /* could not arm -- do not spin */
    }
}

static int int_in_poll_body(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int ep_addr,
                            uint8_t *out, unsigned int len) {
    xhci_hw_t *hw = HW(c);
    volatile uint8_t *bar;
    unsigned int dci = hype_xhci_ep_dci(ep_addr);
    xhci_int_in_hw_t *iin;
    uint32_t evt[4];
    unsigned int spins = 0;
    uint64_t my_trb;
    unsigned int i;

    if (!c->inited || slot == 0u || out == (uint8_t *)0 || len == 0u) {
        return -1;
    }
    /* Lookup only: an endpoint that was never configured has no ring to poll, and must
     * not be handed a free block here. */
    iin = iin_hw_for(c, slot, dci, 0);
    if (iin == (xhci_int_in_hw_t *)0 || len > sizeof(iin->report)) {
        return -1;
    }
    if (iin->backoff != 0u) {
        iin->backoff--;
        return 0; /* idle, not an error -- the endpoint is resting, not gone */
    }
    bar = (volatile uint8_t *)(uintptr_t)c->bar;
    my_trb = phys(iin->ring) + (uint64_t)iin->enq * HYPE_XHCI_TRB_BYTES;
    /*
     * Count silence per POLL OF THIS ENDPOINT, not per poll that found the ring empty.
     *
     * The first version incremented inside the idle branch, which is only reached when the
     * whole shared event ring had nothing on it. With eight interrupt-IN endpoints and hub
     * status endpoints reporting periodically, the ring is frequently NOT empty, so a deaf
     * endpoint's counter advanced far slower than its poll count. Boot 27 measured the
     * consequence: the Pico sat silent for 28,632 polls, needing 8,000, and was revived
     * exactly once -- the trigger it needed never came round again.
     *
     * A report resets this, wherever the completion was routed from.
     */
    iin->silent_polls++;
    /*
     * Evaluated on EVERY poll of this endpoint, not only when the shared ring happens to be
     * empty. The check used to live in the idle branch, so a poll that dequeued ANOTHER
     * endpoint's event returned before reaching it -- and boot 28 showed exactly that: the
     * Pico went deaf and revived ZERO times while a hub endpoint reviving in a loop kept the
     * ring busy enough that the Pico's condition was almost never tested. Counting silence
     * correctly is useless if the test only runs when the bus is quiet.
     */
    if (HYPE_INT_IN_SILENCE_REVIVE && iin->q.inflight != 0u &&
        iin->silent_polls >= iin->revive_after) {
        unsigned int after = iin->revive_after;
        iin->silent_polls = 0;
        if (int_in_revive(c, slot, dci, iin) == 0) {
            if (iin->revive_after < HYPE_INT_IN_SILENT_MAX * 8u) {
                iin->revive_after *= 2u;
            }
            if (iin->revives <= 8ull) {
                hype_debug_print("host-xhci: REVIVE ctrl%u slot=%u ep=%u -- silent for %u "
                                 "polls (next at %u). Stop Endpoint + Set TR Dequeue, ring "
                                 "restarted (revive %llu, reports so far %llu) [#775]\n",
                                 c->log_id, slot, dci, after, iin->revive_after, iin->revives,
                                 iin->reports);
            }
        } else {
            /*
             * A revive that CANNOT run is the thing boot 31 could not see. int_in_revive()
             * needs two commands, both timed out against a stopped command ring, and this
             * branch used to do nothing at all -- so an endpoint deaf for 74 minutes
             * reported revives=0 and looked as though the trigger had never fired.
             */
            iin->revive_fails++;
            if (iin->revive_fails <= 8ull) {
                hype_debug_print("host-xhci: REVIVE FAILED ctrl%u slot=%u ep=%u -- silent for "
                                 "%u polls and the commands did not complete (failure %llu, "
                                 "reports so far %llu). This endpoint is deaf and hype "
                                 "cannot rebuild it [#775]\n",
                                 c->log_id, slot, dci, after, iin->revive_fails, iin->reports);
            }
        }
        int_in_fill(c, iin, slot, dci, len);
        return 0;
    }

    /* Arm exactly one transfer on THIS endpoint, and only when none is outstanding.
     *
     * #734: the TRB is sized by the ENDPOINT's wMaxPacketSize, not by the caller's
     * buffer. A device that sends more than the TRB asked for is a Babble Detected
     * (cc=3), which halts the endpoint -- and the mouse's endpoint is mps=64 while the
     * caller wants 8 bytes of boot-protocol packet. The block's report buffer is sized
     * for the largest mps configure-time accepts, so the extra bytes have somewhere to
     * land; only the first `len` are handed back. */
    /*
     * #764: re-arm an endpoint whose completion was LOST, without touching one that is
     * merely idle.
     *
     * Silence alone cannot tell those apart. An interrupt IN transfer stays outstanding
     * while the device NAKs, so "armed, no completion" is exactly what a keyboard nobody
     * is typing on looks like. Re-arming on a timer would queue a second TRB every time
     * and refill the ring -- #217's original bug, arrived at slowly.
     *
     * The controller's own TR Dequeue Pointer is the discriminator. It names the next TRB
     * the controller will process: still equal to ours means the transfer is genuinely
     * outstanding and the device is simply quiet; advanced past ours means the TRB was
     * consumed, a completion was generated, and hype did not see it -- which is the case
     * `armed` would otherwise latch on forever.
     *
     * Checked only after a long silence, so a healthy endpoint pays one context read per
     * HYPE_INT_IN_SILENT_MAX polls and nothing else.
     *
     * NOT ACTED ON yet -- see the body. It reports; it does not re-arm.
     */
    /*
     * #764 WITHDRAWN: hype cannot compare against the controller's dequeue pointer here.
     *
     * The TR Dequeue Pointer in the OUTPUT Endpoint Context is only architecturally valid
     * when the endpoint is Stopped or Halted (xHCI 4.12.2). While it is Running the
     * controller caches it and need not write it back, so a read returns whatever was there
     * last -- in practice the ring base written by Configure Endpoint.
     *
     * Every divergence this reported on real hardware read deq=+0x0, on endpoints that went
     * on working: two in boot 16 (the keyboard reported 1,326 times afterwards) and three in
     * boot 17. It was comparing a stale field against a live cursor and calling the
     * difference a fault.
     *
     * It was also expensive in the worst place. The report is nine debug lines printed from
     * inside the poll, and boot 17 measured that as 26 ms and 45 ms stalls in the input
     * path -- the diagnostic was causing the hitching it existed to explain.
     *
     * The routing counters below (handed/deliv/own/to_park) are kept: they cost nothing, and
     * they are what actually distinguished a lost completion from one that never happened.
     */
    /*
     * #764: the silence watchdog is withdrawn with the check it depended on.
     *
     * It compared the controller's dequeue pointer against ours, and that field is only
     * architecturally valid on a Stopped or Halted endpoint (xHCI 4.12.2) -- on a Running
     * one it reads back whatever Configure Endpoint left, which is the ring base. Every
     * report it produced on hardware said deq=+0x0, on endpoints that carried on working.
     *
     * Silence on its own cannot stand in for it either: an interrupt IN transfer stays
     * outstanding while the device NAKs, so "armed and quiet" is exactly a keyboard nobody
     * is typing on. Re-arming on a timer would queue a second TRB each time and refill the
     * ring, which is #217 arrived at slowly.
     *
     * A real deaf endpoint is now far less likely anyway -- #761 stopped foreign transfer
     * events being dropped, #766 restored strict TRB attribution, and #773 closed the
     * unarmed window. If one recurs, the honest recovery is Stop Endpoint (which makes the
     * dequeue pointer valid to read) followed by Set TR Dequeue, not a guess from here.
     */
    /* #775: keep the queue topped up, so one lost completion costs one report. */
    int_in_fill(c, iin, slot, dci, len);
    my_trb = (iin->q.inflight != 0u) ? iin->q.pend_trb[0] : 0ull;

    /*
     * A completion already retired and queued for this endpoint -- by another endpoint's
     * poll, by the global pump, or by this endpoint's own previous pass. Checked first, and
     * it is now a QUEUE: the single slot it replaces could hold one completion, so a second
     * arriving before this endpoint was polled had nowhere to go but the evicting parked
     * table, which is how an endpoint went permanently deaf.
     */
    {
        hype_iiq_completion_t comp;
        if (hype_iiq_take(&iin->q, &comp)) {
            uint32_t cc = comp.cc;
            unsigned int buf = comp.buf, xlen = comp.actual;
            if (hype_xhci_cc_is_stopped(cc)) {
                iin->stopped_ccs++;
                int_in_fill(c, iin, slot, dci, len);
                return 0; /* cancelled by our own Stop Endpoint: no report, no recovery */
            }
            if (cc != HYPE_XHCI_CC_SUCCESS && cc != HYPE_XHCI_CC_SHORT_PACKET) {
                int_in_report_error(slot, dci, my_trb, cc);
                int_in_recover(c, slot, dci, iin);
                return -1;
            }
            iin->recoveries = 0;
            iin->backoff = 0;
            iin->deliv++;
            int_in_note_claim(iin, my_trb, cc);
            /* Copy BEFORE re-arming: int_in_fill may hand this buffer straight back to the
             * controller once the FIFO no longer holds it. Bytes past what the device
             * actually sent stay zero, because the buffer is zeroed at arm time. */
            for (i = 0; i < len; i++) out[i] = (i < xlen) ? iin->report[buf][i] : 0u;
            int_in_fill(c, iin, slot, dci, len); /* #773/#775: top up at once */
            return 1;
        }
    }

    /*
     * #759: a POLL asks "is there an event right now", and one read of the cycle bit
     * answers it. This used to spend SPIN/1024 -- 19,531 iterations -- and when the
     * endpoint has nothing to say, which is the overwhelmingly common case, it burned
     * every one of them before returning idle.
     *
     * That was tolerable at two interrupt-IN endpoints. #746 arms one per hub, and the
     * 5950X has five, so the desktop polls eight endpoints a tick and paid ~156,000
     * uncached event-ring reads per tick to learn nothing. It showed up as visible
     * hitching, and it throttled the 125 Hz input tick badly enough that the hot-plug
     * sweep and the HID drain ran at a few hertz.
     *
     * Spinning is right when WAITING for a transfer known to be outstanding -- which is
     * what cmd_submit_wait and next_event_timed do, and they are unchanged. It is wrong
     * here: the transfer is asynchronous and its completion will still be there on the
     * next poll, or waiting in the parked table.
     */
    if (next_event_budget(hw, bar, c->rtsoff, evt, XHCI_POLL_PEEK, &spins) != 0) {
        /*
         * Idle -- the common case, NOT an error. But an endpoint that stays idle while
         * armed is either a device with nothing to say or one hype can no longer hear, and
         * boot 25 established that the second case is real: attached, awake, armed, and
         * permanently silent. Rebuild it rather than wait for a hand on the cable.
         */
        return 0;
    }
    if (hype_xhci_event_slot_id(evt) != slot || hype_xhci_event_ep_id(evt) != dci) {
        unsigned int oslot = hype_xhci_event_slot_id(evt);
        unsigned int odci = hype_xhci_event_ep_id(evt);
        /*
         * #761: if it belongs to another INT-IN endpoint hype owns, hand it straight to
         * that endpoint's own block. The parked table is 8 entries with round-robin
         * eviction and was written for the MSC datapath (#266); with up to
         * HYPE_XHCI_INT_IN_MAX endpoints polling, a hub's rare completion is reliably
         * evicted by a keyboard's frequent ones long before the hub polls again -- and a
         * lost int-in completion is permanent, because `armed` never clears and the
         * endpoint is never re-armed.
         */
        (void)oslot; (void)odci;
        route_foreign_event(c, hw, evt);
        return 0;
    }
    /*
     * This endpoint's own event. It goes through the same retire-and-queue path as a routed
     * one, so there is exactly one place where an interrupt-IN transfer is retired.
     */
    if (!int_in_deliver(iin, hype_xhci_event_trb_ptr(evt), hype_xhci_event_cc(evt),
                        hype_xhci_event_xfer_residue(evt))) {
        return 0; /* stale or duplicate: counted in `lost`, retires nothing */
    }
    {
        hype_iiq_completion_t comp;
        uint32_t cc;
        unsigned int buf, xlen;
        if (!hype_iiq_take(&iin->q, &comp)) {
            return 0; /* cannot happen: deliver queued one */
        }
        cc = comp.cc; buf = comp.buf; xlen = comp.actual;
        if (hype_xhci_cc_is_stopped(cc)) {
            iin->stopped_ccs++;
            int_in_fill(c, iin, slot, dci, len);
            return 0; /* cancelled by our own Stop Endpoint: no report, no recovery */
        }
        if (cc != HYPE_XHCI_CC_SUCCESS && cc != HYPE_XHCI_CC_SHORT_PACKET) {
            int_in_report_error(slot, dci, my_trb, cc);
            int_in_recover(c, slot, dci, iin);
            return -1;
        }
        iin->recoveries = 0;
        iin->backoff = 0;
        /* #764 capture: the event carries the TRB it completed, so record THAT rather than
         * what hype thought was outstanding -- a claim from a TRB never armed is the thing
         * to see. */
        iin->own++;
        int_in_note_claim(iin, hype_xhci_event_trb_ptr(evt), cc);
        for (i = 0; i < len; i++) out[i] = (i < xlen) ? iin->report[buf][i] : 0u;
    }
    int_in_fill(c, iin, slot, dci, len); /* #773/#775 */
    return 1;
}

static int bulk_xfer(hype_xhci_ctrl_t *c, uint8_t *ring, unsigned int *enq, unsigned int *cyc,
                     unsigned int slot, unsigned int dci, uint64_t buf_phys, unsigned int len) {
    xhci_hw_t *hw = HW(c);
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    uint32_t t[4], evt[4];
    unsigned int guard = 64u;
    uint64_t my_trb = phys(ring) + (uint64_t)(*enq) * HYPE_XHCI_TRB_BYTES;
    static unsigned int stale_reported = 0;
    static unsigned int short_reported = 0;

    /*
     * Transfer rings reuse a physical TRB address every 15 submissions. A parked
     * completion has no generation field, so an entry left from the previous use of
     * this address must not be allowed to complete the new transfer.
     */
    if (hype_xhci_parked_drop_exact(&hw->parked, slot, dci, my_trb)) {
        if (stale_reported++ < 16u) {
            hype_debug_print("host-xhci: #377 dropped stale parked completion before "
                             "TRB reuse (slot=%u ep=%u trb=0x%llx)\n",
                             slot, dci, (unsigned long long)my_trb);
        }
    }

    hype_xhci_trb_normal(t, buf_phys, len, (int)(*cyc));
    ring_enqueue(ring, enq, cyc, t);
    wr32(bar, hype_xhci_doorbell_offset(c->dboff, slot), dci);
    /*
     * #266: after the stale-entry purge above, only a completion parked after this
     * submission can match. Claim it before spinning. Otherwise the transfer would burn
     * the whole budget waiting for an event that another poll already consumed.
     */
    {
        uint32_t parked_cc = 0;
        uint32_t parked_residue = 0;
        if (hype_xhci_parked_take(&hw->parked, slot, dci, my_trb, &parked_cc,
                                  &parked_residue)) {
            if (!hype_xhci_xfer_exact_ok(parked_cc, parked_residue) && short_reported++ < 16u) {
                hype_debug_print("host-xhci: #377 rejected incomplete parked transfer "
                                 "(slot=%u ep=%u trb=0x%llx cc=%u residue=%u len=%u)\n",
                                 slot, dci, (unsigned long long)my_trb, parked_cc,
                                 parked_residue, len);
            }
            return hype_xhci_xfer_exact_ok(parked_cc, parked_residue) ? 0 : -1;
        }
    }
    while (guard-- != 0u) {
        if (next_event(hw, bar, c->rtsoff, evt) != 0) {
            /*
             * #266 THE decisive measurement. Every observed failure says "no event
             * arrived", which is a stall rather than a mis-ordering -- but a
             * completion that is merely very late and one that never comes need
             * opposite fixes (wait differently vs find the missed doorbell / ERDP /
             * interrupter bug), and a fixed-budget poll cannot tell them apart.
             *
             * So re-poll with 10x the budget before giving up. Consuming the event if
             * it does turn up is fine and is what we want: the transfer has already
             * failed, recovery follows, and recovery drains the ring anyway.
             */
            unsigned int extra = 0;
            uint32_t late[4];
            int arrived = next_event_budget(hw, bar, c->rtsoff, late, SPIN * 10u, &extra);
            hype_debug_print("host-xhci: #266 bulk TIMEOUT waiting for slot=%u ep=%u trb=0x%llx "
                             "(%u foreign seen this boot)\n",
                             slot, dci, (unsigned long long)my_trb, g_bulk_foreign_seen);
            if (arrived == 0) {
                hype_debug_print("host-xhci: #266   -> the event WAS only LATE: arrived after "
                                 "%u more spins (%ux the normal budget); type=%u slot=%u ep=%u "
                                 "trb=0x%llx cc=%u. FIX = waiting strategy, not a lost event.\n",
                                 extra, (extra / SPIN) + 1u, hype_xhci_trb_type(late),
                                 hype_xhci_event_slot_id(late), hype_xhci_event_ep_id(late),
                                 (unsigned long long)hype_xhci_event_trb_ptr(late),
                                 hype_xhci_event_cc(late));
            } else {
                hype_debug_print("host-xhci: #266   -> STILL nothing after 11x the budget: the "
                                 "completion is NOT merely late. Look upstream (doorbell / ERDP / "
                                 "interrupter), not at the wait.\n");
            }
            hype_debug_print("host-xhci: #266   event latency so far: max=%u spins, mean=%llu "
                             "over %llu events (normal budget %u)\n",
                             g_evt_spin_max,
                             (g_evt_count != 0ull) ? (g_evt_spin_sum / g_evt_count) : 0ull,
                             g_evt_count, (unsigned int)SPIN);
            return -1;
        }
        if (hype_xhci_trb_type(evt) == HYPE_XHCI_TRB_TRANSFER_EVENT) {
            /*
             * #254: the event must be for THIS TRB on THIS endpoint. This
             * controller (1022:15e0) demonstrably delivers events late; the
             * old accept-anything wait let a late completion for an abandoned
             * transfer stand in for the current one, after which host and
             * device disagreed about the BOT stage and a CBW was written to
             * the medium as sector data.
             */
            if (hype_xhci_event_slot_id(evt) == slot && hype_xhci_event_ep_id(evt) == dci &&
                hype_xhci_event_trb_ptr(evt) == my_trb) {
                unsigned int cc = hype_xhci_event_cc(evt);
                unsigned int residue = hype_xhci_event_xfer_residue(evt);
                if (!hype_xhci_xfer_exact_ok(cc, residue) && short_reported++ < 16u) {
                    hype_debug_print("host-xhci: #377 rejected incomplete transfer "
                                     "(slot=%u ep=%u trb=0x%llx cc=%u residue=%u len=%u)\n",
                                     slot, dci, (unsigned long long)my_trb, cc, residue, len);
                }
                return hype_xhci_xfer_exact_ok(cc, residue) ? 0 : -1;
            }
            {
                static int s1 = 0;
                g_bulk_foreign_seen++;
                /*
                 * #266: PARK it, do not discard it. This controller delivers events late
                 * (the command ring already needed the same leniency, #254), so this is
                 * usually the OTHER direction's completion arriving out of order -- not
                 * corruption. Discarding it stranded the transfer that was waiting on its
                 * own endpoint, BOT declared a lost completion, and the write path died.
                 *
                 * Attribution stays strict: the parked event can only ever be claimed by
                 * the exact (slot, dci, trb) it names, so no data can land in the wrong
                 * buffer. Strictness protects attribution; it must not demand an arrival
                 * ORDER this controller declines to provide.
                 */
                hype_xhci_parked_put(&hw->parked, hype_xhci_event_slot_id(evt),
                                     hype_xhci_event_ep_id(evt), hype_xhci_event_trb_ptr(evt),
                                     hype_xhci_event_cc(evt),
                                     hype_xhci_event_xfer_residue(evt));
                if (s1++ < 8) {
                    hype_debug_print("host-xhci: #266 parking out-of-order transfer event "
                                     "(slot=%u ep=%u trb=0x%llx cc=%u, wanted slot=%u ep=%u "
                                     "trb=0x%llx)\n",
                                     hype_xhci_event_slot_id(evt), hype_xhci_event_ep_id(evt),
                                     (unsigned long long)hype_xhci_event_trb_ptr(evt),
                                     hype_xhci_event_cc(evt), slot, dci,
                                     (unsigned long long)my_trb);
                }
            }
            continue;
        }
    }
    hype_debug_print("host-xhci: #266 bulk_xfer GAVE UP on slot=%u ep=%u trb=0x%llx after 64 "
                     "events, none matching (%u foreign total)\n",
                     slot, dci, (unsigned long long)my_trb, g_bulk_foreign_seen);
    return -1;
}

/*
 * #266: consume and discard every event currently queued, without waiting for
 * more. After a reset there is no legitimate pending event: anything still in the
 * ring was generated by the state we just tore down.
 *
 * This matters because ep_recover() restarts the transfer ring at index 0, so the
 * retry's first TRB lands at the SAME physical address the abandoned transfer's TRB
 * occupied. A late completion for the abandoned transfer therefore carries a
 * trb_ptr that matches the fresh TRB exactly, and the strict match in bulk_xfer --
 * which exists precisely to stop a foreign event standing in for ours (#254) --
 * cannot tell them apart. Draining first removes the ambiguity rather than trying
 * to resolve it, which is the only option available: the event carries nothing that
 * distinguishes generations.
 *
 * Returns the number of events discarded, which is worth logging: a non-zero count
 * is direct evidence of the late-delivery behaviour this controller has already
 * demonstrated on its command ring.
 */
static unsigned int drain_events(xhci_hw_t *hw, volatile uint8_t *bar, uint32_t rtsoff) {
    unsigned int drained = 0;
    for (;;) {
        uint32_t d3 = trb_dw(hw->evt_ring, hw->evt_deq, 3);
        if ((int)(d3 & 1u) != (int)hw->evt_cyc) {
            break; /* ring empty: cycle bit says the controller has not written here */
        }
        hype_dma_cqueue_advance(&hw->evt_deq, &hw->evt_cyc, RING_TRBS);
        drained++;
        if (drained > RING_TRBS) break; /* paranoia: never spin forever on a wedged ring */
    }
    if (drained != 0u) {
        wr64(bar, hype_xhci_ir0_offset(rtsoff, HYPE_XHCI_IR_ERDP),
             (phys(hw->evt_ring) + (uint64_t)hw->evt_deq * HYPE_XHCI_TRB_BYTES) | (1u << 3));
    }
    return drained;
}

/*
 * #254: endpoint + ring recovery after a failed transfer. A timed-out TRB is
 * still owned by the controller; issuing more work on the same ring invites a
 * late completion to collide with it. Sequence per xHCI 4.6.9/4.6.8/4.6.10:
 * Stop Endpoint (Running -> Stopped), Reset Endpoint (Halted -> Stopped;
 * harmlessly errors when not halted), then Set TR Dequeue Pointer to a
 * zeroed, restarted ring. Software ring state resets to enq=0/cycle=1.
 */
/*
 * #289: report each recovery step and its completion code.
 *
 * Stop and Reset Endpoint legitimately fail (the endpoint may already be stopped, or not
 * halted), so their result cannot gate the recovery -- but "we do not act on it" is not a
 * reason to throw the evidence away. bot_recover() ran three times on real hardware and
 * the write path never came back, and the log said only "BOT reset recovery" without
 * saying which of the three commands actually completed. On this controller, whose late
 * completions already forced #254's lenient command matching, "the command completed but
 * we gave up waiting" is a live possibility that this distinguishes: a TIMEOUT and a
 * completion with a non-success code look identical in a log that reports neither.
 */
static void ep_recover_step(hype_xhci_ctrl_t *c, uint32_t cmd[4], const char *what,
                            unsigned int slot, unsigned int dci) {
    uint32_t evt[4];

    if (cmd_submit_wait(c, cmd, evt) != 0) {
        hype_debug_print("host-xhci: #289 %s (slot=%u dci=%u) NO COMPLETION (timed out)\n", what,
                         slot, dci);
        return;
    }
    hype_debug_print("host-xhci: #289 %s (slot=%u dci=%u) cc=%u%s\n", what, slot, dci,
                     hype_xhci_event_cc(evt),
                     hype_xhci_event_cc(evt) == HYPE_XHCI_CC_SUCCESS ? " (success)" : "");
}

static int ep_recover(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int dci, uint8_t *ring,
                      unsigned int *enq, unsigned int *cyc) {
    xhci_hw_t *hw = HW(c);
    uint32_t cmd[4], evt[4];
    unsigned int i;

    hype_xhci_trb_stop_endpoint(cmd, slot, dci, (int)hw->cmd_cyc);
    ep_recover_step(c, cmd, "Stop Endpoint", slot, dci);
    hype_xhci_trb_reset_endpoint(cmd, slot, dci, (int)hw->cmd_cyc);
    ep_recover_step(c, cmd, "Reset Endpoint", slot, dci);

    for (i = 0; i < XPAGE; i++) { ring[i] = 0; }
    /*
     * #289: put the Link TRB back. The wipe above clears the whole page INCLUDING the
     * toggle-cycle Link at the last slot that ring_init_link() installed at configure
     * time, so without this a recovered ring is structurally different from a freshly
     * configured one -- it has no Link until the producer happens to wrap and write one.
     * ring_enqueue() does write it on wrap, which is why this has not visibly broken, but
     * "correct only because nobody reached the end yet" is not a state to leave a DMA
     * structure in after a recovery whose whole purpose is to restore a known-good one.
     */
    ring_init_link(ring);
    *enq = 0;
    *cyc = 1;
    hype_xhci_trb_set_tr_dequeue(cmd, phys(ring) | 1u /* DCS=1 */, slot, dci, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) {
        hype_debug_print("host-xhci: #289 Set TR Dequeue (slot=%u dci=%u) NO COMPLETION "
                         "(timed out) -- endpoint left in an unknown ring position\n", slot, dci);
        return -1;
    }
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) {
        hype_debug_print("host-xhci: #289 Set TR Dequeue (slot=%u dci=%u) cc=%u -- controller and "
                         "driver now disagree about the ring position\n", slot, dci,
                         hype_xhci_event_cc(evt));
        return -1;
    }
    hype_debug_print("host-xhci: #289 Set TR Dequeue (slot=%u dci=%u) cc=%u (success) -- ring "
                     "restarted at index 0, producer cycle 1\n", slot, dci,
                     hype_xhci_event_cc(evt));
    return 0;
}

/*
 * #734: recovery for an endpoint that is already HALTED, which is the interrupt-IN case.
 *
 * Two differences from ep_recover(), and both come from the 2026-08-27 11:23 boot, where
 * eight recoveries took 3.6 SECONDS of the guest dispatch loop and the dashboard visibly
 * froze:
 *
 *   - No Stop Endpoint. Stop is defined on a RUNNING endpoint (xHCI 4.6.9); against a
 *     halted one it returns Context State Error, which is exactly what that boot logged
 *     eight times over -- `#289 Stop Endpoint cc=19`. It is a third of the cost for a
 *     command that cannot do anything.
 *   - Quiet by default. ep_recover() prints a line per step because the bulk path runs it
 *     rarely; an interrupt endpoint failing every millisecond must not narrate.
 *
 * Reset Endpoint (Halted -> Stopped) then Set TR Dequeue Pointer to a restarted ring is
 * the whole of what xHCI 4.6.8/4.6.10 asks for here.
 */
static int ep_recover_halted(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int dci,
                             uint8_t *ring, unsigned int *enq, unsigned int *cyc) {
    xhci_hw_t *hw = HW(c);
    uint32_t cmd[4], evt[4];
    unsigned int i;

    hype_xhci_trb_reset_endpoint(cmd, slot, dci, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) return -1;

    for (i = 0; i < XPAGE; i++) { ring[i] = 0; }
    ring_init_link(ring);
    *enq = 0;
    *cyc = 1;
    hype_xhci_trb_set_tr_dequeue(cmd, phys(ring) | 1u /* DCS=1 */, slot, dci, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) return -1;
    return (hype_xhci_event_cc(evt) == HYPE_XHCI_CC_SUCCESS) ? 0 : -1;
}

/*
 * #254: Bulk-Only Transport Reset Recovery (USB MSC BOT spec 5.3.4): after ANY
 * failed stage -- a lost completion, an error CC, a bad CSW -- the host may not
 * simply issue the next CBW: the device may still be inside the old transaction
 * and will consume that CBW as DATA (observed on real hardware: a byte-exact
 * CBW written into a log sector). Recovery = Bulk-Only Mass Storage Reset,
 * Clear-HALT on both bulk endpoints, plus xHCI-side ring recovery on both.
 */
static int bot_recover(hype_xhci_ctrl_t *c, unsigned int slot, const hype_xhci_msc_eps_t *msc) {
    xhci_hw_t *hw = HW(c);
    /*
     * #266: discard this slot's parked completions FIRST. ep_recover() restarts the
     * transfer rings at index 0, so the retry's first TRB lands at the same physical
     * address the abandoned transfer used -- a parked event for the torn-down state would
     * then be claimed by the retry and report someone else's result. Keeping the parking
     * table without this would trade the discard bug for #254's mis-attribution bug.
     */
    unsigned int dci_in = hype_xhci_ep_dci(msc->bulk_in_ep);
    unsigned int dci_out = hype_xhci_ep_dci(msc->bulk_out_ep);
    int rc = 0;

    hype_debug_print("host-xhci: #254 BOT reset recovery (slot=%u)\n", slot);
    hype_xhci_parked_drop_slot(&hw->parked, slot);

    /* Quiesce both rings first so nothing is in flight during the reset. */
    {
        xhci_msc_hw_t *m = msc_hw_for(c, slot, 0);
        if (m == 0) return -1; /* never brought up: nothing to recover */
        if (ep_recover(c, slot, dci_in, m->bulk_in_ring, &m->bin_enq, &m->bin_cyc) != 0) rc = -1;
        if (ep_recover(c, slot, dci_out, m->bulk_out_ring, &m->bout_enq, &m->bout_cyc) != 0) rc = -1;
    }

    /* Bulk-Only Mass Storage Reset: class request 0xFF to the interface. */
    if (control_transfer(c, slot, 0x21, 0xFF, 0, (uint16_t)msc->interface_num, 0, 0, 0) != 0) {
        hype_debug_print("host-xhci: #254 Bulk-Only Mass Storage Reset failed\n");
        rc = -1;
    }
    /* CLEAR_FEATURE(ENDPOINT_HALT) on bulk IN, then bulk OUT (spec order). */
    if (control_transfer(c, slot, 0x02, 0x01, 0, (uint16_t)msc->bulk_in_ep, 0, 0, 0) != 0) {
        hype_debug_print("host-xhci: #254 Clear-HALT (bulk IN) failed\n");
        rc = -1;
    }
    if (control_transfer(c, slot, 0x02, 0x01, 0, (uint16_t)msc->bulk_out_ep, 0, 0, 0) != 0) {
        hype_debug_print("host-xhci: #254 Clear-HALT (bulk OUT) failed\n");
        rc = -1;
    }
    /*
     * #266: drain the event ring LAST, after every reset step, and before any new
     * work can be issued. ep_recover() restarts the transfer rings at index 0, so a
     * still-in-flight completion for the transfer we just abandoned would carry the
     * same trb_ptr as the retry's first TRB and be indistinguishable from it. Draining
     * here is what makes the retry's strict match trustworthy again.
     */
    {
        volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
        unsigned int drained = drain_events(hw, bar, c->rtsoff);
        hype_debug_print("host-xhci: #266 BOT recovery finished rc=%d, drained %u stale event(s) "
                         "(foreign seen so far this boot: %u)\n",
                         rc, drained, g_bulk_foreign_seen);
    }
    return rc;
}

/* Bulk-Only Transport: CBW (out) -> optional data phase -> CSW (in). Data flows
 * through hw->data (bounced). Returns 0 iff the CSW reports command-passed. */
/* #365: freestanding block copy, 8 bytes at a time when both pointers and the length permit.
 * No libc here, and the media path now moves 64 KiB per transfer. */
static void bounce_copy(uint8_t *dst, const uint8_t *src, unsigned int len) {
    unsigned int i = 0;
    if ((((uintptr_t)dst | (uintptr_t)src) & 7u) == 0u) {
        uint64_t *d = (uint64_t *)dst;
        const uint64_t *s8 = (const uint64_t *)src;
        unsigned int words = len >> 3;
        for (; i < words; i++) d[i] = s8[i];
        i <<= 3;
    }
    for (; i < len; i++) dst[i] = src[i];
}

static int bot_scsi_once(hype_xhci_ctrl_t *c, unsigned int slot, const hype_xhci_msc_eps_t *msc,
                    const uint8_t *cdb, unsigned int cdb_len, uint8_t *data, unsigned int data_len,
                    int dir_in) {
    unsigned int dci_in = hype_xhci_ep_dci(msc->bulk_in_ep);
    unsigned int dci_out = hype_xhci_ep_dci(msc->bulk_out_ep);
    xhci_msc_hw_t *m = msc_hw_for(c, slot, 0); /* #387: this device's own rings + bounce */
    uint32_t tag;

    if (m == 0) return -1; /* endpoints never configured for this slot */
    tag = ++m->bot_tag;

    if (data_len > XDATA) return -1;

    /* CBW on the bulk OUT endpoint. */
    hype_usb_bot_cbw(m->cbw, tag, data_len, dir_in, 0, cdb, cdb_len);
    if (bulk_xfer(c, m->bulk_out_ring, &m->bout_enq, &m->bout_cyc, slot, dci_out,
                  phys(m->cbw), HYPE_USB_CBW_LEN) != 0) return -1;

    /* Data phase (bounced through the device's own bounce). */
    if (data_len) {
        if (dir_in) {
            if (bulk_xfer(c, m->bulk_in_ring, &m->bin_enq, &m->bin_cyc, slot, dci_in,
                          phys(m->data), data_len) != 0) return -1;
            /* #365: word-at-a-time when both sides allow it. At 64 KiB a byte loop is 65536
             * iterations on the hot media path; this cuts it to 8192. */
            bounce_copy(data, m->data, data_len);
        } else {
            bounce_copy(m->data, data, data_len);
            if (bulk_xfer(c, m->bulk_out_ring, &m->bout_enq, &m->bout_cyc, slot, dci_out,
                          phys(m->data), data_len) != 0) return -1;
        }
    }

    /* CSW on the bulk IN endpoint. */
    if (bulk_xfer(c, m->bulk_in_ring, &m->bin_enq, &m->bin_cyc, slot, dci_in,
                  phys(m->csw), HYPE_USB_CSW_LEN) != 0) return -1;
    if (hype_usb_bot_csw_ok(m->csw, tag)) {
        return 0;
    }
    /*
     * #516: distinguish the DEVICE saying no from the TRANSPORT breaking. A structurally valid
     * CSW with nonzero status (or unconsumed residue) means the BOT machinery is healthy and
     * the device rejected/shortened the COMMAND -- the answer is REQUEST SENSE, never an
     * endpoint reset. This used to collapse into one silent -1: the 64 GB stick failed hype's
     * first WRITE(10) with a clean status-1 CSW, hype "recovered" a transport that was not
     * broken, the retry met the same verdict, and the loop spammed the screen while the log
     * sink (whose create needed that write) stayed dead. The evidence -- WHICH command, WHAT
     * status -- was discarded exactly where it existed.
     */
    if (hype_usb_bot_csw_valid(m->csw, tag)) {
        static unsigned int csw_fail_reported = 0;
        if (csw_fail_reported++ < 16u) {
            hype_debug_print("host-xhci: #516 CSW: op=0x%02x FAILED status=%u residue=%u "
                             "(len=%u dir=%s) -- device verdict, transport healthy\n",
                             cdb[0], hype_usb_bot_csw_status(m->csw),
                             hype_usb_bot_csw_residue(m->csw), data_len, dir_in ? "in" : "out");
        }
        return 1; /* command failed cleanly: sense, don't reset */
    }
    {
        static unsigned int csw_bad_reported = 0;
        if (csw_bad_reported++ < 16u) {
            hype_debug_print("host-xhci: #516 CSW: op=0x%02x structurally INVALID (bad "
                             "signature/tag) -- transport-level damage\n", cdb[0]);
        }
    }
    return -1;
}

/*
 * #516: absorb a failed command's sense data. Mandatory BOT hygiene after any status!=0 CSW:
 * REQUEST SENSE both NAMES the failure and CLEARS the device's pending sense/unit-attention
 * state -- several devices (this 64 GB Cruzer included) fail every later command until it is
 * read. Bounded reporting; the sense itself travels through a plain BOT transaction.
 */
static void bot_absorb_sense(hype_xhci_ctrl_t *c, unsigned int slot,
                             const hype_xhci_msc_eps_t *msc, uint8_t failed_op) {
    xhci_hw_t *hw = HW(c);
    uint8_t cdb[6];
    uint8_t sense[18];
    unsigned int i;
    static unsigned int sense_reported = 0;

    for (i = 0; i < sizeof(sense); i++) sense[i] = 0;
    hw->last_sense_key = 0xFFu; /* 'none captured' until the parse below succeeds */
    hw->last_sense_asc = 0xFFu;
    hype_scsi_cdb_request_sense(cdb, (uint8_t)sizeof(sense));
    if (bot_scsi_once(c, slot, msc, cdb, 6u, sense, sizeof(sense), 1) < 0) {
        if (sense_reported < 16u) {
            hype_debug_print("host-xhci: #516 REQUEST SENSE itself failed (after op=0x%02x)\n",
                             failed_op);
            sense_reported++;
        }
        return;
    }
    {
        unsigned int key = 0, asc = 0, ascq = 0;
        if (hype_scsi_parse_fixed_sense(sense, sizeof(sense), &key, &asc, &ascq) == 0) {
            hw->last_sense_key = key;
            hw->last_sense_asc = asc;
            if (sense_reported < 16u) {
                hype_debug_print("host-xhci: #516 SENSE for op=0x%02x: key=0x%x asc=0x%02x "
                                 "ascq=0x%02x\n", failed_op, key, asc, ascq);
                sense_reported++;
            }
        } else if (sense_reported < 16u) {
            hype_debug_print("host-xhci: #516 SENSE for op=0x%02x: unparseable (resp=0x%02x)\n",
                             failed_op, sense[0]);
            sense_reported++;
        }
    }
}

/*
 * #254: one transaction, and on ANY failure a full BOT reset recovery followed
 * by exactly one retry. Never a bare retry without recovery -- that is what
 * wrote a CBW into a sector. If the retry fails too the error surfaces to the
 * caller (visible, not silent: blk_usb -> log sink -> reported once).
 */
/*
 * #289: fault injection, off by default. Set to N to make the Nth SCSI command behave as
 * though its transfer was lost, forcing bot_recover() to run on a LIVE datapath and letting
 * the existing "post-recovery retry SUCCEEDED/FAILED" line answer the ticket's actual
 * question -- does Stop -> Reset -> Set TR Dequeue -> BOT reset return the endpoint to a
 * usable state?
 *
 * This does not reproduce the AMD controller's late-completion behaviour, which is what
 * TRIGGERED recovery on real hardware; it exercises the recovery path itself, which is the
 * part that can be tested anywhere. The trigger belongs with a hardware run.
 */
#ifndef HYPE_XHCI_BOT_RECOVER_SELFTEST
#define HYPE_XHCI_BOT_RECOVER_SELFTEST 0
#endif

/*
 * #803: a deadline across the RETRY LADDER below, which is where a slow device turns into a
 * multi-second command.
 *
 * One bot_scsi() call can run up to four bot_scsi_once() attempts plus a bot_recover(), each with
 * its own internal polling. Boot AMD-L0 run 7 measured a single ATAPI command at 6394709 us and
 * the vCPU parked inside hype (`IN-HOST for 3426ms section=774`) while that ladder ran to
 * completion. The guest showed 0% CPU and the machine read as locked.
 *
 * A first attempt is NEVER cut short, and neither is a recovery already in progress -- the
 * deadline is only consulted before starting a further attempt, so this abandons a hopeless
 * sequence rather than interrupting one that might still succeed. Abandoning returns -1, which
 * the caller reports to the guest as MEDIUM ERROR through the existing #287 path.
 *
 * An earlier attempt at this bound (99e5ba9) was placed in blk_usb.c's chunk loop and never
 * fired: USB_MAX_SECTORS is 128, a 64 KiB media read at 512-byte blocks is exactly 128 sectors,
 * so that loop runs ONCE and a between-chunks check is unreachable for the dominant case. The
 * wait is inside a single transfer, so the bound has to be here.
 *
 * 2000 ms is set against measurement: healthy service time on this rig was 793 us (run 3, early),
 * so this is ~2500x the good case. g_tsc_hz == 0 (frequency not yet calibrated) disables the
 * deadline entirely, which is the pre-#803 behaviour.
 */
#define HYPE_BOT_RETRY_BUDGET_MS 2000ull
static volatile unsigned long long g_bot_deadline_abandons;

unsigned long long hype_xhci_bot_abandons(void) { return g_bot_deadline_abandons; }

static int bot_past_deadline(uint64_t t0) {
    if (g_tsc_hz == 0) {
        return 0;
    }
    return (rdtsc_now() - t0) > (g_tsc_hz / 1000ull) * HYPE_BOT_RETRY_BUDGET_MS;
}

static int bot_abandon(const uint8_t *cdb) {
    g_bot_deadline_abandons++;
    hype_debug_print("host-xhci: #803 abandoning SCSI op=0x%02x -- retry ladder exceeded %llu ms; "
                     "reporting a medium error rather than parking the caller\n",
                     (unsigned)cdb[0], (unsigned long long)HYPE_BOT_RETRY_BUDGET_MS);
    return -1;
}

static int bot_scsi(hype_xhci_ctrl_t *c, unsigned int slot, const hype_xhci_msc_eps_t *msc,
                    const uint8_t *cdb, unsigned int cdb_len, uint8_t *data, unsigned int data_len,
                    int dir_in) {
    int forced_fail = 0;
    uint64_t bot_t0 = rdtsc_now();

#if HYPE_XHCI_BOT_RECOVER_SELFTEST
    {
        static unsigned int seen = 0;
        seen++;
        if (seen == (unsigned int)HYPE_XHCI_BOT_RECOVER_SELFTEST) {
            forced_fail = 1;
            hype_debug_print("host-xhci: #289 SELFTEST -- treating SCSI command %u as a lost "
                             "transfer so bot_recover() runs on a live datapath\n", seen);
        }
    }
#endif
    if (!forced_fail) {
        int rc1 = bot_scsi_once(c, slot, msc, cdb, cdb_len, data, data_len, dir_in);
        if (rc1 == 0) {
            return 0;
        }
        /*
         * #516: a clean device NO (valid CSW, status!=0). The transport is healthy: absorb the
         * sense (which also clears a pending unit attention -- the reset-recovery this used to
         * run RE-ARMED unit attention on some devices, guaranteeing the retry failed and the
         * loop never converged), then retry once for the transient-sense case. A second clean
         * NO is the device's final answer; resetting endpoints cannot change its mind.
         */
        if (rc1 == 1) {
            bot_absorb_sense(c, slot, msc, cdb[0]);
            if (bot_past_deadline(bot_t0)) {
                return bot_abandon(cdb);
            }
            rc1 = bot_scsi_once(c, slot, msc, cdb, cdb_len, data, data_len, dir_in);
            if (rc1 == 0) {
                return 0;
            }
            if (rc1 == 1) {
                bot_absorb_sense(c, slot, msc, cdb[0]);
                return -1;
            }
            /* rc1 < 0: the retry broke the TRANSPORT -- fall through to ring recovery. */
        }
    }
    if (bot_past_deadline(bot_t0)) {
        return bot_abandon(cdb);
    }
    if (bot_recover(c, slot, msc) != 0) {
        hype_debug_print("host-xhci: #266 recovery FAILED -- backend not trustworthy, giving up\n");
        return -1; /* recovery itself failed: the backend is not trustworthy */
    }
    {
        /* #266: whether the retry works is the single most useful fact about this
         * path, and it was never reported -- three recoveries in a real-hardware log
         * told us nothing about whether any of them helped. */
        int rc2;
        if (bot_past_deadline(bot_t0)) {
            return bot_abandon(cdb);
        }
        rc2 = bot_scsi_once(c, slot, msc, cdb, cdb_len, data, data_len, dir_in);
        if (rc2 == 1) {
            /* Transport restored; the device then said a clean NO. Same policy as above. */
            bot_absorb_sense(c, slot, msc, cdb[0]);
            if (bot_past_deadline(bot_t0)) {
                return bot_abandon(cdb);
            }
            rc2 = bot_scsi_once(c, slot, msc, cdb, cdb_len, data, data_len, dir_in);
            if (rc2 == 1) {
                bot_absorb_sense(c, slot, msc, cdb[0]);
                rc2 = -1;
            }
        }
        hype_debug_print("host-xhci: #266 post-recovery retry %s\n",
                         (rc2 == 0) ? "SUCCEEDED -- datapath restored"
                                    : "FAILED -- recovery did not restore the datapath");
        return rc2;
    }
}

int hype_xhci_msc_read_capacity(hype_xhci_ctrl_t *c, unsigned int slot,
                                const hype_xhci_msc_eps_t *msc, uint32_t *last_lba,
                                uint32_t *block_size) {
    uint8_t cdb[10];
    uint8_t rc[8];
    hype_scsi_cdb_read_capacity10(cdb);
    if (bot_scsi(c, slot, msc, cdb, 10u, rc, 8u, 1) != 0) return -1;
    hype_scsi_parse_read_capacity10(rc, last_lba, block_size);
    return 0;
}

int hype_xhci_msc_read(hype_xhci_ctrl_t *c, unsigned int slot, const hype_xhci_msc_eps_t *msc,
                       uint32_t lba, unsigned int blocks, unsigned int block_size, void *buf) {
    uint8_t cdb[10];
    unsigned int len = blocks * block_size;
    if (len == 0u || len > XDATA) return -1;
    hype_scsi_cdb_read10(cdb, lba, (uint16_t)blocks);
    return bot_scsi(c, slot, msc, cdb, 10u, (uint8_t *)buf, len, 1);
}

int hype_xhci_msc_write(hype_xhci_ctrl_t *c, unsigned int slot, const hype_xhci_msc_eps_t *msc,
                        uint32_t lba, unsigned int blocks, unsigned int block_size, const void *buf) {
    xhci_hw_t *hw = HW(c);
    xhci_msc_hw_t *m = msc_hw_for(c, slot, 0); /* #387 */
    uint8_t cdb[10];
    unsigned int len = blocks * block_size;
    unsigned int i;
    if (m == 0) return -1;
    if (len == 0u || len > XDATA) return -1;
    /* stage the caller's data (bot_scsi bounces from the device's bounce for OUT). */
    for (i = 0; i < len; i++) ((uint8_t *)m->data)[i] = ((const uint8_t *)buf)[i];
    hype_scsi_cdb_write10(cdb, lba, (uint16_t)blocks);
    /*
     * #596: on a device that rejects SYNCHRONIZE CACHE (#516) hype has no cache-flush barrier, so
     * the durability ORDERING a filesystem writer relies on -- FAT link before the directory size
     * that reaches it (#377), the exFAT DataLength, an ext journal commit -- cannot be established
     * by a flush. Set FUA (Force Unit Access) on every WRITE(10) instead: each write goes straight
     * to the medium and completes only when durable, so writes land in issue order and a metadata
     * pointer never outlives the block it points at. Fs-agnostic: it fixes FAT32/exFAT/ext at once,
     * because the missing guarantee was in the shared block/sync layer, not in any one writer.
     * Only on such devices -- a stick with a working SYNCHRONIZE CACHE keeps write-back speed.
     */
    if (hw != 0 && hw->sync_cache_unsupported) {
        cdb[1] |= 0x08u; /* FUA */
    }
    /* pass the bounce as the data pointer so bot_scsi's OUT copy is a self-copy. */
    return bot_scsi(c, slot, msc, cdb, 10u, (uint8_t *)m->data, len, 0);
}

int hype_xhci_msc_inquiry_vpd(hype_xhci_ctrl_t *c, unsigned int slot,
                              const hype_xhci_msc_eps_t *msc, uint8_t page, uint8_t *buf,
                              unsigned int len) {
    uint8_t cdb[6];
    uint8_t hdr[4];
    unsigned int avail;
    unsigned int i;

    if (buf == 0 || len < 4u || len > 255u) return -1;

    /*
     * Two exact-length reads, never one padded read: #377's exact-transfer
     * rule (residue must be zero) is what keeps log sectors uncorrupted, and
     * a VPD response shorter than the allocation length would trip it. The
     * 4-byte header is always available and carries the page length, so the
     * second read can request exactly what the device holds.
     */
    hype_scsi_cdb_inquiry_vpd(cdb, page, 4u);
    if (bot_scsi(c, slot, msc, cdb, 6u, hdr, 4u, 1) != 0) return -1;
    if (hdr[1] != page) return -1;
    avail = 4u + (unsigned int)hdr[3];
    if (avail > len) return -1;

    for (i = 0; i < len; i++) buf[i] = 0;
    hype_scsi_cdb_inquiry_vpd(cdb, page, (uint8_t)avail);
    return bot_scsi(c, slot, msc, cdb, 6u, buf, avail, 1);
}

int hype_xhci_msc_sync_cache(hype_xhci_ctrl_t *c, unsigned int slot,
                             const hype_xhci_msc_eps_t *msc) {
    xhci_hw_t *hw = HW(c);
    uint8_t cdb[10];
    int rc;

    /*
     * #516: a device that rejected SYNCHRONIZE CACHE as an unknown opcode has no cache to
     * sync; the command is a permanent no-op for it. Re-sending it forever turned every log
     * flush on the 64 GB Cruzer into a fail/recover/retry storm that killed the whole USB
     * datapath (and, before the clean-CSW split, looked exactly like transport damage).
     */
    if (hw->sync_cache_unsupported) {
        return 0;
    }
    hype_scsi_cdb_synchronize_cache10(cdb);
    rc = bot_scsi(c, slot, msc, cdb, 10u, (uint8_t *)0, 0u, 0);
    if (rc != 0 && hw->last_sense_key == 0x5u && hw->last_sense_asc == 0x20u) {
        hw->sync_cache_unsupported = 1;
        hype_debug_print("host-xhci: #516 device rejects SYNCHRONIZE CACHE (ILLEGAL REQUEST/"
                         "invalid opcode) -- no cache-flush barrier; switching all WRITE(10)s to "
                         "FUA so writes are ordered/durable without it [#596]\n");
        return 0;
    }
    return rc;
}

int hype_xhci_host_init(uint64_t bar_phys, hype_xhci_ctrl_t *out) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)bar_phys;
    uint8_t caplen = rd8(bar, HYPE_XHCI_CAP_CAPLENGTH);
    uint32_t hcs1 = rd32(bar, HYPE_XHCI_CAP_HCSPARAMS1);
    uint32_t hcs2 = rd32(bar, HYPE_XHCI_CAP_HCSPARAMS2);
    uint32_t hcc1 = rd32(bar, HYPE_XHCI_CAP_HCCPARAMS1);
    uint32_t op = hype_xhci_op_base(caplen);
    unsigned int max_slots = hype_xhci_max_slots(hcs1);
    unsigned int max_ports = hype_xhci_max_ports(hcs1);
    unsigned int nscratch = hype_xhci_max_scratchpads(hcs2);
    unsigned int i;
    xhci_hw_t *hw;

    out->inited = 0;
    /*
     * #299: claim a per-controller DMA/ring block. Refusing here rather than sharing one
     * is the whole point: two Running controllers on one command ring, one event ring and
     * one dequeue/cycle pair is what broke the Intel box. A machine with more controllers
     * than blocks loses the extra controllers, which is a bounded loss; sharing would
     * corrupt the one carrying hype's log.
     */
    out->hw_slot = HYPE_XHCI_MAX_CTRL;
    for (i = 0; i < HYPE_XHCI_MAX_CTRL; i++) {
        if (!g_hw[i].in_use) {
            out->hw_slot = i;
            break;
        }
    }
    if (out->hw_slot >= HYPE_XHCI_MAX_CTRL) {
        hype_debug_print("host-xhci: bar=0x%llx SKIPPED -- all %u per-controller ring blocks "
                         "are in use; this controller's devices are unreachable this boot "
                         "[#299]\n",
                         (unsigned long long)bar_phys, HYPE_XHCI_MAX_CTRL);
        out->hw_slot = 0; /* keep HW() in range; inited stays 0 so nothing uses it */
        return -1;
    }
    hw = &g_hw[out->hw_slot];
    hw->in_use = 1;
    /*
     * Reset the block's bookkeeping. Necessary because a block can be RE-claimed after a
     * quiesce, and stale dev_used/slot_dev would have the new controller hand out a pool
     * entry the old one still "owns" -- a device context pointing at the wrong device.
     * These cursors were never reset between controllers when they were file-static
     * either; that was latent only because the sweep never got as far as a second
     * controller.
     */
    {
        unsigned int k;
        for (k = 0; k < DEVPOOL; k++) {
            hw->dev_used[k] = 0;
            hw->ep0_enq[k] = 0;
            hw->ep0_cyc[k] = 0;
        }
        for (k = 0; k < 256u; k++) {
            hw->slot_dev[k] = 0;
        }
        /* #734: release this controller's interrupt-IN endpoint blocks with it. */
        hype_xhci_int_in_release_ctrl(g_iin_key, HYPE_XHCI_INT_IN_MAX, out->hw_slot);
        hype_xhci_hub_forget_ctrl(out->hw_slot); /* #746: and its hubs */
        /* #387: release this controller's claimed-MSC blocks with it. */
        {
            unsigned int mi;
            for (mi = 0; mi < HYPE_XHCI_MSC_MAX; mi++) {
                if (g_msc_hw[mi].used && g_msc_hw[mi].ctrl == out->hw_slot) {
                    g_msc_hw[mi].used = 0;
                }
            }
        }
        hype_xhci_parked_reset(&hw->parked);
        /*
         * #785: a re-claimed block starts its new life honest. The cumulative diagnostic
         * counters (timeouts, guard exhaustions) stay, so a run's HIDTICK still shows how
         * often the ring has failed; the latches that say "give up" do not, or the freshly
         * reset controller would refuse its first command.
         */
        hw->cmd_dead = 0;
        hw->reset_wanted = 0;
        hw->cmd_recoveries = 0;
        hw->wedged = 0;
    }

    out->bar = bar_phys;
    out->op = op;
    hw->opreg = op;
    out->dboff = rd32(bar, HYPE_XHCI_CAP_DBOFF);
    out->rtsoff = rd32(bar, HYPE_XHCI_CAP_RTSOFF);
    out->max_slots = max_slots;
    out->max_ports = max_ports;
    out->ctx_size = hype_xhci_context_size(hcc1);

    /* Wait for the controller to be ready (CNR clear). */
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBSTS, HYPE_XHCI_USBSTS_CNR, 0) != 0) { hw->in_use = 0; return -1; }

    /* Stop, then wait halted. */
    wr32(bar, op + HYPE_XHCI_OP_USBCMD,
         rd32(bar, op + HYPE_XHCI_OP_USBCMD) & ~HYPE_XHCI_USBCMD_RS);
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBSTS, HYPE_XHCI_USBSTS_HCH,
                  HYPE_XHCI_USBSTS_HCH) != 0) { hw->in_use = 0; return -1; }

    /* Reset (HCRST); wait it self-clears + CNR clears. */
    wr32(bar, op + HYPE_XHCI_OP_USBCMD, HYPE_XHCI_USBCMD_HCRST);
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBCMD, HYPE_XHCI_USBCMD_HCRST, 0) != 0) { hw->in_use = 0; return -1; }
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBSTS, HYPE_XHCI_USBSTS_CNR, 0) != 0) { hw->in_use = 0; return -1; }

    if (max_slots == 0u) { hw->in_use = 0; return -1; }

    /* Program the number of enabled device slots. */
    wr32(bar, op + HYPE_XHCI_OP_CONFIG, max_slots);

    /* Scratchpad buffers, if the controller wants any: DCBAA[0] points at an
     * array of page pointers, each a hype .bss page. Real HW (unlike qemu-xhci,
     * which asks for none) usually requires these -- if we under-provide, the
     * controller DMAs to zero/garbage scratchpad slots and Address Device fails
     * with no obvious reason, so log the count + PAGESIZE and flag a clamp. */
    {
        uint32_t pagesize = rd32(bar, op + HYPE_XHCI_OP_PAGESIZE);
        hype_debug_print("host-xhci: caps -- slots=%u ports=%u ctx=%uB scratchpads=%u pagesize=0x%04x\n",
                         max_slots, max_ports, out->ctx_size, nscratch, pagesize & 0xFFFFu);
        if (nscratch > MAX_SCRATCH) {
            hype_debug_print("host-xhci: WARNING -- controller wants %u scratchpad buffers but "
                             "hype provides only %u; Address Device will likely FAIL\n",
                             nscratch, MAX_SCRATCH);
        }
    }
    zero(hw->dcbaa, XPAGE);
    if (nscratch > 0u) {
        if (nscratch > MAX_SCRATCH) nscratch = MAX_SCRATCH;
        zero(hw->scratch_arr, XPAGE);
        for (i = 0; i < nscratch; i++) {
            zero(hw->scratch_pages[i], XPAGE);
            put_le64(hw->scratch_arr + i * 8u, phys(hw->scratch_pages[i]));
        }
        put_le64(hw->dcbaa + 0, phys(hw->scratch_arr)); /* DCBAA[0] = scratchpad array */
    }
    wr64(bar, op + HYPE_XHCI_OP_DCBAAP, phys(hw->dcbaa));

    /* Command ring: zeroed TRBs with a Link TRB (toggle-cycle) at the end,
     * pointing back to the start. CRCR = ring | RCS(1). */
    zero(hw->cmd_ring, XPAGE);
    {
        uint32_t link[4];
        hype_xhci_trb_link(link, phys(hw->cmd_ring), 1);
        put_le32(hw->cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
        put_le32(hw->cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
        put_le32(hw->cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
        put_le32(hw->cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
    }
    wr64(bar, op + HYPE_XHCI_OP_CRCR, phys(hw->cmd_ring) | 1u /* RCS */);

    /* Event ring: one segment, described by a single-entry ERST. */
    zero(hw->evt_ring, XPAGE);
    zero(hw->erst, 64);
    put_le64(hw->erst + 0, phys(hw->evt_ring)); /* segment base */
    put_le32(hw->erst + 8, RING_TRBS);        /* segment size (TRBs) */
    {
        uint32_t ir = out->rtsoff;
        wr32(bar, hype_xhci_ir0_offset(ir, HYPE_XHCI_IR_ERSTSZ), 1u); /* one segment */
        wr64(bar, hype_xhci_ir0_offset(ir, HYPE_XHCI_IR_ERDP), phys(hw->evt_ring));
        wr64(bar, hype_xhci_ir0_offset(ir, HYPE_XHCI_IR_ERSTBA), phys(hw->erst));
    }

    /* Run. */
    wr32(bar, op + HYPE_XHCI_OP_USBCMD,
         rd32(bar, op + HYPE_XHCI_OP_USBCMD) | HYPE_XHCI_USBCMD_RS);
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBSTS, HYPE_XHCI_USBSTS_HCH, 0) != 0) { hw->in_use = 0; return -1; }

    /*
     * Let attached devices be re-detected before anyone reads PORTSC.
     *
     * The HCRST above cleared all port state, so every port restarts in
     * RxDetect with CCS=0 regardless of what is plugged in, and the controller
     * needs real time to notice a connection again: USB 2.0's connect debounce
     * alone is 100 ms (TATTDB), and a SuperSpeed port has link training on top.
     * Without this the very first port scan samples CCS too early and every
     * port looks empty -- which is exactly what an Intel i5-13420H reported:
     * all 18 ports across both controllers reading PORTSC=0x000002a0
     * (PP=1, CCS=0, PLS=5/RxDetect) on a machine that had just booted from a
     * stick on one of them, and whose PCH controller also has internal devices
     * (webcam, Bluetooth) that cannot all be absent. The AMD box got away with
     * it only marginally -- its Address Device needed two attempts.
     */
    delay_ms(HYPE_XHCI_CONNECT_DEBOUNCE_MS);

    ring_state_reset(hw);
    hw->up_tsc = rdtsc_now();
    out->inited = 1;
    return 0;
}

void hype_xhci_host_quiesce(hype_xhci_ctrl_t *c) {
    volatile uint8_t *bar;
    uint32_t op;

    if (c == 0 || !c->inited) {
        return;
    }
    bar = (volatile uint8_t *)(uintptr_t)c->bar;
    op = c->op;
    /*
     * Clear Run/Stop, wait for HCHalted, then release this controller's ring block.
     *
     * #299 changed what this is FOR. It used to be mandatory before bringing up another
     * controller, because the DCBAA / command ring / event ring were shared; now each
     * controller owns its own, so this is a genuine per-controller teardown that a caller
     * uses when it is finished with a controller -- and the block goes back to the pool
     * for a later one. Bounded wait: a controller that will not halt must not wedge the
     * boot, since we are giving up on it either way.
     */
    wr32(bar, op + HYPE_XHCI_OP_USBCMD,
         rd32(bar, op + HYPE_XHCI_OP_USBCMD) & ~HYPE_XHCI_USBCMD_RS);
    (void)wait_bits(bar, op + HYPE_XHCI_OP_USBSTS, HYPE_XHCI_USBSTS_HCH,
                    HYPE_XHCI_USBSTS_HCH);
    HW(c)->in_use = 0;
    c->inited = 0;
}

uint32_t hype_xhci_port_status(const hype_xhci_ctrl_t *c, unsigned int port) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;

    return rd32(bar, c->op + hype_xhci_portsc_offset(port));
}

int hype_xhci_reset_port(hype_xhci_ctrl_t *c, unsigned int port, unsigned int *out_speed) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    uint32_t off = c->op + hype_xhci_portsc_offset(port);
    uint32_t sc = rd32(bar, off);

    if (out_speed) *out_speed = 0;

    /* Power the port if it isn't already. */
    if (!(sc & HYPE_XHCI_PORTSC_PP)) {
        wr32(bar, off, hype_xhci_portsc_write_preserve(sc, HYPE_XHCI_PORTSC_PP));
        short_delay();
        sc = rd32(bar, off);
    }
    /*
     * Give the port its own window to report a connection rather than believing
     * a single sample. Powering a port starts the same detect sequence a reset
     * does, and a device attached across a controller reset can take the full
     * debounce to reappear -- sampling once here is what made every port on
     * this machine look empty. Polling in millisecond steps keeps a genuinely
     * empty port cheap (it still costs its window, but only once per boot).
     */
    if (!(sc & HYPE_XHCI_PORTSC_CCS)) {
        unsigned int waited;
        for (waited = 0u; waited < HYPE_XHCI_PORT_CCS_WAIT_MS; waited++) {
            delay_ms(1);
            sc = rd32(bar, off);
            if (sc & HYPE_XHCI_PORTSC_CCS) break;
        }
    }
    if (!(sc & HYPE_XHCI_PORTSC_CCS)) return 0; /* nothing attached */

    /* USB3 ports enable themselves on connect; USB2 need an explicit reset. */
    if (!(sc & HYPE_XHCI_PORTSC_PED)) {
        wr32(bar, off, hype_xhci_portsc_write_preserve(sc, HYPE_XHCI_PORTSC_PR));
        {
            unsigned int s = SPIN;
            while (s-- != 0u) {
                sc = rd32(bar, off);
                if (sc & (HYPE_XHCI_PORTSC_PRC | HYPE_XHCI_PORTSC_PED)) break;
            }
        }
        sc = rd32(bar, off);
    }
    if (sc & HYPE_XHCI_PORTSC_PED) {
        if (out_speed) {
            *out_speed = (sc >> HYPE_XHCI_PORTSC_SPEED_SHIFT) & HYPE_XHCI_PORTSC_SPEED_MASK;
        }
        /* ack the reset/connect change bits */
        wr32(bar, off, hype_xhci_portsc_write_preserve(sc,
             HYPE_XHCI_PORTSC_PRC | HYPE_XHCI_PORTSC_CSC));
        /* USB 2.0 reset recovery: a device needs >=10 ms after reset before it
         * is ready for Address Device -- skipping this makes real HS devices
         * NAK the SET_ADDRESS (USB Transaction Error / timeout). */
        delay_ms(15);
        return 1;
    }
    return 0;
}

unsigned int hype_xhci_detect_device(hype_xhci_ctrl_t *c, unsigned int *out_speed) {
    unsigned int port;
    if (out_speed) *out_speed = 0;
    for (port = 1u; port <= c->max_ports; port++) {
        if (hype_xhci_reset_port(c, port, out_speed)) return port;
    }
    return 0;
}

/* --- USB hub class requests (bmRequestType per USB 2.0 §11.24) --- */

/* Hub-class feature selectors (USB 2.0 Table 11-17). */
/* HUB_FEAT_* and the hub control-transfer helpers are declared up beside the #746
 * status-change poller, which is their first user. */

static int hub_get_descriptor(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int desc_type,
                              uint8_t *buf, unsigned int len) {
    /* GET_DESCRIPTOR(HUB): class/IN/device (0xA0), wValue=type<<8, IN. */
    return control_transfer(c, slot, 0xA0, 6, (uint16_t)(desc_type << 8), 0, buf, len, 1);
}
static int hub_set_port_feature(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int feat,
                                unsigned int port) {
    /* SET_FEATURE: class/OUT/other (0x23), bRequest=3, wValue=feature, wIndex=port. */
    return control_transfer(c, slot, 0x23, 3, (uint16_t)feat, (uint16_t)port, 0, 0u, 0);
}
static int hub_clear_port_feature(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int feat,
                                  unsigned int port) {
    /* CLEAR_FEATURE: class/OUT/other (0x23), bRequest=1. */
    return control_transfer(c, slot, 0x23, 1, (uint16_t)feat, (uint16_t)port, 0, 0u, 0);
}
static int hub_get_port_status(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int port,
                               uint8_t st[4]) {
    /* GET_STATUS: class/IN/other (0xA3), bRequest=0 -> wPortStatus[0:1] + wPortChange[2:3]. */
    return control_transfer(c, slot, 0xA3, 0, 0, (uint16_t)port, st, 4u, 1);
}
/* Downstream-port speed from wPortStatus (USB 2.0: bit9 LS, bit10 HS, else FS). */
static unsigned int hub_port_speed(const uint8_t st[4]) {
    if (st[1] & 0x02u) return HYPE_USB_SPEED_LOW;
    if (st[1] & 0x04u) return HYPE_USB_SPEED_HIGH;
    return HYPE_USB_SPEED_FULL;
}

int hype_xhci_hub_walk(hype_xhci_ctrl_t *c, unsigned int hub_slot,
                       const hype_xhci_devpath_t *hub_path, unsigned int tier,
                       hype_xhci_hub_visit_fn visit, void *ctx) {
    uint8_t hubdesc[16];
    unsigned int nports, port, want, ttt;
    int ss_hub;

    if (!c->inited || hub_slot == 0u || visit == (hype_xhci_hub_visit_fn)0) {
        return HYPE_XHCI_HUB_NOT_WALKED;
    }
    if (tier == 0u || tier > 5u) {
        return HYPE_XHCI_HUB_NOT_WALKED; /* xHCI route strings are 5 hub tiers deep */
    }

    /*
     * #739: a SuperSpeed hub has no USB 2.0 hub descriptor -- it has the SuperSpeed one,
     * type 0x2A (USB 3.2 10.15.2.1). Asking every hub for 0x29 made hype skip both
     * SuperSpeed hubs on the operator's 5950X: one answered with a 0x2A descriptor and
     * was rejected on type, the other refused the request outright. Everything behind
     * either was invisible, inventory included.
     */
    want = (hub_path->speed >= HYPE_USB_SPEED_SUPER) ? HYPE_USB_DESC_HUB_SS
                                                     : HYPE_USB_DESC_HUB;
    if (hub_get_descriptor(c, hub_slot, want, hubdesc, sizeof hubdesc) != 0) {
        /* A hub that answers the other type is worth one retry: a 2.0 hub reached
         * through a SuperSpeed port, or an SS hub that only serves 0x29. */
        unsigned int alt = (want == HYPE_USB_DESC_HUB) ? HYPE_USB_DESC_HUB_SS
                                                       : HYPE_USB_DESC_HUB;
        if (hub_get_descriptor(c, hub_slot, alt, hubdesc, sizeof hubdesc) != 0) {
            hype_debug_print("host-xhci:   hub slot %u GET hub-descriptor FAILED (tried type "
                             "0x%02x then 0x%02x) [#739]\n", hub_slot, want, alt);
            return HYPE_XHCI_HUB_NOT_WALKED;
        }
    }
    if (hubdesc[1] != HYPE_USB_DESC_HUB && hubdesc[1] != HYPE_USB_DESC_HUB_SS) {
        hype_debug_print("host-xhci:   hub slot %u bad hub-descriptor type 0x%02x\n",
                         hub_slot, (unsigned)hubdesc[1]);
        return HYPE_XHCI_HUB_NOT_WALKED;
    }
    ss_hub = (hubdesc[1] == HYPE_USB_DESC_HUB_SS);
    nports = hype_xhci_hub_nbr_ports(hubdesc);
    hype_debug_print("host-xhci:   hub slot %u (tier %u) has %u downstream port(s)\n",
                     hub_slot, tier, nports);

    /*
     * #737: mark this slot as a hub BEFORE addressing anything below it. A child's TT
     * Hub Slot ID points here, and a controller will not build a split-transaction
     * schedule against a slot that still says Hub=0. Not fatal on its own -- a hub
     * whose ports are all high-speed never needs the TT -- so a failure is reported and
     * the descent continues.
     */
    /* TT Think Time is meaningless for a SuperSpeed hub -- it has no Transaction
     * Translator -- and wHubCharacteristics bits 6:5 are reserved there, so read it
     * only from a 2.0 descriptor. */
    ttt = ss_hub ? 0u : hype_xhci_hub_ttt(hubdesc);
    if (hype_xhci_configure_hub_slot(c, hub_slot, hub_path, nports, ttt) != 0) {
        hype_debug_print("host-xhci:   hub slot %u Configure Endpoint (Hub=1 ports=%u ttt=%u) "
                         "FAILED -- LS/FS devices below it may not report [#737]\n",
                         hub_slot, nports, ttt);
    }
    /*
     * #746: and its STATUS-CHANGE endpoint, so this hub's downstream ports can hot-plug.
     *
     * A hub's port changes are not xHCI Port Status Change Events -- those are root ports
     * only. The hub reports them here, on the one interrupt-IN endpoint every hub has, and
     * hype never configured it because until hot-plug nothing needed it.
     *
     * The bitmap is one bit per port plus one for the hub itself, so ceil((nports+1)/8)
     * bytes; a hub's descriptor mps says the same thing and is used when it is sane. The
     * endpoint address is fixed at 0x81 for every hub in the spec's own device class.
     *
     * A failure is NOT fatal and does not stop the descent: a hub whose status endpoint
     * could not be configured still routes traffic perfectly, it just cannot tell hype
     * when something is plugged into it. Registered with ep 0 in that case, so the poller
     * skips it and the log says which hub is deaf.
     */
    {
        unsigned int bmlen = ((nports + 1u) + 7u) / 8u;
        unsigned int hub_ep = 0x81u;
        if (bmlen == 0u) bmlen = 1u;
        if (hype_xhci_configure_hub_int_in(c, hub_slot, hub_path, nports, ttt, hub_ep,
                                           bmlen, ss_hub ? 8u : 12u) == 0) {
            hub_register(c, hub_slot, nports, hub_ep, bmlen, hub_path, tier, ss_hub);
            hype_debug_print("host-xhci:   hub slot %u status-change endpoint 0x%02x armed "
                             "(%u byte bitmap) -- its ports can hot-plug [#746]\n",
                             hub_slot, hub_ep, bmlen);
        } else {
            hub_register(c, hub_slot, nports, 0u, bmlen, hub_path, tier, ss_hub);
            hype_debug_print("host-xhci:   hub slot %u status-change endpoint could NOT be "
                             "configured -- traffic through it is unaffected, but hype will "
                             "not see anything plugged into it [#746]\n", hub_slot);
        }
    }

    for (port = 1u; port <= nports; port++) {
        uint8_t st[4];
        uint8_t devdesc[18];
        uint8_t cfg[256];
        unsigned int cfg_len = 0;
        unsigned int child_speed;
        unsigned int child_slot = 0;
        hype_xhci_devpath_t cp;
        unsigned int guard;

        /* Power the port and see if anything is attached. */
        hub_set_port_feature(c, hub_slot, HUB_FEAT_PORT_POWER, port);
        short_delay();
        if (hub_get_port_status(c, hub_slot, port, st) != 0) continue;
        if (!(st[0] & 0x01u)) continue; /* PORT_CONNECTION clear */

        /* Reset the port and wait for the reset-complete change bit. */
        if (hub_set_port_feature(c, hub_slot, HUB_FEAT_PORT_RESET, port) != 0) continue;
        for (guard = 0; guard < 20u; guard++) {
            short_delay();
            if (hub_get_port_status(c, hub_slot, port, st) != 0) break;
            if (st[2] & 0x10u) break; /* C_PORT_RESET */
        }
        hub_clear_port_feature(c, hub_slot, HUB_FEAT_C_PORT_RESET, port);
        hub_clear_port_feature(c, hub_slot, HUB_FEAT_C_PORT_CONNECTION, port);
        if (hub_get_port_status(c, hub_slot, port, st) != 0) continue;
        if (!(st[0] & 0x02u)) continue; /* PORT_ENABLE clear -> reset didn't take */
        delay_ms(15); /* USB reset recovery before Address Device (see reset_port) */

        /*
         * #739: the USB 2.0 wPortStatus speed bits do not exist on a SuperSpeed hub --
         * its bits 10:12 carry the negotiated link speed instead, and a 2.0 device
         * plugged into a USB-3 receptacle appears on the hub's SEPARATE 2.0 half, which
         * is its own xHCI device. So everything on an SS hub's downstream ports is
         * SuperSpeed, and reading bits 9/10 here would have called it full speed.
         */
        child_speed = ss_hub ? HYPE_USB_SPEED_SUPER : hub_port_speed(st);
        hype_debug_print("host-xhci:   hub slot %u port %u: device attached (speed id %u)\n",
                         hub_slot, port, child_speed);

        /* Extend the topology: append this port at `tier`, carry the root port,
         * and pick the Transaction Translator for a LS/FS child. */
        cp.root_port = hub_path->root_port;
        cp.route = hype_xhci_route_append(hub_path->route, tier, port);
        cp.speed = child_speed;
        if (hype_xhci_tt_required(hub_path->speed, child_speed)) {
            cp.tt_hub_slot = hub_slot; /* this HS hub is the child's TT */
            cp.tt_port = port;
        } else if ((child_speed == HYPE_USB_SPEED_LOW || child_speed == HYPE_USB_SPEED_FULL) &&
                   hub_path->tt_hub_slot) {
            cp.tt_hub_slot = hub_path->tt_hub_slot; /* inherit the upstream HS hub's TT */
            cp.tt_port = hub_path->tt_port;
        } else {
            cp.tt_hub_slot = 0;
            cp.tt_port = 0;
        }

        /* Enumerate the device on this port. */
        if (hype_xhci_enable_slot(c, &child_slot) != 0 || child_slot == 0u) {
            hype_debug_print("host-xhci:   hub slot %u port %u Enable Slot FAILED\n",
                             hub_slot, port);
            continue;
        }
        if (hype_xhci_address_device(c, child_slot, &cp) != 0) {
            hype_debug_print("host-xhci:   hub slot %u port %u Address Device FAILED "
                             "(route 0x%05x tt_hub=%u tt_port=%u)\n", hub_slot, port,
                             cp.route, cp.tt_hub_slot, cp.tt_port);
            hype_xhci_disable_slot(c, child_slot);
            continue;
        }
        if (hype_xhci_get_device_descriptor(c, child_slot, devdesc) != 0) {
            hype_debug_print("host-xhci:   hub slot %u port %u GET_DESCRIPTOR FAILED\n",
                             hub_slot, port);
            hype_xhci_disable_slot(c, child_slot);
            continue;
        }
        hype_debug_print("host-xhci:   hub slot %u port %u dev class=%02x vid=%04x pid=%04x\n",
                         hub_slot, port, (unsigned)devdesc[4],
                         (unsigned)(devdesc[8] | (devdesc[9] << 8)),
                         (unsigned)(devdesc[10] | (devdesc[11] << 8)));

        /* Configuration descriptor where the device will give one -- the visitor needs
         * it to classify a composite device and to read its endpoint set. A failure is
         * not fatal: the device is still reported, with cfg_len 0. */
        cfg_len = 0;
        (void)hype_xhci_get_config_descriptor(c, child_slot, cfg, sizeof cfg, &cfg_len);

        {
            int verdict = visit(ctx, c, child_slot, &cp, devdesc, cfg, cfg_len);

            if (hype_xhci_dev_is_hub(devdesc)) {
                /* A hub is a device AND a topology parent. It was just handed to the
                 * visitor like any other; now configure it so its downstream ports can be
                 * powered, and descend. Its slot stays addressed whatever the visitor
                 * said -- every device below it is addressed through it. */
                if (verdict == HYPE_XHCI_VISIT_STOP) {
                    return 0;
                }
                if (cfg_len >= 6u && hype_xhci_set_configuration(c, child_slot, cfg[5]) == 0) {
                    if (hype_xhci_hub_walk(c, child_slot, &cp, tier + 1u, visit, ctx) == 0) {
                        return 0; /* a visitor below said STOP */
                    }
                }
                continue;
            }
            if (verdict == HYPE_XHCI_VISIT_STOP) {
                return 0; /* left addressed, as the contract promises */
            }
            if (verdict == HYPE_XHCI_VISIT_RELEASE) {
                hype_xhci_disable_slot(c, child_slot);
            }
        }
    }
    /*
     * #746: throw away the hub's FIRST status report.
     *
     * The status-change endpoint was armed before the port loop, so the hub's first report
     * describes the state as it was BEFORE the descent -- every populated port reads as
     * "changed", because nothing had cleared C_PORT_CONNECTION yet. Measured in QEMU as
     * `hub slot 2 status bitmap 0x14` a second into the boot: ports 2 and 4, the keyboard
     * and the mouse, both of which the walk had just enumerated.
     *
     * Left in place that report makes the hot-plug poller re-enumerate devices that never
     * went anywhere -- and re-enumeration RESETS the port, so it would knock out a working
     * keyboard moments after claiming it. The loop above has now cleared every port's
     * change bits, so one drain leaves the endpoint reporting only what happens next.
     */
    {
        int hi2 = hub_reg_find(c->hw_slot, hub_slot);
        if (hi2 >= 0 && g_hubs[hi2].ep != 0u) {
            uint8_t discard[8];
            unsigned int dl = g_hubs[hi2].bitmap_len;
            unsigned int tries;
            if (dl == 0u || dl > sizeof(discard)) dl = 1u;
            for (tries = 0; tries < 8u; tries++) {
                if (hype_xhci_int_in_poll(c, hub_slot, g_hubs[hi2].ep, discard, dl) != 1) {
                    break;
                }
                hype_debug_print("host-xhci:   hub slot %u pre-walk status 0x%02x discarded "
                                 "[#746]\n", hub_slot, (unsigned)discard[0]);
            }
        }
    }
    return -1;
}

/*
 * USB-8 (#231) pt5b: find the first bulk-only mass-storage device behind a hub.
 *
 * #241: now a thin visitor over hype_xhci_hub_walk() rather than its own descent. Two
 * copies of the hub-port reset/address/descriptor sequence would be two places for the
 * timing and route-string arithmetic to drift apart, and that arithmetic is what took
 * several real-hardware runs to get right.
 */
typedef struct {
    unsigned int *out_slot;
    hype_xhci_devpath_t *out_path;
    hype_xhci_msc_eps_t *out_msc;
} hub_msc_seek_t;

static int hub_msc_visit(void *ctx, hype_xhci_ctrl_t *c, unsigned int slot,
                         const hype_xhci_devpath_t *path, const uint8_t *devdesc,
                         const uint8_t *cfg, unsigned int cfg_len) {
    hub_msc_seek_t *seek = (hub_msc_seek_t *)ctx;

    (void)c;
    if (hype_xhci_dev_is_hub(devdesc)) {
        return HYPE_XHCI_VISIT_RELEASE; /* the walk keeps hub slots regardless */
    }
    if (cfg_len != 0u && hype_xhci_msc_find_endpoints(cfg, cfg_len, seek->out_msc) == 0) {
        *seek->out_slot = slot;
        *seek->out_path = *path;
        return HYPE_XHCI_VISIT_STOP; /* left addressed for SET_CONFIGURATION + Configure EP */
    }
    return HYPE_XHCI_VISIT_RELEASE;
}

int hype_xhci_hub_find_msc(hype_xhci_ctrl_t *c, unsigned int hub_slot,
                           const hype_xhci_devpath_t *hub_path, unsigned int tier,
                           unsigned int *out_slot, hype_xhci_devpath_t *out_path,
                           hype_xhci_msc_eps_t *out_msc) {
    hub_msc_seek_t seek;

    if (out_slot == 0 || out_path == 0 || out_msc == 0) {
        return -1;
    }
    seek.out_slot = out_slot;
    seek.out_path = out_path;
    seek.out_msc = out_msc;
    return hype_xhci_hub_walk(c, hub_slot, hub_path, tier, hub_msc_visit, &seek);
}



unsigned int hype_xhci_take_port_change(hype_xhci_ctrl_t *c) {
    xhci_hw_t *hw;
    unsigned int p;

    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) {
        return 0;
    }
    hw = HW(c);
    for (p = 1u; p <= 31u; p++) {
        if (hw->port_changed & (1u << p)) {
            hw->port_changed &= ~(1u << p);
            return p;
        }
    }
    return 0;
}

void hype_xhci_hub_ignore_port(hype_xhci_ctrl_t *c, unsigned int hub_slot, unsigned int port,
                               int ignore) {
    unsigned int i;
    if (c == (hype_xhci_ctrl_t *)0 || port == 0u || port > 31u) return;
    for (i = 0; i < HYPE_XHCI_HUB_MAX; i++) {
        if (g_hubs[i].used && g_hubs[i].ctrl == c->hw_slot && g_hubs[i].slot == hub_slot) {
            if (ignore) g_hubs[i].ignore_ports |= (1u << port);
            else g_hubs[i].ignore_ports &= ~(1u << port);
            return;
        }
    }
}

unsigned int hype_xhci_pump_events(hype_xhci_ctrl_t *c, unsigned int budget) {
    xhci_hw_t *hw;
    volatile uint8_t *bar;
    unsigned int n = 0, spins = 0;
    uint32_t evt[4];

    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) {
        return 0u;
    }
    hw = HW(c);
    bar = (volatile uint8_t *)(uintptr_t)c->bar;
    while (n < budget &&
           next_event_budget(hw, bar, c->rtsoff, evt, XHCI_POLL_PEEK, &spins) == 0) {
        /* Port-change events are banked inside next_event_budget and never reach here.
         * Anything that does is a transfer or command event, and route_foreign_event
         * gives it to its owner or parks it -- never drops it (#761). */
        route_foreign_event(c, hw, evt);
        n++;
    }
    return n;
}

void hype_xhci_int_in_routes(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int ep_addr,
                             unsigned long long *handed, unsigned long long *deliv,
                             unsigned long long *own, unsigned long long *to_park) {
    xhci_int_in_hw_t *iin;
    if (handed) *handed = 0; if (deliv) *deliv = 0;
    if (own) *own = 0; if (to_park) *to_park = 0;
    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) return;
    iin = iin_hw_for(c, slot, hype_xhci_ep_dci(ep_addr), 0);
    if (iin == (xhci_int_in_hw_t *)0) return;
    if (handed) *handed = iin->handed;
    if (deliv) *deliv = iin->deliv;
    if (own) *own = iin->own;
    if (to_park) *to_park = iin->to_park;
}

unsigned long long hype_xhci_int_in_arms(hype_xhci_ctrl_t *c, unsigned int slot,
                                         unsigned int ep_addr) {
    xhci_int_in_hw_t *iin;
    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) return 0ull;
    iin = iin_hw_for(c, slot, hype_xhci_ep_dci(ep_addr), 0);
    return (iin != (xhci_int_in_hw_t *)0) ? iin->arms : 0ull;
}

/*
 * The two numbers that tell a lost completion apart from a stopped controller.
 *
 * `lost` counts completions that named a TRB this endpoint did not hold outstanding --
 * stale or duplicate, correctly refused. `skipped` counts transfers retired because a
 * LATER one completed, which means their own events were never seen. Both should stay at
 * zero; either one moving says where to look next.
 */
/* Revives performed on this endpoint. Non-zero means hype rebuilt a silent endpoint; if
 * reports resume afterwards, the revive worked and the deafness is recoverable in software. */
unsigned long long hype_xhci_int_in_revives(hype_xhci_ctrl_t *c, unsigned int slot,
                                            unsigned int ep_addr) {
    xhci_int_in_hw_t *iin;
    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) return 0ull;
    iin = iin_hw_for(c, slot, hype_xhci_ep_dci(ep_addr), 0);
    return (iin != (xhci_int_in_hw_t *)0) ? iin->revives : 0ull;
}

/*
 * Revives ATTEMPTED that failed, and transfers retired by our own Stop Endpoint. A deaf
 * endpoint with revive_fails climbing is a stopped command ring, not a stopped device --
 * the distinction boot 31 spent 74 minutes unable to make.
 */
void hype_xhci_int_in_revive_health(hype_xhci_ctrl_t *c, unsigned int slot,
                                    unsigned int ep_addr, unsigned long long *fails,
                                    unsigned long long *stopped) {
    xhci_int_in_hw_t *iin;
    if (fails) *fails = 0;
    if (stopped) *stopped = 0;
    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) return;
    iin = iin_hw_for(c, slot, hype_xhci_ep_dci(ep_addr), 0);
    if (iin == (xhci_int_in_hw_t *)0) return;
    if (fails) *fails = iin->revive_fails;
    if (stopped) *stopped = iin->stopped_ccs;
}

/*
 * Command-ring health. `dead` non-zero means hype has stopped trying: either the controller
 * reported HSE/HCE, or the ring could not be restarted. Every device on this controller is
 * unreachable at that point, and saying so is the whole reason this exists.
 */
unsigned int hype_xhci_silence_revive_enabled(void) {
    return HYPE_INT_IN_SILENCE_REVIVE ? 1u : 0u;
}

void hype_xhci_cmd_ring_health(hype_xhci_ctrl_t *c, unsigned long long *timeouts,
                               unsigned long long *guard, unsigned long long *recoveries,
                               int *dead) {
    xhci_hw_t *hw;
    if (timeouts) *timeouts = 0;
    if (guard) *guard = 0;
    if (recoveries) *recoveries = 0;
    if (dead) *dead = 0;
    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) return;
    hw = HW(c);
    if (timeouts) *timeouts = hw->cmd_timeouts;
    if (guard) *guard = hw->cmd_guard_exhausted;
    if (recoveries) *recoveries = hw->cmd_recoveries;
    if (dead) *dead = hw->cmd_dead;
}

int hype_xhci_reset_wanted(const hype_xhci_ctrl_t *c) {
    if (c == (const hype_xhci_ctrl_t *)0 || !c->inited) return 0;
    return HW(c)->reset_wanted;
}

unsigned int hype_xhci_wedge_injection_ms(unsigned int *ctrl) {
    if (ctrl) *ctrl = (unsigned int)HYPE_781_WEDGE_CTRL;
    return (unsigned int)HYPE_781_WEDGE_MS;
}

void hype_xhci_int_in_losses(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int ep_addr,
                             unsigned long long *lost, unsigned long long *skipped) {
    xhci_int_in_hw_t *iin;
    if (lost) *lost = 0;
    if (skipped) *skipped = 0;
    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) return;
    iin = iin_hw_for(c, slot, hype_xhci_ep_dci(ep_addr), 0);
    if (iin == (xhci_int_in_hw_t *)0) return;
    if (lost) *lost = iin->q.lost;
    if (skipped) *skipped = iin->q.skipped;
}

/*
 * Controller-wide event health. `ring_full` non-zero is the controller saying it stopped
 * (xHCI 4.9.4); `evictions` non-zero is the shared parked table dropping a completion,
 * which no longer happens for interrupt-IN endpoints and so now points at the MSC path.
 */
void hype_xhci_event_health(hype_xhci_ctrl_t *c, unsigned long long *hc_events,
                            unsigned long long *ring_full, unsigned long long *evictions) {
    xhci_hw_t *hw;
    if (hc_events) *hc_events = 0;
    if (ring_full) *ring_full = 0;
    if (evictions) *evictions = 0;
    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) return;
    hw = HW(c);
    if (hc_events) *hc_events = hw->hc_events;
    if (ring_full) *ring_full = hw->ring_full_events;
    if (evictions) *evictions = hype_xhci_parked_evictions(&hw->parked);
}

int hype_xhci_probe_silence(hype_xhci_ctrl_t *c, unsigned int root_port,
                            hype_xhci_silence_probe_t *out) {
    xhci_hw_t *hw;
    volatile uint8_t *bar;
    uint32_t ir, cmd[4], evt[4];
    uint64_t t0;

    if (out == (hype_xhci_silence_probe_t *)0) return -1;
    zero((uint8_t *)out, sizeof *out);
    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) return -1;
    hw = HW(c);
    bar = (volatile uint8_t *)(uintptr_t)c->bar;
    ir = c->rtsoff + HYPE_XHCI_RT_IR0;

    out->usbsts = rd32(bar, c->op + HYPE_XHCI_OP_USBSTS);
    out->usbcmd = rd32(bar, c->op + HYPE_XHCI_OP_USBCMD);
    out->crcr_lo = rd32(bar, c->op + HYPE_XHCI_OP_CRCR);
    out->iman = rd32(bar, ir + HYPE_XHCI_IR_IMAN);
    out->imod = rd32(bar, ir + HYPE_XHCI_IR_IMOD);
    out->erdp = (uint64_t)rd32(bar, ir + HYPE_XHCI_IR_ERDP) |
                ((uint64_t)rd32(bar, ir + HYPE_XHCI_IR_ERDP + 4u) << 32);
    out->sw_deq = phys(hw->evt_ring) + (uint64_t)hw->evt_deq * HYPE_XHCI_TRB_BYTES;
    out->pending_event = ((int)(trb_dw(hw->evt_ring, hw->evt_deq, 3) & 1u) == (int)hw->evt_cyc)
                             ? 1u : 0u;
    if (root_port != 0u) {
        out->portsc = rd32(bar, c->op + hype_xhci_portsc_offset(root_port));
    }

    /*
     * The No-Op goes through the same path every real command takes, so a timeout here is
     * counted, recovered and latched exactly as one would be for a Stop Endpoint -- which is
     * the point: this is the cheapest way to find out what a Stop Endpoint would have done.
     */
    hype_xhci_trb_noop_cmd(cmd, (int)hw->cmd_cyc);
    t0 = rdtsc_now();
    out->noop_rc = cmd_submit_wait(c, cmd, evt);
    out->noop_us = g_tsc_hz != 0ull
                       ? (unsigned int)((rdtsc_now() - t0) / (g_tsc_hz / 1000000ull))
                       : 0u;
    if (out->noop_rc == 0) out->noop_cc = hype_xhci_event_cc(evt);
    out->cmd_timeouts = hw->cmd_timeouts;
    return 0;
}

unsigned int hype_xhci_discard_port_changes(hype_xhci_ctrl_t *c) {
    xhci_hw_t *hw;
    unsigned int n = 0u, p;

    if (c == (hype_xhci_ctrl_t *)0 || !c->inited) {
        return 0u;
    }
    hw = HW(c);
    for (p = 1u; p <= 31u; p++) {
        if (hw->port_changed & (1u << p)) {
            n++;
        }
    }
    hw->port_changed = 0u;
    return n;
}

unsigned long long hype_xhci_port_event_count(const hype_xhci_ctrl_t *c) {
    return (c == (const hype_xhci_ctrl_t *)0 || !c->inited) ? 0ull : HW(c)->port_events;
}

int hype_xhci_port_connected(hype_xhci_ctrl_t *c, unsigned int port, int *out_connected) {
    volatile uint8_t *bar;
    uint32_t sc;

    if (c == (hype_xhci_ctrl_t *)0 || !c->inited || port == 0u || out_connected == (int *)0) {
        return -1;
    }
    bar = (volatile uint8_t *)(uintptr_t)c->bar;
    sc = rd32(bar, c->op + hype_xhci_portsc_offset(port));
    /*
     * #758: read CCS twice and require agreement before believing it.
     *
     * A port that has just changed is still settling, and a single read can catch it
     * mid-transition -- rig 744 failed about one run in four with "port N changed --
     * something is attached" on an unplug, and because the change bits were ACKed in the
     * same breath, no further event came and the departure was lost for good.
     *
     * The two reads are separated by a PORTSC read of the same register, which is a real
     * MMIO round trip rather than a delay loop: enough for a transitioning line to settle,
     * and nothing measurable in a sweep that runs from the guest dispatch loop.
     */
    {
        uint32_t sc2 = rd32(bar, c->op + hype_xhci_portsc_offset(port));
        if ((sc ^ sc2) & HYPE_XHCI_PORTSC_CCS) {
            /*
             * Still moving. Do NOT ACK -- leaving the change bits set is what keeps the
             * controller willing to tell us again, and re-banking the port means the sweep
             * reconsiders it next tick rather than committing to a reading it cannot
             * justify. Refusing to classify is the safe answer here: a wrong "now empty"
             * tears down a working device, and a wrong "attached" loses a departure.
             */
            xhci_hw_t *hw = HW(c);
            if (port <= 31u) hw->port_changed |= (1u << port);
            return -1;
        }
        sc = sc2;
    }
    /*
     * #744: ACK the change bits while we are here, by writing them back. PORTSC's change
     * bits are write-1-to-clear and the controller will not raise another event for this
     * port until they are cleared -- so a reader that only read would see the first
     * unplug and nothing ever again.
     *
     * It must be a read-modify-write. This was `sc & HYPE_XHCI_PORTSC_CHANGE_MASK`, which
     * clears the change bits correctly and writes 0 into every other RW bit in the register
     * on the way past -- PP, Port Power, among them. Boot 32 measured the result: hype saw
     * the keyboard leave root port 10, ACKed the event, and switched the port off in the
     * same write. Plugging the keyboard back in produced nothing whatsoever, because an
     * unpowered port never reports a connect, so no further event was ever raised and
     * #745's arrival path was never reached. See hype_xhci_portsc_ack_changes().
     */
    wr32(bar, c->op + hype_xhci_portsc_offset(port), hype_xhci_portsc_ack_changes(sc));
    *out_connected = (sc & HYPE_XHCI_PORTSC_CCS) ? 1 : 0;
    return 0;
}

int hype_xhci_release_device(hype_xhci_ctrl_t *c, unsigned int slot) {
    xhci_hw_t *hw;
    int rc;

    if (c == (hype_xhci_ctrl_t *)0 || !c->inited || slot == 0u) {
        return -1;
    }
    hw = HW(c);
    /*
     * #744: drop this slot's interrupt-IN blocks BEFORE the Disable Slot.
     *
     * They are a fixed pool of HYPE_XHCI_INT_IN_MAX keyed by (controller, slot, dci). A
     * departed device that keeps its block starves the pool -- and worse, the next device
     * to take that slot id would find a block already claimed for its (slot, dci) holding
     * the previous tenant's ring, cycle state and armed flag, which is #734's shared-state
     * bug wearing a different hat.
     */
    hype_xhci_int_in_release_slot(g_iin_key, HYPE_XHCI_INT_IN_MAX, c->hw_slot, slot);
    rc = hype_xhci_disable_slot(c, slot);
    /*
     * And the DCBAA entry. dev_free() (inside disable_slot) releases hype's pool index but
     * left DCBAA[slot] pointing at a device-context page that has gone back into the pool,
     * so the controller held a live pointer to memory hype had reassigned. Harmless in
     * practice today -- hype_xhci_address_device() rewrites the entry before it issues the
     * command -- but "correct only because nobody reads it in between" is not a state to
     * leave a DMA structure in, and #743 is a live question about exactly this transition.
     */
    put_le64(hw->dcbaa + (slot & 0xFFu) * 8u, 0ull);
    return rc;
}

int hype_xhci_disable_slot(hype_xhci_ctrl_t *c, unsigned int slot) {
    xhci_hw_t *hw = HW(c);
    uint32_t cmd[4], evt[4];
    if (!c->inited || slot == 0u) return -1;
    hype_xhci_trb_disable_slot(cmd, slot, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) { dev_free(hw, slot); return -1; }
    hype_debug_print("host-xhci:     Disable Slot: %u [#734]\n", slot);
    dev_free(hw, slot); /* release the pool entry regardless of the controller's verdict */
    return (hype_xhci_event_cc(evt) == HYPE_XHCI_CC_SUCCESS) ? 0 : -1;
}
