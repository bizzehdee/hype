#include "xhci.h"

/* --- register-offset helpers --- */

uint32_t hype_xhci_portsc_offset(unsigned int port_1based) {
    /* PORTSC of port N (1-based), relative to the operational-register base. */
    return HYPE_XHCI_OP_PORTS_BASE + (port_1based - 1u) * HYPE_XHCI_PORT_STRIDE;
}

uint32_t hype_xhci_doorbell_offset(uint32_t dboff, unsigned int slot) {
    /* DBOFF[31:2] is the dword-aligned array base; each doorbell is 4 bytes. */
    return (dboff & ~0x3u) + slot * 4u;
}

uint32_t hype_xhci_ir0_offset(uint32_t rtsoff, uint32_t ir_reg) {
    /* RTSOFF[31:5] is the runtime base; interrupter 0 sits at +0x20. */
    return (rtsoff & ~0x1Fu) + HYPE_XHCI_RT_IR0 + ir_reg;
}

/* --- capability field extraction --- */

unsigned int hype_xhci_max_slots(uint32_t hcsparams1) { return hcsparams1 & 0xFFu; }
unsigned int hype_xhci_max_intrs(uint32_t hcsparams1) { return (hcsparams1 >> 8) & 0x7FFu; }
unsigned int hype_xhci_max_ports(uint32_t hcsparams1) { return (hcsparams1 >> 24) & 0xFFu; }

unsigned int hype_xhci_max_scratchpads(uint32_t hcsparams2) {
    unsigned int hi = (hcsparams2 >> 21) & 0x1Fu; /* Max Scratchpad Bufs Hi [25:21] */
    unsigned int lo = (hcsparams2 >> 27) & 0x1Fu; /* Max Scratchpad Bufs Lo [31:27] */
    return (hi << 5) | lo;
}

int hype_xhci_ac64(uint32_t hccparams1) { return (int)(hccparams1 & 1u); }
unsigned int hype_xhci_context_size(uint32_t hccparams1) {
    return (hccparams1 & (1u << 2)) ? 64u : 32u; /* CSZ */
}
uint32_t hype_xhci_xecp_offset(uint32_t hccparams1) { return (hccparams1 >> 16) & 0xFFFFu; }

/* --- TRB encode/decode --- */

/* control dword (dword3): cycle[0], IOC[5], IDT[6], TC[1 for Link], DIR[16],
 * TRT[17:16 for Setup], TRB type[15:10], slot id[31:24]. */
static uint32_t ctrl(hype_xhci_trb_type_t type, int cycle) {
    return (cycle ? 1u : 0u) | ((uint32_t)type << 10);
}

void hype_xhci_trb_zero(uint32_t trb[4]) {
    trb[0] = trb[1] = trb[2] = trb[3] = 0u;
}

hype_xhci_trb_type_t hype_xhci_trb_type(const uint32_t trb[4]) {
    return (hype_xhci_trb_type_t)((trb[3] >> 10) & 0x3Fu);
}
int hype_xhci_trb_cycle(const uint32_t trb[4]) { return (int)(trb[3] & 1u); }

void hype_xhci_trb_link(uint32_t trb[4], uint64_t ring_base_phys, int cycle) {
    trb[0] = (uint32_t)(ring_base_phys & ~0xFull);
    trb[1] = (uint32_t)(ring_base_phys >> 32);
    trb[2] = 0u;
    trb[3] = ctrl(HYPE_XHCI_TRB_LINK, cycle) | (1u << 1); /* TC: toggle cycle */
}

void hype_xhci_trb_noop_cmd(uint32_t trb[4], int cycle) {
    hype_xhci_trb_zero(trb);
    trb[3] = ctrl(HYPE_XHCI_TRB_NOOP_CMD, cycle);
}

void hype_xhci_trb_enable_slot(uint32_t trb[4], int cycle) {
    hype_xhci_trb_zero(trb);
    trb[3] = ctrl(HYPE_XHCI_TRB_ENABLE_SLOT, cycle); /* Slot Type 0 (USB) in [20:16] */
}

void hype_xhci_trb_address_device(uint32_t trb[4], uint64_t input_ctx_phys,
                                  unsigned int slot_id, int bsr, int cycle) {
    trb[0] = (uint32_t)(input_ctx_phys & ~0xFull); /* input context is 16-byte aligned */
    trb[1] = (uint32_t)(input_ctx_phys >> 32);
    trb[2] = 0u;
    trb[3] = ctrl(HYPE_XHCI_TRB_ADDRESS_DEVICE, cycle) | (bsr ? (1u << 9) : 0u) |
             ((slot_id & 0xFFu) << 24);
}

void hype_xhci_trb_disable_slot(uint32_t trb[4], unsigned int slot_id, int cycle) {
    hype_xhci_trb_zero(trb);
    trb[3] = ctrl(HYPE_XHCI_TRB_DISABLE_SLOT, cycle) | ((slot_id & 0xFFu) << 24);
}

void hype_xhci_trb_configure_endpoint(uint32_t trb[4], uint64_t input_ctx_phys,
                                      unsigned int slot_id, int cycle) {
    trb[0] = (uint32_t)(input_ctx_phys & ~0xFull);
    trb[1] = (uint32_t)(input_ctx_phys >> 32);
    trb[2] = 0u;
    trb[3] = ctrl(HYPE_XHCI_TRB_CONFIG_EP, cycle) | ((slot_id & 0xFFu) << 24);
}

void hype_xhci_trb_setup_stage(uint32_t trb[4], uint8_t bm_request_type, uint8_t b_request,
                               uint16_t w_value, uint16_t w_index, uint16_t w_length,
                               unsigned int trt, int cycle) {
    /* SETUP packet as Immediate Data (IDT=1): the 8 bytes live in dword0/1. */
    trb[0] = (uint32_t)bm_request_type | ((uint32_t)b_request << 8) | ((uint32_t)w_value << 16);
    trb[1] = (uint32_t)w_index | ((uint32_t)w_length << 16);
    trb[2] = 8u; /* TRB transfer length = 8 (interrupter target 0) */
    trb[3] = ctrl(HYPE_XHCI_TRB_SETUP_STAGE, cycle) | (1u << 6) /* IDT */ |
             ((trt & 0x3u) << 16);
}

void hype_xhci_trb_data_stage(uint32_t trb[4], uint64_t buffer_phys, uint32_t length,
                              int dir_in, int cycle) {
    trb[0] = (uint32_t)(buffer_phys & 0xFFFFFFFFull);
    trb[1] = (uint32_t)(buffer_phys >> 32);
    trb[2] = length & 0x1FFFFu; /* TRB Transfer Length [16:0]; TD size/intr target 0 */
    trb[3] = ctrl(HYPE_XHCI_TRB_DATA_STAGE, cycle) | (dir_in ? (1u << 16) : 0u);
}

void hype_xhci_trb_status_stage(uint32_t trb[4], int dir_in, int ioc, int cycle) {
    hype_xhci_trb_zero(trb);
    trb[3] = ctrl(HYPE_XHCI_TRB_STATUS_STAGE, cycle) | (dir_in ? (1u << 16) : 0u) |
             (ioc ? (1u << 5) : 0u);
}

/* --- device/input context encoders (xHCI 6.2) --- */

static void ctx_zero(uint32_t c[8]) { unsigned i; for (i = 0; i < 8u; i++) c[i] = 0u; }

void hype_xhci_input_ctrl_ctx(uint32_t icc[8], uint32_t add_flags, uint32_t drop_flags) {
    ctx_zero(icc);
    icc[0] = drop_flags; /* D2..D31 (D0/D1 must be 0) */
    icc[1] = add_flags;  /* A0..A31 */
}

void hype_xhci_slot_ctx(uint32_t sc[8], unsigned int route, unsigned int speed,
                        unsigned int ctx_entries, unsigned int root_port,
                        unsigned int tt_hub_slot, unsigned int tt_port) {
    ctx_zero(sc);
    /* dword0: Route String[19:0], Speed[23:20], Context Entries[31:27]. */
    sc[0] = (route & 0xFFFFFu) | ((speed & 0xFu) << 20) | ((ctx_entries & 0x1Fu) << 27);
    /* dword1: Root Hub Port Number[23:16]. */
    sc[1] = (root_port & 0xFFu) << 16;
    /* dword2: TT Hub Slot ID[7:0] + TT Port Number[15:8] -- LS/FS behind a HS hub. */
    if (tt_hub_slot) {
        sc[2] = (tt_hub_slot & 0xFFu) | ((tt_port & 0xFFu) << 8);
    }
}

unsigned int hype_xhci_route_append(unsigned int parent_route, unsigned int tier, unsigned int port) {
    if (tier == 0u || tier > 5u) return parent_route; /* xHCI supports 5 hub tiers */
    return parent_route | ((port & 0xFu) << ((tier - 1u) * 4u));
}

void hype_xhci_ep_ctx(uint32_t ep[8], unsigned int ep_type, unsigned int max_packet,
                      uint64_t tr_dequeue_phys, int dcs) {
    ctx_zero(ep);
    /* dword1: CErr[2:1]=3, EP Type[5:3], Max Packet Size[31:16]. */
    ep[1] = (3u << 1) | ((ep_type & 0x7u) << 3) | ((max_packet & 0xFFFFu) << 16);
    /* dword2/3: TR Dequeue Pointer (16-byte aligned) | DCS[0]. */
    ep[2] = (uint32_t)((tr_dequeue_phys & ~0xFull) | (dcs ? 1u : 0u));
    ep[3] = (uint32_t)(tr_dequeue_phys >> 32);
    /* dword4: Average TRB Length (8 is the conventional value; informational). */
    ep[4] = 8u;
}

void hype_xhci_ep0_ctx(uint32_t ep[8], unsigned int max_packet, uint64_t tr_dequeue_phys, int dcs) {
    hype_xhci_ep_ctx(ep, HYPE_XHCI_EP_TYPE_CONTROL, max_packet, tr_dequeue_phys, dcs);
}

unsigned int hype_xhci_default_mps(unsigned int speed_id) {
    /* PORTSC speed ids: 1=Full,2=Low,3=High,4=SuperSpeed,5+=SSP. */
    switch (speed_id) {
        case 2:  return 8u;    /* Low speed */
        case 4:  return 512u;  /* SuperSpeed */
        default: return 64u;   /* Full/High (and >=5 use 512 via IN; 64 is safe start) */
    }
}

/* --- USB Mass Storage endpoint discovery --- */

int hype_xhci_msc_find_endpoints(const uint8_t *cfg, unsigned int len, hype_xhci_msc_eps_t *out) {
    unsigned int i = 0;
    int in_msc_iface = 0;

    out->found = 0;
    out->bulk_in_ep = out->bulk_out_ep = 0;
    out->bulk_in_mps = out->bulk_out_mps = 0;
    out->interface_num = 0;
    out->config_value = 0;

    while (i + 2u <= len) {
        unsigned int blen = cfg[i];
        unsigned int btype = cfg[i + 1u];
        if (blen < 2u || i + blen > len) break; /* malformed / truncated */

        if (btype == HYPE_USB_DESC_CONFIG && blen >= 6u) {
            out->config_value = cfg[i + 5u]; /* bConfigurationValue */
        } else if (btype == HYPE_USB_DESC_INTERFACE && blen >= 9u) {
            in_msc_iface = (cfg[i + 5u] == HYPE_USB_CLASS_MSC &&
                            cfg[i + 6u] == HYPE_USB_SUBCLASS_SCSI &&
                            cfg[i + 7u] == HYPE_USB_PROTO_BOT);
            if (in_msc_iface) out->interface_num = cfg[i + 2u];
        } else if (btype == HYPE_USB_DESC_ENDPOINT && blen >= 7u && in_msc_iface) {
            unsigned int addr = cfg[i + 2u];
            unsigned int attr = cfg[i + 3u];
            unsigned int mps = (unsigned int)cfg[i + 4u] | ((unsigned int)cfg[i + 5u] << 8);
            if ((attr & 0x3u) == 0x2u) { /* bulk */
                if (addr & 0x80u) { out->bulk_in_ep = addr; out->bulk_in_mps = mps; }
                else              { out->bulk_out_ep = addr; out->bulk_out_mps = mps; }
            }
        }
        i += blen;
    }

    if (out->bulk_in_ep && out->bulk_out_ep) {
        out->found = 1;
        return 0;
    }
    return -1;
}

unsigned int hype_xhci_ep_dci(unsigned int ep_addr) {
    /* DCI = (endpoint number * 2) + direction (IN=1, OUT=0). */
    return ((ep_addr & 0x0Fu) * 2u) + ((ep_addr & 0x80u) ? 1u : 0u);
}

void hype_xhci_trb_normal(uint32_t trb[4], uint64_t buffer_phys, uint32_t length, int cycle) {
    trb[0] = (uint32_t)(buffer_phys & 0xFFFFFFFFull);
    trb[1] = (uint32_t)(buffer_phys >> 32);
    trb[2] = length & 0x1FFFFu; /* TRB Transfer Length [16:0]; TD size/intr target 0 */
    /* IOC (bit5) + ISP (bit2) so a single-TRB bulk transfer always events. */
    trb[3] = ctrl(HYPE_XHCI_TRB_NORMAL, cycle) | (1u << 5) | (1u << 2);
}

/* --- event TRB decode --- */

unsigned int hype_xhci_event_cc(const uint32_t trb[4]) { return (trb[2] >> 24) & 0xFFu; }
unsigned int hype_xhci_event_slot_id(const uint32_t trb[4]) { return (trb[3] >> 24) & 0xFFu; }
unsigned int hype_xhci_event_ep_id(const uint32_t trb[4]) { return (trb[3] >> 16) & 0x1Fu; }

void hype_xhci_trb_stop_endpoint(uint32_t trb[4], unsigned int slot, unsigned int dci, int cycle) {
    hype_xhci_trb_zero(trb);
    trb[3] = ctrl(HYPE_XHCI_TRB_STOP_EP, cycle) | ((slot & 0xFFu) << 24) | ((dci & 0x1Fu) << 16);
}

void hype_xhci_trb_reset_endpoint(uint32_t trb[4], unsigned int slot, unsigned int dci, int cycle) {
    hype_xhci_trb_zero(trb);
    trb[3] = ctrl(HYPE_XHCI_TRB_RESET_EP, cycle) | ((slot & 0xFFu) << 24) | ((dci & 0x1Fu) << 16);
}

void hype_xhci_trb_set_tr_dequeue(uint32_t trb[4], uint64_t dequeue_dcs, unsigned int slot,
                                  unsigned int dci, int cycle) {
    trb[0] = (uint32_t)(dequeue_dcs & 0xFFFFFFFFull);
    trb[1] = (uint32_t)(dequeue_dcs >> 32);
    trb[2] = 0;
    trb[3] = ctrl(HYPE_XHCI_TRB_SET_TR_DEQUEUE, cycle) | ((slot & 0xFFu) << 24) |
             ((dci & 0x1Fu) << 16);
}

uint64_t hype_xhci_event_trb_ptr(const uint32_t trb[4]) {
    return (uint64_t)trb[0] | ((uint64_t)trb[1] << 32);
}
unsigned int hype_xhci_event_port_id(const uint32_t trb[4]) { return (trb[0] >> 24) & 0xFFu; }
unsigned int hype_xhci_event_xfer_residue(const uint32_t trb[4]) { return trb[2] & 0xFFFFFFu; }

void hype_xhci_parked_reset(hype_xhci_parked_t *p) {
    unsigned i;
    for (i = 0; i < HYPE_XHCI_PARKED_MAX; i++) {
        p->e[i].used = 0;
        p->e[i].slot = 0;
        p->e[i].dci = 0;
        p->e[i].trb = 0;
        p->e[i].cc = 0;
        p->e[i].residue = 0;
    }
    p->next = 0;
}

void hype_xhci_parked_put(hype_xhci_parked_t *p, uint32_t slot, uint32_t dci, uint64_t trb,
                          uint32_t cc, uint32_t residue) {
    unsigned i;

    /* Replace an existing entry for the same transfer rather than adding a duplicate:
     * a controller that re-reports a completion must not fill the table. */
    for (i = 0; i < HYPE_XHCI_PARKED_MAX; i++) {
        if (p->e[i].used && p->e[i].slot == slot && p->e[i].dci == dci && p->e[i].trb == trb) {
            p->e[i].cc = cc;
            p->e[i].residue = residue;
            return;
        }
    }
    for (i = 0; i < HYPE_XHCI_PARKED_MAX; i++) {
        if (!p->e[i].used) {
            p->e[i].used = 1;
            p->e[i].slot = slot;
            p->e[i].dci = dci;
            p->e[i].trb = trb;
            p->e[i].cc = cc;
            p->e[i].residue = residue;
            return;
        }
    }
    /* Full: evict round-robin. A stale parked event is worth less than a fresh one, and
     * silently refusing to record the newest would recreate the discard bug. */
    i = p->next % HYPE_XHCI_PARKED_MAX;
    p->next = (p->next + 1u) % HYPE_XHCI_PARKED_MAX;
    p->e[i].used = 1;
    p->e[i].slot = slot;
    p->e[i].dci = dci;
    p->e[i].trb = trb;
    p->e[i].cc = cc;
    p->e[i].residue = residue;
}

int hype_xhci_parked_take(hype_xhci_parked_t *p, uint32_t slot, uint32_t dci, uint64_t trb,
                          uint32_t *out_cc, uint32_t *out_residue) {
    unsigned i;
    for (i = 0; i < HYPE_XHCI_PARKED_MAX; i++) {
        if (p->e[i].used && p->e[i].slot == slot && p->e[i].dci == dci && p->e[i].trb == trb) {
            if (out_cc != 0) {
                *out_cc = p->e[i].cc;
            }
            if (out_residue != 0) {
                *out_residue = p->e[i].residue;
            }
            p->e[i].used = 0; /* consumed: one event must not satisfy two waits */
            return 1;
        }
    }
    return 0;
}

int hype_xhci_parked_drop_exact(hype_xhci_parked_t *p, uint32_t slot, uint32_t dci,
                                uint64_t trb) {
    unsigned i;
    for (i = 0; i < HYPE_XHCI_PARKED_MAX; i++) {
        if (p->e[i].used && p->e[i].slot == slot && p->e[i].dci == dci && p->e[i].trb == trb) {
            p->e[i].used = 0;
            return 1;
        }
    }
    return 0;
}

void hype_xhci_parked_drop_slot(hype_xhci_parked_t *p, uint32_t slot) {
    unsigned i;
    for (i = 0; i < HYPE_XHCI_PARKED_MAX; i++) {
        if (p->e[i].used && p->e[i].slot == slot) {
            p->e[i].used = 0;
        }
    }
}

int hype_xhci_xfer_exact_ok(uint32_t cc, uint32_t residue) {
    return (cc == HYPE_XHCI_CC_SUCCESS || cc == HYPE_XHCI_CC_SHORT_PACKET) && residue == 0u;
}

/* --- #340: USB string-descriptor identity --- */

int hype_usb_string_desc_langid0(const uint8_t *desc, unsigned int desc_len, uint16_t *out) {
    if (desc == 0 || out == 0 || desc_len < 4u) return -1;
    if (desc[1] != HYPE_USB_DESC_STRING || desc[0] < 4u) return -1;
    *out = (uint16_t)((unsigned int)desc[2] | ((unsigned int)desc[3] << 8));
    return 0;
}

int hype_usb_string_desc_ascii(const uint8_t *desc, unsigned int desc_len, char *out,
                               unsigned int out_cap) {
    unsigned int blen;
    unsigned int chars;
    unsigned int start;
    unsigned int end;
    unsigned int i;
    unsigned int n;

    if (desc == 0 || out == 0 || out_cap < 2u || desc_len < 2u) return -1;
    if (desc[1] != HYPE_USB_DESC_STRING) return -1;
    blen = desc[0];
    if (blen < 2u || blen > desc_len || (blen & 1u) != 0u) return -1;
    chars = (blen - 2u) / 2u;
    if (chars == 0u) return -1; /* an empty string is not an identity */

    /*
     * UTF-16LE code units. Only printable ASCII (0020h..007Eh) is accepted:
     * this serial is matched byte-exactly against what an operator types into
     * hype.cfg, and a code point that cannot round-trip through that config
     * line is not a usable identity. Refuse rather than transliterate (#323).
     */
    start = 0;
    end = chars;
    while (start < end && desc[2u + 2u * start] == 0x20u && desc[3u + 2u * start] == 0u) start++;
    while (end > start && desc[2u + 2u * (end - 1u)] == 0x20u && desc[3u + 2u * (end - 1u)] == 0u)
        end--;
    if (start == end) return -1; /* all spaces: no identity */
    for (i = start; i < end; i++) {
        if (desc[3u + 2u * i] != 0u) return -1;
        if (desc[2u + 2u * i] < 0x20u || desc[2u + 2u * i] > 0x7Eu) return -1;
    }
    n = end - start;
    if (n > out_cap - 1u) return -1;
    for (i = 0; i < n; i++) out[i] = (char)desc[2u + 2u * (start + i)];
    out[n] = '\0';
    return (int)n;
}

/* --- USB-7 (#241): device inventory --- */

void hype_usb_inventory_reset(hype_usb_inventory_t *inv) {
    unsigned int i;
    if (inv == (hype_usb_inventory_t *)0) {
        return;
    }
    inv->count = 0;
    inv->overflow = 0;
    for (i = 0; i < HYPE_USB_INVENTORY_MAX; i++) {
        inv->dev[i].controller = 0;
        inv->dev[i].root_port = 0;
        inv->dev[i].route = 0;
        inv->dev[i].slot = 0;
        inv->dev[i].speed = 0;
        inv->dev[i].vid = 0;
        inv->dev[i].pid = 0;
        inv->dev[i].dev_class = 0;
        inv->dev[i].dev_subclass = 0;
        inv->dev[i].dev_protocol = 0;
        inv->dev[i].owner = (uint8_t)HYPE_USB_OWNER_NONE;
    }
}

int hype_usb_inventory_find(const hype_usb_inventory_t *inv, unsigned int controller,
                            unsigned int root_port, unsigned int route) {
    unsigned int i;
    if (inv == (const hype_usb_inventory_t *)0) {
        return -1;
    }
    for (i = 0; i < inv->count; i++) {
        if (inv->dev[i].controller == controller && inv->dev[i].root_port == root_port &&
            inv->dev[i].route == route) {
            return (int)i;
        }
    }
    return -1;
}

int hype_usb_inventory_add(hype_usb_inventory_t *inv, const hype_usb_devinfo_t *d) {
    int existing;
    if (inv == (hype_usb_inventory_t *)0 || d == (const hype_usb_devinfo_t *)0) {
        return -1;
    }
    existing = hype_usb_inventory_find(inv, d->controller, d->root_port, d->route);
    if (existing >= 0) {
        /* Same physical position: update in place. The sweep can revisit a
         * position (descending a hub re-reads its parent), and two rows for one
         * port would make "what is on port N" ambiguous. */
        uint8_t keep_owner = inv->dev[existing].owner;
        inv->dev[existing] = *d;
        /* An update must not silently un-claim a device hype is already using. */
        if (keep_owner != (uint8_t)HYPE_USB_OWNER_NONE &&
            d->owner == (uint8_t)HYPE_USB_OWNER_NONE) {
            inv->dev[existing].owner = keep_owner;
        }
        return existing;
    }
    if (inv->count >= HYPE_USB_INVENTORY_MAX) {
        inv->overflow++;
        return -1;
    }
    inv->dev[inv->count] = *d;
    inv->count++;
    return (int)(inv->count - 1u);
}

void hype_usb_inventory_claim(hype_usb_inventory_t *inv, int index, hype_usb_owner_t owner) {
    if (inv == (hype_usb_inventory_t *)0 || index < 0 || (unsigned int)index >= inv->count) {
        return;
    }
    inv->dev[index].owner = (uint8_t)owner;
}

int hype_usb_inventory_next_unclaimed_class(const hype_usb_inventory_t *inv, uint8_t dev_class,
                                            int after) {
    unsigned int i;
    if (inv == (const hype_usb_inventory_t *)0) {
        return -1;
    }
    for (i = (after < 0) ? 0u : (unsigned int)(after + 1); i < inv->count; i++) {
        if (inv->dev[i].dev_class == dev_class &&
            inv->dev[i].owner == (uint8_t)HYPE_USB_OWNER_NONE) {
            return (int)i;
        }
    }
    return -1;
}

unsigned int hype_usb_inventory_count_owner(const hype_usb_inventory_t *inv,
                                            hype_usb_owner_t owner) {
    unsigned int i, n = 0;
    if (inv == (const hype_usb_inventory_t *)0) {
        return 0;
    }
    for (i = 0; i < inv->count; i++) {
        if (inv->dev[i].owner == (uint8_t)owner) {
            n++;
        }
    }
    return n;
}

const char *hype_usb_owner_str(hype_usb_owner_t owner) {
    switch (owner) {
        case HYPE_USB_OWNER_NONE: return "free";
        case HYPE_USB_OWNER_HYPE: return "hype";
        case HYPE_USB_OWNER_GUEST: return "guest";
        default: return "?";
    }
}

int hype_usb_first_iface_class(const uint8_t *cfg, unsigned int len, uint8_t *out_class,
                               uint8_t *out_subclass, uint8_t *out_protocol) {
    unsigned int off = 0;

    if (cfg == (const uint8_t *)0) {
        return 0;
    }
    while (off + 2u <= len) {
        unsigned int dlen = cfg[off];
        unsigned int dtype = cfg[off + 1u];
        /* A zero/short length would loop forever on a malformed descriptor -- the
         * buffer is device-supplied, so it gets the same treatment as any other
         * untrusted input rather than being trusted to terminate. */
        if (dlen < 2u || off + dlen > len) {
            return 0;
        }
        if (dtype == HYPE_USB_DESC_INTERFACE && dlen >= 9u) {
            if (out_class != (uint8_t *)0) *out_class = cfg[off + 5u];
            if (out_subclass != (uint8_t *)0) *out_subclass = cfg[off + 6u];
            if (out_protocol != (uint8_t *)0) *out_protocol = cfg[off + 7u];
            return 1;
        }
        off += dlen;
    }
    return 0;
}

unsigned int hype_xhci_interval_encode(unsigned int speed_id, unsigned int b_interval) {
    if (speed_id == HYPE_USB_SPEED_FULL || speed_id == HYPE_USB_SPEED_LOW) {
        /* bInterval is a frame count; convert to an exponent. A count of 0 is illegal
         * in the descriptor -- treat it as the fastest legal poll rather than
         * computing log2(0), since a keyboard that polls too often still works while
         * one that never polls does not. */
        unsigned int exp = 0;
        unsigned int v = (b_interval == 0u) ? 1u : b_interval;
        while ((v >> 1) != 0u) { exp++; v >>= 1; }
        exp += 3u;
        if (exp < 3u) exp = 3u;
        if (exp > 10u) exp = 10u;
        return exp;
    }
    /* High speed and above: already an exponent. */
    if (b_interval == 0u) {
        return 0u;
    }
    if (b_interval - 1u > 15u) {
        return 15u;
    }
    return b_interval - 1u;
}

void hype_xhci_ep_ctx_interval(uint32_t ep[8], unsigned int ep_type, unsigned int max_packet,
                               uint64_t tr_dequeue_phys, int dcs, unsigned int interval) {
    hype_xhci_ep_ctx(ep, ep_type, max_packet, tr_dequeue_phys, dcs);
    /* dword0: Interval[23:16]. */
    ep[0] = (ep[0] & ~0x00FF0000u) | ((interval & 0xFFu) << 16);
}

unsigned int hype_usb_collect_endpoints(const uint8_t *cfg, unsigned int len, hype_usb_ep_t *out,
                                        unsigned int cap) {
    unsigned int off = 0;
    unsigned int n = 0;

    if (cfg == (const uint8_t *)0 || out == (hype_usb_ep_t *)0) {
        return 0;
    }
    while (off + 2u <= len && n < cap) {
        unsigned int dlen = cfg[off];
        unsigned int dtype = cfg[off + 1u];

        if (dlen < 2u || off + dlen > len) {
            break; /* device-supplied buffer: a bad length ends the walk, never spins */
        }
        if (dtype == HYPE_USB_DESC_ENDPOINT && dlen >= 7u) {
            out[n].addr = cfg[off + 2u];
            out[n].attributes = cfg[off + 3u];
            out[n].mps = (uint16_t)((unsigned int)cfg[off + 4u] |
                                    ((unsigned int)cfg[off + 5u] << 8));
            out[n].interval = cfg[off + 6u];
            n++;
        }
        off += dlen;
    }
    return n;
}
