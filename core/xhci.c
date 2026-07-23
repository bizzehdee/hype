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

/* --- event TRB decode --- */

unsigned int hype_xhci_event_cc(const uint32_t trb[4]) { return (trb[2] >> 24) & 0xFFu; }
unsigned int hype_xhci_event_slot_id(const uint32_t trb[4]) { return (trb[3] >> 24) & 0xFFu; }
uint64_t hype_xhci_event_trb_ptr(const uint32_t trb[4]) {
    return (uint64_t)trb[0] | ((uint64_t)trb[1] << 32);
}
unsigned int hype_xhci_event_port_id(const uint32_t trb[4]) { return (trb[0] >> 24) & 0xFFu; }
unsigned int hype_xhci_event_xfer_residue(const uint32_t trb[4]) { return trb[2] & 0xFFFFFFu; }
