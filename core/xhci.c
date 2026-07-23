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
                        unsigned int ctx_entries, unsigned int root_port) {
    ctx_zero(sc);
    /* dword0: Route String[19:0], Speed[23:20], Context Entries[31:27]. */
    sc[0] = (route & 0xFFFFFu) | ((speed & 0xFu) << 20) | ((ctx_entries & 0x1Fu) << 27);
    /* dword1: Root Hub Port Number[23:16]. */
    sc[1] = (root_port & 0xFFu) << 16;
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
uint64_t hype_xhci_event_trb_ptr(const uint32_t trb[4]) {
    return (uint64_t)trb[0] | ((uint64_t)trb[1] << 32);
}
unsigned int hype_xhci_event_port_id(const uint32_t trb[4]) { return (trb[0] >> 24) & 0xFFu; }
unsigned int hype_xhci_event_xfer_residue(const uint32_t trb[4]) { return trb[2] & 0xFFFFFFu; }
