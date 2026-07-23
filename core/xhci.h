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

/* --- event TRB decode (pure) --- */
unsigned int hype_xhci_event_cc(const uint32_t trb[4]);       /* completion code, status[31:24] */
unsigned int hype_xhci_event_slot_id(const uint32_t trb[4]);  /* control[31:24] */
uint64_t hype_xhci_event_trb_ptr(const uint32_t trb[4]);      /* dword0/1: TRB pointer (cmd/xfer) */
unsigned int hype_xhci_event_port_id(const uint32_t trb[4]);  /* Port Status Change: param[31:24] */
unsigned int hype_xhci_event_xfer_residue(const uint32_t trb[4]); /* status[23:0] bytes not transferred */

#endif /* HYPE_CORE_XHCI_H */
