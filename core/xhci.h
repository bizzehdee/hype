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
    HYPE_XHCI_TRB_DISABLE_SLOT    = 10,
    HYPE_XHCI_TRB_ADDRESS_DEVICE  = 11,
    HYPE_XHCI_TRB_CONFIG_EP       = 12,
    HYPE_XHCI_TRB_RESET_EP        = 14,
    HYPE_XHCI_TRB_STOP_EP         = 15,
    HYPE_XHCI_TRB_SET_TR_DEQUEUE  = 16,
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

/* --- USB Mass Storage endpoint discovery (pure, xHCI-independent USB descr) --- */

/* USB Mass Storage class/subclass/protocol (bulk-only SCSI). */
#define HYPE_USB_CLASS_MSC       0x08u
#define HYPE_USB_SUBCLASS_SCSI   0x06u
#define HYPE_USB_PROTO_BOT       0x50u
/* USB descriptor types. */
#define HYPE_USB_DESC_CONFIG     0x02u
#define HYPE_USB_DESC_INTERFACE  0x04u
#define HYPE_USB_DESC_ENDPOINT   0x05u
/* USB hub class (device descriptor bDeviceClass == 0x09). */
#define HYPE_USB_CLASS_HUB       0x09u
#define HYPE_USB_DESC_HUB        0x29u /* wValue high byte for GET hub descriptor */

/* PORTSC/hub speed ids (shared with hype_xhci_default_mps). */
#define HYPE_USB_SPEED_FULL  1u
#define HYPE_USB_SPEED_LOW   2u
#define HYPE_USB_SPEED_HIGH  3u

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
    int used;
} hype_xhci_parked_evt_t;

typedef struct {
    hype_xhci_parked_evt_t e[HYPE_XHCI_PARKED_MAX];
    uint32_t next; /* round-robin victim when full */
} hype_xhci_parked_t;

void hype_xhci_parked_reset(hype_xhci_parked_t *p);

/* Remember a completion that is not the one currently awaited. */
void hype_xhci_parked_put(hype_xhci_parked_t *p, uint32_t slot, uint32_t dci, uint64_t trb,
                          uint32_t cc);

/*
 * Claim a previously parked completion for exactly this (slot, dci, trb). Returns 1 and
 * writes *out_cc when found, and REMOVES it so a single event cannot satisfy two waits.
 * Returns 0 otherwise.
 */
int hype_xhci_parked_take(hype_xhci_parked_t *p, uint32_t slot, uint32_t dci, uint64_t trb,
                          uint32_t *out_cc);

/*
 * Drop every parked event for a slot. Called after a reset/recovery: the transfer rings
 * restart at index 0, so a parked event for the torn-down state would carry a TRB
 * address the RETRY is about to reuse -- and would then be claimed by the wrong
 * transfer. That is precisely the mis-attribution strictness exists to prevent.
 */
void hype_xhci_parked_drop_slot(hype_xhci_parked_t *p, uint32_t slot);


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

typedef struct {
    unsigned int controller;   /* index of the xHCI controller in PCI scan order */
    unsigned int root_port;    /* 1-based root port on that controller */
    unsigned int route;        /* xHCI Route String; 0 = directly on the root port */
    unsigned int slot;         /* xHCI slot id, 0 if the slot was released */
    unsigned int speed;        /* PORTSC/hub speed id */
    uint16_t vid;
    uint16_t pid;
    uint8_t dev_class;         /* bDeviceClass from the device descriptor */
    uint8_t dev_subclass;
    uint8_t dev_protocol;
    uint8_t owner;             /* hype_usb_owner_t */
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

/* Endpoint context with an explicit Interval -- for interrupt (and isoch) endpoints. */
void hype_xhci_ep_ctx_interval(uint32_t ep[8], unsigned int ep_type, unsigned int max_packet,
                               uint64_t tr_dequeue_phys, int dcs, unsigned int interval);
#endif /* HYPE_CORE_XHCI_H */
