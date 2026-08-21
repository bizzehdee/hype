#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../devices/xhci_dev.h"

/* #591: unit tests for the guest-facing xHCI controller. Rings and contexts are hand-built in one
 * host buffer mapped as a single GPA region, the way test_virtio_blk.c builds a virtqueue. The
 * tests drive the model exactly as a guest xhci-hcd would: program the registers, write TRBs onto
 * the command ring, ring doorbell 0, and read the event ring back. */

static int failures = 0;

#define CHECK_INT(desc, expected, actual)                                                          \
    do {                                                                                           \
        long long _e = (long long)(expected), _a = (long long)(actual);                            \
        if (_e != _a) {                                                                            \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), _e, _a);                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_HEX(desc, expected, actual)                                                          \
    do {                                                                                           \
        unsigned long long _e = (unsigned long long)(expected), _a = (unsigned long long)(actual); \
        if (_e != _a) {                                                                            \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), _e, _a);                      \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* One contiguous guest image. Layout (offsets within the buffer == GPA - GBASE):
 *   0x00000 command ring (64 TRBs)
 *   0x01000 event ring segment (64 TRBs)
 *   0x02000 ERST (one entry)
 *   0x03000 DCBAA (16 entries)
 *   0x04000 device context (slot 1 output)
 *   0x05000 input context
 */
#define GBASE 0x40000000ull
#define OFF_CMD 0x00000u
#define OFF_EVT 0x01000u
#define OFF_ERST 0x02000u
#define OFF_DCBAA 0x03000u
#define OFF_DEVCTX 0x04000u
#define OFF_INCTX 0x05000u
#define IMG_SIZE 0x08000u

typedef struct {
    uint8_t img[IMG_SIZE];
    hype_gpa_map_t map;
    hype_xhci_dev_t dev;
    uint64_t cmd_enqueue; /* our (test producer) command-ring enqueue offset */
    uint8_t cmd_pcs;      /* producer cycle state we write with */
} rig_t;

static void put32(rig_t *r, uint32_t off, uint32_t v) {
    r->img[off + 0] = (uint8_t)v;
    r->img[off + 1] = (uint8_t)(v >> 8);
    r->img[off + 2] = (uint8_t)(v >> 16);
    r->img[off + 3] = (uint8_t)(v >> 24);
}
static uint32_t get32(rig_t *r, uint32_t off) {
    return (uint32_t)r->img[off] | ((uint32_t)r->img[off + 1] << 8) |
           ((uint32_t)r->img[off + 2] << 16) | ((uint32_t)r->img[off + 3] << 24);
}
static void put64(rig_t *r, uint32_t off, uint64_t v) {
    put32(r, off, (uint32_t)v);
    put32(r, off + 4, (uint32_t)(v >> 32));
}

/* MMIO write shorthands. */
static void wr32(rig_t *r, uint32_t off, uint32_t v) {
    int rc = hype_xhci_dev_mmio_write(&r->dev, off, 4u, v, &r->map);
    CHECK_INT("mmio_write rc", 0, rc);
}
static uint32_t rd32(rig_t *r, uint32_t off) {
    uint64_t v = 0;
    int rc = hype_xhci_dev_mmio_read(&r->dev, off, 4u, &v);
    CHECK_INT("mmio_read rc", 0, rc);
    return (uint32_t)v;
}

/* Append a command TRB to the command ring at the test producer's enqueue point. */
static uint64_t push_cmd(rig_t *r, uint64_t param, uint32_t status, uint8_t type, uint8_t slot) {
    uint32_t off = OFF_CMD + (uint32_t)r->cmd_enqueue * HYPE_GXHCI_TRB_SIZE;
    uint32_t control = ((uint32_t)type << HYPE_GXHCI_TRB_TYPE_SHIFT) |
                       ((uint32_t)slot << HYPE_GXHCI_TRB_SLOT_SHIFT) |
                       (r->cmd_pcs ? HYPE_GXHCI_TRB_CYCLE : 0u);
    uint64_t gpa = GBASE + off;
    put64(r, off, param);
    put32(r, off + 8, status);
    put32(r, off + 12, control);
    r->cmd_enqueue++;
    return gpa;
}

static void rig_init(rig_t *r) {
    memset(r, 0, sizeof(*r));
    hype_gpa_map_reset(&r->map);
    hype_gpa_map_add(&r->map, GBASE, (uint64_t)(uintptr_t)r->img, IMG_SIZE);
    hype_xhci_dev_reset(&r->dev, 1 /* device present */);
    r->cmd_enqueue = 0;
    r->cmd_pcs = 1;

    /* Program the event ring: ERST[0] = { seg base = OFF_EVT, size = 64 }. */
    put64(r, OFF_ERST + 0, GBASE + OFF_EVT);
    put32(r, OFF_ERST + 8, 64u);
    put32(r, OFF_ERST + 12, 0u);
    wr32(r, HYPE_GXHCI_RT_ERSTSZ, 1u);
    wr32(r, HYPE_GXHCI_RT_ERSTBA_LO, (uint32_t)(GBASE + OFF_ERST));
    wr32(r, HYPE_GXHCI_RT_ERSTBA_HI, (uint32_t)((GBASE + OFF_ERST) >> 32));
    wr32(r, HYPE_GXHCI_RT_ERDP_LO, (uint32_t)(GBASE + OFF_EVT));
    wr32(r, HYPE_GXHCI_RT_ERDP_HI, (uint32_t)((GBASE + OFF_EVT) >> 32));
    wr32(r, HYPE_GXHCI_RT_IMAN, HYPE_GXHCI_IMAN_IE);

    /* DCBAAP + the command ring (RCS = 1). */
    put64(r, OFF_DCBAA + 8 * 1, GBASE + OFF_DEVCTX); /* DCBAA[1] -> slot 1 device context */
    wr32(r, HYPE_GXHCI_OP_DCBAAP_LO, (uint32_t)(GBASE + OFF_DCBAA));
    wr32(r, HYPE_GXHCI_OP_DCBAAP_HI, (uint32_t)((GBASE + OFF_DCBAA) >> 32));
    wr32(r, HYPE_GXHCI_OP_CRCR_LO, (uint32_t)(GBASE + OFF_CMD) | HYPE_GXHCI_CRCR_RCS);
    wr32(r, HYPE_GXHCI_OP_CRCR_HI, (uint32_t)((GBASE + OFF_CMD) >> 32));
    wr32(r, HYPE_GXHCI_OP_USBCMD, HYPE_GXHCI_USBCMD_RS | HYPE_GXHCI_USBCMD_INTE);
}

/* Read the Nth event TRB (0-based) from the event ring segment. */
static void read_event(rig_t *r, unsigned n, uint64_t *param, uint32_t *status, uint32_t *control) {
    uint32_t off = OFF_EVT + n * HYPE_GXHCI_TRB_SIZE;
    *param = (uint64_t)get32(r, off) | ((uint64_t)get32(r, off + 4) << 32);
    *status = get32(r, off + 8);
    *control = get32(r, off + 12);
}

/* ---- tests ------------------------------------------------------------------------------- */

static void test_capability_registers(void) {
    rig_t r;
    rig_init(&r);
    CHECK_HEX("CAPLENGTH", HYPE_GXHCI_CAPLENGTH, rd32(&r, HYPE_GXHCI_CAP_CAPLENGTH) & 0xFFu);
    CHECK_HEX("HCIVERSION", HYPE_GXHCI_HCIVERSION, (rd32(&r, HYPE_GXHCI_CAP_CAPLENGTH) >> 16) & 0xFFFFu);
    CHECK_HEX("HCSPARAMS1", HYPE_GXHCI_HCSPARAMS1, rd32(&r, HYPE_GXHCI_CAP_HCSPARAMS1));
    CHECK_HEX("DBOFF", HYPE_GXHCI_DBOFF, rd32(&r, HYPE_GXHCI_CAP_DBOFF));
    CHECK_HEX("RTSOFF", HYPE_GXHCI_RTSOFF, rd32(&r, HYPE_GXHCI_CAP_RTSOFF));
    /* MaxPorts and MaxSlots fields. */
    CHECK_INT("MaxSlots", HYPE_GXHCI_MAX_SLOTS, rd32(&r, HYPE_GXHCI_CAP_HCSPARAMS1) & 0xFFu);
    CHECK_INT("MaxPorts", HYPE_GXHCI_MAX_PORTS, (rd32(&r, HYPE_GXHCI_CAP_HCSPARAMS1) >> 24) & 0xFFu);
    /* Capability registers are read-only. */
    wr32(&r, HYPE_GXHCI_CAP_HCSPARAMS1, 0xDEADBEEFu);
    CHECK_HEX("HCSPARAMS1 stays RO", HYPE_GXHCI_HCSPARAMS1, rd32(&r, HYPE_GXHCI_CAP_HCSPARAMS1));
}

static void test_port_reset(void) {
    rig_t r;
    uint32_t psc;
    rig_init(&r);
    psc = rd32(&r, HYPE_GXHCI_OP_PORTSC);
    CHECK_INT("port connected at reset", 1, (psc & HYPE_GXHCI_PORTSC_CCS) ? 1 : 0);
    CHECK_INT("connect change set", 1, (psc & HYPE_GXHCI_PORTSC_CSC) ? 1 : 0);
    CHECK_INT("not yet enabled", 0, (psc & HYPE_GXHCI_PORTSC_PED) ? 1 : 0);
    /* Ack the connect change (RW1C) and issue a port reset. */
    wr32(&r, HYPE_GXHCI_OP_PORTSC, HYPE_GXHCI_PORTSC_CSC | HYPE_GXHCI_PORTSC_PR);
    psc = rd32(&r, HYPE_GXHCI_OP_PORTSC);
    CHECK_INT("connect change cleared", 0, (psc & HYPE_GXHCI_PORTSC_CSC) ? 1 : 0);
    CHECK_INT("enabled after reset", 1, (psc & HYPE_GXHCI_PORTSC_PED) ? 1 : 0);
    CHECK_INT("reset change set", 1, (psc & HYPE_GXHCI_PORTSC_PRC) ? 1 : 0);
    CHECK_INT("PR self-cleared", 0, (psc & HYPE_GXHCI_PORTSC_PR) ? 1 : 0);
}

static void test_enable_slot(void) {
    rig_t r;
    uint64_t param;
    uint32_t status, control;
    rig_init(&r);
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_ENABLE_SLOT, 0);
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);
    CHECK_INT("one command processed", 1, r.dev.commands_processed);
    CHECK_INT("one event posted", 1, r.dev.events_posted);
    read_event(&r, 0, &param, &status, &control);
    CHECK_INT("event type = cmd completion", HYPE_GXHCI_TRB_CMD_COMPLETION,
              (control >> HYPE_GXHCI_TRB_TYPE_SHIFT) & 0x3Fu);
    CHECK_INT("completion = success", HYPE_GXHCI_CC_SUCCESS, (status >> 24) & 0xFFu);
    CHECK_INT("slot 1 allocated", 1, (control >> HYPE_GXHCI_TRB_SLOT_SHIFT) & 0xFFu);
    CHECK_INT("event cycle = 1 (first pass)", 1, control & HYPE_GXHCI_TRB_CYCLE);
    CHECK_HEX("event param = command TRB gpa", GBASE + OFF_CMD, param);
    CHECK_INT("slot state enabled", HYPE_GXHCI_SLOT_ENABLED, r.dev.slots[1].state);
    CHECK_INT("irq asserted", 1, hype_xhci_dev_irq_pending(&r.dev));
}

static void test_bringup_sequence(void) {
    rig_t r;
    uint64_t param;
    uint32_t status, control;
    rig_init(&r);

    /* Build the input context for Address Device: EP0 (DCI 1) transfer ring pointer with DCS=1. */
    put64(&r, OFF_INCTX + 32u + 32u + 8u, (GBASE + 0x6000u) | 1u); /* EP0 TR dequeue ptr, DCS=1 */
    /* Input context for Configure Endpoint: add EP DCI 2 (bulk), TR ptr with DCS=1. */
    /* (reuse the same input context region -- set add flags + the DCI-2 EP context below.) */

    /* 1. Enable Slot. */
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_ENABLE_SLOT, 0);
    /* 2. Address Device (slot 1, input context pointer). */
    push_cmd(&r, GBASE + OFF_INCTX, 0, HYPE_GXHCI_TRB_ADDRESS_DEVICE, 1);
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);

    CHECK_INT("two commands so far", 2, r.dev.commands_processed);
    read_event(&r, 1, &param, &status, &control);
    CHECK_INT("address device success", HYPE_GXHCI_CC_SUCCESS, (status >> 24) & 0xFFu);
    CHECK_INT("slot addressed", HYPE_GXHCI_SLOT_ADDRESSED, r.dev.slots[1].state);
    /* The output device context slot-context state field must read Addressed, address = slot 1. */
    CHECK_INT("devctx slot state", HYPE_GXHCI_SLOT_ADDRESSED, (get32(&r, OFF_DEVCTX + 12u) >> 27) & 0x1Fu);
    CHECK_INT("devctx usb address", 1, get32(&r, OFF_DEVCTX + 12u) & 0xFFu);
    CHECK_HEX("EP0 ring recorded", GBASE + 0x6000u, r.dev.slots[1].ep_ring[1]);

    /* 3. Configure Endpoint: add DCI 2, its TR ptr at input ctx (32 + 2*32) + 8. */
    put32(&r, OFF_INCTX + 4u, (1u << 2)); /* add-flags: A2 */
    put64(&r, OFF_INCTX + 32u + 2u * 32u + 8u, (GBASE + 0x7000u) | 1u);
    push_cmd(&r, GBASE + OFF_INCTX, 0, HYPE_GXHCI_TRB_CONFIGURE_ENDPOINT, 1);
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);

    read_event(&r, 2, &param, &status, &control);
    CHECK_INT("configure endpoint success", HYPE_GXHCI_CC_SUCCESS, (status >> 24) & 0xFFu);
    CHECK_INT("slot configured", HYPE_GXHCI_SLOT_CONFIGURED, r.dev.slots[1].state);
    CHECK_INT("ep2 configured", 1, r.dev.slots[1].ep_configured[2]);
    CHECK_HEX("ep2 ring recorded", GBASE + 0x7000u, r.dev.slots[1].ep_ring[2]);
    CHECK_INT("three events posted", 3, r.dev.events_posted);
}

static void test_link_trb_and_noop(void) {
    rig_t r;
    uint64_t param;
    uint32_t status, control;
    rig_init(&r);
    /* Put a No-Op at TRB 0, then a Link TRB at TRB 1 pointing back to TRB 0's neighbour region.
     * Instead: No-Op at 0, Link at 1 -> jump to TRB 2 (no toggle), No-Op at 2, then a stale TRB. */
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_NOOP_CMD, 0); /* TRB 0 */
    /* TRB 1 = Link -> TRB 2 (GBASE+OFF_CMD+2*16), no toggle. Build it directly (not a command). */
    {
        uint32_t off = OFF_CMD + 1u * HYPE_GXHCI_TRB_SIZE;
        put64(&r, off, GBASE + OFF_CMD + 2u * HYPE_GXHCI_TRB_SIZE);
        put32(&r, off + 8, 0);
        put32(&r, off + 12,
              ((uint32_t)HYPE_GXHCI_TRB_LINK << HYPE_GXHCI_TRB_TYPE_SHIFT) | HYPE_GXHCI_TRB_CYCLE);
        r.cmd_enqueue = 2; /* producer now at TRB 2 */
    }
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_NOOP_CMD, 0); /* TRB 2 */
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);
    CHECK_INT("two no-ops executed across a link", 2, r.dev.commands_processed);
    read_event(&r, 0, &param, &status, &control);
    CHECK_INT("first no-op success", HYPE_GXHCI_CC_SUCCESS, (status >> 24) & 0xFFu);
    read_event(&r, 1, &param, &status, &control);
    CHECK_INT("second no-op success", HYPE_GXHCI_CC_SUCCESS, (status >> 24) & 0xFFu);
    CHECK_HEX("second no-op TRB gpa (past the link)", GBASE + OFF_CMD + 2u * HYPE_GXHCI_TRB_SIZE,
              param);
}

static void test_address_device_bad_slot(void) {
    rig_t r;
    uint64_t param;
    uint32_t status, control;
    rig_init(&r);
    /* Address Device on a slot that was never enabled -> Slot Not Enabled error. */
    push_cmd(&r, GBASE + OFF_INCTX, 0, HYPE_GXHCI_TRB_ADDRESS_DEVICE, 3);
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);
    read_event(&r, 0, &param, &status, &control);
    CHECK_INT("slot not enabled error", HYPE_GXHCI_CC_SLOT_NOT_ENABLED, (status >> 24) & 0xFFu);
}

static void test_hcreset_clears_state(void) {
    rig_t r;
    rig_init(&r);
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_ENABLE_SLOT, 0);
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);
    CHECK_INT("slot enabled before reset", 1, r.dev.slots_enabled);
    wr32(&r, HYPE_GXHCI_OP_USBCMD, HYPE_GXHCI_USBCMD_HCRST);
    CHECK_INT("slots cleared by HCRST", 0, r.dev.slots_enabled);
    CHECK_INT("halted after reset", 1, (rd32(&r, HYPE_GXHCI_OP_USBSTS) & HYPE_GXHCI_USBSTS_HCH) ? 1 : 0);
    CHECK_INT("port still connected", 1, (rd32(&r, HYPE_GXHCI_OP_PORTSC) & HYPE_GXHCI_PORTSC_CCS) ? 1 : 0);
}

static void test_mmio_access_widths(void) {
    rig_t r;
    uint64_t v;
    rig_init(&r);
    /* 8-byte read of DCBAAP returns the full 64-bit value programmed in rig_init. */
    CHECK_INT("8-byte read rc", 0, hype_xhci_dev_mmio_read(&r.dev, HYPE_GXHCI_OP_DCBAAP_LO, 8u, &v));
    CHECK_HEX("DCBAAP 64-bit", GBASE + OFF_DCBAA, v);
    /* Byte read of CAPLENGTH. */
    CHECK_INT("byte read rc", 0, hype_xhci_dev_mmio_read(&r.dev, HYPE_GXHCI_CAP_CAPLENGTH, 1u, &v));
    CHECK_HEX("CAPLENGTH byte", HYPE_GXHCI_CAPLENGTH, v);
    /* Word read of HCIVERSION (offset 0x02). */
    CHECK_INT("word read rc", 0, hype_xhci_dev_mmio_read(&r.dev, HYPE_GXHCI_CAP_HCIVERSION, 2u, &v));
    CHECK_HEX("HCIVERSION word", HYPE_GXHCI_HCIVERSION, v);
    /* Bad width and out-of-range are rejected. */
    CHECK_INT("width 3 rejected", -1, hype_xhci_dev_mmio_read(&r.dev, 0u, 3u, &v));
    CHECK_INT("past BAR rejected", -1,
              hype_xhci_dev_mmio_read(&r.dev, HYPE_GXHCI_BAR_SIZE, 4u, &v));
    /* 8-byte write of ERSTBA then read back. */
    CHECK_INT("8-byte write rc", 0,
              hype_xhci_dev_mmio_write(&r.dev, HYPE_GXHCI_RT_ERSTBA_LO, 8u,
                                       (GBASE + OFF_ERST) | 0u, &r.map));
    CHECK_HEX("ERSTBA lo readback", (uint32_t)(GBASE + OFF_ERST), rd32(&r, HYPE_GXHCI_RT_ERSTBA_LO));
}

static void test_usbsts_and_iman_rw1c(void) {
    rig_t r;
    rig_init(&r);
    /* Post an event (Enable Slot) -> EINT + IMAN.IP set, IRQ pending. */
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_ENABLE_SLOT, 0);
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);
    CHECK_INT("EINT set", 1, (rd32(&r, HYPE_GXHCI_OP_USBSTS) & HYPE_GXHCI_USBSTS_EINT) ? 1 : 0);
    CHECK_INT("IP set", 1, (rd32(&r, HYPE_GXHCI_RT_IMAN) & HYPE_GXHCI_IMAN_IP) ? 1 : 0);
    CHECK_INT("irq pending", 1, hype_xhci_dev_irq_pending(&r.dev));
    /* Guest clears EINT (write-1-to-clear) and IMAN.IP; IRQ deasserts. */
    wr32(&r, HYPE_GXHCI_OP_USBSTS, HYPE_GXHCI_USBSTS_EINT);
    wr32(&r, HYPE_GXHCI_RT_IMAN, HYPE_GXHCI_IMAN_IP | HYPE_GXHCI_IMAN_IE);
    CHECK_INT("EINT cleared", 0, (rd32(&r, HYPE_GXHCI_OP_USBSTS) & HYPE_GXHCI_USBSTS_EINT) ? 1 : 0);
    CHECK_INT("IP cleared", 0, (rd32(&r, HYPE_GXHCI_RT_IMAN) & HYPE_GXHCI_IMAN_IP) ? 1 : 0);
    CHECK_INT("irq deasserted", 0, hype_xhci_dev_irq_pending(&r.dev));
    /* INTE gates the IRQ: clear it and even a pending IP does not assert. */
    wr32(&r, HYPE_GXHCI_OP_USBCMD, HYPE_GXHCI_USBCMD_RS); /* INTE clear */
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_NOOP_CMD, 0);
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);
    CHECK_INT("no irq without INTE", 0, hype_xhci_dev_irq_pending(&r.dev));
}

static void test_event_ring_wrap(void) {
    rig_t r;
    uint64_t param;
    uint32_t status, control;
    unsigned i;
    rig_init(&r);
    /* Shrink the event ring to 2 TRBs so a third event wraps and toggles the producer cycle. */
    put32(&r, OFF_ERST + 8, 2u);
    wr32(&r, HYPE_GXHCI_RT_ERSTSZ, 1u);
    wr32(&r, HYPE_GXHCI_RT_ERSTBA_LO, (uint32_t)(GBASE + OFF_ERST)); /* re-latch */
    /* Three No-Ops -> three events; the third wraps back to slot 0 with cycle toggled to 0. */
    for (i = 0; i < 3; i++) {
        push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_NOOP_CMD, 0);
    }
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);
    CHECK_INT("three events posted", 3, r.dev.events_posted);
    /* The third event overwrote slot 0; its cycle bit is 0 (toggled after the wrap). */
    read_event(&r, 0, &param, &status, &control);
    CHECK_INT("wrapped event cycle = 0", 0, control & HYPE_GXHCI_TRB_CYCLE);
    CHECK_INT("producer cycle toggled", 0, r.dev.event_pcs);
}

static void test_disable_slot(void) {
    rig_t r;
    uint64_t param;
    uint32_t status, control;
    rig_init(&r);
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_ENABLE_SLOT, 0);
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_DISABLE_SLOT, 1);
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);
    read_event(&r, 1, &param, &status, &control);
    CHECK_INT("disable slot success", HYPE_GXHCI_CC_SUCCESS, (status >> 24) & 0xFFu);
    CHECK_INT("slot disabled", HYPE_GXHCI_SLOT_DISABLED, r.dev.slots[1].state);
    CHECK_INT("count back to zero", 0, r.dev.slots_enabled);
    /* Disabling an already-disabled slot errors. */
    push_cmd(&r, 0, 0, HYPE_GXHCI_TRB_DISABLE_SLOT, 2);
    hype_xhci_dev_doorbell(&r.dev, HYPE_GXHCI_DB_COMMAND, 0u, &r.map);
    read_event(&r, 2, &param, &status, &control);
    CHECK_INT("disable unenabled errors", HYPE_GXHCI_CC_SLOT_NOT_ENABLED, (status >> 24) & 0xFFu);
}

int main(void) {
    test_capability_registers();
    test_port_reset();
    test_enable_slot();
    test_bringup_sequence();
    test_link_trb_and_noop();
    test_address_device_bad_slot();
    test_hcreset_clears_state();
    test_mmio_access_widths();
    test_usbsts_and_iman_rw1c();
    test_event_ring_wrap();
    test_disable_slot();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
