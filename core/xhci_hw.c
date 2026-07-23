#include "xhci.h"

/*
 * Hardware shim for the xHCI host driver: real MMIO bring-up + port reset.
 * Coverage-exempt (like nvme_host_hw.c / ahci_host_hw.c) -- it pokes device
 * registers and spins on status bits. The pure register/TRB/ring model it
 * builds on lives in xhci.c and is unit-tested. Identity-mapped physical ==
 * pointer, per hype's flat map; all DMA-visible structures live in hype's .bss.
 * Post-ExitBootServices only.
 */

#define XPAGE 4096u
#define RING_TRBS 16u
#define MAX_SCRATCH 64u
#define SPIN 20000000u

/* DMA-visible controller structures (physically contiguous, hype .bss). */
static uint8_t g_dcbaa[XPAGE] __attribute__((aligned(XPAGE)));       /* device context base addr array */
static uint8_t g_cmd_ring[XPAGE] __attribute__((aligned(XPAGE)));    /* command ring */
static uint8_t g_evt_ring[XPAGE] __attribute__((aligned(XPAGE)));    /* event ring segment 0 */
static uint8_t g_erst[64] __attribute__((aligned(64)));              /* event ring segment table */
static uint8_t g_scratch_arr[XPAGE] __attribute__((aligned(XPAGE))); /* scratchpad buffer array */
static uint8_t g_scratch_pages[MAX_SCRATCH][XPAGE] __attribute__((aligned(XPAGE)));
/* pt3b: one addressed device's Input/Device contexts, EP0 ring + a transfer buf. */
static uint8_t g_input_ctx[XPAGE] __attribute__((aligned(XPAGE)));
static uint8_t g_dev_ctx[XPAGE] __attribute__((aligned(XPAGE)));
static uint8_t g_ep0_ring[XPAGE] __attribute__((aligned(XPAGE)));
static uint8_t g_xfer_buf[XPAGE] __attribute__((aligned(XPAGE)));
static unsigned int g_ep0_enq;
static unsigned int g_ep0_cyc;

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

/* --- command + event ring state (single controller) --- */
static unsigned int g_cmd_enq;   /* command-ring enqueue index */
static unsigned int g_cmd_cyc;   /* command-ring producer cycle bit */
static unsigned int g_evt_deq;   /* event-ring dequeue index */
static unsigned int g_evt_cyc;   /* event-ring consumer cycle bit */

static void ring_state_reset(void) { g_cmd_enq = 0; g_cmd_cyc = 1; g_evt_deq = 0; g_evt_cyc = 1; }

/* Volatile read of dword `dw` of TRB `idx` in a ring (controller DMAs into it). */
static uint32_t trb_dw(const uint8_t *ring, unsigned int idx, unsigned int dw) {
    return *(volatile uint32_t *)(ring + idx * HYPE_XHCI_TRB_BYTES + dw * 4u);
}

/* Enqueue a fully-built command TRB (cycle already stamped by the caller as
 * g_cmd_cyc) onto the command ring, handling the Link-TRB wrap. */
static void cmd_enqueue(const uint32_t trb[4]) {
    uint8_t *slot = g_cmd_ring + g_cmd_enq * HYPE_XHCI_TRB_BYTES;
    put_le32(slot + 0, trb[0]);
    put_le32(slot + 4, trb[1]);
    put_le32(slot + 8, trb[2]);
    put_le32(slot + 12, trb[3]);
    g_cmd_enq++;
    if (g_cmd_enq == RING_TRBS - 1u) { /* reached the Link TRB slot */
        uint32_t link[4];
        hype_xhci_trb_link(link, phys(g_cmd_ring), g_cmd_cyc); /* match current producer cycle */
        put_le32(g_cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
        put_le32(g_cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
        put_le32(g_cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
        put_le32(g_cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
        g_cmd_enq = 0;
        g_cmd_cyc ^= 1u;
    }
}

/* Poll the event ring for the next valid event (cycle == consumer cycle),
 * copy it to out[4], advance the dequeue pointer + ERDP. -1 on timeout. */
static int next_event(volatile uint8_t *bar, uint32_t rtsoff, uint32_t out[4]) {
    unsigned int spins = SPIN;
    while (spins-- != 0u) {
        uint32_t d3 = trb_dw(g_evt_ring, g_evt_deq, 3);
        if ((int)(d3 & 1u) == (int)g_evt_cyc) {
            out[0] = trb_dw(g_evt_ring, g_evt_deq, 0);
            out[1] = trb_dw(g_evt_ring, g_evt_deq, 1);
            out[2] = trb_dw(g_evt_ring, g_evt_deq, 2);
            out[3] = d3;
            g_evt_deq++;
            if (g_evt_deq >= RING_TRBS) { g_evt_deq = 0; g_evt_cyc ^= 1u; }
            /* ERDP = address of the new dequeue slot, with EHB (bit3) written 1 to clear. */
            wr64(bar, hype_xhci_ir0_offset(rtsoff, HYPE_XHCI_IR_ERDP),
                 (phys(g_evt_ring) + (uint64_t)g_evt_deq * HYPE_XHCI_TRB_BYTES) | (1u << 3));
            return 0;
        }
    }
    return -1;
}

/* Enqueue a command TRB, ring the command doorbell (DB[0]), and consume events
 * until the matching Command Completion Event. Returns it in evt[4], or -1. */
static int cmd_submit_wait(hype_xhci_ctrl_t *c, uint32_t cmd[4], uint32_t evt[4]) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    unsigned int guard = 64u; /* bound the number of skipped (e.g. port-change) events */
    cmd_enqueue(cmd);
    wr32(bar, hype_xhci_doorbell_offset(c->dboff, 0), 0u); /* command doorbell, target 0 */
    while (guard-- != 0u) {
        if (next_event(bar, c->rtsoff, evt) != 0) return -1;
        if (hype_xhci_trb_type(evt) == HYPE_XHCI_TRB_CMD_COMPLETION) return 0;
        /* else: a Port Status Change or other event queued earlier -- skip it. */
    }
    return -1;
}

int hype_xhci_enable_slot(hype_xhci_ctrl_t *c, unsigned int *out_slot) {
    uint32_t cmd[4], evt[4];
    if (!c->inited) return -1;
    hype_xhci_trb_enable_slot(cmd, (int)g_cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) return -1;
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) return -1;
    if (out_slot) *out_slot = hype_xhci_event_slot_id(evt);
    return 0;
}

/* Write an 8-dword context into `base` at byte offset `off`. */
static void write_ctx(uint8_t *base, unsigned int off, const uint32_t c[8]) {
    unsigned int i;
    for (i = 0; i < 8u; i++) put_le32(base + off + i * 4u, c[i]);
}

/* Enqueue a transfer TRB (cycle already = g_ep0_cyc) on the EP0 ring, with the
 * same Link-TRB wrap handling as the command ring. */
static void ep0_enqueue(const uint32_t trb[4]) {
    uint8_t *slot = g_ep0_ring + g_ep0_enq * HYPE_XHCI_TRB_BYTES;
    put_le32(slot + 0, trb[0]);
    put_le32(slot + 4, trb[1]);
    put_le32(slot + 8, trb[2]);
    put_le32(slot + 12, trb[3]);
    g_ep0_enq++;
    if (g_ep0_enq == RING_TRBS - 1u) {
        uint32_t link[4];
        hype_xhci_trb_link(link, phys(g_ep0_ring), g_ep0_cyc);
        put_le32(g_ep0_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
        put_le32(g_ep0_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
        put_le32(g_ep0_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
        put_le32(g_ep0_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
        g_ep0_enq = 0;
        g_ep0_cyc ^= 1u;
    }
}

int hype_xhci_address_device(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int root_port,
                             unsigned int speed) {
    unsigned int cs = c->ctx_size;
    uint32_t ctx[8];
    uint32_t cmd[4], evt[4];

    if (!c->inited || slot == 0u) return -1;

    /* Fresh Input/Device contexts + EP0 transfer ring (Link TRB at the end). */
    zero(g_input_ctx, XPAGE);
    zero(g_dev_ctx, XPAGE);
    zero(g_ep0_ring, XPAGE);
    {
        uint32_t link[4];
        hype_xhci_trb_link(link, phys(g_ep0_ring), 1);
        put_le32(g_ep0_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
        put_le32(g_ep0_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
        put_le32(g_ep0_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
        put_le32(g_ep0_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
    }
    g_ep0_enq = 0;
    g_ep0_cyc = 1;

    /* Input Control Context (offset 0): add the slot + EP0 contexts. */
    hype_xhci_input_ctrl_ctx(ctx, HYPE_XHCI_ADD_SLOT | HYPE_XHCI_ADD_EP0, 0);
    write_ctx(g_input_ctx, 0, ctx);
    /* Slot Context (offset 1*ctx_size): route 0, 1 valid context entry (EP0). */
    hype_xhci_slot_ctx(ctx, 0, speed, 1, root_port);
    write_ctx(g_input_ctx, cs, ctx);
    /* EP0 Context (offset 2*ctx_size). */
    hype_xhci_ep0_ctx(ctx, hype_xhci_default_mps(speed), phys(g_ep0_ring), 1);
    write_ctx(g_input_ctx, 2u * cs, ctx);

    /* DCBAA[slot] -> the output Device Context. */
    put_le64(g_dcbaa + slot * 8u, phys(g_dev_ctx));

    hype_xhci_trb_address_device(cmd, phys(g_input_ctx), slot, 0, (int)g_cmd_cyc);
    if (cmd_submit_wait(c, cmd, evt) != 0) return -1;
    if (hype_xhci_event_cc(evt) != HYPE_XHCI_CC_SUCCESS) return -1;
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
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    uint32_t t[4], evt[4];
    unsigned int guard = 64u;
    unsigned int trt, status_dir_in, i;

    if (!c->inited || slot == 0u || len > XPAGE) return -1;
    trt = (len == 0u) ? HYPE_XHCI_TRT_NO_DATA : (dir_in ? HYPE_XHCI_TRT_IN : HYPE_XHCI_TRT_OUT);

    if (len && !dir_in && buf) {
        for (i = 0; i < len; i++) g_xfer_buf[i] = ((const uint8_t *)buf)[i];
    } else {
        zero(g_xfer_buf, XPAGE);
    }

    hype_xhci_trb_setup_stage(t, bm_req, b_req, wvalue, windex, (uint16_t)len, trt, (int)g_ep0_cyc);
    ep0_enqueue(t);
    if (len) {
        hype_xhci_trb_data_stage(t, phys(g_xfer_buf), len, dir_in, (int)g_ep0_cyc);
        ep0_enqueue(t);
    }
    /* Status stage direction is opposite the data direction (IN if no data). */
    status_dir_in = (len && dir_in) ? 0u : 1u;
    hype_xhci_trb_status_stage(t, (int)status_dir_in, 1, (int)g_ep0_cyc);
    ep0_enqueue(t);

    wr32(bar, hype_xhci_doorbell_offset(c->dboff, slot), 1u); /* DCI 1 = EP0 */

    while (guard-- != 0u) {
        if (next_event(bar, c->rtsoff, evt) != 0) return -1;
        if (hype_xhci_trb_type(evt) == HYPE_XHCI_TRB_TRANSFER_EVENT) {
            unsigned int cc = hype_xhci_event_cc(evt);
            if (cc != HYPE_XHCI_CC_SUCCESS && cc != HYPE_XHCI_CC_SHORT_PACKET) return -1;
            if (len && dir_in && buf) {
                for (i = 0; i < len; i++) ((uint8_t *)buf)[i] = g_xfer_buf[i];
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

int hype_xhci_set_configuration(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int config_value) {
    /* SET_CONFIGURATION: bmRequestType=0x00 (OUT/standard/device), bRequest=9,
     * wValue=config, no data stage. */
    return control_transfer(c, slot, 0x00, 9, (uint16_t)config_value, 0, 0, 0u, 0);
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

    out->inited = 0;
    out->bar = bar_phys;
    out->op = op;
    out->dboff = rd32(bar, HYPE_XHCI_CAP_DBOFF);
    out->rtsoff = rd32(bar, HYPE_XHCI_CAP_RTSOFF);
    out->max_slots = max_slots;
    out->max_ports = max_ports;
    out->ctx_size = hype_xhci_context_size(hcc1);

    /* Wait for the controller to be ready (CNR clear). */
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBSTS, HYPE_XHCI_USBSTS_CNR, 0) != 0) return -1;

    /* Stop, then wait halted. */
    wr32(bar, op + HYPE_XHCI_OP_USBCMD,
         rd32(bar, op + HYPE_XHCI_OP_USBCMD) & ~HYPE_XHCI_USBCMD_RS);
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBSTS, HYPE_XHCI_USBSTS_HCH,
                  HYPE_XHCI_USBSTS_HCH) != 0) return -1;

    /* Reset (HCRST); wait it self-clears + CNR clears. */
    wr32(bar, op + HYPE_XHCI_OP_USBCMD, HYPE_XHCI_USBCMD_HCRST);
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBCMD, HYPE_XHCI_USBCMD_HCRST, 0) != 0) return -1;
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBSTS, HYPE_XHCI_USBSTS_CNR, 0) != 0) return -1;

    if (max_slots == 0u) return -1;

    /* Program the number of enabled device slots. */
    wr32(bar, op + HYPE_XHCI_OP_CONFIG, max_slots);

    /* Scratchpad buffers, if the controller wants any: DCBAA[0] points at an
     * array of page pointers, each a hype .bss page. */
    zero(g_dcbaa, XPAGE);
    if (nscratch > 0u) {
        if (nscratch > MAX_SCRATCH) nscratch = MAX_SCRATCH;
        zero(g_scratch_arr, XPAGE);
        for (i = 0; i < nscratch; i++) {
            zero(g_scratch_pages[i], XPAGE);
            put_le64(g_scratch_arr + i * 8u, phys(g_scratch_pages[i]));
        }
        put_le64(g_dcbaa + 0, phys(g_scratch_arr)); /* DCBAA[0] = scratchpad array */
    }
    wr64(bar, op + HYPE_XHCI_OP_DCBAAP, phys(g_dcbaa));

    /* Command ring: zeroed TRBs with a Link TRB (toggle-cycle) at the end,
     * pointing back to the start. CRCR = ring | RCS(1). */
    zero(g_cmd_ring, XPAGE);
    {
        uint32_t link[4];
        hype_xhci_trb_link(link, phys(g_cmd_ring), 1);
        put_le32(g_cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 0, link[0]);
        put_le32(g_cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 4, link[1]);
        put_le32(g_cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 8, link[2]);
        put_le32(g_cmd_ring + (RING_TRBS - 1u) * HYPE_XHCI_TRB_BYTES + 12, link[3]);
    }
    wr64(bar, op + HYPE_XHCI_OP_CRCR, phys(g_cmd_ring) | 1u /* RCS */);

    /* Event ring: one segment, described by a single-entry ERST. */
    zero(g_evt_ring, XPAGE);
    zero(g_erst, 64);
    put_le64(g_erst + 0, phys(g_evt_ring)); /* segment base */
    put_le32(g_erst + 8, RING_TRBS);        /* segment size (TRBs) */
    {
        uint32_t ir = out->rtsoff;
        wr32(bar, hype_xhci_ir0_offset(ir, HYPE_XHCI_IR_ERSTSZ), 1u); /* one segment */
        wr64(bar, hype_xhci_ir0_offset(ir, HYPE_XHCI_IR_ERDP), phys(g_evt_ring));
        wr64(bar, hype_xhci_ir0_offset(ir, HYPE_XHCI_IR_ERSTBA), phys(g_erst));
    }

    /* Run. */
    wr32(bar, op + HYPE_XHCI_OP_USBCMD,
         rd32(bar, op + HYPE_XHCI_OP_USBCMD) | HYPE_XHCI_USBCMD_RS);
    if (wait_bits(bar, op + HYPE_XHCI_OP_USBSTS, HYPE_XHCI_USBSTS_HCH, 0) != 0) return -1;

    ring_state_reset();
    out->inited = 1;
    return 0;
}

unsigned int hype_xhci_detect_device(hype_xhci_ctrl_t *c, unsigned int *out_speed) {
    volatile uint8_t *bar = (volatile uint8_t *)(uintptr_t)c->bar;
    unsigned int port;

    if (out_speed) *out_speed = 0;
    for (port = 1u; port <= c->max_ports; port++) {
        uint32_t off = c->op + hype_xhci_portsc_offset(port);
        uint32_t sc = rd32(bar, off);

        /* Power the port if it isn't already. */
        if (!(sc & HYPE_XHCI_PORTSC_PP)) {
            wr32(bar, off, hype_xhci_portsc_write_preserve(sc, HYPE_XHCI_PORTSC_PP));
            short_delay();
            sc = rd32(bar, off);
        }
        if (!(sc & HYPE_XHCI_PORTSC_CCS)) continue; /* nothing attached */

        /* USB3 ports enable themselves on connect; USB2 need an explicit reset. */
        if (!(sc & HYPE_XHCI_PORTSC_PED)) {
            wr32(bar, off, hype_xhci_portsc_write_preserve(sc, HYPE_XHCI_PORTSC_PR));
            /* wait for reset to complete (PRC) or the port to enable */
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
            return port;
        }
    }
    return 0;
}
