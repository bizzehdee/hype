#include "xhci_dev.h"

/*
 * #591: the guest-facing xHCI controller state machine. See xhci_dev.h for scope. Register access
 * is pure; ring processing reads/writes guest memory through a hype_gpa_map_t (translate, then a
 * bounds-checked little-endian byte copy -- there is no libc memcpy in this freestanding build,
 * and hype_gpa_to_host returning 0 is a hard reject, never a raw deref).
 */

/* ---- little-endian guest-memory helpers -------------------------------------------------- */
static int gmem_read(const hype_gpa_map_t *m, uint64_t gpa, void *dst, uint32_t len) {
    uint64_t h = hype_gpa_to_host(m, gpa, len);
    const uint8_t *s;
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    if (h == 0) {
        return -1;
    }
    s = (const uint8_t *)(uintptr_t)h;
    for (i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 0;
}

static int gmem_write(const hype_gpa_map_t *m, uint64_t gpa, const void *src, uint32_t len) {
    uint64_t h = hype_gpa_to_host(m, gpa, len);
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d;
    uint32_t i;
    if (h == 0) {
        return -1;
    }
    d = (uint8_t *)(uintptr_t)h;
    for (i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 0;
}

static int gread32(const hype_gpa_map_t *m, uint64_t gpa, uint32_t *out) {
    uint8_t b[4];
    if (gmem_read(m, gpa, b, 4) != 0) {
        return -1;
    }
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

static int gread64(const hype_gpa_map_t *m, uint64_t gpa, uint64_t *out) {
    uint32_t lo, hi;
    if (gread32(m, gpa, &lo) != 0 || gread32(m, gpa + 4u, &hi) != 0) {
        return -1;
    }
    *out = (uint64_t)lo | ((uint64_t)hi << 32);
    return 0;
}

static int gwrite32(const hype_gpa_map_t *m, uint64_t gpa, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    return gmem_write(m, gpa, b, 4);
}

/* ---- reset ------------------------------------------------------------------------------- */
static void slots_reset(hype_xhci_dev_t *dev) {
    unsigned int s, e;
    for (s = 0; s <= HYPE_GXHCI_MAX_SLOTS; s++) {
        dev->slots[s].state = HYPE_GXHCI_SLOT_DISABLED;
        dev->slots[s].device_ctx_gpa = 0;
        for (e = 0; e < 32u; e++) {
            dev->slots[s].ep_ring[e] = 0;
            dev->slots[s].ep_cycle[e] = 1u;
            dev->slots[s].ep_configured[e] = 0u;
        }
    }
    dev->slots_enabled = 0u;
}

void hype_xhci_dev_reset(hype_xhci_dev_t *dev, int device_present) {
    if (dev == 0) {
        return;
    }
    dev->usbcmd = 0u;
    dev->usbsts = HYPE_GXHCI_USBSTS_HCH; /* halted at reset; CNR clear (we are always ready) */
    dev->dnctrl = 0u;
    dev->config = 0u;
    dev->crcr = 0u;
    dev->dcbaap = 0u;
    /* One port. If the device is present it reads as connected from reset (the cable is soldered
     * in for an emulated stick), with a Connect Status Change the guest will service. */
    dev->portsc = HYPE_GXHCI_PORTSC_PP;
    if (device_present) {
        dev->portsc |= HYPE_GXHCI_PORTSC_CCS | HYPE_GXHCI_PORTSC_CSC |
                       ((uint32_t)HYPE_GXHCI_PORTSC_SPEED_HS << HYPE_GXHCI_PORTSC_SPEED_SHIFT);
    }
    dev->iman = 0u;
    dev->imod = 0u;
    dev->erstsz = 0u;
    dev->erstba = 0u;
    dev->erdp = 0u;
    dev->cmd_ring_ptr = 0u;
    dev->cmd_ccs = 1u;
    dev->crcr_latched = 0;
    dev->event_ring_ptr = 0u;
    dev->event_seg_base = 0u;
    dev->event_seg_size = 0u;
    dev->event_trbs_left = 0u;
    dev->event_pcs = 1u;
    dev->erst_latched = 0;
    dev->device_present = device_present ? 1 : 0;
    dev->events_posted = 0u;
    dev->commands_processed = 0u;
    dev->transfers_processed = 0u;
    slots_reset(dev);
    /* dev->usb is intentionally NOT cleared: the class device is attached once at present time and
     * must survive a guest HCRST, which re-enters this function. */
}

void hype_xhci_dev_attach(hype_xhci_dev_t *dev, const hype_usb_device_t *usb) {
    if (dev == 0) {
        return;
    }
    dev->usb = usb;
    dev->device_present = (usb != 0) ? 1 : 0;
}

/* ---- event ring -------------------------------------------------------------------------- */
/* Load event-ring segment 0 from the ERST. Single-segment is all v1 needs; a guest that programs
 * more segments still works, we just wrap within segment 0. */
static int event_ring_latch(hype_xhci_dev_t *dev, const hype_gpa_map_t *m) {
    uint64_t seg_base;
    uint32_t seg_size;
    if (dev->erstba == 0u || dev->erstsz == 0u) {
        return -1;
    }
    if (gread64(m, dev->erstba, &seg_base) != 0) {
        return -1;
    }
    if (gread32(m, dev->erstba + 8u, &seg_size) != 0) {
        return -1;
    }
    seg_base &= ~0x3Full; /* [63:6] segment base; low bits reserved */
    seg_size &= 0xFFFFu;  /* ring segment size is a 16-bit TRB count */
    if (seg_base == 0u || seg_size == 0u) {
        return -1;
    }
    dev->event_seg_base = seg_base;
    dev->event_seg_size = seg_size;
    dev->event_ring_ptr = seg_base;
    dev->event_trbs_left = seg_size;
    dev->event_pcs = 1u;
    dev->erst_latched = 1;
    return 0;
}

/* Post one event TRB (param, status, and the type/slot part of control -- the cycle bit is ours).
 * Sets the interrupt-pending state. Returns -1 if the event ring is not usable. */
static int event_post(hype_xhci_dev_t *dev, const hype_gpa_map_t *m, uint64_t param, uint32_t status,
                      uint32_t control_no_cycle) {
    uint32_t control;
    if (!dev->erst_latched && event_ring_latch(dev, m) != 0) {
        return -1;
    }
    control = control_no_cycle | (dev->event_pcs ? HYPE_GXHCI_TRB_CYCLE : 0u);
    if (gwrite32(m, dev->event_ring_ptr + 0u, (uint32_t)param) != 0 ||
        gwrite32(m, dev->event_ring_ptr + 4u, (uint32_t)(param >> 32)) != 0 ||
        gwrite32(m, dev->event_ring_ptr + 8u, status) != 0 ||
        gwrite32(m, dev->event_ring_ptr + 12u, control) != 0) {
        return -1;
    }
    dev->event_ring_ptr += HYPE_GXHCI_TRB_SIZE;
    dev->event_trbs_left--;
    if (dev->event_trbs_left == 0u) {
        dev->event_ring_ptr = dev->event_seg_base;
        dev->event_trbs_left = dev->event_seg_size;
        dev->event_pcs ^= 1u; /* wrap toggles the producer cycle state */
    }
    dev->events_posted++;
    /* Raise the interrupt: EINT in USBSTS, IP in the interrupter. The guest clears both. */
    dev->usbsts |= HYPE_GXHCI_USBSTS_EINT;
    dev->iman |= HYPE_GXHCI_IMAN_IP;
    return 0;
}

static void post_cmd_completion(hype_xhci_dev_t *dev, const hype_gpa_map_t *m, uint64_t cmd_trb_gpa,
                                uint32_t completion_code, uint8_t slot_id) {
    uint32_t status = completion_code << 24;
    uint32_t control = ((uint32_t)HYPE_GXHCI_TRB_CMD_COMPLETION << HYPE_GXHCI_TRB_TYPE_SHIFT) |
                       ((uint32_t)slot_id << HYPE_GXHCI_TRB_SLOT_SHIFT);
    (void)event_post(dev, m, cmd_trb_gpa, status, control);
}

/* ---- command execution ------------------------------------------------------------------- */
static uint8_t first_free_slot(hype_xhci_dev_t *dev) {
    unsigned int s;
    for (s = 1u; s <= HYPE_GXHCI_MAX_SLOTS; s++) {
        if (dev->slots[s].state == HYPE_GXHCI_SLOT_DISABLED) {
            return (uint8_t)s;
        }
    }
    return 0u; /* none free */
}

/* Address Device: read DCBAA[slot] to find the output device context, copy the input slot/EP0
 * contexts into it, mark the slot Addressed and record EP0's transfer-ring pointer. Returns a
 * completion code. `input_ctx` is the command TRB parameter (Input Context pointer). */
static uint32_t cmd_address_device(hype_xhci_dev_t *dev, const hype_gpa_map_t *m, uint8_t slot,
                                   uint64_t input_ctx) {
    uint64_t dev_ctx, ep0_tr;
    uint32_t slot_ctx0, ep0_ctx1;
    uint32_t in_slot_off, in_ep0_off;
    if (slot == 0u || slot > HYPE_GXHCI_MAX_SLOTS ||
        dev->slots[slot].state == HYPE_GXHCI_SLOT_DISABLED) {
        return HYPE_GXHCI_CC_SLOT_NOT_ENABLED;
    }
    if (dev->dcbaap == 0u || (input_ctx & 0xFu) != 0u) {
        return HYPE_GXHCI_CC_PARAMETER_ERROR;
    }
    if (gread64(m, dev->dcbaap + 8u * (uint64_t)slot, &dev_ctx) != 0 || dev_ctx == 0u) {
        return HYPE_GXHCI_CC_PARAMETER_ERROR;
    }
    dev_ctx &= ~0x3Full;
    /* Input Context = Input Control Context (32B) + Slot Context (32B) + EP contexts (32B each,
     * DCI 1 = EP0 control). 32-byte contexts (CSZ=0). */
    in_slot_off = 32u;      /* slot context in the input context */
    in_ep0_off = 32u + 32u; /* EP0 (DCI 1) context in the input context */
    if (gread32(m, input_ctx + in_slot_off, &slot_ctx0) != 0 ||
        gread32(m, input_ctx + in_ep0_off + 4u, &ep0_ctx1) != 0 ||
        gread64(m, input_ctx + in_ep0_off + 8u, &ep0_tr) != 0) {
        return HYPE_GXHCI_CC_PARAMETER_ERROR;
    }
    /* Output device context: copy the slot context, force slot state = Addressed (bits [31:27] of
     * slot-context dword 3) and give the device USB address = slot id. */
    if (gwrite32(m, dev_ctx + 0u, slot_ctx0) != 0) {
        return HYPE_GXHCI_CC_TRB_ERROR;
    }
    /* Slot-context dword 3: [7:0] USB Device Address, [31:27] Slot State (2 = Addressed). */
    if (gwrite32(m, dev_ctx + 12u, ((uint32_t)slot) | ((uint32_t)HYPE_GXHCI_SLOT_ADDRESSED << 27)) != 0) {
        return HYPE_GXHCI_CC_TRB_ERROR;
    }
    /* EP0 output context (DCI 1, at device-context offset 32). Set EP State = Running (dword0
     * [2:0] = 1) -- without it xhci-hcd reads the endpoint back as Disabled and refuses to submit
     * URBs ("WARN urb submitted to disabled ep"). Copy dword1 (EP type etc) and the TR pointer. */
    (void)gwrite32(m, dev_ctx + 32u + 0u, 1u);
    (void)gwrite32(m, dev_ctx + 32u + 4u, ep0_ctx1);
    (void)gwrite32(m, dev_ctx + 32u + 8u, (uint32_t)(ep0_tr & 0xFFFFFFFFu));
    (void)gwrite32(m, dev_ctx + 32u + 12u, (uint32_t)(ep0_tr >> 32));

    dev->slots[slot].device_ctx_gpa = dev_ctx;
    dev->slots[slot].ep_ring[1] = ep0_tr & ~0xFull;
    dev->slots[slot].ep_cycle[1] = (uint8_t)(ep0_tr & 0x1u);
    dev->slots[slot].ep_configured[1] = 1u;
    dev->slots[slot].state = HYPE_GXHCI_SLOT_ADDRESSED;
    return HYPE_GXHCI_CC_SUCCESS;
}

/* Configure Endpoint: read the Input Control Context add-flags; for each added endpoint DCI, read
 * its endpoint context, record the transfer-ring pointer, and mark it configured. */
static uint32_t cmd_configure_endpoint(hype_xhci_dev_t *dev, const hype_gpa_map_t *m, uint8_t slot,
                                       uint64_t input_ctx) {
    uint32_t add_flags;
    uint64_t dev_ctx;
    unsigned int dci;
    if (slot == 0u || slot > HYPE_GXHCI_MAX_SLOTS ||
        dev->slots[slot].state < HYPE_GXHCI_SLOT_ADDRESSED) {
        return HYPE_GXHCI_CC_SLOT_NOT_ENABLED;
    }
    if ((input_ctx & 0xFu) != 0u) {
        return HYPE_GXHCI_CC_PARAMETER_ERROR;
    }
    /* Input Control Context dword 1 = Add Context flags (A0..A31); bit DCI set = add that EP. */
    if (gread32(m, input_ctx + 4u, &add_flags) != 0) {
        return HYPE_GXHCI_CC_PARAMETER_ERROR;
    }
    dev_ctx = dev->slots[slot].device_ctx_gpa;
    for (dci = 2u; dci < 32u; dci++) {
        uint64_t ep_tr;
        uint32_t ep1;
        uint64_t in_ep_off = 32u + (uint64_t)dci * 32u; /* input ctx: control+slot then EP DCIs */
        if ((add_flags & (1u << dci)) == 0u) {
            continue;
        }
        if (gread32(m, input_ctx + in_ep_off + 4u, &ep1) != 0 ||
            gread64(m, input_ctx + in_ep_off + 8u, &ep_tr) != 0) {
            return HYPE_GXHCI_CC_PARAMETER_ERROR;
        }
        dev->slots[slot].ep_ring[dci] = ep_tr & ~0xFull;
        dev->slots[slot].ep_cycle[dci] = (uint8_t)(ep_tr & 0x1u);
        dev->slots[slot].ep_configured[dci] = 1u;
        /* Mirror into the output device context so the guest reads back a configured, Running EP
         * (dword0 [2:0] = 1). */
        if (dev_ctx != 0u) {
            (void)gwrite32(m, dev_ctx + (uint64_t)dci * 32u + 0u, 1u);
            (void)gwrite32(m, dev_ctx + (uint64_t)dci * 32u + 4u, ep1);
            (void)gwrite32(m, dev_ctx + (uint64_t)dci * 32u + 8u, (uint32_t)(ep_tr & 0xFFFFFFFFu));
            (void)gwrite32(m, dev_ctx + (uint64_t)dci * 32u + 12u, (uint32_t)(ep_tr >> 32));
        }
    }
    /* Slot state -> Configured in the output device context. */
    if (dev_ctx != 0u) {
        (void)gwrite32(m, dev_ctx + 12u,
                       ((uint32_t)slot) | ((uint32_t)HYPE_GXHCI_SLOT_CONFIGURED << 27));
    }
    dev->slots[slot].state = HYPE_GXHCI_SLOT_CONFIGURED;
    return HYPE_GXHCI_CC_SUCCESS;
}

/* Process the command ring until a TRB with the wrong cycle bit (ring empty). */
static void process_command_ring(hype_xhci_dev_t *dev, const hype_gpa_map_t *m) {
    unsigned int guard = 0u;
    if (!dev->crcr_latched) {
        dev->cmd_ring_ptr = dev->crcr & HYPE_GXHCI_CRCR_PTR_MASK;
        dev->cmd_ccs = (uint8_t)(dev->crcr & HYPE_GXHCI_CRCR_RCS);
        if (dev->cmd_ring_ptr == 0u) {
            return;
        }
        dev->crcr_latched = 1;
    }
    dev->crcr |= HYPE_GXHCI_CRCR_CRR; /* Command Ring Running */
    /* Bounded so a malformed self-linking ring cannot spin the host forever. */
    while (guard++ < 4096u) {
        uint32_t p0lo, p0hi, status, control;
        uint64_t trb_gpa = dev->cmd_ring_ptr;
        uint8_t type, cycle, slot;
        if (gread32(m, trb_gpa + 0u, &p0lo) != 0 || gread32(m, trb_gpa + 4u, &p0hi) != 0 ||
            gread32(m, trb_gpa + 8u, &status) != 0 || gread32(m, trb_gpa + 12u, &control) != 0) {
            break;
        }
        (void)status;
        cycle = (uint8_t)(control & HYPE_GXHCI_TRB_CYCLE);
        if (cycle != dev->cmd_ccs) {
            break; /* producer has not written this slot yet -- ring drained */
        }
        type = (uint8_t)((control >> HYPE_GXHCI_TRB_TYPE_SHIFT) & 0x3Fu);
        slot = (uint8_t)((control >> HYPE_GXHCI_TRB_SLOT_SHIFT) & 0xFFu);
        if (type == HYPE_GXHCI_TRB_LINK) {
            uint64_t next = ((uint64_t)p0lo | ((uint64_t)p0hi << 32)) & ~0xFull;
            if (control & HYPE_GXHCI_TRB_LINK_TC) {
                dev->cmd_ccs ^= 1u;
            }
            dev->cmd_ring_ptr = next;
            continue;
        }
        {
            uint64_t param = (uint64_t)p0lo | ((uint64_t)p0hi << 32);
            uint32_t cc = HYPE_GXHCI_CC_TRB_ERROR;
            uint8_t ev_slot = slot;
            switch (type) {
            case HYPE_GXHCI_TRB_ENABLE_SLOT: {
                uint8_t ns = first_free_slot(dev);
                if (ns == 0u) {
                    cc = HYPE_GXHCI_CC_TRB_ERROR;
                } else {
                    dev->slots[ns].state = HYPE_GXHCI_SLOT_ENABLED;
                    dev->slots_enabled++;
                    ev_slot = ns; /* the allocated slot is reported in the completion event */
                    cc = HYPE_GXHCI_CC_SUCCESS;
                }
                break;
            }
            case HYPE_GXHCI_TRB_DISABLE_SLOT:
                if (slot >= 1u && slot <= HYPE_GXHCI_MAX_SLOTS &&
                    dev->slots[slot].state != HYPE_GXHCI_SLOT_DISABLED) {
                    dev->slots[slot].state = HYPE_GXHCI_SLOT_DISABLED;
                    if (dev->slots_enabled > 0u) dev->slots_enabled--;
                    cc = HYPE_GXHCI_CC_SUCCESS;
                } else {
                    cc = HYPE_GXHCI_CC_SLOT_NOT_ENABLED;
                }
                break;
            case HYPE_GXHCI_TRB_ADDRESS_DEVICE:
                cc = cmd_address_device(dev, m, slot, param & ~0xFull);
                break;
            case HYPE_GXHCI_TRB_CONFIGURE_ENDPOINT:
                cc = cmd_configure_endpoint(dev, m, slot, param & ~0xFull);
                break;
            case HYPE_GXHCI_TRB_EVALUATE_CONTEXT:
                /* Accepted: v1 does not re-derive max-packet from a re-evaluated context. */
                cc = HYPE_GXHCI_CC_SUCCESS;
                break;
            case HYPE_GXHCI_TRB_RESET_DEVICE:
                /* Return the slot to the Default state: keep it enabled, drop its endpoint
                 * configuration except the control endpoint. Linux resets the device mid-
                 * enumeration; rejecting it (the old default: TRB_ERROR) stalled every probe. */
                if (slot >= 1u && slot <= HYPE_GXHCI_MAX_SLOTS &&
                    dev->slots[slot].state != HYPE_GXHCI_SLOT_DISABLED) {
                    unsigned int e;
                    for (e = 2u; e < 32u; e++) {
                        dev->slots[slot].ep_configured[e] = 0u;
                    }
                    dev->slots[slot].state = HYPE_GXHCI_SLOT_ENABLED;
                    cc = HYPE_GXHCI_CC_SUCCESS;
                } else {
                    cc = HYPE_GXHCI_CC_SLOT_NOT_ENABLED;
                }
                break;
            case HYPE_GXHCI_TRB_RESET_ENDPOINT:
            case HYPE_GXHCI_TRB_STOP_ENDPOINT:
                /* Clear a halted/stopped condition. v1 never halts an endpoint, so there is nothing
                 * to unwind -- accept it so the guest's error-recovery path completes. */
                cc = HYPE_GXHCI_CC_SUCCESS;
                break;
            case HYPE_GXHCI_TRB_SET_TR_DEQUEUE: {
                /* Repoint an endpoint's transfer ring. Endpoint ID (DCI) is control[20:16]; the new
                 * dequeue pointer + DCS bit is the TRB parameter. */
                uint8_t dci = (uint8_t)((control >> 16) & 0x1Fu);
                if (slot >= 1u && slot <= HYPE_GXHCI_MAX_SLOTS && dci < 32u &&
                    dev->slots[slot].state != HYPE_GXHCI_SLOT_DISABLED) {
                    dev->slots[slot].ep_ring[dci] = param & ~0xFull;
                    dev->slots[slot].ep_cycle[dci] = (uint8_t)(param & 0x1u);
                    cc = HYPE_GXHCI_CC_SUCCESS;
                } else {
                    cc = HYPE_GXHCI_CC_SLOT_NOT_ENABLED;
                }
                break;
            }
            case HYPE_GXHCI_TRB_NOOP_CMD:
                cc = HYPE_GXHCI_CC_SUCCESS;
                break;
            default:
                cc = HYPE_GXHCI_CC_TRB_ERROR;
                break;
            }
            post_cmd_completion(dev, m, trb_gpa, cc, ev_slot);
            dev->commands_processed++;
        }
        dev->cmd_ring_ptr += HYPE_GXHCI_TRB_SIZE;
    }
    dev->crcr &= ~(uint64_t)HYPE_GXHCI_CRCR_CRR;
}

/* #592: post a Transfer Event for a completed transfer TRB. `residual` is the untransferred byte
 * count the guest reads out of status[23:0]. */
static void post_transfer_event(hype_xhci_dev_t *dev, const hype_gpa_map_t *m, uint64_t trb_gpa,
                                uint8_t slot, uint8_t dci, uint32_t cc, uint32_t residual) {
    uint32_t status = (cc << 24) | (residual & 0x00FFFFFFu);
    uint32_t control = ((uint32_t)HYPE_GXHCI_TRB_TRANSFER_EVENT << HYPE_GXHCI_TRB_TYPE_SHIFT) |
                       ((uint32_t)dci << 16) | ((uint32_t)slot << HYPE_GXHCI_TRB_SLOT_SHIFT);
    (void)event_post(dev, m, trb_gpa, status, control);
}

/*
 * #592: walk one endpoint's transfer ring on a slot doorbell and hand each transfer to the USB
 * function. DCI 1 is the control endpoint (Setup/Data/Status stages); an even DCI is a bulk-OUT
 * endpoint (host-to-device), an odd DCI >= 3 a bulk-IN endpoint (device-to-host). A Transfer Event
 * is posted for every TRB that requested Interrupt On Completion.
 */
static void process_transfer_ring(hype_xhci_dev_t *dev, const hype_gpa_map_t *m, uint8_t slot,
                                  uint8_t dci) {
    unsigned int guard = 0u;
    uint8_t setup[8];
    int have_setup = 0;
    int control_done = 0; /* the device->control call already happened for this TD */
    hype_xhci_slot_t *sl;

    if (slot == 0u || slot > HYPE_GXHCI_MAX_SLOTS || dci == 0u || dci >= 32u) {
        return;
    }
    sl = &dev->slots[slot];
    if (sl->state < HYPE_GXHCI_SLOT_ADDRESSED || !sl->ep_configured[dci] || sl->ep_ring[dci] == 0u) {
        return;
    }
    if (dev->usb == 0 || dev->usb->ops == 0) {
        return;
    }

    while (guard++ < 4096u) {
        uint32_t p0lo, p0hi, status, control;
        uint64_t trb_gpa = sl->ep_ring[dci];
        uint8_t type, cycle;
        uint32_t xfer_len, cc = HYPE_GXHCI_CC_SUCCESS, residual = 0u;
        int ioc;
        if (gread32(m, trb_gpa + 0u, &p0lo) != 0 || gread32(m, trb_gpa + 4u, &p0hi) != 0 ||
            gread32(m, trb_gpa + 8u, &status) != 0 || gread32(m, trb_gpa + 12u, &control) != 0) {
            break;
        }
        cycle = (uint8_t)(control & HYPE_GXHCI_TRB_CYCLE);
        if (cycle != sl->ep_cycle[dci]) {
            break; /* ring drained */
        }
        type = (uint8_t)((control >> HYPE_GXHCI_TRB_TYPE_SHIFT) & 0x3Fu);
        ioc = (control & HYPE_GXHCI_TRB_IOC) != 0;
        xfer_len = status & HYPE_GXHCI_TRB_XFER_LEN_MASK;

        if (type == HYPE_GXHCI_TRB_LINK) {
            uint64_t next = ((uint64_t)p0lo | ((uint64_t)p0hi << 32)) & ~0xFull;
            if (control & HYPE_GXHCI_TRB_LINK_TC) {
                sl->ep_cycle[dci] ^= 1u;
            }
            sl->ep_ring[dci] = next;
            continue;
        }

        if (type == HYPE_GXHCI_TRB_SETUP_STAGE) {
            /* Immediate 8-byte SETUP packet in the TRB parameter dwords. */
            setup[0] = (uint8_t)p0lo;
            setup[1] = (uint8_t)(p0lo >> 8);
            setup[2] = (uint8_t)(p0lo >> 16);
            setup[3] = (uint8_t)(p0lo >> 24);
            setup[4] = (uint8_t)p0hi;
            setup[5] = (uint8_t)(p0hi >> 8);
            setup[6] = (uint8_t)(p0hi >> 16);
            setup[7] = (uint8_t)(p0hi >> 24);
            have_setup = 1;
            control_done = 0;
        } else if (type == HYPE_GXHCI_TRB_DATA_STAGE) {
            /* Control data phase: run the control transfer against this buffer. */
            uint64_t buf = (uint64_t)p0lo | ((uint64_t)p0hi << 32);
            uint64_t host = hype_gpa_to_host(m, buf, xfer_len);
            uint32_t dlen = xfer_len;
            if (host == 0 || !have_setup) {
                cc = HYPE_GXHCI_CC_TRB_ERROR;
            } else if (dev->usb->ops->control(dev->usb->ctx, setup, (uint8_t *)(uintptr_t)host,
                                              xfer_len, &dlen) != 0) {
                cc = HYPE_GXHCI_CC_TRB_ERROR;
            } else {
                residual = (dlen < xfer_len) ? (xfer_len - dlen) : 0u;
            }
            control_done = 1;
        } else if (type == HYPE_GXHCI_TRB_STATUS_STAGE) {
            /* If there was no data phase, run a zero-length control transfer now. */
            if (have_setup && !control_done) {
                uint32_t dlen = 0;
                if (dev->usb->ops->control(dev->usb->ctx, setup, 0, 0, &dlen) != 0) {
                    cc = HYPE_GXHCI_CC_TRB_ERROR;
                }
            }
            have_setup = 0;
            control_done = 0;
        } else if (type == HYPE_GXHCI_TRB_NORMAL) {
            uint64_t buf = (uint64_t)p0lo | ((uint64_t)p0hi << 32);
            uint64_t host = hype_gpa_to_host(m, buf, xfer_len);
            if (host == 0) {
                cc = HYPE_GXHCI_CC_TRB_ERROR;
            } else if ((dci & 1u) == 0u) {
                /* even DCI = bulk OUT (host to device) */
                if (dev->usb->ops->bulk_out(dev->usb->ctx, (const uint8_t *)(uintptr_t)host,
                                            xfer_len) != 0) {
                    cc = HYPE_GXHCI_CC_TRB_ERROR;
                }
            } else {
                /* odd DCI = bulk IN (device to host) */
                uint32_t got = 0;
                if (dev->usb->ops->bulk_in(dev->usb->ctx, (uint8_t *)(uintptr_t)host, xfer_len,
                                           &got) != 0) {
                    cc = HYPE_GXHCI_CC_TRB_ERROR;
                } else {
                    residual = (got < xfer_len) ? (xfer_len - got) : 0u;
                }
            }
        }
        /* else: an unmodeled transfer TRB type -- accepted, no data moved. */

        if (ioc) {
            post_transfer_event(dev, m, trb_gpa, slot, dci, cc, residual);
        }
        dev->transfers_processed++;
        sl->ep_ring[dci] += HYPE_GXHCI_TRB_SIZE;
    }
}

void hype_xhci_dev_doorbell(hype_xhci_dev_t *dev, uint32_t db_index, uint32_t db_value,
                            const hype_gpa_map_t *dma_map) {
    if (dev == 0 || dma_map == 0) {
        return;
    }
    if (db_index == HYPE_GXHCI_DB_COMMAND) {
        process_command_ring(dev, dma_map);
        return;
    }
    /* A slot doorbell: db_value[7:0] is the endpoint DCI to service. */
    process_transfer_ring(dev, dma_map, (uint8_t)db_index, (uint8_t)(db_value & 0xFFu));
}

/* ---- MMIO register access ---------------------------------------------------------------- */
static uint32_t cap_reg32(uint32_t off) {
    switch (off) {
    case HYPE_GXHCI_CAP_CAPLENGTH: /* CAPLENGTH byte + HCIVERSION word in the same dword */
        return HYPE_GXHCI_CAPLENGTH | ((uint32_t)HYPE_GXHCI_HCIVERSION << 16);
    case HYPE_GXHCI_CAP_HCSPARAMS1:
        return HYPE_GXHCI_HCSPARAMS1;
    case HYPE_GXHCI_CAP_HCSPARAMS2:
        return 0u;
    case HYPE_GXHCI_CAP_HCSPARAMS3:
        return 0u;
    case HYPE_GXHCI_CAP_HCCPARAMS1:
        return HYPE_GXHCI_HCCPARAMS1;
    case HYPE_GXHCI_CAP_DBOFF:
        return HYPE_GXHCI_DBOFF;
    case HYPE_GXHCI_CAP_RTSOFF:
        return HYPE_GXHCI_RTSOFF;
    case HYPE_GXHCI_CAP_HCCPARAMS2:
        return 0u;
    default:
        return 0u;
    }
}

/* xHCI Extended Capabilities: one Supported Protocol Capability (USB 2.0) covering our single
 * port. Without it Linux's xhci-hcd cannot classify the port and aborts roothub setup. */
static uint32_t xecp_read32(uint32_t off) {
    switch (off - HYPE_GXHCI_XECP_OFF) {
    case 0x00u:
        /* [7:0] Cap ID = 2 (Supported Protocol), [15:8] Next Ptr = 0 (last), [23:16] Minor Rev = 0,
         * [31:24] Major Rev = 2 (USB 2.0). */
        return 0x02000002u;
    case 0x04u:
        return 0x20425355u; /* Name String "USB " */
    case 0x08u:
        /* [7:0] Compatible Port Offset = 1, [15:8] Compatible Port Count = 1. */
        return 0x00000101u;
    case 0x0Cu:
        return 0x00000000u; /* [4:0] Protocol Slot Type = 0 */
    default:
        return 0u;
    }
}

static uint32_t op_read32(const hype_xhci_dev_t *dev, uint32_t off) {
    switch (off) {
    case HYPE_GXHCI_OP_USBCMD:
        return dev->usbcmd;
    case HYPE_GXHCI_OP_USBSTS:
        return dev->usbsts;
    case HYPE_GXHCI_OP_PAGESIZE:
        return 0x1u; /* 4 KiB pages supported (bit 0) */
    case HYPE_GXHCI_OP_DNCTRL:
        return dev->dnctrl;
    case HYPE_GXHCI_OP_CRCR_LO:
        /* CRCR reads back 0 in the pointer/flag bits except CRR; the command ring pointer is not
         * host-readable per spec (RsvdZ on read). */
        return (uint32_t)(dev->crcr & HYPE_GXHCI_CRCR_CRR);
    case HYPE_GXHCI_OP_CRCR_HI:
        return 0u;
    case HYPE_GXHCI_OP_DCBAAP_LO:
        return (uint32_t)dev->dcbaap;
    case HYPE_GXHCI_OP_DCBAAP_HI:
        return (uint32_t)(dev->dcbaap >> 32);
    case HYPE_GXHCI_OP_CONFIG:
        return dev->config;
    case HYPE_GXHCI_OP_PORTSC:
        return dev->portsc;
    default:
        return 0u;
    }
}

static uint32_t rt_read32(const hype_xhci_dev_t *dev, uint32_t off) {
    switch (off) {
    case HYPE_GXHCI_RT_MFINDEX:
        return 0u;
    case HYPE_GXHCI_RT_IMAN:
        return dev->iman;
    case HYPE_GXHCI_RT_IMOD:
        return dev->imod;
    case HYPE_GXHCI_RT_ERSTSZ:
        return dev->erstsz;
    case HYPE_GXHCI_RT_ERSTBA_LO:
        return (uint32_t)dev->erstba;
    case HYPE_GXHCI_RT_ERSTBA_HI:
        return (uint32_t)(dev->erstba >> 32);
    case HYPE_GXHCI_RT_ERDP_LO:
        return (uint32_t)dev->erdp;
    case HYPE_GXHCI_RT_ERDP_HI:
        return (uint32_t)(dev->erdp >> 32);
    default:
        return 0u;
    }
}

int hype_xhci_dev_mmio_read(const hype_xhci_dev_t *dev, uint32_t offset, uint8_t size_bytes,
                            uint64_t *out_value) {
    uint32_t base;
    uint32_t v32;
    if (dev == 0 || out_value == 0) {
        return -1;
    }
    if (size_bytes != 1u && size_bytes != 2u && size_bytes != 4u && size_bytes != 8u) {
        return -1;
    }
    if (offset + size_bytes > HYPE_GXHCI_BAR_SIZE) {
        return -1;
    }
    if (size_bytes == 8u) {
        uint64_t lo, hi;
        if (hype_xhci_dev_mmio_read(dev, offset, 4u, &lo) != 0 ||
            hype_xhci_dev_mmio_read(dev, offset + 4u, 4u, &hi) != 0) {
            return -1;
        }
        *out_value = lo | (hi << 32);
        return 0;
    }
    /* Reads are dword-granular in the register file; sub-dword reads take the aligned dword and
     * shift. The guest's xhci-hcd reads registers at their natural width, so this is exact. */
    base = offset & ~0x3u;
    if (base < HYPE_GXHCI_CAPLENGTH) {
        v32 = cap_reg32(base);
    } else if (base >= HYPE_GXHCI_XECP_OFF && base < HYPE_GXHCI_XECP_OFF + 0x10u) {
        v32 = xecp_read32(base);
    } else if (base >= HYPE_GXHCI_RTSOFF && base < HYPE_GXHCI_DBOFF) {
        v32 = rt_read32(dev, base);
    } else if (base >= HYPE_GXHCI_DBOFF) {
        v32 = 0u; /* doorbells read as 0 */
    } else {
        v32 = op_read32(dev, base);
    }
    {
        uint32_t shift = (offset & 0x3u) * 8u;
        uint32_t masked = (size_bytes == 4u) ? v32 : (v32 >> shift);
        if (size_bytes == 1u) {
            masked &= 0xFFu;
        } else if (size_bytes == 2u) {
            masked &= 0xFFFFu;
        }
        *out_value = masked;
    }
    return 0;
}

static void op_write32(hype_xhci_dev_t *dev, uint32_t off, uint32_t value,
                       const hype_gpa_map_t *m) {
    switch (off) {
    case HYPE_GXHCI_OP_USBCMD:
        if (value & HYPE_GXHCI_USBCMD_HCRST) {
            /* Host Controller Reset: back to power-on, but the port stays connected. */
            int present = dev->device_present;
            hype_xhci_dev_reset(dev, present);
            return;
        }
        dev->usbcmd = value & (HYPE_GXHCI_USBCMD_RS | HYPE_GXHCI_USBCMD_INTE);
        if (value & HYPE_GXHCI_USBCMD_RS) {
            dev->usbsts &= ~(uint32_t)HYPE_GXHCI_USBSTS_HCH; /* running */
        } else {
            dev->usbsts |= HYPE_GXHCI_USBSTS_HCH;
        }
        break;
    case HYPE_GXHCI_OP_USBSTS:
        /* RW1C bits: EINT, PCD. */
        dev->usbsts &= ~(value & (HYPE_GXHCI_USBSTS_EINT | HYPE_GXHCI_USBSTS_PCD));
        break;
    case HYPE_GXHCI_OP_DNCTRL:
        dev->dnctrl = value & 0xFFFFu;
        break;
    case HYPE_GXHCI_OP_CRCR_LO:
        dev->crcr = (dev->crcr & 0xFFFFFFFF00000000ull) | value;
        dev->crcr_latched = 0; /* a new ring pointer -- re-latch on the next doorbell */
        break;
    case HYPE_GXHCI_OP_CRCR_HI:
        dev->crcr = (dev->crcr & 0x00000000FFFFFFFFull) | ((uint64_t)value << 32);
        dev->crcr_latched = 0;
        break;
    case HYPE_GXHCI_OP_DCBAAP_LO:
        dev->dcbaap = (dev->dcbaap & 0xFFFFFFFF00000000ull) | (value & ~0x3Fu);
        break;
    case HYPE_GXHCI_OP_DCBAAP_HI:
        dev->dcbaap = (dev->dcbaap & 0x00000000FFFFFFFFull) | ((uint64_t)value << 32);
        break;
    case HYPE_GXHCI_OP_CONFIG:
        dev->config = value;
        break;
    case HYPE_GXHCI_OP_PORTSC: {
        /* Preserve status bits; apply RW1C acknowledgements; act on a port reset. */
        uint32_t cleared = dev->portsc & ~(value & HYPE_GXHCI_PORTSC_RW1C);
        if ((value & HYPE_GXHCI_PORTSC_PR) && dev->device_present) {
            /* Reset completes immediately for an emulated device: enable the port, flag PRC. */
            cleared |= HYPE_GXHCI_PORTSC_PED | HYPE_GXHCI_PORTSC_PRC;
            cleared &= ~(uint32_t)HYPE_GXHCI_PORTSC_PR;
            (void)m;
        }
        dev->portsc = cleared;
        break;
    }
    default:
        break;
    }
}

static void rt_write32(hype_xhci_dev_t *dev, uint32_t off, uint32_t value) {
    switch (off) {
    case HYPE_GXHCI_RT_IMAN:
        /* IP is RW1C; IE is RW. */
        if (value & HYPE_GXHCI_IMAN_IP) {
            dev->iman &= ~(uint32_t)HYPE_GXHCI_IMAN_IP;
        }
        dev->iman = (dev->iman & ~(uint32_t)HYPE_GXHCI_IMAN_IE) | (value & HYPE_GXHCI_IMAN_IE);
        break;
    case HYPE_GXHCI_RT_IMOD:
        dev->imod = value;
        break;
    case HYPE_GXHCI_RT_ERSTSZ:
        dev->erstsz = value & 0xFFFFu;
        dev->erst_latched = 0;
        break;
    case HYPE_GXHCI_RT_ERSTBA_LO:
        dev->erstba = (dev->erstba & 0xFFFFFFFF00000000ull) | (value & ~0x3Fu);
        dev->erst_latched = 0;
        break;
    case HYPE_GXHCI_RT_ERSTBA_HI:
        dev->erstba = (dev->erstba & 0x00000000FFFFFFFFull) | ((uint64_t)value << 32);
        dev->erst_latched = 0;
        break;
    case HYPE_GXHCI_RT_ERDP_LO:
        /* Guest advances the dequeue pointer and clears EHB. */
        dev->erdp = (dev->erdp & 0xFFFFFFFF00000000ull) | value;
        break;
    case HYPE_GXHCI_RT_ERDP_HI:
        dev->erdp = (dev->erdp & 0x00000000FFFFFFFFull) | ((uint64_t)value << 32);
        break;
    default:
        break;
    }
}

int hype_xhci_dev_mmio_write(hype_xhci_dev_t *dev, uint32_t offset, uint8_t size_bytes,
                             uint64_t value, const hype_gpa_map_t *dma_map) {
    if (dev == 0) {
        return -1;
    }
    if (size_bytes != 1u && size_bytes != 2u && size_bytes != 4u && size_bytes != 8u) {
        return -1;
    }
    if (offset + size_bytes > HYPE_GXHCI_BAR_SIZE) {
        return -1;
    }
    if (size_bytes == 8u) {
        (void)hype_xhci_dev_mmio_write(dev, offset, 4u, value & 0xFFFFFFFFu, dma_map);
        return hype_xhci_dev_mmio_write(dev, offset + 4u, 4u, value >> 32, dma_map);
    }
    /* Registers are written dword-aligned by the guest driver; a sub-dword write folds into the
     * aligned dword (only USBSTS/PORTSC have byte-addressable RW1C behaviour, and xhci-hcd writes
     * them as dwords). */
    if (size_bytes != 4u) {
        uint64_t cur = 0;
        uint32_t base = offset & ~0x3u;
        uint32_t shift = (offset & 0x3u) * 8u;
        uint32_t width_mask = (size_bytes == 1u) ? 0xFFu : 0xFFFFu;
        if (hype_xhci_dev_mmio_read(dev, base, 4u, &cur) != 0) {
            return -1;
        }
        value = ((uint32_t)cur & ~(width_mask << shift)) |
                (((uint32_t)value & width_mask) << shift);
        offset = base;
    }
    if (offset < HYPE_GXHCI_CAPLENGTH) {
        return 0; /* capability registers are read-only */
    }
    if (offset >= HYPE_GXHCI_DBOFF) {
        uint32_t db_index = (offset - HYPE_GXHCI_DBOFF) / 4u;
        hype_xhci_dev_doorbell(dev, db_index, (uint32_t)value, dma_map);
        return 0;
    }
    if (offset >= HYPE_GXHCI_RTSOFF) {
        rt_write32(dev, offset, (uint32_t)value);
        return 0;
    }
    op_write32(dev, offset, (uint32_t)value, dma_map);
    return 0;
}

int hype_xhci_dev_irq_pending(const hype_xhci_dev_t *dev) {
    if (dev == 0) {
        return 0;
    }
    return (dev->iman & HYPE_GXHCI_IMAN_IP) && (dev->iman & HYPE_GXHCI_IMAN_IE) &&
           (dev->usbcmd & HYPE_GXHCI_USBCMD_INTE);
}
