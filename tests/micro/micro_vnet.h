#ifndef HYPE_MICRO_VNET_H
#define HYPE_MICRO_VNET_H

/*
 * A guest-side virtio-net driver, shared by the tests that need a working network rather than just a
 * working device.
 *
 * Extracted from tests/micro/netdns.c when a second network test (netpeer) needed the same 200
 * lines. tests/micro/virtionet.c deliberately does NOT use it: that test's whole job is to check the
 * bring-up itself -- capability chain, per-queue registers, distinct doorbells -- and a test that
 * shares its subject's implementation cannot find a fault in it. This header is for tests that take
 * bring-up as given and are about traffic.
 *
 * Include micro_pci.h before this.
 */

#include "micro_pci.h"
#include "micro_netframes.h"

#define VIRTIO_VENDOR_ID 0x1AF4u
#define VIRTIO_NET_DEVICE_ID 0x1041u
#define PCI_CLASS_ETHERNET 0x020000u

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01u
#define VIRTIO_STATUS_DRIVER 0x02u
#define VIRTIO_STATUS_DRIVER_OK 0x04u
#define VIRTIO_STATUS_FEATURES_OK 0x08u
#define VIRTIO_F_VERSION_1_BIT 32u
#define VIRTIO_NET_F_MAC_BIT 5u

#define CFG_DEVICE_FEATURE_SELECT 0x00u
#define CFG_DEVICE_FEATURE 0x04u
#define CFG_DRIVER_FEATURE_SELECT 0x08u
#define CFG_DRIVER_FEATURE 0x0Cu
#define CFG_DEVICE_STATUS 0x14u
#define CFG_QUEUE_SELECT 0x16u
#define CFG_QUEUE_SIZE 0x18u
#define CFG_QUEUE_ENABLE 0x1Cu
#define CFG_QUEUE_NOTIFY_OFF 0x1Eu
#define CFG_QUEUE_DESC_LO 0x20u
#define CFG_QUEUE_DESC_HI 0x24u
#define CFG_QUEUE_DRIVER_LO 0x28u
#define CFG_QUEUE_DRIVER_HI 0x2Cu
#define CFG_QUEUE_DEVICE_LO 0x30u
#define CFG_QUEUE_DEVICE_HI 0x34u

#define PCI_CAP_POINTER 0x34u
#define PCI_CAP_ID_VENDOR 0x09u

#define VIRTQ_DESC_F_WRITE 0x0002u

#define VQ_RX 0u
#define VQ_TX 1u
#define QUEUE_SIZE 8u
#define NET_HDR_LEN 12u
#define BAR_GPA 0xC0000000ull

#define TX_DESC_GPA 0x640000ull
#define TX_AVAIL_GPA 0x641000ull
#define TX_USED_GPA 0x642000ull
#define RX_DESC_GPA 0x643000ull
#define RX_AVAIL_GPA 0x644000ull
#define RX_USED_GPA 0x645000ull
#define TX_BUF_GPA 0x646000ull
#define RX_BUF_BASE 0x648000ull
#define RX_BUF_STRIDE 0x1000ull

/* Our own addressing. hype is never told these -- see the header comment on proxy ARP. */
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

static volatile uint8_t *g_bar;
static uint32_t g_common_off = 0xFFFFFFFFu;
static uint32_t g_notify_off = 0xFFFFFFFFu;
static uint32_t g_isr_off = 0xFFFFFFFFu;
static uint32_t g_device_off = 0xFFFFFFFFu;
static uint32_t g_notify_mult;
static uint16_t g_tx_notify;

/* Explicit MOV forms: hype decodes the faulting instruction and supports a specific set, and at -O2
 * clang folds a plain volatile store into forms it refuses (see virtioblk.c and #575). */
static uint8_t mmio_r8(uint32_t off) {
    uint8_t v;
    __asm__ volatile("movb (%1), %0" : "=q"(v) : "r"(g_bar + off) : "memory");
    return v;
}
static uint16_t mmio_r16(uint32_t off) {
    uint16_t v;
    __asm__ volatile("movw (%1), %0" : "=r"(v) : "r"(g_bar + off) : "memory");
    return v;
}
static void mmio_w8(uint32_t off, uint8_t v) {
    __asm__ volatile("movb %0, (%1)" : : "q"(v), "r"(g_bar + off) : "memory");
}
static void mmio_w16(uint32_t off, uint16_t v) {
    __asm__ volatile("movw %0, (%1)" : : "r"(v), "r"(g_bar + off) : "memory");
}
static void mmio_w32(uint32_t off, uint32_t v) {
    __asm__ volatile("movl %0, (%1)" : : "r"(v), "r"(g_bar + off) : "memory");
}

static int find_and_bring_up(void) {
    unsigned dev;
    unsigned func;
    int found = -1;
    unsigned cap;
    unsigned guard = 0;

    for (dev = 0; dev < 32u && found < 0; dev++) {
        for (func = 0; func < 8u; func++) {
            uint32_t id;
            if (!micro_pci_fpresent(dev, func) ||
                micro_pci_fclass(dev, func) != PCI_CLASS_ETHERNET) {
                continue;
            }
            id = micro_pci_fread32(dev, func, MICRO_PCI_VENDOR_ID);
            if ((id & 0xFFFFu) != VIRTIO_VENDOR_ID ||
                ((id >> 16) & 0xFFFFu) != VIRTIO_NET_DEVICE_ID || func != 0u) {
                continue;
            }
            found = (int)dev;
            break;
        }
    }
    if (found < 0) {
        micro_fail(NAME, "no virtio-net device -- the VM's config needs `net_mode = nat`");
        return -1;
    }
    g_bar = (volatile uint8_t *)(uintptr_t)micro_pci_place_bar((unsigned)found, 4u, BAR_GPA);

    cap = micro_pci_read32((unsigned)found, PCI_CAP_POINTER) & 0xFFu;
    while (cap >= 0x40u && cap < 0x100u && guard++ < 48u) {
        uint32_t w0 = micro_pci_read32((unsigned)found, cap);
        uint32_t off = micro_pci_read32((unsigned)found, cap + 8u);
        unsigned next = (w0 >> 8) & 0xFFu;
        if ((w0 & 0xFFu) == PCI_CAP_ID_VENDOR) {
            unsigned type = (w0 >> 24) & 0xFFu;
            if (type == 1u) {
                g_common_off = off;
            } else if (type == 2u) {
                g_notify_off = off;
                g_notify_mult = micro_pci_read32((unsigned)found, cap + 16u);
            } else if (type == 3u) {
                g_isr_off = off;
            } else if (type == 4u) {
                g_device_off = off;
            }
        }
        if (next == 0u || next == cap) {
            break;
        }
        cap = next;
    }
    if (g_common_off == 0xFFFFFFFFu || g_notify_off == 0xFFFFFFFFu || g_isr_off == 0xFFFFFFFFu ||
        g_device_off == 0xFFFFFFFFu) {
        micro_fail(NAME, "the virtio capability chain is incomplete -- see the virtionet test, "
                         "which checks the chain specifically");
        return -1;
    }

    /* Reset, negotiate VERSION_1 + F_MAC, read the MAC. */
    mmio_w8(g_common_off + CFG_DEVICE_STATUS, 0u);
    mmio_w8(g_common_off + CFG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_w8(g_common_off + CFG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    mmio_w32(g_common_off + CFG_DRIVER_FEATURE_SELECT, 0u);
    mmio_w32(g_common_off + CFG_DRIVER_FEATURE, 1u << VIRTIO_NET_F_MAC_BIT);
    mmio_w32(g_common_off + CFG_DRIVER_FEATURE_SELECT, 1u);
    mmio_w32(g_common_off + CFG_DRIVER_FEATURE, 1u << (VIRTIO_F_VERSION_1_BIT - 32u));
    mmio_w8(g_common_off + CFG_DEVICE_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    if ((mmio_r8(g_common_off + CFG_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0u) {
        micro_fail(NAME, "the device rejected the feature set");
        return -1;
    }
    {
        unsigned int i;
        for (i = 0; i < 6u; i++) {
            g_mac[i] = mmio_r8(g_device_off + i);
        }
    }

    /* Both rings. */
    {
        unsigned int q;
        const uint64_t desc[2] = {RX_DESC_GPA, TX_DESC_GPA};
        const uint64_t avail[2] = {RX_AVAIL_GPA, TX_AVAIL_GPA};
        const uint64_t used[2] = {RX_USED_GPA, TX_USED_GPA};
        unsigned int b;

        for (q = 0; q < 2u; q++) {
            for (b = 0; b < QUEUE_SIZE * sizeof(virtq_desc_t); b++) {
                *(volatile uint8_t *)(uintptr_t)(desc[q] + b) = 0;
            }
            for (b = 0; b < 4u + 2u * QUEUE_SIZE + 2u; b++) {
                *(volatile uint8_t *)(uintptr_t)(avail[q] + b) = 0;
            }
            for (b = 0; b < 4u + 8u * QUEUE_SIZE + 2u; b++) {
                *(volatile uint8_t *)(uintptr_t)(used[q] + b) = 0;
            }
            mmio_w16(g_common_off + CFG_QUEUE_SELECT, (uint16_t)q);
            mmio_w16(g_common_off + CFG_QUEUE_SIZE, (uint16_t)QUEUE_SIZE);
            mmio_w32(g_common_off + CFG_QUEUE_DESC_LO, (uint32_t)desc[q]);
            mmio_w32(g_common_off + CFG_QUEUE_DESC_HI, 0u);
            mmio_w32(g_common_off + CFG_QUEUE_DRIVER_LO, (uint32_t)avail[q]);
            mmio_w32(g_common_off + CFG_QUEUE_DRIVER_HI, 0u);
            mmio_w32(g_common_off + CFG_QUEUE_DEVICE_LO, (uint32_t)used[q]);
            mmio_w32(g_common_off + CFG_QUEUE_DEVICE_HI, 0u);
            mmio_w16(g_common_off + CFG_QUEUE_ENABLE, 1u);
        }
        mmio_w16(g_common_off + CFG_QUEUE_SELECT, VQ_TX);
        g_tx_notify = mmio_r16(g_common_off + CFG_QUEUE_NOTIFY_OFF);
    }

    /* Post every receive buffer BEFORE DRIVER_OK, so nothing can arrive with no place to go. */
    {
        unsigned int i;
        virtq_desc_t *rd = (virtq_desc_t *)(uintptr_t)RX_DESC_GPA;
        volatile uint16_t *ra = (volatile uint16_t *)(uintptr_t)RX_AVAIL_GPA;
        for (i = 0; i < QUEUE_SIZE; i++) {
            rd[i].addr = RX_BUF_BASE + (uint64_t)i * RX_BUF_STRIDE;
            rd[i].len = 2048u;
            rd[i].flags = VIRTQ_DESC_F_WRITE;
            rd[i].next = 0u;
            ra[2 + i] = (uint16_t)i;
        }
        ra[1] = (uint16_t)QUEUE_SIZE;
    }
    mmio_w8(g_common_off + CFG_DEVICE_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK |
                VIRTIO_STATUS_DRIVER_OK);
    return 0;
}

/* Puts one frame on the transmit ring and rings the transmit doorbell. */
/*
 * THE AVAILABLE INDEX IS MONOTONIC AND THE DESCRIPTOR SLOT WRAPS. Those are two different counters
 * and conflating them is a bug I shipped once: a caller passing `slot % QUEUE_SIZE` made this write
 * `avail->idx = (slot % 8) + 1`, so after eight frames the index went BACKWARDS to 1 and hype's ring
 * walker -- which advances its own last_avail_idx monotonically -- saw the index disagree and
 * reprocessed stale entries forever. 40 frames sent, 327,680 processed.
 *
 * The device tracks its position with a 16-bit counter that wraps naturally at 65536, so the driver
 * must too: publish an ever-increasing index and let it wrap on its own. `g_tx_avail` is that
 * counter, kept here rather than in the caller so a caller cannot get it wrong again.
 */
static uint16_t g_tx_avail;

static void send_frame(const uint8_t *payload, unsigned int payload_len) {
    virtq_desc_t *td = (virtq_desc_t *)(uintptr_t)TX_DESC_GPA;
    volatile uint16_t *ta = (volatile uint16_t *)(uintptr_t)TX_AVAIL_GPA;
    unsigned int slot = (unsigned int)(g_tx_avail % QUEUE_SIZE);
    uint8_t *buf = (uint8_t *)(uintptr_t)(TX_BUF_GPA + (uint64_t)slot * 0x800ull);
    unsigned int i;

    for (i = 0; i < NET_HDR_LEN; i++) {
        buf[i] = 0;
    }
    for (i = 0; i < payload_len; i++) {
        buf[NET_HDR_LEN + i] = payload[i];
    }
    td[slot].addr = TX_BUF_GPA + (uint64_t)slot * 0x800ull;
    td[slot].len = NET_HDR_LEN + payload_len;
    td[slot].flags = 0u;
    td[slot].next = 0u;
    ta[2 + slot] = (uint16_t)slot;
    /* The ring entry is written BEFORE the index that makes it visible -- the device reads the index
     * to decide the entry is valid, and the other order hands it an entry not yet written. */
    g_tx_avail++;
    ta[1] = g_tx_avail;
    mmio_w32(g_notify_off + (uint32_t)g_tx_notify * g_notify_mult, VQ_TX);
}




#endif /* HYPE_MICRO_VNET_H */
