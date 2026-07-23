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

/* PORTSC bits (xHCI 5.4.8) */
#define HYPE_XHCI_PORTSC_CCS      (1u << 0)  /* Current Connect Status */
#define HYPE_XHCI_PORTSC_PED      (1u << 1)  /* Port Enabled/Disabled */
#define HYPE_XHCI_PORTSC_PR       (1u << 4)  /* Port Reset */
#define HYPE_XHCI_PORTSC_PP       (1u << 9)  /* Port Power */
#define HYPE_XHCI_PORTSC_PRC      (1u << 21) /* Port Reset Change */
#define HYPE_XHCI_PORTSC_CSC      (1u << 17) /* Connect Status Change */
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
    HYPE_XHCI_TRB_ADDRESS_DEVICE  = 11,
    HYPE_XHCI_TRB_CONFIG_EP       = 12,
    HYPE_XHCI_TRB_NOOP_CMD        = 23,
    HYPE_XHCI_TRB_TRANSFER_EVENT  = 32,
    HYPE_XHCI_TRB_CMD_COMPLETION  = 33,
    HYPE_XHCI_TRB_PORT_STATUS     = 34
} hype_xhci_trb_type_t;

/* Completion codes (event TRB status bits 31:24), xHCI 6.4.5. */
#define HYPE_XHCI_CC_SUCCESS      1u
#define HYPE_XHCI_CC_SHORT_PACKET 13u

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

/* --- device/input context encoders (xHCI 6.2), pure --- */
/* Contexts are 32 or 64 bytes (CSZ); these fill the first 8 dwords (the rest is
 * reserved/padding). Callers place them at ctx_size-byte strides in the
 * input/device context. */

/* Input Control Context: drop-context + add-context bitmaps (A0=slot, A1=EP0). */
#define HYPE_XHCI_ADD_SLOT (1u << 0)
#define HYPE_XHCI_ADD_EP0  (1u << 1)
void hype_xhci_input_ctrl_ctx(uint32_t icc[8], uint32_t add_flags, uint32_t drop_flags);

/* Slot Context: route string, PORTSC speed, context-entries (highest valid DCI),
 * and the root-hub port number the device is attached to. */
void hype_xhci_slot_ctx(uint32_t sc[8], unsigned int route, unsigned int speed,
                        unsigned int ctx_entries, unsigned int root_port);

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

/* --- USB Mass Storage endpoint discovery (pure, xHCI-independent USB descr) --- */

/* USB Mass Storage class/subclass/protocol (bulk-only SCSI). */
#define HYPE_USB_CLASS_MSC       0x08u
#define HYPE_USB_SUBCLASS_SCSI   0x06u
#define HYPE_USB_PROTO_BOT       0x50u
/* USB descriptor types. */
#define HYPE_USB_DESC_CONFIG     0x02u
#define HYPE_USB_DESC_INTERFACE  0x04u
#define HYPE_USB_DESC_ENDPOINT   0x05u

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

/* Captured controller geometry + register bases, filled by hype_xhci_host_init. */
typedef struct {
    uint64_t bar;            /* xHCI MMIO BAR0 (identity-mapped) */
    uint32_t op;             /* operational-register base offset (= CAPLENGTH) */
    uint32_t dboff;          /* raw DBOFF */
    uint32_t rtsoff;         /* raw RTSOFF */
    unsigned int max_slots;
    unsigned int max_ports;
    unsigned int ctx_size;   /* 32 or 64 (CSZ) */
    int inited;
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
int hype_xhci_address_device(hype_xhci_ctrl_t *c, unsigned int slot, unsigned int root_port,
                             unsigned int speed);

/*
 * Reads the 18-byte USB device descriptor from an addressed device via a
 * GET_DESCRIPTOR control transfer on EP0 (Setup/Data/Status). Writes it to
 * buf18 (>= 18 bytes). Returns 0 on success, -1 on error/timeout.
 */
int hype_xhci_get_device_descriptor(hype_xhci_ctrl_t *c, unsigned int slot, uint8_t *buf18);

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
 * Issues a Configure Endpoint command adding the MSC bulk IN + bulk OUT
 * endpoints (from *msc) to `slot`'s device context, each with a fresh transfer
 * ring. root_port/speed re-provide the input Slot Context. Returns 0 on success.
 * After this, bulk transfers on those endpoints are possible.
 */
int hype_xhci_configure_bulk_endpoints(hype_xhci_ctrl_t *c, unsigned int slot,
                                       unsigned int root_port, unsigned int speed,
                                       const hype_xhci_msc_eps_t *msc);

#endif /* HYPE_CORE_XHCI_H */
