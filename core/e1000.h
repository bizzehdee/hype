/*
 * NET-1 (#80): Intel 82540EM ("e1000") host NIC -- the PURE half.
 *
 * Register offsets, descriptor formats, ring arithmetic and EEPROM/MAC decode, with no MMIO, no
 * allocation and no globals. core/e1000_hw.c does the real register access and owns the rings;
 * this file holds the parts that get subtly wrong -- ring wrap, descriptor ownership, MAC byte
 * order -- so they can be tested without hardware, the same split core/nvme_host.c uses.
 *
 * Source: Intel's "PCI/PCI-X Family of Gigabit Ethernet Controllers Software Developer's Manual",
 * section 13 (register descriptions) and 3.2-3.4 (descriptor formats). The 82540EM is the simplest
 * member of the family and the one QEMU emulates, which is what makes the whole NET stack testable
 * in the existing rig rather than only on hardware.
 *
 * hype owns exactly ONE NIC (plan.md §6e): it is the hypervisor's uplink, and guests reach it
 * through NAT (#83) rather than by touching it.
 */
#ifndef HYPE_CORE_E1000_H
#define HYPE_CORE_E1000_H

#include <stdint.h>

/* PCI identity. Class 0x02 (network) / subclass 0x00 (ethernet). */
#define HYPE_E1000_PCI_VENDOR 0x8086u
#define HYPE_E1000_PCI_DEVICE_82540EM 0x100Eu
#define HYPE_E1000_PCI_CLASS_ETHERNET 0x0200u /* class<<8 | subclass */

/* Registers (byte offsets into BAR0). */
#define HYPE_E1000_REG_CTRL 0x0000u   /* Device Control */
#define HYPE_E1000_REG_STATUS 0x0008u /* Device Status */
#define HYPE_E1000_REG_EECD 0x0010u   /* EEPROM/Flash Control & Data */
#define HYPE_E1000_REG_EERD 0x0014u   /* EEPROM Read */
#define HYPE_E1000_REG_ICR 0x00C0u    /* Interrupt Cause Read */
#define HYPE_E1000_REG_IMS 0x00D0u    /* Interrupt Mask Set */
#define HYPE_E1000_REG_IMC 0x00D8u    /* Interrupt Mask Clear */
#define HYPE_E1000_REG_RCTL 0x0100u   /* Receive Control */
#define HYPE_E1000_REG_TCTL 0x0400u   /* Transmit Control */
#define HYPE_E1000_REG_TIPG 0x0410u   /* Transmit Inter Packet Gap */

#define HYPE_E1000_REG_RDBAL 0x2800u /* RX descriptor base, low */
#define HYPE_E1000_REG_RDBAH 0x2804u /* RX descriptor base, high */
#define HYPE_E1000_REG_RDLEN 0x2808u /* RX ring length in BYTES */
#define HYPE_E1000_REG_RDH 0x2810u   /* RX head (hardware-owned) */
#define HYPE_E1000_REG_RDT 0x2818u   /* RX tail (driver-owned) */

#define HYPE_E1000_REG_TDBAL 0x3800u
#define HYPE_E1000_REG_TDBAH 0x3804u
#define HYPE_E1000_REG_TDLEN 0x3808u
#define HYPE_E1000_REG_TDH 0x3810u
#define HYPE_E1000_REG_TDT 0x3818u

/* Receive Address, entry 0 -- the MAC the hardware filters on. */
#define HYPE_E1000_REG_RAL0 0x5400u
#define HYPE_E1000_REG_RAH0 0x5404u

/* CTRL bits. */
#define HYPE_E1000_CTRL_FD (1u << 0)     /* Full Duplex */
#define HYPE_E1000_CTRL_ASDE (1u << 5)   /* Auto-Speed Detection Enable */
#define HYPE_E1000_CTRL_SLU (1u << 6)    /* Set Link Up */
#define HYPE_E1000_CTRL_RST (1u << 26)   /* Device Reset (self-clearing) */

/* STATUS bits. */
#define HYPE_E1000_STATUS_LU (1u << 1) /* Link Up */

/* RCTL bits. */
#define HYPE_E1000_RCTL_EN (1u << 1)      /* Receiver Enable */
#define HYPE_E1000_RCTL_SBP (1u << 2)     /* Store Bad Packets */
#define HYPE_E1000_RCTL_UPE (1u << 3)     /* Unicast Promiscuous */
#define HYPE_E1000_RCTL_MPE (1u << 4)     /* Multicast Promiscuous */
#define HYPE_E1000_RCTL_BAM (1u << 15)    /* Broadcast Accept Mode */
#define HYPE_E1000_RCTL_SECRC (1u << 26)  /* Strip Ethernet CRC */
/* Buffer size field: 00 = 2048 bytes when RCTL.BSEX is clear, which is what this driver uses. */
#define HYPE_E1000_RCTL_BSIZE_2048 0u

/* TCTL bits. */
#define HYPE_E1000_TCTL_EN (1u << 1)  /* Transmitter Enable */
#define HYPE_E1000_TCTL_PSP (1u << 3) /* Pad Short Packets */

/* EERD bits. */
#define HYPE_E1000_EERD_START (1u << 0)
#define HYPE_E1000_EERD_DONE (1u << 4)
#define HYPE_E1000_EERD_ADDR_SHIFT 8u
#define HYPE_E1000_EERD_DATA_SHIFT 16u

/*
 * Legacy TX descriptor (manual 3.3.3), 16 bytes. `cmd` and `status` are the driver/hardware
 * handshake: the driver sets RS to ask for write-back, the hardware sets DD when done.
 */
#define HYPE_E1000_TXD_CMD_EOP (1u << 0) /* End Of Packet */
#define HYPE_E1000_TXD_CMD_IFCS (1u << 1) /* Insert FCS/CRC */
#define HYPE_E1000_TXD_CMD_RS (1u << 3)  /* Report Status */
#define HYPE_E1000_TXD_STA_DD (1u << 0)  /* Descriptor Done */

/* Legacy RX descriptor (manual 3.2.3), 16 bytes. */
#define HYPE_E1000_RXD_STA_DD (1u << 0)  /* Descriptor Done */
#define HYPE_E1000_RXD_STA_EOP (1u << 1) /* End Of Packet */

#define HYPE_E1000_DESC_BYTES 16u

/* Ring sizes. A compile-time constant on purpose (#80): not a config key until something needs
 * one. Must be a multiple of 8 -- the hardware requires the ring length in bytes to be
 * 128-byte aligned, and 16-byte descriptors make that 8 descriptors. */
#define HYPE_E1000_RX_RING 32u
#define HYPE_E1000_TX_RING 32u
#define HYPE_E1000_BUF_BYTES 2048u

/* A decoded MAC address. */
typedef struct {
    uint8_t addr[6];
    int valid;
} hype_e1000_mac_t;

/*
 * Decode a MAC from RAL/RAH (register order, little-endian within each word).
 *
 * RAL holds bytes 0..3 and RAH bytes 4..5, with RAH bit 31 (AV, Address Valid) saying whether the
 * hardware considers the entry populated. A NIC whose EEPROM was never programmed reads back
 * all-zero or all-ones, and both are refused rather than used -- sending frames from 00:00:00:00:
 * 00:00 produces a network fault that looks like a switch problem.
 */
void hype_e1000_mac_from_ral_rah(uint32_t ral, uint32_t rah, hype_e1000_mac_t *out);

/*
 * Decode a MAC from the first three EEPROM words (offsets 0x00..0x02), which is where it lives
 * when RAL/RAH are not pre-populated. Each word holds two bytes, LOW byte first.
 */
void hype_e1000_mac_from_eeprom(uint16_t w0, uint16_t w1, uint16_t w2, hype_e1000_mac_t *out);

/*
 * Ring arithmetic. `size` is the descriptor count.
 *
 * The tail is the driver's; the head is the hardware's. A ring is FULL when advancing the tail
 * would make it equal the head, because tail == head is how "empty" is encoded -- so one slot is
 * always left unused. Getting this wrong produces a ring that silently drops the frame it thinks
 * it queued.
 */
unsigned int hype_e1000_ring_next(unsigned int index, unsigned int size);
int hype_e1000_ring_full(unsigned int head, unsigned int tail, unsigned int size);
unsigned int hype_e1000_ring_used(unsigned int head, unsigned int tail, unsigned int size);

/* Is this descriptor's status byte the hardware saying "done with it"? */
int hype_e1000_txd_done(uint8_t status);
int hype_e1000_rxd_done(uint8_t status);

/*
 * The value to program into RDLEN/TDLEN: the ring length in BYTES. Returns 0 for a descriptor
 * count the hardware cannot express (the length must be a multiple of 128 bytes), which the
 * caller must treat as a programming error rather than writing a 0-length ring.
 */
uint32_t hype_e1000_ring_len_bytes(unsigned int descriptors);

#endif /* HYPE_CORE_E1000_H */
