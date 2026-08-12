#include "xhci.h"
#include "usb_msc.h"
#include "fatal.h" /* hype_debug_print -- hub-descent diagnostics (real HW visibility) */

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
#define RING_TRBS 16u
#define MAX_SCRATCH 64u
#define DEVPOOL 8u
#define SPIN 20000000u
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

    /* USB-5 (#217): the HID keyboard's interrupt-IN transfer ring and report buffer. Kept
     * separate from the bulk rings so a keyboard poll can never disturb the MSC datapath
     * the log sink depends on. */
    uint8_t int_in_ring[XPAGE] __attribute__((aligned(XPAGE)));
    uint8_t hid_report[64] __attribute__((aligned(64)));
    unsigned int iin_enq;
    unsigned int iin_cyc;
    /*
     * Is a report transfer currently OUTSTANDING?
     *
     * An interrupt endpoint needs exactly ONE transfer queued at a time, re-armed after
     * each completion. Enqueuing on every poll call -- which the guest dispatch loop makes
     * thousands of times a second -- fills the 256-TRB ring in a fraction of a second and
     * the keyboard goes silent, which is precisely what the first QEMU test showed: the
     * endpoint configured, the device claimed, and not one report ever delivered.
     */
    int iin_armed;
    uint64_t iin_pending_trb;

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

    /* MSC bulk endpoint transfer rings. */
    uint8_t bulk_in_ring[XPAGE] __attribute__((aligned(XPAGE)));
    uint8_t bulk_out_ring[XPAGE] __attribute__((aligned(XPAGE)));
    unsigned int bin_enq, bin_cyc, bout_enq, bout_cyc;
    /* BOT command/status wrappers + a bulk data bounce buffer. */
    uint8_t cbw[64] __attribute__((aligned(64)));
    uint8_t csw[64] __attribute__((aligned(64)));
    /* Page-aligned for DMA; larger than a page, but only page alignment is architecturally
     * required. */
    uint8_t data[XDATA] __attribute__((aligned(4096)));
    uint32_t bot_tag;

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
} xhci_hw_t;

static xhci_hw_t g_hw[HYPE_XHCI_MAX_CTRL];

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
 * handling the Link-TRB wrap + producer-cycle toggle. */
static void ring_enqueue(uint8_t *ring, unsigned int *enq, unsigned int *cyc,
                         const uint32_t trb[4]) {
    uint8_t *slot = ring + (*enq) * HYPE_XHCI_TRB_BYTES;
    put_le32(slot + 0, trb[0]);
    put_le32(slot + 4, trb[1]);
    put_le32(slot + 8, trb[2]);
    put_le32(slot + 12, trb[3]);
    (*enq)++;
    if (*enq == RING_TRBS - 1u) { /* reached the Link TRB slot */
        uint32_t link[4];
        hype_xhci_trb_link(link, phys(ring), *cyc); /* match current producer cycle */
        put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
        put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
        put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
        put_le32(ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
        *enq = 0;
        *cyc ^= 1u;
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
static int next_event_budget(xhci_hw_t *hw, volatile uint8_t *bar, uint32_t rtsoff,
                             uint32_t out[4], unsigned int budget, unsigned int *spins_used) {
    unsigned int spins = budget;
    while (spins-- != 0u) {
        uint32_t d3 = trb_dw(hw->evt_ring, hw->evt_deq, 3);
        if ((int)(d3 & 1u) == (int)hw->evt_cyc) {
            out[0] = trb_dw(hw->evt_ring, hw->evt_deq, 0);
            out[1] = trb_dw(hw->evt_ring, hw->evt_deq, 1);
            out[2] = trb_dw(hw->evt_ring, hw->evt_deq, 2);
            out[3] = d3;
            hw->evt_deq++;
            if (hw->evt_deq >= RING_TRBS) { hw->evt_deq = 0; hw->evt_cyc ^= 1u; }
            /* ERDP = address of the new dequeue slot, with EHB (bit3) written 1 to clear. */
            wr64(bar, hype_xhci_ir0_offset(rtsoff, HYPE_XHCI_IR_ERDP),
                 (phys(hw->evt_ring) + (uint64_t)hw->evt_deq * HYPE_XHCI_TRB_BYTES) | (1u << 3));
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
static int cmd_submit_wait(hype_xhci_ctrl_t *c, uint32_t cmd[4], uint32_t evt[4]) {
    xhci_hw_t *hw = HW(c);
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    unsigned int guard = 64u; /* bound the number of skipped (e.g. port-change) events */
    uint64_t my_trb = phys(hw->cmd_ring) + (uint64_t)hw->cmd_enq * HYPE_XHCI_TRB_BYTES;
    cmd_enqueue(hw, cmd);
    wr32(bar, hype_xhci_doorbell_offset(c->dboff, 0), 0u); /* command doorbell, target 0 */
    while (guard-- != 0u) {
        if (next_event(hw, bar, c->rtsoff, evt) != 0) {
            /* #266: distinguish "nothing arrived" from "things arrived, none ours". */
            hype_debug_print("host-xhci: #266 command TIMEOUT waiting for trb=0x%llx "
                             "(no event arrived)\n", (unsigned long long)my_trb);
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
        /* else: a Port Status Change or other event queued earlier -- skip it. */
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
    return 0;
}

/* Write an 8-dword context into `base` at byte offset `off`. */
static void write_ctx(uint8_t *base, unsigned int off, const uint32_t c[8]) {
    unsigned int i;
    for (i = 0; i < 8u; i++) put_le32(base + off + i * 4u, c[i]);
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
                continue; /* another endpoint's (or a stale) event */
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

    if (!c->inited || slot == 0u) return -1;

    /* Fresh bulk transfer rings. */
    zero(hw->bulk_in_ring, XPAGE);
    zero(hw->bulk_out_ring, XPAGE);
    ring_init_link(hw->bulk_in_ring);
    ring_init_link(hw->bulk_out_ring);
    hw->bin_enq = 0; hw->bin_cyc = 1;
    hw->bout_enq = 0; hw->bout_cyc = 1;

    /* Input Context: add the Slot + both bulk endpoint contexts. The Slot
     * Context must re-provide the device's full topology (route/root/TT). */
    zero(hw->input_ctx, XPAGE);
    hype_xhci_input_ctrl_ctx(ctx, HYPE_XHCI_ADD_SLOT | (1u << dci_in) | (1u << dci_out), 0);
    write_ctx(hw->input_ctx, 0, ctx);
    hype_xhci_slot_ctx(ctx, path->route, path->speed, max_dci, path->root_port,
                       path->tt_hub_slot, path->tt_port); /* context entries = highest DCI */
    write_ctx(hw->input_ctx, cs, ctx);
    hype_xhci_ep_ctx(ctx, HYPE_XHCI_EP_TYPE_BULK_IN, msc->bulk_in_mps, phys(hw->bulk_in_ring), 1);
    write_ctx(hw->input_ctx, (1u + dci_in) * cs, ctx);
    hype_xhci_ep_ctx(ctx, HYPE_XHCI_EP_TYPE_BULK_OUT, msc->bulk_out_mps, phys(hw->bulk_out_ring), 1);
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
    uint32_t ctx[8], cmd[4], evt[4];

    if (!c->inited || slot == 0u || path == (const hype_xhci_devpath_t *)0) return -1;
    if (mps == 0u || mps > sizeof(hw->hid_report)) return -1;

    zero(hw->int_in_ring, XPAGE);
    ring_init_link(hw->int_in_ring);
    hw->iin_enq = 0; hw->iin_cyc = 1; hw->iin_armed = 0; hw->iin_pending_trb = 0;

    zero(hw->input_ctx, XPAGE);
    hype_xhci_input_ctrl_ctx(ctx, HYPE_XHCI_ADD_SLOT | (1u << dci), 0);
    write_ctx(hw->input_ctx, 0, ctx);
    hype_xhci_slot_ctx(ctx, path->route, path->speed, dci, path->root_port,
                       path->tt_hub_slot, path->tt_port);
    write_ctx(hw->input_ctx, cs, ctx);
    /* #217: the Interval is what gives the controller a schedule to poll on. Without
     * it the endpoint configures cleanly and never reports. */
    hype_xhci_ep_ctx_interval(ctx, HYPE_XHCI_EP_TYPE_INT_IN, mps, phys(hw->int_in_ring), 1,
                              hype_xhci_interval_encode(path->speed, interval));
    write_ctx(hw->input_ctx, (1u + dci) * cs, ctx);

    hype_xhci_trb_configure_endpoint(cmd, phys(hw->input_ctx), slot, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) return -1;
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) return -1;
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
int hype_xhci_int_in_poll(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int ep_addr,
                          uint8_t *out, unsigned int len) {
    xhci_hw_t *hw = HW(c);
    volatile uint8_t *bar;
    unsigned int dci = hype_xhci_ep_dci(ep_addr);
    uint32_t t[4], evt[4];
    unsigned int spins = 0;
    uint64_t my_trb;
    unsigned int i;

    if (!c->inited || slot == 0u || out == (uint8_t *)0 || len == 0u ||
        len > sizeof(hw->hid_report)) {
        return -1;
    }
    bar = (volatile uint8_t *)(uintptr_t)c->bar;
    my_trb = phys(hw->int_in_ring) + (uint64_t)hw->iin_enq * HYPE_XHCI_TRB_BYTES;

    /* Arm exactly one transfer, and only when none is outstanding. */
    if (!hw->iin_armed) {
        hype_xhci_trb_normal(t, phys(hw->hid_report), len, (int)hw->iin_cyc);
        ring_enqueue(hw->int_in_ring, &hw->iin_enq, &hw->iin_cyc, t);
        wr32(bar, hype_xhci_doorbell_offset(c->dboff, slot), dci);
        hw->iin_armed = 1;
        hw->iin_pending_trb = my_trb;
    }
    my_trb = hw->iin_pending_trb;

    /* #266: a completion for this armed transfer may already be parked from a previous
     * poll -- claim it before spinning. */
    {
        uint32_t parked_cc = 0;
        uint32_t parked_residue = 0;
        if (hype_xhci_parked_take(&hw->parked, slot, dci, my_trb, &parked_cc,
                                  &parked_residue)) {
            hw->iin_armed = 0; /* consumed -- next poll re-arms */
            if (parked_cc != HYPE_XHCI_CC_SUCCESS && parked_cc != HYPE_XHCI_CC_SHORT_PACKET) {
                return -1;
            }
            for (i = 0; i < len; i++) out[i] = hw->hid_report[i];
            return 1;
        }
    }
    if (next_event_budget(hw, bar, c->rtsoff, evt, SPIN / 1024u, &spins) != 0) {
        return 0; /* idle -- the common case, NOT an error */
    }
    if (hype_xhci_event_slot_id(evt) != slot || hype_xhci_event_ep_id(evt) != dci) {
        /* Someone else's completion. Park it rather than discard: discarding is the
         * #266 bug, and the MSC datapath may be waiting for exactly this. */
        (void)hype_xhci_parked_put(&hw->parked, hype_xhci_event_slot_id(evt),
                                   hype_xhci_event_ep_id(evt),
                                   hype_xhci_event_trb_ptr(evt), hype_xhci_event_cc(evt),
                                   hype_xhci_event_xfer_residue(evt));
        return 0;
    }
    hw->iin_armed = 0; /* our completion arrived -- re-arm on the next poll */
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS &&
        hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SHORT_PACKET) {
        return -1;
    }
    for (i = 0; i < len; i++) out[i] = hw->hid_report[i];
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
        hw->evt_deq++;
        if (hw->evt_deq >= RING_TRBS) { hw->evt_deq = 0; hw->evt_cyc ^= 1u; }
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
    if (ep_recover(c, slot, dci_in, hw->bulk_in_ring, &hw->bin_enq, &hw->bin_cyc) != 0) rc = -1;
    if (ep_recover(c, slot, dci_out, hw->bulk_out_ring, &hw->bout_enq, &hw->bout_cyc) != 0) rc = -1;

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
    xhci_hw_t *hw = HW(c);
    unsigned int dci_in = hype_xhci_ep_dci(msc->bulk_in_ep);
    unsigned int dci_out = hype_xhci_ep_dci(msc->bulk_out_ep);
    uint32_t tag = ++hw->bot_tag;

    if (data_len > XDATA) return -1;

    /* CBW on the bulk OUT endpoint. */
    hype_usb_bot_cbw(hw->cbw, tag, data_len, dir_in, 0, cdb, cdb_len);
    if (bulk_xfer(c, hw->bulk_out_ring, &hw->bout_enq, &hw->bout_cyc, slot, dci_out,
                  phys(hw->cbw), HYPE_USB_CBW_LEN) != 0) return -1;

    /* Data phase (bounced through hw->data). */
    if (data_len) {
        if (dir_in) {
            if (bulk_xfer(c, hw->bulk_in_ring, &hw->bin_enq, &hw->bin_cyc, slot, dci_in,
                          phys(hw->data), data_len) != 0) return -1;
            /* #365: word-at-a-time when both sides allow it. At 64 KiB a byte loop is 65536
             * iterations on the hot media path; this cuts it to 8192. */
            bounce_copy(data, hw->data, data_len);
        } else {
            bounce_copy(hw->data, data, data_len);
            if (bulk_xfer(c, hw->bulk_out_ring, &hw->bout_enq, &hw->bout_cyc, slot, dci_out,
                          phys(hw->data), data_len) != 0) return -1;
        }
    }

    /* CSW on the bulk IN endpoint. */
    if (bulk_xfer(c, hw->bulk_in_ring, &hw->bin_enq, &hw->bin_cyc, slot, dci_in,
                  phys(hw->csw), HYPE_USB_CSW_LEN) != 0) return -1;
    return hype_usb_bot_csw_ok(hw->csw, tag) ? 0 : -1;
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

static int bot_scsi(hype_xhci_ctrl_t *c, unsigned int slot, const hype_xhci_msc_eps_t *msc,
                    const uint8_t *cdb, unsigned int cdb_len, uint8_t *data, unsigned int data_len,
                    int dir_in) {
    int forced_fail = 0;

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
    if (!forced_fail && bot_scsi_once(c, slot, msc, cdb, cdb_len, data, data_len, dir_in) == 0) {
        return 0;
    }
    if (bot_recover(c, slot, msc) != 0) {
        hype_debug_print("host-xhci: #266 recovery FAILED -- backend not trustworthy, giving up\n");
        return -1; /* recovery itself failed: the backend is not trustworthy */
    }
    {
        /* #266: whether the retry works is the single most useful fact about this
         * path, and it was never reported -- three recoveries in a real-hardware log
         * told us nothing about whether any of them helped. */
        int rc2 = bot_scsi_once(c, slot, msc, cdb, cdb_len, data, data_len, dir_in);
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
    uint8_t cdb[10];
    unsigned int len = blocks * block_size;
    unsigned int i;
    if (len == 0u || len > XDATA) return -1;
    /* stage the caller's data (bot_scsi bounces from hw->data for OUT). */
    for (i = 0; i < len; i++) ((uint8_t *)hw->data)[i] = ((const uint8_t *)buf)[i];
    hype_scsi_cdb_write10(cdb, lba, (uint16_t)blocks);
    /* pass hw->data as the data pointer so bot_scsi's OUT copy is a self-copy. */
    return bot_scsi(c, slot, msc, cdb, 10u, (uint8_t *)hw->data, len, 0);
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
    uint8_t cdb[10];
    hype_scsi_cdb_synchronize_cache10(cdb);
    return bot_scsi(c, slot, msc, cdb, 10u, (uint8_t *)0, 0u, 0);
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
        hw->iin_enq = 0;
        hw->iin_cyc = 1u;
        hw->iin_armed = 0;
        hw->iin_pending_trb = 0;
        hw->bin_enq = 0; hw->bin_cyc = 0; hw->bout_enq = 0; hw->bout_cyc = 0;
        hw->bot_tag = 0;
        hype_xhci_parked_reset(&hw->parked);
    }

    out->bar = bar_phys;
    out->op = op;
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
#define HUB_FEAT_PORT_RESET        4u
#define HUB_FEAT_PORT_POWER        8u
#define HUB_FEAT_C_PORT_CONNECTION 16u
#define HUB_FEAT_C_PORT_RESET      20u

static int hub_get_descriptor(hype_xhci_ctrl_t *c, unsigned int slot, uint8_t *buf,
                              unsigned int len) {
    /* GET_DESCRIPTOR(HUB): class/IN/device (0xA0), wValue=0x2900, IN. */
    return control_transfer(c, slot, 0xA0, 6, (uint16_t)(HYPE_USB_DESC_HUB << 8), 0, buf, len, 1);
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
    unsigned int nports, port;

    if (!c->inited || hub_slot == 0u || visit == (hype_xhci_hub_visit_fn)0) return -1;
    if (tier == 0u || tier > 5u) return -1; /* xHCI route strings are 5 hub tiers deep */

    if (hub_get_descriptor(c, hub_slot, hubdesc, sizeof hubdesc) != 0) {
        hype_debug_print("host-xhci:   hub slot %u GET hub-descriptor FAILED\n", hub_slot);
        return -1;
    }
    if (hubdesc[1] != HYPE_USB_DESC_HUB) {
        hype_debug_print("host-xhci:   hub slot %u bad hub-descriptor type 0x%02x\n",
                         hub_slot, (unsigned)hubdesc[1]);
        return -1;
    }
    nports = hype_xhci_hub_nbr_ports(hubdesc);
    hype_debug_print("host-xhci:   hub slot %u (tier %u) has %u downstream port(s)\n",
                     hub_slot, tier, nports);

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

        child_speed = hub_port_speed(st);
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



int hype_xhci_disable_slot(hype_xhci_ctrl_t *c, unsigned int slot) {
    xhci_hw_t *hw = HW(c);
    uint32_t cmd[4], evt[4];
    if (!c->inited || slot == 0u) return -1;
    hype_xhci_trb_disable_slot(cmd, slot, (int)hw->cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) { dev_free(hw, slot); return -1; }
    dev_free(hw, slot); /* release the pool entry regardless of the controller's verdict */
    return (hype_xhci_event_cc(evt) == HYPE_XHCI_CC_SUCCESS) ? 0 : -1;
}
