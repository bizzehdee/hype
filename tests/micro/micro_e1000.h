#ifndef HYPE_MICRO_E1000_H
#define HYPE_MICRO_E1000_H

/*
 * A guest-side e1000 driver, for the tests that exercise hype's e1000 frontend (#82).
 *
 * The counterpart of micro_vnet.h, and deliberately a SEPARATE file rather than a mode switch inside
 * it: the two devices share nothing but the frames that pass through them. What they DO share --
 * building an ARP request, a DNS query, an ICMP echo -- already lives in micro_vnet.h and is included
 * from there, so a test can send the same traffic over either NIC and hype's forwarding plane is the
 * only thing that differs. That is the point: the plane must not care.
 *
 * Include micro_pci.h before this.
 */

#include "micro_pci.h"

#define E1000_VENDOR 0x8086u
#define E1000_DEVICE 0x100Eu /* 82540EM */
#define E1000_CLASS_ETHERNET 0x020000u

#define E1000_REG_CTRL 0x0000u
#define E1000_REG_STATUS 0x0008u
#define E1000_REG_EERD 0x0014u
#define E1000_REG_ICR 0x00C0u
#define E1000_REG_IMS 0x00D0u
#define E1000_REG_RCTL 0x0100u
#define E1000_REG_TCTL 0x0400u
#define E1000_REG_RDBAL 0x2800u
#define E1000_REG_RDBAH 0x2804u
#define E1000_REG_RDLEN 0x2808u
#define E1000_REG_RDH 0x2810u
#define E1000_REG_RDT 0x2818u
#define E1000_REG_TDBAL 0x3800u
#define E1000_REG_TDBAH 0x3804u
#define E1000_REG_TDLEN 0x3808u
#define E1000_REG_TDH 0x3810u
#define E1000_REG_TDT 0x3818u
#define E1000_REG_RAL0 0x5400u
#define E1000_REG_RAH0 0x5404u

#define E1000_CTRL_RST (1u << 26)
#define E1000_STATUS_LU (1u << 1)
#define E1000_RCTL_EN (1u << 1)
#define E1000_TCTL_EN (1u << 1)
#define E1000_ICR_TXDW (1u << 0)
#define E1000_ICR_RXT0 (1u << 7)
#define E1000_TXD_CMD_EOP (1u << 0)
#define E1000_TXD_CMD_RS (1u << 3)
#define E1000_TXD_STA_DD (1u << 0)
#define E1000_RXD_STA_DD (1u << 0)

#define E1000_DESC_BYTES 16u
#define E1000_RING 8u
#define E1000_BUF_BYTES 2048u

/* Guest-physical scratch, clear of the payload at 16 MB and of the BAR window at 3 GB. */
#define E1000_BAR_GPA 0xC0000000ull
#define E1000_TX_RING_GPA 0x660000ull
#define E1000_RX_RING_GPA 0x661000ull
#define E1000_TX_BUF_GPA 0x662000ull
#define E1000_RX_BUF_GPA 0x672000ull

static volatile uint8_t *g_e1000_bar;
static uint8_t g_e1000_mac[6];
static uint16_t g_e1000_tdt;
static uint16_t g_e1000_rx_next; /* which descriptor this driver will inspect next */

/* Explicit MOV forms, for the reason virtioblk.c records and #575 filed: at -O2 clang folds a plain
 * volatile access into forms hype's MMIO decoder does not handle. */
static uint32_t e1000_r(uint32_t off) {
    uint32_t v;
    __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"(g_e1000_bar + off) : "memory");
    return v;
}
static void e1000_w(uint32_t off, uint32_t v) {
    __asm__ volatile("movl %0, (%1)" : : "r"(v), "r"(g_e1000_bar + off) : "memory");
}

static void e1000_put64(uint8_t *p, uint64_t v) {
    unsigned int i;
    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}
static void e1000_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static uint16_t e1000_get16(const volatile uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/*
 * Brings the NIC up the way a real driver does, in the manual's own order. Returns 0 on success and
 * has already called micro_fail() on failure.
 */
static int e1000_up(const char *name) {
    unsigned dev;
    unsigned func;
    int found = -1;
    unsigned int i;

    for (dev = 0; dev < 32u && found < 0; dev++) {
        for (func = 0; func < 8u; func++) {
            uint32_t id;
            if (!micro_pci_fpresent(dev, func) ||
                micro_pci_fclass(dev, func) != E1000_CLASS_ETHERNET) {
                continue;
            }
            id = micro_pci_fread32(dev, func, MICRO_PCI_VENDOR_ID);
            if ((id & 0xFFFFu) != E1000_VENDOR || ((id >> 16) & 0xFFFFu) != E1000_DEVICE ||
                func != 0u) {
                continue;
            }
            found = (int)dev;
            break;
        }
    }
    if (found < 0) {
        micro_fail(name, "no 8086:100E on the PCI bus -- the VM needs `net_mode = nat` AND "
                         "`os_hint = windows`, which is what selects the e1000 frontend over "
                         "virtio-net (#82)");
        return -1;
    }
    /* BAR0, not BAR4: the e1000's registers are at an architectural BAR, which is part of why a
     * driver needs no discovery. */
    g_e1000_bar = (volatile uint8_t *)(uintptr_t)micro_pci_place_bar((unsigned)found, 0u,
                                                                    E1000_BAR_GPA);

    e1000_w(E1000_REG_CTRL, E1000_CTRL_RST);
    if ((e1000_r(E1000_REG_STATUS) & E1000_STATUS_LU) == 0u) {
        micro_fail(name, "STATUS.LU is clear, so a real driver would wait for link forever and never "
                         "transmit");
        return -1;
    }

    /* The MAC from RAL/RAH. A driver may instead walk the EEPROM; both must agree, which
     * core/tests/test_e1000_dev.c pins. */
    {
        uint32_t ral = e1000_r(E1000_REG_RAL0);
        uint32_t rah = e1000_r(E1000_REG_RAH0);
        if ((rah & (1u << 31)) == 0u) {
            micro_fail(name, "RAH0.AV is clear, so the address hype reports is not marked valid and "
                             "a driver is entitled to ignore it");
            return -1;
        }
        g_e1000_mac[0] = (uint8_t)(ral & 0xFFu);
        g_e1000_mac[1] = (uint8_t)((ral >> 8) & 0xFFu);
        g_e1000_mac[2] = (uint8_t)((ral >> 16) & 0xFFu);
        g_e1000_mac[3] = (uint8_t)((ral >> 24) & 0xFFu);
        g_e1000_mac[4] = (uint8_t)(rah & 0xFFu);
        g_e1000_mac[5] = (uint8_t)((rah >> 8) & 0xFFu);
    }

    /* Rings, zeroed first so a stale descriptor cannot look like a completion. */
    for (i = 0; i < E1000_RING * E1000_DESC_BYTES; i++) {
        *(volatile uint8_t *)(uintptr_t)(E1000_TX_RING_GPA + i) = 0;
        *(volatile uint8_t *)(uintptr_t)(E1000_RX_RING_GPA + i) = 0;
    }
    /* Receive descriptors point at buffers and are ALL posted: RDT one behind RDH's wrap means the
     * whole ring is available. */
    for (i = 0; i < E1000_RING; i++) {
        uint8_t *d = (uint8_t *)(uintptr_t)(E1000_RX_RING_GPA + i * E1000_DESC_BYTES);
        e1000_put64(d, E1000_RX_BUF_GPA + (uint64_t)i * E1000_BUF_BYTES);
        e1000_put16(d + 8, 0u);
        d[12] = 0;
    }

    e1000_w(E1000_REG_RDBAL, (uint32_t)E1000_RX_RING_GPA);
    e1000_w(E1000_REG_RDBAH, 0u);
    e1000_w(E1000_REG_RDLEN, E1000_RING * E1000_DESC_BYTES);
    e1000_w(E1000_REG_RDH, 0u);
    e1000_w(E1000_REG_RDT, E1000_RING - 1u);
    e1000_w(E1000_REG_TDBAL, (uint32_t)E1000_TX_RING_GPA);
    e1000_w(E1000_REG_TDBAH, 0u);
    e1000_w(E1000_REG_TDLEN, E1000_RING * E1000_DESC_BYTES);
    e1000_w(E1000_REG_TDH, 0u);
    e1000_w(E1000_REG_TDT, 0u);
    e1000_w(E1000_REG_IMS, E1000_ICR_TXDW | E1000_ICR_RXT0);
    /* Enable last, as the manual's init sequence has it: a receiver enabled before its ring is
     * addressed would be a receiver pointed at nothing. */
    e1000_w(E1000_REG_RCTL, E1000_RCTL_EN);
    e1000_w(E1000_REG_TCTL, E1000_TCTL_EN);

    g_e1000_tdt = 0u;
    g_e1000_rx_next = 0u;
    return 0;
}

/*
 * Transmits one frame. The TAIL write is the doorbell -- there is no separate notify register, which
 * is the main structural difference from virtio and the thing hype has to get right.
 */
static void e1000_send(const uint8_t *frame, unsigned int len) {
    unsigned int slot = g_e1000_tdt % E1000_RING;
    uint8_t *d = (uint8_t *)(uintptr_t)(E1000_TX_RING_GPA + slot * E1000_DESC_BYTES);
    uint8_t *buf = (uint8_t *)(uintptr_t)(E1000_TX_BUF_GPA + (uint64_t)slot * E1000_BUF_BYTES);
    unsigned int i;

    for (i = 0; i < len; i++) {
        buf[i] = frame[i];
    }
    e1000_put64(d, E1000_TX_BUF_GPA + (uint64_t)slot * E1000_BUF_BYTES);
    e1000_put16(d + 8, (uint16_t)len);
    d[11] = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
    d[12] = 0; /* clear DD so a stale one cannot read as this frame's completion */
    g_e1000_tdt = (uint16_t)((g_e1000_tdt + 1u) % E1000_RING);
    e1000_w(E1000_REG_TDT, g_e1000_tdt);
}

/*
 * Returns the next received frame, or 0 if none. `*out_len` is its length.
 *
 * Descriptors are walked in order and recycled by advancing RDT, which is what keeps the ring from
 * running dry after E1000_RING frames.
 */
static const uint8_t *e1000_recv(unsigned int *out_len) {
    volatile uint8_t *d =
        (volatile uint8_t *)(uintptr_t)(E1000_RX_RING_GPA + g_e1000_rx_next * E1000_DESC_BYTES);

    if ((d[12] & E1000_RXD_STA_DD) == 0u) {
        return 0;
    }
    *out_len = e1000_get16(d + 8);
    {
        const uint8_t *buf =
            (const uint8_t *)(uintptr_t)(E1000_RX_BUF_GPA + (uint64_t)g_e1000_rx_next *
                                                                E1000_BUF_BYTES);
        d[12] = 0; /* hand the descriptor back */
        e1000_w(E1000_REG_RDT, g_e1000_rx_next);
        g_e1000_rx_next = (uint16_t)((g_e1000_rx_next + 1u) % E1000_RING);
        return buf;
    }
}

#endif /* HYPE_MICRO_E1000_H */
