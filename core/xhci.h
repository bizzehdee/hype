#ifndef HYPE_CORE_XHCI_H
#define HYPE_CORE_XHCI_H

#include <stdint.h>

/*
 * USB-1 (#213): pure xHCI model -- register-offset math, TRB (Transfer Request
 * Block) encode/decode, and ring cycle-bit management. This is the freestanding,
 * unit-testable half of hype's post-EBS xHCI host driver; the real MMIO bring-up
 * (reset, DCBAA, command/event rings, port reset, doorbells, control transfers)
 * lives in the coverage-exempt shim core/xhci_hw.c, exactly like the
 * nvme_host.c / nvme_host_hw.c split.
 *
 * Spec references are to the xHCI 1.2 specification. All MMIO + in-memory
 * structures are little-endian; hype runs little-endian x86-64 so the encoders
 * write native u32/u64 words directly.
 */

/* --- Capability registers (at BAR0 + 0), xHCI 5.3 --- */
#define HYPE_XHCI_CAP_CAPLENGTH   0x00u /* u8: length of the capability regs (= op-reg offset) */
#define HYPE_XHCI_CAP_HCIVERSION  0x02u /* u16 */
#define HYPE_XHCI_CAP_HCSPARAMS1  0x04u /* u32: MaxSlots[7:0], MaxIntrs[18:8], MaxPorts[31:24] */
#define HYPE_XHCI_CAP_HCSPARAMS2  0x08u /* u32: incl. Max Scratchpad Buffers */
#define HYPE_XHCI_CAP_HCCPARAMS1  0x10u /* u32: AC64[0], CSZ[2], xECP[31:16] */
#define HYPE_XHCI_CAP_DBOFF       0x14u /* u32: doorbell array offset (dword-aligned, [31:2]) */
#define HYPE_XHCI_CAP_RTSOFF      0x18u /* u32: runtime register-space offset ([31:5]) */

/* --- Operational registers (at BAR0 + CAPLENGTH), xHCI 5.4 --- */
#define HYPE_XHCI_OP_USBCMD       0x00u /* R/S[0], HCRST[1], INTE[2], HSEE[3] */
#define HYPE_XHCI_OP_USBSTS       0x04u /* HCH[0], HSE[2], EINT[3], PCD[4], CNR[11] */
#define HYPE_XHCI_OP_PAGESIZE     0x08u
#define HYPE_XHCI_OP_DNCTRL       0x14u
#define HYPE_XHCI_OP_CRCR         0x18u /* command ring control (u64): RCS[0], ring ptr[63:6] */
#define HYPE_XHCI_OP_DCBAAP       0x30u /* device context base address array pointer (u64) */
#define HYPE_XHCI_OP_CONFIG       0x38u /* MaxSlotsEn[7:0] */
#define HYPE_XHCI_OP_PORTS_BASE   0x400u /* PORTSC of port 1; each port block is 0x10 */
#define HYPE_XHCI_PORT_STRIDE     0x10u

#define HYPE_XHCI_USBCMD_RS       (1u << 0)
#define HYPE_XHCI_USBCMD_HCRST    (1u << 1)
#define HYPE_XHCI_USBCMD_INTE     (1u << 2)
#define HYPE_XHCI_USBSTS_HCH      (1u << 0)
#define HYPE_XHCI_USBSTS_CNR      (1u << 11) /* Controller Not Ready */
/*
 * The two status bits that say the controller itself has failed, as opposed to a transfer
 * having failed. HSE is an error on the controller's own memory accesses; HCE is an
 * internal error. Neither is recoverable by aborting a command -- both need a controller
 * reset, which tears down every addressed device -- so they must be read and named apart
 * from a merely stuck command ring, not lumped in with it.
 */
#define HYPE_XHCI_USBSTS_HSE      (1u << 2)  /* Host System Error */
#define HYPE_XHCI_USBSTS_HCE      (1u << 12) /* Host Controller Error */

/*
 * Command Ring Control bits (xHCI 5.4.5). The pointer field is only writable while CRR is
 * clear, which is what makes CA/CRR the whole of command-ring recovery: abort the command
 * in flight, wait for the controller to stop running the ring, then re-point it.
 */
#define HYPE_XHCI_CRCR_RCS        (1u << 0) /* Ring Cycle State */
#define HYPE_XHCI_CRCR_CS         (1u << 1) /* Command Stop */
#define HYPE_XHCI_CRCR_CA         (1u << 2) /* Command Abort */
#define HYPE_XHCI_CRCR_CRR        (1u << 3) /* Command Ring Running */

/* PORTSC bits (xHCI 5.4.8) */
#define HYPE_XHCI_PORTSC_CCS      (1u << 0)  /* Current Connect Status */
#define HYPE_XHCI_PORTSC_PED      (1u << 1)  /* Port Enabled/Disabled */
#define HYPE_XHCI_PORTSC_PR       (1u << 4)  /* Port Reset */
#define HYPE_XHCI_PORTSC_PP       (1u << 9)  /* Port Power */
#define HYPE_XHCI_PORTSC_PRC      (1u << 21) /* Port Reset Change */
#define HYPE_XHCI_PORTSC_CSC      (1u << 17) /* Connect Status Change */
/*
 * #744: every RW1C change bit in PORTSC, as one mask.
 *
 * CSC(17) PEC(18) WRC(19) OCC(20) PRC(21) PLC(22) CEC(23). Writing them back is what ACKs
 * a port event -- the controller raises no further Port Status Change Event for a port
 * whose change bits are still set, so a reader that only reads sees the first unplug and
 * then nothing ever again.
 */
#define HYPE_XHCI_PORTSC_CHANGE_MASK 0x00FE0000u
#define HYPE_XHCI_PORTSC_SPEED_SHIFT 10u
#define HYPE_XHCI_PORTSC_SPEED_MASK  0x0Fu
/* Writing PORTSC: bits that are RW1CS (change bits) must be preserved carefully;
 * the shim uses hype_xhci_portsc_write_preserve() to avoid clearing them. */

/* --- Runtime registers: interrupter 0 at RTSOFF + 0x20 (xHCI 5.5) --- */
#define HYPE_XHCI_RT_IR0          0x20u
#define HYPE_XHCI_IR_IMAN         0x00u
#define HYPE_XHCI_IR_IMOD         0x04u
#define HYPE_XHCI_IR_ERSTSZ       0x08u
#define HYPE_XHCI_IR_ERSTBA       0x10u
#define HYPE_XHCI_IR_ERDP         0x18u

/* --- TRB (xHCI 6.4): 16 bytes = 4 little-endian dwords --- */
#define HYPE_XHCI_TRB_DWORDS      4u
#define HYPE_XHCI_TRB_BYTES       16u

/* TRB Type field (control dword bits 15:10). */
typedef enum {
    HYPE_XHCI_TRB_NORMAL          = 1,
    HYPE_XHCI_TRB_SETUP_STAGE     = 2,
    HYPE_XHCI_TRB_DATA_STAGE      = 3,
    HYPE_XHCI_TRB_STATUS_STAGE    = 4,
    HYPE_XHCI_TRB_LINK            = 6,
    HYPE_XHCI_TRB_ENABLE_SLOT     = 9,
    HYPE_XHCI_TRB_DISABLE_SLOT    = 10,
    HYPE_XHCI_TRB_ADDRESS_DEVICE  = 11,
    HYPE_XHCI_TRB_CONFIG_EP       = 12,
    HYPE_XHCI_TRB_RESET_EP        = 14,
    HYPE_XHCI_TRB_STOP_EP         = 15,
    HYPE_XHCI_TRB_SET_TR_DEQUEUE  = 16,
    HYPE_XHCI_TRB_NOOP_CMD        = 23,
    HYPE_XHCI_TRB_TRANSFER_EVENT  = 32,
    HYPE_XHCI_TRB_CMD_COMPLETION  = 33,
    HYPE_XHCI_TRB_PORT_STATUS     = 34,
    HYPE_XHCI_TRB_HOST_CONTROLLER = 37
} hype_xhci_trb_type_t;

/* Completion codes (event TRB status bits 31:24), xHCI 6.4.5. */
#define HYPE_XHCI_CC_SUCCESS      1u
#define HYPE_XHCI_CC_SHORT_PACKET 13u
/*
 * xHCI 6.4.5 table 6-90. The xHC posts this in a Host Controller Event when it has run out
 * of room on the Event Ring, and stops Command and Transfer Ring processing until software
 * advances ERDP. It is the one completion code that says "the controller stopped", as
 * opposed to "a transfer failed", and hype had no name for it.
 */
#define HYPE_XHCI_CC_EVENT_RING_FULL 21u
/*
 * The completion codes a STOP means, not a failure means.
 *
 * A Stop Endpoint command retires every TRB still outstanding on that endpoint with one of
 * these -- the transfer was cancelled by software, which is the whole point of issuing it.
 * hype read them as transfer failures and "recovered" an endpoint that was stopped rather
 * than halted, issuing a Reset Endpoint against a Stopped endpoint. That is a Context State
 * Error, and on boot 31 (2026-08-30) the command ring stopped answering from that moment on
 * and every interrupt-IN endpoint on the controller went deaf for the remaining 74 minutes.
 */
#define HYPE_XHCI_CC_CMD_RING_STOPPED 24u
#define HYPE_XHCI_CC_CMD_ABORTED      25u
#define HYPE_XHCI_CC_STOPPED          26u
#define HYPE_XHCI_CC_STOPPED_LENGTH   27u
#define HYPE_XHCI_CC_STOPPED_SHORT    28u

/* 1 if `cc` is one of the STOPPED codes above: the transfer was cancelled by software, not
 * failed by the device. Pure, so it is unit tested -- see the note at the definition. */
int hype_xhci_cc_is_stopped(uint32_t cc);

/* Control-transfer TRT (Setup Stage control dword bits 17:16), xHCI 6.4.1.2.1. */
#define HYPE_XHCI_TRT_NO_DATA     0u
#define HYPE_XHCI_TRT_OUT         2u
#define HYPE_XHCI_TRT_IN          3u

/* --- register-offset helpers (pure) --- */

/* Operational-register base = capability length. */
static inline uint32_t hype_xhci_op_base(uint8_t caplength) { return (uint32_t)caplength; }
/* PORTSC offset (from op base) for 1-based port number. */
uint32_t hype_xhci_portsc_offset(unsigned int port_1based);
/* Doorbell[slot] offset from BAR0. dboff is the raw DBOFF register value. */
uint32_t hype_xhci_doorbell_offset(uint32_t dboff, unsigned int slot);
/* Interrupter-0 register offset from BAR0. rtsoff is the raw RTSOFF value. */
uint32_t hype_xhci_ir0_offset(uint32_t rtsoff, uint32_t ir_reg);

/* --- capability field extraction (pure) --- */
unsigned int hype_xhci_max_slots(uint32_t hcsparams1);   /* [7:0] */
unsigned int hype_xhci_max_intrs(uint32_t hcsparams1);   /* [18:8] */
unsigned int hype_xhci_max_ports(uint32_t hcsparams1);   /* [31:24] */
unsigned int hype_xhci_max_scratchpads(uint32_t hcsparams2); /* hi[31:27]<<5 | lo[25:21] */
int hype_xhci_ac64(uint32_t hccparams1);                 /* [0]: 64-bit addressing capable */
unsigned int hype_xhci_context_size(uint32_t hccparams1);/* CSZ[2]: 64 if set else 32 bytes */
uint32_t hype_xhci_xecp_offset(uint32_t hccparams1);     /* [31:16] in dwords, *4 = byte off */

/* --- TRB encode/decode (pure). trb is a 4-u32 array (little-endian dwords). --- */

/* Zero a TRB. */
void hype_xhci_trb_zero(uint32_t trb[4]);
/* Raw field access to the control dword (dword3). */
hype_xhci_trb_type_t hype_xhci_trb_type(const uint32_t trb[4]);
int hype_xhci_trb_cycle(const uint32_t trb[4]);

/* Link TRB pointing at ring_base_phys, with Toggle Cycle set (for the last slot
 * of a ring). cycle is the producer cycle bit to stamp. */
void hype_xhci_trb_link(uint32_t trb[4], uint64_t ring_base_phys, int cycle);

/* Command-ring TRBs. cycle = producer cycle bit. */
void hype_xhci_trb_noop_cmd(uint32_t trb[4], int cycle);
void hype_xhci_trb_enable_slot(uint32_t trb[4], int cycle);
void hype_xhci_trb_address_device(uint32_t trb[4], uint64_t input_ctx_phys,
                                  unsigned int slot_id, int bsr, int cycle);
void hype_xhci_trb_disable_slot(uint32_t trb[4], unsigned int slot_id, int cycle);
void hype_xhci_trb_configure_endpoint(uint32_t trb[4], uint64_t input_ctx_phys,
                                      unsigned int slot_id, int cycle);

/*
 * Control-transfer TRBs (on a device's default-control endpoint transfer ring).
 * The 8-byte SETUP packet is passed as its five fields; hype_xhci_trb_setup_stage
 * packs them as Immediate Data (IDT=1). `trt` is one of HYPE_XHCI_TRT_*.
 */
void hype_xhci_trb_setup_stage(uint32_t trb[4], uint8_t bm_request_type, uint8_t b_request,
                               uint16_t w_value, uint16_t w_index, uint16_t w_length,
                               unsigned int trt, int cycle);
/* Data Stage: buffer_phys, transfer length, dir_in (1=IN), cycle. */
void hype_xhci_trb_data_stage(uint32_t trb[4], uint64_t buffer_phys, uint32_t length,
                              int dir_in, int cycle);
/* Status Stage: dir_in (1=IN), ioc (interrupt-on-completion), cycle. */
void hype_xhci_trb_status_stage(uint32_t trb[4], int dir_in, int ioc, int cycle);

/* Normal TRB for a bulk transfer: data buffer + length, with IOC + Interrupt-on-
 * Short-Packet set so a single-TRB transfer always yields a Transfer Event. */
void hype_xhci_trb_normal(uint32_t trb[4], uint64_t buffer_phys, uint32_t length, int cycle);

/* --- device/input context encoders (xHCI 6.2), pure --- */
/* Contexts are 32 or 64 bytes (CSZ); these fill the first 8 dwords (the rest is
 * reserved/padding). Callers place them at ctx_size-byte strides in the
 * input/device context. */

/* Input Control Context: drop-context + add-context bitmaps (A0=slot, A1=EP0). */
#define HYPE_XHCI_ADD_SLOT (1u << 0)
#define HYPE_XHCI_ADD_EP0  (1u << 1)
void hype_xhci_input_ctrl_ctx(uint32_t icc[8], uint32_t add_flags, uint32_t drop_flags);

/* Slot Context: route string, PORTSC speed, context-entries (highest valid DCI),
 * the root-hub port the device's topology hangs off, and (for a LS/FS device
 * behind a HS hub) the Transaction Translator hub slot id + port -- pass
 * tt_hub_slot 0 for a direct/HS/SS device (no TT). */
void hype_xhci_slot_ctx(uint32_t sc[8], unsigned int route, unsigned int speed,
                        unsigned int ctx_entries, unsigned int root_port,
                        unsigned int tt_hub_slot, unsigned int tt_port);

/*
 * #737/#736: mark an already-built Slot Context as a HUB. xHCI 6.2.2 gives a hub slot
 * three fields a function slot does not use: Hub (dword0 bit 26), Number of Ports
 * (dword1 bits 31:24) and TT Think Time (dword2 bits 17:16). They are evaluated by a
 * Configure Endpoint command with A0 set (xHCI 4.6.6).
 *
 * hype used to leave all three at zero, so every hub it addressed looked like a plain
 * function. That is not cosmetic: a child's Slot Context names the hub as its
 * Transaction Translator by slot id, and a TT Hub Slot ID pointing at a slot with
 * Hub=0 leaves the controller unable to build a split-transaction schedule for it.
 * Measured: Address Device rejected with Parameter Error (code 17) for a full-speed
 * device behind a high-speed hub on one controller, and 68818 polls with reports=0 for
 * a keyboard on another.
 *
 * mtt is 0 for every hub hype drives today -- single-TT is the safe encoding and a
 * multi-TT hub still works as one TT.
 */
void hype_xhci_slot_ctx_set_hub(uint32_t sc[8], unsigned int nbr_ports, unsigned int ttt,
                                unsigned int mtt);

/* TT Think Time for a hub's Slot Context: wHubCharacteristics (hub descriptor bytes
 * 3..4) bits 6:5, already in the field's own encoding. */
unsigned int hype_xhci_hub_ttt(const uint8_t *hubdesc);

/* A device's topology path, needed to Address Device + Configure Endpoints for
 * a device that may sit behind one or more hubs. root_port is the ROOT port the
 * chain hangs off; route is the xHCI Route String (nibble per hub tier). */
typedef struct {
    unsigned int root_port;
    unsigned int route;
    unsigned int speed;        /* PORTSC/hub-status speed id */
    unsigned int tt_hub_slot;  /* 0 = no TT (direct or HS/SS device) */
    unsigned int tt_port;
} hype_xhci_devpath_t;

/* Compose a Route String: append downstream `port` at hub `tier` (1-based) to a
 * parent route. Tier 1 occupies bits 3:0, tier 2 bits 7:4, ... (xHCI 8.9). */
unsigned int hype_xhci_route_append(unsigned int parent_route, unsigned int tier, unsigned int port);

/* Endpoint Context EP Type field (dword1 bits 5:3), xHCI 6.2.3. */
#define HYPE_XHCI_EP_TYPE_ISOCH_OUT 1u
#define HYPE_XHCI_EP_TYPE_BULK_OUT  2u
#define HYPE_XHCI_EP_TYPE_INT_OUT   3u
#define HYPE_XHCI_EP_TYPE_CONTROL   4u
#define HYPE_XHCI_EP_TYPE_BULK_IN   6u
#define HYPE_XHCI_EP_TYPE_INT_IN    7u

/* Generic Endpoint Context: EP Type, CErr=3, Max Packet Size, TR Dequeue (+DCS). */
void hype_xhci_ep_ctx(uint32_t ep[8], unsigned int ep_type, unsigned int max_packet,
                      uint64_t tr_dequeue_phys, int dcs);
/* Default-control Endpoint (EP0) Context = ep_ctx with EP Type = Control. */
void hype_xhci_ep0_ctx(uint32_t ep[8], unsigned int max_packet, uint64_t tr_dequeue_phys, int dcs);

/* Initial control-endpoint Max Packet Size for a PORTSC speed id (xHCI/USB). */
unsigned int hype_xhci_default_mps(unsigned int speed_id);

/* --- event TRB decode (pure) --- */
unsigned int hype_xhci_event_cc(const uint32_t trb[4]);       /* completion code, status[31:24] */
unsigned int hype_xhci_event_slot_id(const uint32_t trb[4]);  /* control[31:24] */
uint64_t hype_xhci_event_trb_ptr(const uint32_t trb[4]);      /* dword0/1: TRB pointer (cmd/xfer) */
unsigned int hype_xhci_event_ep_id(const uint32_t trb[4]);    /* dword3 [20:16]: endpoint DCI */

/*
 * #254: endpoint-recovery command TRBs (xHCI 4.6.9 / 4.6.8 / 4.6.10). Used by
 * the Bulk-Only reset recovery: a timed-out transfer leaves its TRB owned by
 * the controller, so before any retry the endpoint is stopped (and reset, if
 * halted) and its transfer ring re-pointed -- otherwise a LATE completion for
 * the abandoned TRB desynchronises every later transfer on the ring.
 */
void hype_xhci_trb_stop_endpoint(uint32_t trb[4], unsigned int slot, unsigned int dci, int cycle);
void hype_xhci_trb_reset_endpoint(uint32_t trb[4], unsigned int slot, unsigned int dci, int cycle);
/* `dequeue_dcs` = new dequeue pointer with the DCS (cycle) bit ORed into bit 0. */
void hype_xhci_trb_set_tr_dequeue(uint32_t trb[4], uint64_t dequeue_dcs, unsigned int slot,
                                  unsigned int dci, int cycle);
unsigned int hype_xhci_event_port_id(const uint32_t trb[4]);  /* Port Status Change: param[31:24] */
unsigned int hype_xhci_event_xfer_residue(const uint32_t trb[4]); /* status[23:0] bytes not transferred */

/*
 * PORTSC is a minefield of RW1C ("write-1-to-clear") change bits plus PED which
 * also clears-on-1. To set an RW bit (e.g. PR) without accidentally clearing a
 * change bit or disabling the port, always write
 *   (current & ~HYPE_XHCI_PORTSC_RW1C) | bits_to_set
 * This mask names every bit that must be written as 0 to be left untouched.
 */
#define HYPE_XHCI_PORTSC_RW1C  ((1u<<1) | (1u<<17) | (1u<<18) | (1u<<19) | \
                                (1u<<20) | (1u<<21) | (1u<<22) | (1u<<23))
static inline uint32_t hype_xhci_portsc_write_preserve(uint32_t current, uint32_t bits_to_set) {
    return (current & ~HYPE_XHCI_PORTSC_RW1C) | bits_to_set;
}

/*
 * Bits that ACT when written as 1 rather than describing state: PR (Port Reset), LWS (the
 * Port Link State write strobe) and WPR (Warm Port Reset). A read-modify-write must never
 * echo them back, or it fires a reset nobody asked for.
 */
#define HYPE_XHCI_PORTSC_STROBE ((1u << 4) | (1u << 16) | (1u << 31))

/*
 * The value to write back to PORTSC to ACK its change bits and change nothing else.
 *
 * Boot 32 (2026-08-30) is why this is a named function with a test rather than an
 * expression at the call site. The ACK was written as `sc & HYPE_XHCI_PORTSC_CHANGE_MASK`,
 * which does clear the change bits -- and also writes 0 into every other RW bit in the
 * register, PP (Port Power, bit 9) included. So hype saw the keyboard leave the root port,
 * ACKed the event, and switched the port off in the same write. Plugging the keyboard back
 * in did nothing at all: an unpowered port never reports a connect, so no Port Status Change
 * Event was ever raised and #745's arrival path was never reached.
 *
 * Preserve everything, clear only the change bits, fire no strobes.
 */
static inline uint32_t hype_xhci_portsc_ack_changes(uint32_t current) {
    return (current & ~(HYPE_XHCI_PORTSC_RW1C | HYPE_XHCI_PORTSC_STROBE))
           | (current & HYPE_XHCI_PORTSC_CHANGE_MASK);
}

/* --- USB Mass Storage endpoint discovery (pure, xHCI-independent USB descr) --- */

/* USB Mass Storage class/subclass/protocol (bulk-only SCSI). */
#define HYPE_USB_CLASS_MSC       0x08u
#define HYPE_USB_SUBCLASS_SCSI   0x06u
#define HYPE_USB_PROTO_BOT       0x50u
/* USB descriptor types. */
#define HYPE_USB_DESC_CONFIG     0x02u
#define HYPE_USB_DESC_STRING     0x03u
#define HYPE_USB_DESC_INTERFACE  0x04u
#define HYPE_USB_DESC_ENDPOINT   0x05u
/* USB hub class (device descriptor bDeviceClass == 0x09). */
#define HYPE_USB_CLASS_HUB       0x09u
#define HYPE_USB_DESC_HUB        0x29u /* wValue high byte for GET hub descriptor */
/* #739: a SuperSpeed hub answers 0x2A, not 0x29 (USB 3.2 10.15.2.1). bNbrPorts is
 * byte 2 in both, so the port count and the walk are shared; TT Think Time is not. */
#define HYPE_USB_DESC_HUB_SS     0x2Au

/* PORTSC/hub speed ids (shared with hype_xhci_default_mps). */
#define HYPE_USB_SPEED_FULL  1u
#define HYPE_USB_SPEED_LOW   2u
#define HYPE_USB_SPEED_HIGH  3u
#define HYPE_USB_SPEED_SUPER 4u

/* A standard USB device descriptor is 18 bytes; bDeviceClass is byte 4. Returns
 * 1 if this device is a hub (its interface may instead carry the class, but the
 * device-descriptor class is authoritative for hubs). */
static inline int hype_xhci_dev_is_hub(const uint8_t *devdesc18) {
    return devdesc18[4] == HYPE_USB_CLASS_HUB;
}
/* bNbrPorts is byte 2 of a hub descriptor. Clamp to a sane maximum so a bogus
 * descriptor can't drive an unbounded downstream-port loop. */
static inline unsigned int hype_xhci_hub_nbr_ports(const uint8_t *hubdesc) {
    unsigned int n = hubdesc[2];
    return (n > 15u) ? 15u : n;
}
/* A LS/FS device behind a HS hub needs that hub named as its Transaction
 * Translator; direct/HS/SS chains do not. */
static inline int hype_xhci_tt_required(unsigned int hub_speed, unsigned int child_speed) {
    return hub_speed == HYPE_USB_SPEED_HIGH &&
           (child_speed == HYPE_USB_SPEED_LOW || child_speed == HYPE_USB_SPEED_FULL);
}

typedef struct {
    int found;                   /* 1 if a bulk-only SCSI MSC interface with both bulk EPs */
    unsigned int interface_num;
    unsigned int config_value;   /* bConfigurationValue to SET_CONFIGURATION */
    unsigned int bulk_in_ep;     /* endpoint address incl. 0x80 direction bit */
    unsigned int bulk_out_ep;
    unsigned int bulk_in_mps;    /* wMaxPacketSize */
    unsigned int bulk_out_mps;
} hype_xhci_msc_eps_t;

/*
 * Walks a USB configuration descriptor blob [cfg, cfg+len) (config + interface +
 * endpoint descriptors) and, if it contains a bulk-only-transport SCSI Mass
 * Storage interface (class 08 / sub 06 / proto 50) with a bulk IN and bulk OUT
 * endpoint, fills *out (found=1). Returns 0 if found, -1 otherwise. Pure.
 */
int hype_xhci_msc_find_endpoints(const uint8_t *cfg, unsigned int len, hype_xhci_msc_eps_t *out);

/* Endpoint Context DCI for an endpoint address: (num*2) + (IN?1:0). EP0 = 1. */
unsigned int hype_xhci_ep_dci(unsigned int ep_addr);

/* --- hardware bring-up (coverage-exempt shim core/xhci_hw.c; real MMIO). --- */

/* Provide the calibrated host-TSC frequency so the driver can honor real USB
 * timing (post-reset settle, SET_ADDRESS recovery). Optional -- without it the
 * driver falls back to a coarse busy-spin. Call once after TSC calibration. */
void hype_xhci_set_tsc_hz(uint64_t hz);

/* Captured controller geometry + register bases, filled by hype_xhci_host_init. */
/*
 * #299: how many controllers can be Running at once.
 *
 * Each needs its own DCBAA, command ring, event ring, ERST, scratchpad array, device
 * pool and dequeue/cycle state -- they used to be file-static singletons in xhci_hw.c,
 * which meant the host sweep had to stop controller N before bringing up N+1. Two
 * Running controllers DMAing into one command ring and one event ring with one shared
 * dequeue/cycle pair is what broke the Intel box, the first two-xHCI machine this ran
 * on. But quiescing the controller holding hype's log/boot medium is not an option, so
 * devices on every OTHER controller were unreachable -- which on a laptop with the boot
 * stick and the internal keyboard on different controllers is the normal case.
 *
 * Bounded because each block is ~380 KB of .bss (64 scratchpad pages dominate), so this
 * is a real memory tradeoff, not an arbitrary number. Two covers the machines in hand
 * and the case that matters: storage on one controller, keyboard on the other. A third
 * controller does not fail the boot -- it is refused a block, logged by name, and
 * skipped, degrading to fewer controllers rather than back to sharing.
 */
#define HYPE_XHCI_MAX_CTRL 2u
/* #387: claimed MSC devices across all controllers (each owns its bulk rings + BOT bounce --
 * ~72 KiB of BSS each). Matches HYPE_MEDIA_MAX_DEVS: a claimed stick exists to be a media
 * device, and a block with no media slot to land in would be claimed for nothing. */
#define HYPE_XHCI_MSC_MAX 4u

/*
 * #734: interrupt-IN endpoints hype polls for ITSELF -- a keyboard (#217) and a mouse
 * (#219) today. Each needs its own transfer ring, report buffer and "is a transfer
 * outstanding" flag.
 *
 * One shared set per controller is what this was until #734, and it made two HIDs
 * mutually exclusive on the operator's 5950X, where keyboard and mouse are both on
 * controller 2: whichever polled first armed the single flag, and because an idle input
 * device never completes a transfer, the flag never cleared -- so the other endpoint's
 * doorbell was never rung again and its reports=0 forever. Both endpoint contexts also
 * named the SAME TR Dequeue Pointer and the same report buffer, so a completion could
 * have been parsed as the other device's report.
 *
 * 4, like HYPE_XHCI_MSC_MAX: 2 covers the machines in hand, and each block is ~4 KiB.
 */
/*
 * #746: 12, not 4. Every HUB now owns one of these for its status-change endpoint, and the
 * 5950X alone has five hubs across two controllers on top of a keyboard and a mouse. Sized
 * for a hub-heavy desk rather than for the two HIDs it originally served; the cost is one
 * page-aligned ring plus a 64-byte report each.
 */
/* #765: one block per polled interrupt-IN endpoint. Must cover HYPE_XHCI_HUB_MAX hub
 * devices (each hub has a status-change endpoint) PLUS every claimed HID, or a keyboard
 * arrives to find no block free and is enumerated but never polled. */
#define HYPE_XHCI_INT_IN_MAX 24u

/*
 * The identity of one pooled interrupt-IN endpoint. Kept separate from the DMA block it
 * indexes so the pool arithmetic is a pure function that can be unit-tested -- the DMA
 * blocks themselves live in xhci_hw.c, which no host test can touch.
 */
typedef struct {
    int used;
    unsigned int ctrl; /* hw_slot of the owning controller */
    unsigned int slot;
    unsigned int dci;
} hype_xhci_int_in_key_t;

/*
 * Index of the pool entry for (ctrl, slot, dci), or -1.
 *
 * alloc = 0 looks up an existing entry only (a poll for an endpoint that was never
 * configured must fail, not silently seize a free block). alloc = 1 claims a free entry
 * on first sight, and returns -1 when the pool is full.
 */
int hype_xhci_int_in_index(hype_xhci_int_in_key_t *keys, unsigned int n, unsigned int ctrl,
                           unsigned int slot, unsigned int dci, int alloc);

/* Release every pool entry belonging to one controller (its block is being re-claimed). */
/*
 * #744: release the pooled interrupt-IN block(s) of ONE slot on one controller.
 *
 * The controller-wide twin below is for tearing a controller down. A departed device needs
 * just its own blocks back, and needs it before the next device inherits its slot id --
 * otherwise that device finds a block already keyed to its (slot, dci) holding the
 * previous tenant's ring, cycle state and armed flag.
 */
void hype_xhci_int_in_release_slot(hype_xhci_int_in_key_t *keys, unsigned int n,
                                   unsigned int ctrl, unsigned int slot);

void hype_xhci_int_in_release_ctrl(hype_xhci_int_in_key_t *keys, unsigned int n,
                                   unsigned int ctrl);

typedef struct {
    uint64_t bar;            /* xHCI MMIO BAR0 (identity-mapped) */
    uint32_t op;             /* operational-register base offset (= CAPLENGTH) */
    uint32_t dboff;          /* raw DBOFF */
    uint32_t rtsoff;         /* raw RTSOFF */
    unsigned int max_slots;
    unsigned int max_ports;
    unsigned int ctx_size;   /* 32 or 64 (CSZ) */
    int inited;
    /*
     * #299: which per-controller DMA/ring block in xhci_hw.c this controller owns.
     * Assigned by hype_xhci_host_init(), released by hype_xhci_host_quiesce(). Opaque to
     * everyone outside that file -- it exists so two controllers can be Running at once,
     * each with its own command ring, event ring and dequeue/cycle state, which is what
     * lets a keyboard on one controller be polled while storage streams from another.
     */
    unsigned int hw_slot;
    /*
     * The number the REST of the log calls this controller.
     *
     * hw_slot is an internal pool index and is not that number -- boot 38 proved the
     * difference matters: a wedge on the controller the enumeration log calls controller[2]
     * was reported as "ctrl1 command ring stopped answering", which is precisely the
     * confusion the label was added to remove. Set by the caller right after
     * hype_xhci_host_init(), from the same counter that prints "controller[N] at bb:dd.f".
     * Zero means nobody set it, and prints as ctrl? rather than as a wrong number.
     */
    unsigned int log_id;
} hype_xhci_ctrl_t;

/*
 * Brings up the xHCI controller at bar_phys (identity-mapped MMIO): waits ready,
 * stops + resets, programs MaxSlotsEn + DCBAA (+ scratchpads), the command ring
 * (CRCR) and event ring (ERST/ERDP), then sets Run. Fills *out. Returns 0 on
 * success, -1 on a timeout/error. Post-ExitBootServices only.
 */
int hype_xhci_host_init(uint64_t bar_phys, hype_xhci_ctrl_t *out);

/*
 * Powers + resets every root port and returns the 1-based number of the first
 * port that comes up connected + enabled, with its PORTSC speed field in
 * *out_speed; returns 0 if no device is present. Pure PORTSC polling (no event
 * ring needed for detection).
 */
unsigned int hype_xhci_detect_device(hype_xhci_ctrl_t *c, unsigned int *out_speed);

/*
 * USB-8 (#231): power + reset ONE root port; returns 1 if it comes up
 * connected + enabled (PORTSC speed id in *out_speed), 0 if nothing is there.
 * Lets the caller enumerate EVERY root port (not just the first), which real
 * hardware needs -- the boot medium is often behind a hub or on a later port.
 */
int hype_xhci_reset_port(hype_xhci_ctrl_t *c, unsigned int port, unsigned int *out_speed);

/*
 * Raw PORTSC of a 1-based root port, for diagnostics only.
 *
 * hype_xhci_reset_port() collapses every "not usable" outcome into 0, so a scan
 * that finds nothing cannot say WHY -- no device attached, port unpowered, reset
 * never completed, and a port that does not exist all look identical. On the
 * Intel i5-13420H all 16 root ports came back empty even though the machine had
 * just booted from a stick on one of them, and with no port state logged there
 * was nothing to go on. Read-only; touches no controller state.
 */
uint32_t hype_xhci_port_status(const hype_xhci_ctrl_t *c, unsigned int port);

/*
 * Stop a controller (clear Run/Stop, wait for HCHalted) and mark it uninited.
 *
 * Required before bringing up ANOTHER controller, because the DMA structures in
 * xhci_hw.c -- DCBAA, command ring, event ring, ERST -- plus the ring cursors
 * are single-instance, deliberately "single controller" (see their comments).
 * hype_xhci_host_init() leaves the controller Running, so on a machine with two
 * xHCI controllers the second one gets pointed at the exact memory the first is
 * still live on: two controllers DMAing into one event ring, sharing one
 * consumer cycle/dequeue state. The result is precisely what an Intel
 * i5-13420H showed -- the first command on the second controller succeeds, then
 * Address Device never sees its completion and the command after it reads a
 * foreign event ("completion code 19"). A single-xHCI machine (the AMD laptop)
 * never exercises it.
 *
 * Quiescing the ones we are finished with keeps exactly one controller live
 * against the shared rings. The proper fix -- per-controller rings, so several
 * can be up at once -- is required by #241 (full device inventory) and belongs
 * there.
 */
void hype_xhci_host_quiesce(hype_xhci_ctrl_t *c);

/* Disable (free) a device slot -- used to release a slot between enumeration
 * probes when the device isn't the one we're after. */
int hype_xhci_disable_slot(hype_xhci_ctrl_t *c, unsigned int slot);

/*
 * #744: the next root port whose status changed, 1-based, or 0 when none is pending.
 * Clears the port's flag, so a caller loops until it returns 0.
 */
unsigned int hype_xhci_take_port_change(hype_xhci_ctrl_t *c);

/*
 * #757: drop every port-change bit banked so far, and report how many there were.
 *
 * The bitmap is filled by the single event-dequeue point, which also runs throughout
 * ENUMERATION -- so by the time the dispatch loop takes its first hot-plug sweep, one bit
 * is set for every port that changed while hype was bringing devices up. Nothing had
 * cleared them.
 *
 * Re-processing those is not hot-plug detection. `hype_xhci_port_connected()` is evaluated
 * when the bit is DRAINED, not when the event arrived, so a port hype already enumerated
 * and settled is re-judged against a fresh PORTSC read seconds later. A port that reads
 * back not-connected at that moment produces a DEPARTURE for a device that never left --
 * which releases its slot, and for the boot medium marks the log sink gone STICKILY
 * (#747), so the failure erases its own evidence.
 *
 * Call once, after enumeration and before the first sweep. Events raised after that are
 * genuine hot-plug and must NOT be discarded.
 */
/*
 * #769: drain up to `budget` events from this controller's ring, and report how many.
 *
 * A Port Status Change Event is only noticed when something dequeues the event ring, and
 * the only things that do are transfers and commands ON THAT CONTROLLER. A controller with
 * no claimed interrupt-IN endpoint is therefore never drained once enumeration is over, and
 * a device plugged into one of its root ports is never seen.
 *
 * Boot 13 measured exactly that: the keyboard and mouse are on controller 2, so controller 2
 * is pumped by HID polling and its hot-plug works. Controller 1 had no polled endpoint, and
 * its ring was drained only incidentally by the guest's ISO reads -- so the FIRST plug into
 * its front USB-C port was seen (the kernel was still loading) and every later one produced
 * no event at all. Five port events on that controller for the whole run.
 *
 * Called from the hot-plug sweep for every live controller, so noticing a plug does not
 * depend on something else happening to be busy.
 */
/*
 * #770: stop reporting this hub port, or start again.
 *
 * Its change bits are still CLEARED on every poll -- leaving them set would make the hub
 * report its whole bitmap forever -- but the port is not handed back to the caller. For a
 * device hype has given up enumerating whose link keeps flapping, that is the difference
 * between a bounded cost and 4,610 log lines from one port.
 */
void hype_xhci_hub_ignore_port(hype_xhci_ctrl_t *c, unsigned int hub_slot, unsigned int port,
                               int ignore);

/* One full Event Ring per pass: enough to empty it, bounded so a wedged ring cannot spin. */
#define HYPE_XHCI_PUMP_BUDGET 256u
unsigned int hype_xhci_pump_events(hype_xhci_ctrl_t *c, unsigned int budget);

/* #775: how many times this interrupt-IN endpoint has been armed. With the report count
 * beside it, "armed once and never answered" is distinguishable from "armed repeatedly and
 * answering", which no other pair of numbers says. */
/* Revives performed on this endpoint -- see hype_xhci_int_in_revives(). */
unsigned long long hype_xhci_int_in_revives(hype_xhci_ctrl_t *c, unsigned int slot,
                                            unsigned int ep_addr);

/* Revives that were attempted and FAILED, and transfers retired by hype's own Stop
 * Endpoint. A silent endpoint with revives=0 used to be indistinguishable from one whose
 * every revive attempt timed out against a stopped command ring -- see boot 31. */
void hype_xhci_int_in_revive_health(hype_xhci_ctrl_t *c, unsigned int slot,
                                    unsigned int ep_addr, unsigned long long *fails,
                                    unsigned long long *stopped);

/* 1 if the build revives an interrupt-IN endpoint on silence alone (HYPE_INT_IN_SILENCE_REVIVE),
 * 0 if only a halted endpoint is rebuilt. Printed on every HIDTICK line so a hardware log
 * states which policy produced it. */
unsigned int hype_xhci_silence_revive_enabled(void);

/* Command-ring health: timeouts seen, abort/restart recoveries performed, and whether the
 * ring has been given up on. `dead` non-zero means every device on this controller is
 * unreachable. */
void hype_xhci_cmd_ring_health(hype_xhci_ctrl_t *c, unsigned long long *timeouts,
                               unsigned long long *guard, unsigned long long *recoveries,
                               int *dead);

/* Per-endpoint loss counters; see the implementation for what each one means. */
void hype_xhci_int_in_losses(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int ep_addr,
                             unsigned long long *lost, unsigned long long *skipped);

/*
 * #781: what a controller whose every interrupt-IN endpoint has gone quiet looks like from
 * its registers, plus the answer to the only question that settles it: does a No-Op command
 * still complete. Every field is a raw register read except the last five.
 */
typedef struct {
    uint32_t usbsts;
    uint32_t usbcmd;
    uint32_t crcr_lo;          /* CRR (bit 3) is the controller saying its command ring runs */
    uint32_t iman;
    uint32_t imod;
    uint64_t erdp;             /* the controller's view of the event ring dequeue pointer */
    uint64_t sw_deq;           /* software's: where the next event is expected to land */
    unsigned int pending_event;/* 1 = the TRB at sw_deq already carries the consumer cycle,
                                  so an event was delivered and nobody consumed it */
    uint32_t portsc;           /* PORTSC of the root port asked for, 0 if none */
    int noop_rc;               /* 0 = the No-Op completed, -1 = it did not */
    uint32_t noop_cc;          /* its completion code when it did */
    unsigned int noop_us;      /* how long the No-Op took, capped by the event timeout */
    unsigned long long cmd_timeouts; /* the controller's running total AFTER the No-Op */
} hype_xhci_silence_probe_t;

/* Fill *out. `root_port` may be 0 to skip the PORTSC read. Costs one command round trip,
 * at most the one-second event timeout. Returns -1 only if `c` is not an initialised
 * controller. */
int hype_xhci_probe_silence(hype_xhci_ctrl_t *c, unsigned int root_port,
                            hype_xhci_silence_probe_t *out);

/*
 * #785: non-zero once this controller's command ring has been given up on (cmd_dead) and
 * only a host-controller reset can bring its devices back. Cleared by hype_xhci_host_init().
 */
int hype_xhci_reset_wanted(const hype_xhci_ctrl_t *c);

/* #782: the compiled-in wedge injection -- 0 in a normal build; else the delay in ms, with
 * the 1-based log number of the controller it targets in *ctrl. */
unsigned int hype_xhci_wedge_injection_ms(unsigned int *ctrl);

/*
 * #783 (decision 75): which controllers hype must never reset. Indices are the enumeration
 * log's 1-based numbers; 0 means "no such controller recorded". Pure logic, so the refusal
 * is unit-tested rather than discovered on the machine whose log it would have cut off.
 */
typedef struct {
    unsigned int log_ctrl;  /* the controller the log sink writes through */
    unsigned int boot_ctrl; /* the controller hype's boot/media medium is on */
} hype_xhci_reset_policy_t;

/* 1 = may reset; 0 = refuse, with *reason naming why (static string). */
int hype_xhci_may_reset(const hype_xhci_reset_policy_t *p, unsigned int ctrl_idx,
                        const char **reason);

void hype_xhci_event_health(hype_xhci_ctrl_t *c, unsigned long long *hc_events,
                            unsigned long long *ring_full, unsigned long long *evictions);

unsigned long long hype_xhci_int_in_arms(hype_xhci_ctrl_t *c, unsigned int slot,
                                         unsigned int ep_addr);

/*
 * #775: where this endpoint's completions went. A completion reaches its owner three ways --
 * handed over by another poll, taken from the parked table, or dequeued by its own poll --
 * and is lost one way, by being parked and evicted. With QEMU reporting 299 transfers
 * started on an endpoint hype scored at zero reports, these four are what say which.
 */
void hype_xhci_int_in_routes(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int ep_addr,
                             unsigned long long *handed, unsigned long long *deliv,
                             unsigned long long *own, unsigned long long *to_park);

unsigned int hype_xhci_discard_port_changes(hype_xhci_ctrl_t *c);

/* #744: how many Port Status Change Events this controller has produced, ever. */
unsigned long long hype_xhci_port_event_count(const hype_xhci_ctrl_t *c);

/*
 * #744: is anything attached to `port` now? Also ACKS the port's change bits, which is
 * what lets the controller raise the NEXT event for it. 0 on success.
 */
int hype_xhci_port_connected(hype_xhci_ctrl_t *c, unsigned int port, int *out_connected);

/*
 * #744: tear a departed device down -- its pooled interrupt-IN blocks, its slot, and its
 * DCBAA entry. Use this rather than hype_xhci_disable_slot() for anything that was
 * actually in use; disable_slot alone leaks the endpoint blocks.
 */
int hype_xhci_release_device(hype_xhci_ctrl_t *c, unsigned int slot);

/*
 * USB-1 (#213) pt3: issue an Enable Slot command on the command ring and wait
 * (via the event ring) for its Command Completion Event. On success stores the
 * assigned device slot id (1..MaxSlots) in *out_slot and returns 0; returns -1
 * on a command error or timeout. Exercises the command+event ring machinery.
 */
int hype_xhci_enable_slot(hype_xhci_ctrl_t *c, unsigned int *out_slot);

/*
 * USB-1 (#213) pt3b: Address a freshly-enabled device on `slot` attached to
 * `root_port` at PORTSC `speed`. Builds the Input Context (slot + EP0), points
 * DCBAA[slot] at a fresh Device Context, sets up the EP0 transfer ring, and
 * issues an Address Device command. Returns 0 on success, -1 on error/timeout.
 */
int hype_xhci_address_device(hype_xhci_ctrl_t *c, unsigned int slot,
                             const hype_xhci_devpath_t *path);

/*
 * Reads the 18-byte USB device descriptor from an addressed device via a
 * GET_DESCRIPTOR control transfer on EP0 (Setup/Data/Status). Writes it to
 * buf18 (>= 18 bytes). Returns 0 on success, -1 on error/timeout.
 */
int hype_xhci_get_device_descriptor(hype_xhci_ctrl_t *c, unsigned int slot, uint8_t *buf18);

/*
 * #737: Configure Endpoint (A0 only) that marks an addressed slot as a hub --
 * Hub=1, Number of Ports, TT Think Time. Must run after Address Device on the hub and
 * BEFORE any device below it is addressed, because a child's Slot Context names this
 * slot as its Transaction Translator. Returns 0 on success, -1 on error/timeout.
 */
int hype_xhci_configure_hub_slot(hype_xhci_ctrl_t *c, unsigned int slot,
                                 const hype_xhci_devpath_t *path, unsigned int nbr_ports,
                                 unsigned int ttt);

/*
 * #746: add a HUB's status-change interrupt-IN endpoint, keeping it a hub while doing it.
 *
 * NOT hype_xhci_configure_int_in_endpoint(): that rebuilds the Slot Context through
 * hype_xhci_slot_ctx(), which does not set Hub, Number of Ports or TT Think Time -- so
 * using it on a hub would clear Hub=1 as a side effect of adding the endpoint, and every
 * LS/FS device below it would lose its Transaction Translator. That is #737's own failure,
 * re-created from the other direction.
 */
int hype_xhci_configure_hub_int_in(hype_xhci_ctrl_t *c, unsigned int slot,
                                   const hype_xhci_devpath_t *path, unsigned int nbr_ports,
                                   unsigned int ttt, unsigned int ep_addr, unsigned int mps,
                                   unsigned int interval);

/* #746: how many hubs the walk registered, and one's identity. */
/*
 * #765: hub DEVICES hype will track, which is not the same as hubs a person plugged in.
 *
 * A USB-3 hub is two USB devices -- a 2.0 hub and a 3.0 hub, each with its own
 * status-change endpoint that has to be polled separately. So the 5950X's "two hubs plus
 * one on the motherboard" is FIVE entries here, and 6 left room for exactly one more
 * device.
 *
 * That is too tight to survive ordinary hardware. A keyboard with a built-in hub -- common,
 * and the reason a keyboard can appear behind two tiers -- adds one entry, or two if it is
 * USB 3. Running out means the hub is registered with ep 0 and is silently unable to report
 * hot-plug for anything below it.
 *
 * Not to be confused with the xHCI route string's limit of five hub TIERS, which is a
 * chain depth and unrelated to how many hubs exist.
 */
#define HYPE_XHCI_HUB_MAX 16u
/*
 * Hub port status for the device at `route`, read over ep0 so it answers even when that
 * device's interrupt-IN endpoint has stopped completing. st[0] bit 2 is PORT_SUSPEND: set
 * means the device slept, clear means it is awake and hype simply is not hearing it. Behind-
 * hub devices only. Returns 0 on success.
 */
int hype_xhci_port_status_for_route(hype_xhci_ctrl_t *c, unsigned int root_port,
                                    unsigned int route, uint8_t st[4],
                                    unsigned int *out_hub_slot, unsigned int *out_port);

unsigned int hype_xhci_hub_count(void);
int hype_xhci_hub_at(unsigned int i, unsigned int *out_ctrl, unsigned int *out_slot,
                     unsigned int *out_nports, unsigned int *out_ep);

/*
 * #746: the next downstream port of hub `i` whose status changed, 1-based, or 0.
 *
 * Polls the hub's status-change endpoint (non-blocking, the same three-way contract as
 * hype_xhci_int_in_poll) and drains one port per call, clearing that port's C_PORT_CONNECTION
 * so the hub will report the next change. *out_connected says what is there NOW.
 */
int hype_xhci_hub_take_change(hype_xhci_ctrl_t *c, unsigned int i, unsigned int *out_port,
                              int *out_connected);

/* #746: hub status-endpoint poll counters -- "hype is not polling" and "the hub is not
 * reporting" are different faults and look identical without them. */
void hype_xhci_hub_poll_stats(unsigned long long *polls, unsigned long long *reports,
                              unsigned long long *errs);

/* #746: forget every hub registered on this controller (it is being torn down). */
void hype_xhci_hub_forget_ctrl(unsigned int ctrl);

/*
 * #746: the route string a device on `port` of this hub would have. No bus traffic --
 * used to identify which claimed device just left, where its slot is already meaningless.
 */
int hype_xhci_hub_child_route(hype_xhci_ctrl_t *c, unsigned int hub_slot, unsigned int port,
                              unsigned int *out_route);

/* #746: the root port this hub hangs off, which the inventory keys on alongside the route. */
int hype_xhci_hub_root_port(hype_xhci_ctrl_t *c, unsigned int hub_slot, unsigned int *out_port);

/*
 * #746: reset a hub's downstream port and build the topology a device there needs --
 * route, speed and, for a LS/FS device under a HS hub, the Transaction Translator (#218).
 * Does bus traffic. 0 on success.
 */
int hype_xhci_hub_child_path(hype_xhci_ctrl_t *c, unsigned int hub_slot, unsigned int port,
                             hype_xhci_devpath_t *out_hub, hype_xhci_devpath_t *out_child,
                             unsigned int *out_speed);

/*
 * Reads the full configuration descriptor (config + interface + endpoint
 * descriptors) into buf (capped at maxlen), setting *out_len to the byte count.
 * Two control transfers: the 9-byte header for wTotalLength, then the whole
 * thing. Returns 0 on success, -1 on error.
 */
int hype_xhci_get_config_descriptor(hype_xhci_ctrl_t *c, unsigned int slot, uint8_t *buf,
                                    unsigned int maxlen, unsigned int *out_len);

/* SET_CONFIGURATION(config_value) control transfer (no data). 0 on success. */
int hype_xhci_set_configuration(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int config_value);

/*
 * #734: SET_PROTOCOL(Boot) on a HID interface. 0 on success.
 *
 * A boot-subclass interface descriptor says the device CAN speak boot protocol, not that
 * it is doing so: HID 1.11 7.2.6 says every device powers up in REPORT protocol, and the
 * host must ask for boot. hype only ever parses boot reports (core/usb_hid.c), so without
 * this it reads report-protocol data with a boot-protocol layout -- and a report-protocol
 * report is usually LONGER than the boot one, which on the wire is a Babble Detected
 * (cc=3) against a boot-sized TRB rather than a misread key.
 *
 * A device that refuses the request is not fatal: some report the boot subclass and
 * answer boot reports regardless, so the caller logs and continues.
 */
int hype_xhci_hid_set_boot_protocol(hype_xhci_ctrl_t *c, unsigned int slot,
                                    unsigned int interface_num);

/*
 * Issues a Configure Endpoint command adding the MSC bulk IN + bulk OUT
 * endpoints (from *msc) to `slot`'s device context, each with a fresh transfer
 * ring. root_port/speed re-provide the input Slot Context. Returns 0 on success.
 * After this, bulk transfers on those endpoints are possible.
 */
int hype_xhci_configure_bulk_endpoints(hype_xhci_ctrl_t *c, unsigned int slot,
                                       const hype_xhci_devpath_t *path,
                                       const hype_xhci_msc_eps_t *msc);

/*
 * USB-8 (#231) pt5b: descend into a USB hub. `hub_slot` is an addressed+
 * configured hub whose own path is *hub_path (speed in it); `tier` is its hub
 * tier (1 for a root-port hub). Powers/resets each downstream port, enumerates
 * the device on it (Enable Slot -> Address Device with the extended route/TT
 * path -> descriptors), recursing into nested hubs. On finding the first bulk-
 * only mass-storage device, fills *out_slot (addressed), *out_path, *out_msc and
 * returns 0; returns -1 if none is found below this hub.
 */
int hype_xhci_hub_find_msc(hype_xhci_ctrl_t *c, unsigned int hub_slot,
                           const hype_xhci_devpath_t *hub_path, unsigned int tier,
                           unsigned int *out_slot, hype_xhci_devpath_t *out_path,
                           hype_xhci_msc_eps_t *out_msc);

/*
 * USB-7 (#241): walk EVERY device behind a hub and hand each one to a visitor.
 *
 * hype_xhci_hub_find_msc() above enumerates the same topology but throws away anything
 * that is not storage, so a keyboard or a camera behind a hub was addressed, read, and
 * then forgotten -- the inventory could never describe it, and "where a device is plugged
 * in must be irrelevant" was not true for anything below a hub. This is the same descent
 * with the decision moved out to the caller.
 *
 * The visitor sees each device fully enumerated (addressed, device descriptor read, and
 * configuration descriptor read where possible) and decides what happens to its slot.
 * Nested hubs are visited too -- a hub is a device, and passthrough wants it listed --
 * and are then descended into regardless of what the visitor returned, except on STOP.
 *
 * A hub's own slot stays addressed for the rest of the boot even if the visitor releases
 * it: it is the topology parent every device below it is addressed through, and freeing
 * it would invalidate its children.
 */
#define HYPE_XHCI_VISIT_RELEASE 0 /* keep walking; this device's slot may be freed */
#define HYPE_XHCI_VISIT_KEEP 1    /* keep walking; leave the slot addressed */
#define HYPE_XHCI_VISIT_STOP 2    /* stop the walk; leave the slot addressed */

typedef int (*hype_xhci_hub_visit_fn)(void *ctx, hype_xhci_ctrl_t *c, unsigned int slot,
                                      const hype_xhci_devpath_t *path, const uint8_t *devdesc,
                                      const uint8_t *cfg, unsigned int cfg_len);

/* Returns 0 if a visitor asked to STOP (so the caller knows it found what it wanted),
 * -1 if the walk completed or the hub could not be read. */
int hype_xhci_hub_walk(hype_xhci_ctrl_t *c, unsigned int hub_slot,
                       const hype_xhci_devpath_t *hub_path, unsigned int tier,
                       hype_xhci_hub_visit_fn visit, void *ctx);

/*
 * #739: hub_walk's -1 used to mean two different things -- "walked it all, no visitor
 * said STOP" and "could not walk it at all". A caller that cannot tell them apart
 * reported "its devices are still in the inventory" for a hub whose devices had never
 * been looked at. This return says the descent never happened.
 */
#define HYPE_XHCI_HUB_NOT_WALKED (-2)

/*
 * USB-3 (#215) block I/O over Bulk-Only Transport (SCSI). Require the device to
 * be Enable-Slot'd, Address'd, SET_CONFIGURATION'd and its bulk endpoints
 * Configure-Endpoint'd (see above). All bounce through an internal page, so
 * `blocks * block_size` must be <= 4096. Return 0 on success.
 */
int hype_xhci_msc_read_capacity(hype_xhci_ctrl_t *c, unsigned int slot,
                                const hype_xhci_msc_eps_t *msc, uint32_t *last_lba,
                                uint32_t *block_size);
int hype_xhci_msc_read(hype_xhci_ctrl_t *c, unsigned int slot, const hype_xhci_msc_eps_t *msc,
                       uint32_t lba, unsigned int blocks, unsigned int block_size, void *buf);
int hype_xhci_msc_write(hype_xhci_ctrl_t *c, unsigned int slot, const hype_xhci_msc_eps_t *msc,
                        uint32_t lba, unsigned int blocks, unsigned int block_size, const void *buf);
/* Ask the mass-storage device to make preceding writes durable. */
int hype_xhci_msc_sync_cache(hype_xhci_ctrl_t *c, unsigned int slot,
                             const hype_xhci_msc_eps_t *msc);

/*
 * #266 defect 1: a parking table for transfer completions that arrive for an endpoint
 * OTHER than the one currently being waited on.
 *
 * Controller 1022:15e0 demonstrably delivers events late -- the command ring already
 * needed leniency for exactly this (#254), and the log says so in as many words. On the
 * BULK rings matching was kept strict, because mis-attributing a DATA completion is
 * what caused #254's original corruption (a CBW written into the medium as sector
 * data). But strict was implemented as DISCARD, and that is the bug: the late event for
 * the other direction was thrown away, the transfer waiting on its own endpoint never
 * completed, and BOT declared a lost completion. Recovery then ran three times and the
 * write path stayed dead.
 *
 * Parking keeps both properties at once: an event is still only ever applied to the
 * exact (slot, dci, trb) it names -- so no data can land in the wrong buffer -- but it
 * is REMEMBERED instead of dropped, so the endpoint it belongs to can consume it when
 * its turn comes. Strictness protects attribution; it must not require events to arrive
 * in an order this controller declines to deliver them in.
 *
 * Small and fixed-size: only a handful of transfers are ever outstanding (one bulk-in
 * and one bulk-out per BOT stage), so 8 slots is generous. A full table drops the
 * OLDEST entry, since a stale parked event is worth less than a fresh one.
 */
#define HYPE_XHCI_PARKED_MAX 8u

typedef struct {
    uint32_t slot;
    uint32_t dci;
    uint64_t trb;
    uint32_t cc;
    uint32_t residue;
    int used;
} hype_xhci_parked_evt_t;

typedef struct {
    hype_xhci_parked_evt_t e[HYPE_XHCI_PARKED_MAX];
    uint32_t next; /* round-robin victim when full */
    /*
     * Evictions, counted. This table drops the oldest entry when full, and for an
     * interrupt-IN endpoint a dropped completion is not a dropped report -- the transfer
     * stays counted as outstanding for ever and the endpoint is never re-armed. That is
     * the failure the operator sees as a keyboard going deaf mid-boot, and nothing
     * counted it.
     */
    unsigned long long evictions;
} hype_xhci_parked_t;

/* Evictions this table has performed. See the struct comment: each one is potentially a
 * permanently deaf endpoint. */
unsigned long long hype_xhci_parked_evictions(const hype_xhci_parked_t *p);

void hype_xhci_parked_reset(hype_xhci_parked_t *p);

/* Remember a completion that is not the one currently awaited. */
void hype_xhci_parked_put(hype_xhci_parked_t *p, uint32_t slot, uint32_t dci, uint64_t trb,
                          uint32_t cc, uint32_t residue);

/*
 * Claim a previously parked completion for exactly this (slot, dci, trb). Returns 1 and
 * writes the completion code and transfer residue when their output pointers are non-null.
 * The claim REMOVES the event so one event cannot satisfy two waits. Returns 0 otherwise.
 */
int hype_xhci_parked_take(hype_xhci_parked_t *p, uint32_t slot, uint32_t dci, uint64_t trb,
                          uint32_t *out_cc, uint32_t *out_residue);

/* Drop a completion before reusing its transfer-ring TRB address. */
int hype_xhci_parked_drop_exact(hype_xhci_parked_t *p, uint32_t slot, uint32_t dci,
                                uint64_t trb);

/*
 * Drop every parked event for a slot. Called after a reset/recovery: the transfer rings
 * restart at index 0, so a parked event for the torn-down state would carry a TRB
 * address the RETRY is about to reuse -- and would then be claimed by the wrong
 * transfer. That is precisely the mis-attribution strictness exists to prevent.
 */
void hype_xhci_parked_drop_slot(hype_xhci_parked_t *p, uint32_t slot);

/* Fixed-length BOT stages must transfer every requested byte. */
int hype_xhci_xfer_exact_ok(uint32_t cc, uint32_t residue);

/* --- #340: USB mass-storage identity --- */

/* iSerialNumber: the string-descriptor index of the device's serial, byte 16
 * of the 18-byte device descriptor. 0 means the device offers none. */
static inline unsigned int hype_usb_dev_iserial_index(const uint8_t *devdesc18) {
    return devdesc18[16];
}

/* First LANGID from string descriptor 0 (the language-list descriptor).
 * Returns 0 and writes *out, or -1 on a malformed descriptor. */
int hype_usb_string_desc_langid0(const uint8_t *desc, unsigned int desc_len, uint16_t *out);

/*
 * Extract a USB string descriptor's text as ASCII, trimmed of space padding
 * and NUL-terminated into `out`. Returns its length, or -1 when the
 * descriptor is malformed, empty, does not fit `out_cap`, or contains any
 * code unit outside printable ASCII -- a serial the operator cannot type into
 * hype.cfg is not a usable identity, and an unidentified device must stay
 * unmatchable (#323).
 */
int hype_usb_string_desc_ascii(const uint8_t *desc, unsigned int desc_len, char *out,
                               unsigned int out_cap);

/* GET_DESCRIPTOR(STRING, index) on EP0. `langid` is 0 for the language-list
 * descriptor (index 0). Returns 0 and the raw descriptor bytes in `buf`. */
int hype_xhci_get_string_descriptor(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int index,
                                    uint16_t langid, uint8_t *buf, unsigned int maxlen);

/* INQUIRY with EVPD for `page` over the bulk-only transport. `len` bytes of
 * the response land in `buf`; the device pads short pages with zeros. */
int hype_xhci_msc_inquiry_vpd(hype_xhci_ctrl_t *c, unsigned int slot,
                              const hype_xhci_msc_eps_t *msc, uint8_t page, uint8_t *buf,
                              unsigned int len);


/* --- USB-7 (#241): device inventory across all controllers/ports --- */

/*
 * A persistent record of EVERY USB device the host sweep enumerated, on every
 * controller, root port, and hub port.
 *
 * Groundwork for passthrough, and immediately useful for USB HID host input
 * (#217/#219): today's scan is written to find one bulk-only mass-storage device
 * and stop, freeing the slot of anything else it meets. A keyboard on a port
 * after the stick is therefore never even seen. Recording what the sweep finds
 * costs nothing and makes the topology observable rather than inferred from a
 * failure trace.
 *
 * Deliberately just DATA plus pure operations -- no xHCI register access, no
 * transfers -- so the bookkeeping (capacity, de-duplication, claim marking,
 * lookup) is unit-tested rather than exercised only against real silicon on a
 * machine with no serial port.
 */
#define HYPE_USB_INVENTORY_MAX 32u

/* Who owns a device, so hype's own boot medium / keyboard is never offered to a
 * guest by a later passthrough feature. */
typedef enum {
    HYPE_USB_OWNER_NONE = 0,  /* enumerated, unclaimed -- a passthrough candidate */
    HYPE_USB_OWNER_HYPE,      /* hype uses it itself (log/boot medium, host HID) */
    HYPE_USB_OWNER_GUEST      /* passed through to a guest (reserved; no user yet) */
} hype_usb_owner_t;

/*
 * USB-7 (#241): one endpoint of an enumerated device, as passthrough needs it -- a guest
 * cannot be handed a device without knowing what endpoints to expose. Straight from the
 * configuration descriptor; no interpretation.
 */
#define HYPE_USB_MAX_ENDPOINTS 8u

/*
 * #741: how many interface descriptors one device's entry records.
 *
 * 8 covers what this project meets: a Logitech Unifying receiver presents 3, a composite
 * QMK keyboard 3-4, a UVC webcam 2 plus its streaming alternates. A device with more is
 * truncated and says so via iface_overflow rather than silently reporting fewer.
 */
#define HYPE_USB_MAX_IFACES 8u

typedef struct {
    uint8_t addr;       /* bEndpointAddress, including the 0x80 IN bit */
    uint8_t attributes; /* bmAttributes: bits 1:0 are the transfer type */
    uint8_t interval;   /* bInterval, raw (its meaning depends on speed) */
    uint16_t mps;       /* wMaxPacketSize */
} hype_usb_ep_t;

/*
 * Collect a configuration descriptor's endpoints into `out`, returning how many were
 * written. Stops at `cap` rather than overflowing -- a device with more endpoints than
 * hype records is reported short, which the caller can see, instead of corrupting the
 * table next to it.
 *
 * Walks the whole configuration (all interfaces), because an entry describes the DEVICE:
 * a composite peripheral's endpoints all belong to it whichever interface declares them.
 * Malformed lengths terminate the walk instead of spinning, same rule as every other
 * descriptor walker here.
 */
unsigned int hype_usb_collect_endpoints(const uint8_t *cfg, unsigned int len, hype_usb_ep_t *out,
                                        unsigned int cap);

/*
 * #741: one interface descriptor's identity.
 *
 * A device gets ONE class triple in the fields below, taken from its first interface, and
 * that is wrong for anything multi-function. The 5950X's `046d:c547` is a Logitech
 * receiver presenting a boot-KEYBOARD interface and a boot-MOUSE interface on one device;
 * hype recorded 03/01/02 and could only ever claim it as a pointer. On a machine whose
 * only input is a single combo dongle that means no keyboard at all, with nothing in the
 * log to explain it.
 */
typedef struct {
    uint8_t number;    /* bInterfaceNumber */
    uint8_t cls;       /* bInterfaceClass -- `class` is a C++ keyword and this header is
                        * included from tests that may be built as C++ */
    uint8_t subclass;
    uint8_t protocol;
} hype_usb_iface_t;

typedef struct {
    unsigned int controller;   /* index of the xHCI controller in PCI scan order */
    unsigned int root_port;    /* 1-based root port on that controller */
    unsigned int route;        /* xHCI Route String; 0 = directly on the root port */
    unsigned int slot;         /* xHCI slot id, 0 if the slot was released */
    unsigned int speed;        /* PORTSC/hub speed id */
    /*
     * #218: the Transaction Translator this device reaches its bus through, or 0 when it
     * needs none (a direct, HS or SS device). A LS/FS device behind a HS hub can only be
     * talked to through split transactions, and the slot context carries the TT that routes
     * them. Recorded here because EVERY later command that re-provides the slot context --
     * Configure Endpoint in particular -- must supply the SAME TT, and a claim path that
     * cannot look it up ends up passing zero and silently unrouting the device.
     */
    unsigned int tt_hub_slot;
    unsigned int tt_port;
    uint16_t vid;
    uint16_t pid;
    uint8_t dev_class;         /* bDeviceClass from the device descriptor */
    uint8_t dev_subclass;
    uint8_t dev_protocol;
    uint8_t owner;             /* hype_usb_owner_t */
    /* #241: the device's endpoint set, for passthrough. ep_count 0 means the
     * configuration descriptor could not be read, not that the device has none. */
    uint8_t ep_count;
    hype_usb_ep_t eps[HYPE_USB_MAX_ENDPOINTS];
    /*
     * #741: EVERY interface, not just the first. dev_class/subclass/protocol above stay as
     * they are -- the first interface -- because existing lookups key off them and a
     * single-function device's answer is unchanged; this is the fuller truth alongside it.
     */
    uint8_t iface_count;
    uint8_t iface_overflow;    /* interfaces seen but not recorded (table full) */
    hype_usb_iface_t ifaces[HYPE_USB_MAX_IFACES];
} hype_usb_devinfo_t;

typedef struct {
    hype_usb_devinfo_t dev[HYPE_USB_INVENTORY_MAX];
    unsigned int count;
    /* Devices seen but NOT recorded because the table was full. Counted rather
     * than dropped silently: an inventory that quietly truncates would report a
     * complete topology while hiding ports, which is worse than saying so. */
    unsigned int overflow;
} hype_usb_inventory_t;

void hype_usb_inventory_reset(hype_usb_inventory_t *inv);

/*
 * Record one enumerated device. Returns the new entry's index, or -1 when the
 * table is full (bumping `overflow`).
 *
 * Identity is (controller, root_port, route) -- the physical position, which is
 * what makes a device the same device across re-scans. Re-adding the same
 * position UPDATES that entry instead of duplicating it, because the sweep can
 * legitimately revisit a position (a hub descent re-reads its parent) and two
 * entries for one port would make any later "which device is on port N" answer
 * ambiguous. VID/PID deliberately do NOT participate: two identical sticks in
 * two ports are two devices.
 */
int hype_usb_inventory_add(hype_usb_inventory_t *inv, const hype_usb_devinfo_t *d);

/*
 * #744: mark the device at this position as gone -- slot 0 (which the struct already
 * defines as "the slot was released") and owner NONE.
 *
 * The entry is kept rather than removed: removing it would shift every later index, and
 * callers hold indices across the sweep. Returns 1 if an entry was found and updated.
 */
int hype_usb_inventory_note_departed(hype_usb_inventory_t *inv, unsigned int controller,
                                     unsigned int root_port, unsigned int route);

/* Position lookup; -1 if absent. */
int hype_usb_inventory_find(const hype_usb_inventory_t *inv, unsigned int controller,
                            unsigned int root_port, unsigned int route);

/* Mark the device at `index` as owned. No-op for an out-of-range index. */
void hype_usb_inventory_claim(hype_usb_inventory_t *inv, int index, hype_usb_owner_t owner);

/*
 * First device matching `dev_class` that is still unclaimed, or -1. `after` is an
 * exclusive start index so a caller can walk every match (pass -1 to start).
 * Used by #217 to find a HID keyboard hype can take without stealing the boot
 * medium.
 */
int hype_usb_inventory_next_unclaimed_class(const hype_usb_inventory_t *inv, uint8_t dev_class,
                                            int after);

/* Count of entries with the given owner -- for the summary line. */
unsigned int hype_usb_inventory_count_owner(const hype_usb_inventory_t *inv,
                                            hype_usb_owner_t owner);

const char *hype_usb_owner_str(hype_usb_owner_t owner);

/* USB HID class, and the boot-protocol subclass/protocols #217/#219 need. */
#define HYPE_USB_CLASS_HID        0x03u
#define HYPE_USB_SUBCLASS_BOOT    0x01u
#define HYPE_USB_PROTO_KEYBOARD   0x01u
#define HYPE_USB_PROTO_MOUSE      0x02u


/*
 * #241: class/subclass/protocol of the FIRST interface in a configuration
 * descriptor, for devices whose bDeviceClass is 0.
 *
 * Needed because a composite device -- which is most USB peripherals, including
 * QEMU's usb-kbd and usb-storage -- reports bDeviceClass 0 and puts the real class
 * in its interface descriptors. Measured: hype's inventory recorded both a keyboard
 * and a mass-storage stick as class=00/00/00, so a class lookup for HID (what #217
 * needs to find a keyboard) matched neither.
 *
 * Returns 1 and fills the outputs when an interface descriptor is found, 0
 * otherwise. Pure -- walks the caller's buffer only. Takes the FIRST interface: a
 * multi-function device needs per-interface handling that nothing needs yet, and
 * guessing which of several is "the" class would be worse than reporting the first
 * and saying so.
 */
int hype_usb_first_iface_class(const uint8_t *cfg, unsigned int len, uint8_t *out_class,
                               uint8_t *out_subclass, uint8_t *out_protocol);

/*
 * #741: collect EVERY interface descriptor's identity from a configuration descriptor.
 *
 * Returns how many were written (capped at `cap`); *out_overflow, when given, receives the
 * number seen beyond the cap so truncation is visible rather than silent. Pure -- walks the
 * caller's buffer only, and a malformed length terminates the walk instead of spinning,
 * same rule as every other descriptor walker here.
 *
 * Alternate settings of the same interface number are recorded as they appear: hype does
 * not issue SET_INTERFACE, so alt 0 is what is live, and collapsing them here would hide a
 * device whose alt 0 is a placeholder.
 */
unsigned int hype_usb_collect_interfaces(const uint8_t *cfg, unsigned int len,
                                         hype_usb_iface_t *out, unsigned int cap,
                                         uint8_t *out_overflow);

/*
 * #741: index of the first interface on `d` matching class/subclass/protocol, or -1.
 *
 * This is what a claim path should ask instead of comparing d->dev_subclass and
 * d->dev_protocol, which only ever describe the first interface.
 */
int hype_usb_devinfo_find_iface(const hype_usb_devinfo_t *d, uint8_t cls, uint8_t subclass,
                                uint8_t protocol);

/*
 * #741: next inventory entry (after index `after`, -1 to start) carrying an interface with
 * this class/subclass/protocol, or -1.
 *
 * Deliberately does NOT filter on owner, unlike hype_usb_inventory_next_unclaimed_class().
 * A composite dongle can legitimately be claimed twice -- once for its boot-keyboard
 * interface and once for its boot-mouse interface -- and an owner filter here is exactly
 * what stopped the second claim. Callers that want only unowned devices check the owner
 * themselves, where they can tell "already mine, for a different interface" from
 * "somebody else's".
 */
int hype_usb_inventory_next_iface(const hype_usb_inventory_t *inv, uint8_t cls,
                                  uint8_t subclass, uint8_t protocol, int after);

/* USB-5 (#217): configure a HID keyboard's interrupt-IN endpoint, then poll it.
 * hype_xhci_int_in_poll returns 1 = report copied, 0 = none yet (the normal idle
 * case), -1 = transfer error. The three-way return matters: collapsing idle into an
 * error would disable the keyboard whenever nobody typed. */
int hype_xhci_configure_int_in_endpoint(hype_xhci_ctrl_t *c, unsigned int slot,
                                       const hype_xhci_devpath_t *path, unsigned int ep_addr,
                                       unsigned int mps, unsigned int interval);
int hype_xhci_int_in_poll(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int ep_addr,
                          uint8_t *out, unsigned int len);

/*
 * USB-5 (#217): encode a descriptor bInterval into the xHCI Endpoint Context Interval
 * field (xHCI 6.2.3.6), for the given PORTSC speed id.
 *
 * Mandatory for INTERRUPT endpoints and ignored for bulk -- which is why
 * hype_xhci_ep_ctx() never set it and why a keyboard configured through that path
 * enumerated correctly and then delivered no reports at all: Interval 0 leaves the
 * controller with no schedule on which to poll the device.
 *
 * The two speed families encode differently, and conflating them is the easy mistake:
 *   - High speed and above: bInterval is ALREADY an exponent (period = 2^(bInterval-1)
 *     microframes), so Interval = bInterval - 1.
 *   - Full and low speed: bInterval is a frame COUNT (1..255), so it must be converted
 *     to an exponent: Interval = log2(bInterval) + 3.
 * Results are clamped to the field's legal range rather than truncated.
 */
unsigned int hype_xhci_interval_encode(unsigned int speed_id, unsigned int b_interval);

/* Endpoint context with an explicit Interval and Max ESIT Payload -- for interrupt
 * (and isoch) endpoints. */
void hype_xhci_ep_ctx_interval(uint32_t ep[8], unsigned int ep_type, unsigned int max_packet,
                               uint64_t tr_dequeue_phys, int dcs, unsigned int interval);
#endif /* HYPE_CORE_XHCI_H */
