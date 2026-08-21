#ifndef HYPE_DEVICES_XHCI_DEV_H
#define HYPE_DEVICES_XHCI_DEV_H

#include <stdint.h>
#include "../core/guest_mem.h"

/*
 * #591 (USB-GUEST-1): a GUEST-FACING emulated xHCI USB host controller.
 *
 * This is the controller the GUEST enumerates and drives -- DISTINCT from core/xhci.c, which is
 * hype's HOST-side driver for real controllers. The guest's own xhci-hcd sees this PCI function,
 * maps its BAR, and runs the standard bring-up: reset, program the command ring / event ring /
 * DCBAA, then Enable Slot -> Address Device -> Configure Endpoint to bring one device up.
 *
 * Scope of #591 is the controller and the slot bring-up. No class logic: the USB Mass Storage
 * (BOT/SCSI) layer that answers descriptor and transfer requests on the endpoints is #592, and it
 * is backed through the same hype_blk_backend_t vtable the other disk front-ends use.
 *
 * The register decode is pure (no guest-memory access) so it unit-tests directly. The ring
 * processing (command ring -> event ring, reading input contexts, writing device contexts) needs
 * guest memory and takes a hype_gpa_map_t, exactly like process_virtio_blk_queue -- so it is a
 * vendor-neutral function unit-tested against hand-built rings in a host buffer.
 */

/* ---- BAR layout (all offsets from BAR0 base) --------------------------------------------- */
#define HYPE_XHCI_BAR_SIZE 0x1000u

/* Capability registers (read-only), 0x00..0x1F. */
#define HYPE_XHCI_CAP_CAPLENGTH 0x00u /* byte: operational-register offset */
#define HYPE_XHCI_CAP_HCIVERSION 0x02u
#define HYPE_XHCI_CAP_HCSPARAMS1 0x04u
#define HYPE_XHCI_CAP_HCSPARAMS2 0x08u
#define HYPE_XHCI_CAP_HCSPARAMS3 0x0Cu
#define HYPE_XHCI_CAP_HCCPARAMS1 0x10u
#define HYPE_XHCI_CAP_DBOFF 0x14u
#define HYPE_XHCI_CAP_RTSOFF 0x18u
#define HYPE_XHCI_CAP_HCCPARAMS2 0x1Cu

#define HYPE_XHCI_CAPLENGTH 0x20u  /* operational registers begin here */
#define HYPE_XHCI_HCIVERSION 0x0100u
#define HYPE_XHCI_MAX_SLOTS 8u
#define HYPE_XHCI_MAX_PORTS 1u
/* HCSPARAMS1: MaxSlots[7:0], MaxIntrs[18:8], MaxPorts[31:24]. */
#define HYPE_XHCI_HCSPARAMS1 \
    (((uint32_t)HYPE_XHCI_MAX_PORTS << 24) | (1u << 8) | HYPE_XHCI_MAX_SLOTS)
/* HCCPARAMS1 = 0: 32-bit addressing (AC64=0), 32-byte contexts (CSZ=0), no xECP. */
#define HYPE_XHCI_HCCPARAMS1 0x00000000u
#define HYPE_XHCI_DBOFF 0x0800u  /* must be 4-byte aligned; low 2 bits reserved 0 */
#define HYPE_XHCI_RTSOFF 0x0600u /* must be 32-byte aligned; low 5 bits reserved 0 */

/* Operational registers (absolute offsets = CAPLENGTH + reg). */
#define HYPE_XHCI_OP_USBCMD (HYPE_XHCI_CAPLENGTH + 0x00u)
#define HYPE_XHCI_OP_USBSTS (HYPE_XHCI_CAPLENGTH + 0x04u)
#define HYPE_XHCI_OP_PAGESIZE (HYPE_XHCI_CAPLENGTH + 0x08u)
#define HYPE_XHCI_OP_DNCTRL (HYPE_XHCI_CAPLENGTH + 0x14u)
#define HYPE_XHCI_OP_CRCR_LO (HYPE_XHCI_CAPLENGTH + 0x18u)
#define HYPE_XHCI_OP_CRCR_HI (HYPE_XHCI_CAPLENGTH + 0x1Cu)
#define HYPE_XHCI_OP_DCBAAP_LO (HYPE_XHCI_CAPLENGTH + 0x30u)
#define HYPE_XHCI_OP_DCBAAP_HI (HYPE_XHCI_CAPLENGTH + 0x34u)
#define HYPE_XHCI_OP_CONFIG (HYPE_XHCI_CAPLENGTH + 0x38u)
/* Port register set: operational base + 0x400 + 0x10 * (port-1). One port. */
#define HYPE_XHCI_OP_PORTSC (HYPE_XHCI_CAPLENGTH + 0x400u)

/* USBCMD bits. */
#define HYPE_XHCI_USBCMD_RS 0x0001u    /* Run/Stop */
#define HYPE_XHCI_USBCMD_HCRST 0x0002u /* Host Controller Reset */
#define HYPE_XHCI_USBCMD_INTE 0x0004u  /* Interrupter Enable */
/* USBSTS bits. */
#define HYPE_XHCI_USBSTS_HCH 0x0001u   /* HCHalted */
#define HYPE_XHCI_USBSTS_EINT 0x0008u  /* Event Interrupt */
#define HYPE_XHCI_USBSTS_PCD 0x0010u   /* Port Change Detect */
#define HYPE_XHCI_USBSTS_CNR 0x0800u   /* Controller Not Ready */
/* CRCR bits (in the low dword). */
#define HYPE_XHCI_CRCR_RCS 0x1u  /* Ring Cycle State */
#define HYPE_XHCI_CRCR_CS 0x2u   /* Command Stop */
#define HYPE_XHCI_CRCR_CA 0x4u   /* Command Abort */
#define HYPE_XHCI_CRCR_CRR 0x8u  /* Command Ring Running */
#define HYPE_XHCI_CRCR_PTR_MASK 0xFFFFFFFFFFFFFFC0ull /* [63:6] command ring pointer */

/* PORTSC bits. */
#define HYPE_XHCI_PORTSC_CCS 0x00000001u  /* Current Connect Status */
#define HYPE_XHCI_PORTSC_PED 0x00000002u  /* Port Enabled/Disabled */
#define HYPE_XHCI_PORTSC_PR 0x00000010u   /* Port Reset */
#define HYPE_XHCI_PORTSC_PP 0x00000200u   /* Port Power */
#define HYPE_XHCI_PORTSC_CSC 0x00020000u  /* Connect Status Change */
#define HYPE_XHCI_PORTSC_PRC 0x00200000u  /* Port Reset Change */
#define HYPE_XHCI_PORTSC_SPEED_SHIFT 10u  /* Port Speed [13:10] */
#define HYPE_XHCI_PORTSC_SPEED_HS 3u      /* High-Speed (USB 2.0) */
/* Write-1-to-clear change bits the guest acknowledges. */
#define HYPE_XHCI_PORTSC_RW1C (HYPE_XHCI_PORTSC_CSC | HYPE_XHCI_PORTSC_PRC)

/* Runtime registers (absolute = RTSOFF + reg). Interrupter 0 only. */
#define HYPE_XHCI_RT_MFINDEX (HYPE_XHCI_RTSOFF + 0x00u)
#define HYPE_XHCI_RT_IMAN (HYPE_XHCI_RTSOFF + 0x20u)
#define HYPE_XHCI_RT_IMOD (HYPE_XHCI_RTSOFF + 0x24u)
#define HYPE_XHCI_RT_ERSTSZ (HYPE_XHCI_RTSOFF + 0x28u)
#define HYPE_XHCI_RT_ERSTBA_LO (HYPE_XHCI_RTSOFF + 0x30u)
#define HYPE_XHCI_RT_ERSTBA_HI (HYPE_XHCI_RTSOFF + 0x34u)
#define HYPE_XHCI_RT_ERDP_LO (HYPE_XHCI_RTSOFF + 0x38u)
#define HYPE_XHCI_RT_ERDP_HI (HYPE_XHCI_RTSOFF + 0x3Cu)
/* IMAN bits. */
#define HYPE_XHCI_IMAN_IP 0x1u /* Interrupt Pending (write-1-to-clear) */
#define HYPE_XHCI_IMAN_IE 0x2u /* Interrupt Enable */
/* ERDP bit 3: Event Handler Busy (write-1-to-clear). */
#define HYPE_XHCI_ERDP_EHB 0x8u
#define HYPE_XHCI_ERDP_PTR_MASK 0xFFFFFFFFFFFFFFF0ull

/* Doorbell registers (absolute = DBOFF + 4 * target). Target 0 = command ring. */
#define HYPE_XHCI_DB_COMMAND 0u

/* ---- TRB (Transfer Request Block) -------------------------------------------------------- */
/* A TRB is 16 bytes: two dwords of parameter, one status dword, one control dword. Bit 0 of the
 * control dword is the Cycle bit; bits [15:10] are the TRB Type; bits [31:24] the Slot ID. */
#define HYPE_XHCI_TRB_SIZE 16u
#define HYPE_XHCI_TRB_CYCLE 0x1u
#define HYPE_XHCI_TRB_TYPE_SHIFT 10u
#define HYPE_XHCI_TRB_TYPE_MASK 0xFC00u /* [15:10] within the control dword's low 16 bits */
#define HYPE_XHCI_TRB_SLOT_SHIFT 24u

/* TRB types (control dword bits [15:10]). */
#define HYPE_XHCI_TRB_NORMAL 1u
#define HYPE_XHCI_TRB_LINK 6u
#define HYPE_XHCI_TRB_ENABLE_SLOT 9u
#define HYPE_XHCI_TRB_DISABLE_SLOT 10u
#define HYPE_XHCI_TRB_ADDRESS_DEVICE 11u
#define HYPE_XHCI_TRB_CONFIGURE_ENDPOINT 12u
#define HYPE_XHCI_TRB_EVALUATE_CONTEXT 13u
#define HYPE_XHCI_TRB_NOOP_CMD 23u
#define HYPE_XHCI_TRB_TRANSFER_EVENT 32u
#define HYPE_XHCI_TRB_CMD_COMPLETION 33u
#define HYPE_XHCI_TRB_PORT_STATUS_CHANGE 34u
/* Link-TRB control bit 1: Toggle Cycle. */
#define HYPE_XHCI_TRB_LINK_TC 0x2u

/* Completion codes (event TRB status [31:24]). */
#define HYPE_XHCI_CC_SUCCESS 1u
#define HYPE_XHCI_CC_TRB_ERROR 5u
#define HYPE_XHCI_CC_SLOT_NOT_ENABLED 11u
#define HYPE_XHCI_CC_PARAMETER_ERROR 17u

/* Slot state (device-context slot-context byte, and our own tracking). */
typedef enum {
    HYPE_XHCI_SLOT_DISABLED = 0,
    HYPE_XHCI_SLOT_ENABLED = 1,
    HYPE_XHCI_SLOT_ADDRESSED = 2,
    HYPE_XHCI_SLOT_CONFIGURED = 3
} hype_xhci_slot_state_t;

/* Per-slot tracking (one device per controller in v1, but the array keeps slot IDs honest). */
typedef struct {
    hype_xhci_slot_state_t state;
    uint64_t device_ctx_gpa; /* DCBAA[slot] value set at Address Device */
    uint64_t ep_ring[32];    /* transfer-ring dequeue pointer per endpoint DCI (1..31); [0] unused */
    uint8_t ep_cycle[32];    /* consumer cycle state per endpoint ring */
    uint8_t ep_configured[32];
} hype_xhci_slot_t;

typedef struct {
    /* Operational register shadow. */
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t dnctrl;
    uint32_t config;
    uint64_t crcr;   /* command ring control (pointer + flags) */
    uint64_t dcbaap; /* device context base address array pointer */
    uint32_t portsc; /* the single port */

    /* Runtime interrupter 0. */
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint64_t erstba; /* event ring segment table base */
    uint64_t erdp;   /* event ring dequeue pointer (guest-owned, we read it) */

    /* Command ring processing state. */
    uint64_t cmd_ring_ptr; /* our dequeue pointer into the command ring */
    uint8_t cmd_ccs;       /* consumer cycle state for the command ring */
    int crcr_latched;      /* cmd_ring_ptr has been loaded from CRCR */

    /* Event ring producer state. */
    uint64_t event_ring_ptr;  /* our enqueue pointer into the current event-ring segment */
    uint64_t event_seg_base;  /* current segment base (from the ERST) */
    uint32_t event_seg_size;  /* current segment size in TRBs */
    uint32_t event_trbs_left; /* TRBs remaining in this segment before wrap */
    uint8_t event_pcs;        /* producer cycle state for the event ring */
    int erst_latched;         /* event ring segment loaded from the ERST */

    hype_xhci_slot_t slots[HYPE_XHCI_MAX_SLOTS + 1]; /* [0] unused; slot IDs are 1-based */
    unsigned int slots_enabled;

    int device_present; /* 1 once a backing device is attached (set by the MSC layer, #592) */

    /* Counters for tests/diagnostics. */
    uint32_t events_posted;
    uint32_t commands_processed;
} hype_xhci_dev_t;

/* Reset to power-on: HCHalted set, CNR clear, one connected port, rings empty. `device_present`
 * declares the port has the emulated device on it (so PORTSC reports CCS=1). */
void hype_xhci_dev_reset(hype_xhci_dev_t *dev, int device_present);

/*
 * Pure MMIO register access. offset is BAR-relative; size_bytes is 1/2/4/8. Returns 0 on a
 * recognized access, -1 on a bad width or an offset outside the register file. A read of a
 * reserved-but-in-file offset yields 0; a write to a read-only or reserved offset is ignored.
 * These never touch guest memory -- ring work happens in hype_xhci_dev_run() below.
 */
int hype_xhci_dev_mmio_read(const hype_xhci_dev_t *dev, uint32_t offset, uint8_t size_bytes,
                            uint64_t *out_value);
int hype_xhci_dev_mmio_write(hype_xhci_dev_t *dev, uint32_t offset, uint8_t size_bytes,
                             uint64_t value, const hype_gpa_map_t *dma_map);

/*
 * A doorbell write (BAR offset DBOFF + 4*target) kicks ring processing. Target 0 drains the
 * command ring; a slot target (1..MaxSlots) is a transfer-ring kick handled by the class layer
 * (#592) -- in #591 a slot doorbell is accepted and ignored. Called from mmio_write when the
 * offset lands in the doorbell region; exposed for the unit test to drive directly.
 */
void hype_xhci_dev_doorbell(hype_xhci_dev_t *dev, uint32_t target, const hype_gpa_map_t *dma_map);

/* 1 if the controller has an interrupt asserted the guest has not yet acknowledged. The arch
 * dispatch raises the device's GSI/MSI while this is true. */
int hype_xhci_dev_irq_pending(const hype_xhci_dev_t *dev);

#endif /* HYPE_DEVICES_XHCI_DEV_H */
