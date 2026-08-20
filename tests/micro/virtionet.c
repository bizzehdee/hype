/*
 * NET-2 (#81): the guest virtio-net adapter, driven by a real guest-side driver.
 *
 * THIS TEST DISCOVERS THE DEVICE'S MMIO REGIONS BY WALKING THE PCI CAPABILITY CHAIN, and that is
 * the point of it rather than an incidental detail. tests/micro/virtioblk.c could not: it names the
 * assumption in its own header comment -- "hype's virtio-blk publishes NO virtio PCI capability
 * chain ... nothing builds them" -- and uses the fixed BAR offsets instead (#550, spun off as
 * #569). A real Linux or FreeBSD virtio driver walks the chain, so a device that has none is a
 * device those drivers cannot bind. virtio-net publishes one, and this test proves it by finding
 * every region through it and refusing to fall back to a fixed offset.
 *
 * THE DRIVER, in spec order (3.1.1 device initialisation):
 *
 *   find the device by CLASS CODE (network/ethernet), not by slot number
 *   walk the capability chain for the common-cfg / notify / ISR / device-cfg regions
 *   place BAR4 itself, enable memory decoding and bus mastering
 *   reset, then ACKNOWLEDGE | DRIVER
 *   negotiate: require VIRTIO_F_VERSION_1 and VIRTIO_NET_F_MAC, offer nothing else
 *   FEATURES_OK, read back -- a device that clears it has rejected the driver
 *   read the MAC out of device config
 *   set up BOTH virtqueues, receive (0) and transmit (1), then DRIVER_OK
 *   transmit one frame and check the descriptor comes back on the used ring
 *
 * WHY BOTH QUEUES MATTER. virtio-blk here is single-queue, so its queue registers could ignore
 * queue_select. A NIC cannot, and each queue also gets its own notify slot. This test programs both
 * and reads back each one's own addresses, because a device that aliased them would look perfectly
 * healthy until traffic went the other way.
 *
 * WHAT IT DOES NOT CLAIM. Nothing is on the wire yet: the forwarding plane is #83 (NAT) and #85
 * (peer rules), and until one exists hype counts an outbound frame and drops it. So this test
 * proves the DEVICE -- discovery, negotiation, both rings, a completed transmit -- and says so,
 * rather than implying the guest reached a network.
 */
#include "micro_virtio.h"

#define NAME "virtionet"

#define VIRTIO_VENDOR_ID 0x1AF4u
#define VIRTIO_NET_DEVICE_ID 0x1041u
/*
 * class(23:16) | subclass(15:8) | prog-IF(7:0), which is what micro_pci_fclass() returns -- it
 * has already shifted the revision byte off. Compared as one 24-bit value, matching micro_ahci.h
 * and bochsvbe.c. My first cut compared `(cls >> 8) & 0xFF` against 0x02 and so tested the
 * SUBCLASS against the class number: the device was present and correctly presented, and the scan
 * walked straight past it.
 */
#define PCI_CLASS_ETHERNET 0x020000u

/* Device status bits (spec 2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01u
#define VIRTIO_STATUS_DRIVER 0x02u
#define VIRTIO_STATUS_DRIVER_OK 0x04u
#define VIRTIO_STATUS_FEATURES_OK 0x08u

#define VIRTIO_F_VERSION_1_BIT 32u
#define VIRTIO_NET_F_MAC_BIT 5u

/* common-cfg register offsets (spec 4.1.4.3). */
#define CFG_DEVICE_FEATURE_SELECT 0x00u
#define CFG_DEVICE_FEATURE 0x04u
#define CFG_DRIVER_FEATURE_SELECT 0x08u
#define CFG_DRIVER_FEATURE 0x0Cu
#define CFG_NUM_QUEUES 0x12u
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

#define VIRTQ_DESC_F_NEXT 0x0001u
#define VIRTQ_DESC_F_WRITE 0x0002u

#define VQ_RX 0u
#define VQ_TX 1u
#define QUEUE_SIZE 8u

/* virtio-net's buffer header is 12 bytes once VERSION_1 is negotiated: num_buffers is present
 * whenever VERSION_1 is set, not only under MRG_RXBUF (spec 5.1.6). */
#define NET_HDR_LEN 12u

#define BAR_GPA 0xC0000000ull

/* Guest-physical scratch, clear of the payload at 16 MB and of the BAR window at 3 GB. */
#define TX_DESC_GPA 0x620000ull
#define TX_AVAIL_GPA 0x621000ull
#define TX_USED_GPA 0x622000ull
#define RX_DESC_GPA 0x623000ull
#define RX_AVAIL_GPA 0x624000ull
#define RX_USED_GPA 0x625000ull
#define TX_FRAME_GPA 0x626000ull
#define RX_BUF_GPA 0x627000ull

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

static volatile uint8_t *g_bar;

/* Region offsets, DISCOVERED rather than assumed. 0xFFFFFFFF means "not found in the chain". */
#define NOT_FOUND MICRO_VIRTIO_NOT_FOUND
static uint32_t g_common_off = NOT_FOUND;
static uint32_t g_notify_off = NOT_FOUND;
static uint32_t g_isr_off = NOT_FOUND;
static uint32_t g_device_off = NOT_FOUND;
static uint32_t g_notify_mult;
static unsigned g_dev;

/*
 * Explicit MOV forms rather than plain volatile stores, for the reason virtioblk.c records: hype
 * decodes the faulting instruction and supports a specific set of forms, and at -O2 clang folded
 * one C store into a form the virtio path refused. Writing the instruction out means a failure here
 * is the device's fault rather than codegen's.
 */
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
static uint32_t mmio_r32(uint32_t off) {
    uint32_t v;
    __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"(g_bar + off) : "memory");
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

/* Find the NIC by class code. Naming a slot number would make this test pass or fail on hype's
 * slot map rather than on whether a guest can find the device the way a real driver does -- and it
 * is how tests/micro/ahci.c came to look for the wrong controller. */
static int find_nic(void) {
    unsigned dev;
    unsigned func;

    for (dev = 0; dev < 32u; dev++) {
        for (func = 0; func < 8u; func++) {
            uint32_t cls;
            uint32_t id;
            if (!micro_pci_fpresent(dev, func)) {
                continue;
            }
            cls = micro_pci_fclass(dev, func);
            if (cls != PCI_CLASS_ETHERNET) {
                continue;
            }
            id = micro_pci_fread32(dev, func, MICRO_PCI_VENDOR_ID);
            if ((id & 0xFFFFu) != VIRTIO_VENDOR_ID) {
                continue;
            }
            if (func != 0u) {
                continue; /* this device is a single-function one; a match elsewhere is not it */
            }
            micro_puts("micro/" NAME ": network device at PCI dev ");
            micro_put_uint(dev);
            micro_puts(", id ");
            micro_put_hex((id >> 16) & 0xFFFFu);
            micro_puts("\n");
            g_dev = dev;
            return (int)dev;
        }
    }
    return -1;
}

/*
 * #569: the walk itself now lives in micro_virtio.h, shared with virtioblk.c. This keeps the
 * file-local offsets the rest of the test reads so those ~50 call sites are untouched, and gains
 * the property that matters: ONE implementation of the bus walk, so the two virtio tests cannot
 * drift on what "discovered the regions" means.
 */
static int walk_caps(unsigned dev) {
    micro_virtio_caps_t caps;

    if (micro_virtio_walk_caps(dev, 4u, NAME, &caps) != 0) {
        return -1; /* already reported which part of the chain failed */
    }
    g_common_off = caps.common_off;
    g_notify_off = caps.notify_off;
    g_isr_off = caps.isr_off;
    g_device_off = caps.device_off;
    g_notify_mult = caps.notify_mult;
    return 0;
}

static void queue_setup(unsigned q, uint64_t desc, uint64_t avail, uint64_t used) {
    mmio_w16(g_common_off + CFG_QUEUE_SELECT, (uint16_t)q);
    mmio_w16(g_common_off + CFG_QUEUE_SIZE, (uint16_t)QUEUE_SIZE);
    mmio_w32(g_common_off + CFG_QUEUE_DESC_LO, (uint32_t)desc);
    mmio_w32(g_common_off + CFG_QUEUE_DESC_HI, (uint32_t)(desc >> 32));
    mmio_w32(g_common_off + CFG_QUEUE_DRIVER_LO, (uint32_t)avail);
    mmio_w32(g_common_off + CFG_QUEUE_DRIVER_HI, (uint32_t)(avail >> 32));
    mmio_w32(g_common_off + CFG_QUEUE_DEVICE_LO, (uint32_t)used);
    mmio_w32(g_common_off + CFG_QUEUE_DEVICE_HI, (uint32_t)(used >> 32));
    mmio_w16(g_common_off + CFG_QUEUE_ENABLE, 1u);
}

static int check_queue_readback(unsigned q, uint64_t desc, uint64_t avail, uint64_t used) {
    uint32_t d;

    mmio_w16(g_common_off + CFG_QUEUE_SELECT, (uint16_t)q);
    d = mmio_r32(g_common_off + CFG_QUEUE_DESC_LO);
    if (d != (uint32_t)desc) {
        micro_puts("micro/" NAME ": queue ");
        micro_put_uint(q);
        micro_puts(" desc read back ");
        micro_put_hex(d);
        micro_puts(" want ");
        micro_put_hex((uint32_t)desc);
        micro_puts("\n");
        return -1;
    }
    if (mmio_r32(g_common_off + CFG_QUEUE_DRIVER_LO) != (uint32_t)avail) {
        return -1;
    }
    if (mmio_r32(g_common_off + CFG_QUEUE_DEVICE_LO) != (uint32_t)used) {
        return -1;
    }
    if (mmio_r16(g_common_off + CFG_QUEUE_ENABLE) != 1u) {
        return -1;
    }
    return 0;
}

void micro_main(void) {
    int dev;
    uint32_t bar_size;
    uint64_t features_lo;
    uint64_t features_hi;
    uint8_t status;
    uint8_t mac[6];
    uint16_t nq;
    uint16_t rx_notify;
    uint16_t tx_notify;
    unsigned i;
    virtq_desc_t *tx_desc;
    volatile uint16_t *tx_avail;
    volatile uint16_t *tx_used;
    virtq_desc_t *rx_desc;
    volatile uint16_t *rx_avail;
    uint8_t *frame;

    micro_puts("micro/" NAME ": start\n");

    dev = find_nic();
    if (dev < 0) {
        micro_fail(NAME, "no virtio network device on the PCI bus -- the VM's config needs "
                         "`net_mode = nat`, and without it hype presents no NIC at all (#81)");
        micro_halt();
    }
    if ((micro_pci_read32((unsigned)dev, MICRO_PCI_VENDOR_ID) >> 16) != VIRTIO_NET_DEVICE_ID) {
        micro_fail(NAME, "the network device is not the modern virtio-net ID 0x1041 -- a legacy or "
                         "transitional ID would mean a different register layout entirely");
        micro_halt();
    }

    bar_size = micro_pci_bar_size((unsigned)dev, 4u);
    micro_puts("micro/" NAME ": BAR4 size ");
    micro_put_hex(bar_size);
    micro_puts("\n");
    if (bar_size == 0u) {
        micro_fail(NAME, "BAR4 is unimplemented, so there is nowhere to map the device's registers");
        micro_halt();
    }

    g_bar = (volatile uint8_t *)(uintptr_t)micro_pci_place_bar((unsigned)dev, 4u, BAR_GPA);

    if (walk_caps((unsigned)dev) != 0) {
        micro_halt(); /* walk_caps has already said which part of the chain failed */
    }

    /* Reset, then announce the driver. A device that does not clear its status on a 0 write has not
     * reset, and everything after this would be negotiating against leftovers. */
    mmio_w8(g_common_off + CFG_DEVICE_STATUS, 0u);
    status = mmio_r8(g_common_off + CFG_DEVICE_STATUS);
    if (status != 0u) {
        /* Print what was actually read. "did not clear" without the value is the #572 shape: it
         * names a conclusion and withholds the evidence, and 0xFF (a read that reached nothing)
         * means something completely different from 0x01 (a status that survived). */
        micro_puts("micro/" NAME ": device_status after reset reads ");
        micro_put_hex(status);
        micro_puts("\n");
        micro_fail(NAME, "device_status did not clear on reset -- the device kept state a driver is "
                         "entitled to assume is gone (0xff here would mean the read reached no "
                         "device at all, which is a different fault)");
        micro_halt();
    }
    mmio_w8(g_common_off + CFG_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_w8(g_common_off + CFG_DEVICE_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* What the device offers. */
    mmio_w32(g_common_off + CFG_DEVICE_FEATURE_SELECT, 0u);
    features_lo = mmio_r32(g_common_off + CFG_DEVICE_FEATURE);
    mmio_w32(g_common_off + CFG_DEVICE_FEATURE_SELECT, 1u);
    features_hi = mmio_r32(g_common_off + CFG_DEVICE_FEATURE);
    micro_puts("micro/" NAME ": device features hi=");
    micro_put_hex((unsigned long long)features_hi);
    micro_puts(" lo=");
    micro_put_hex((unsigned long long)features_lo);
    micro_puts("\n");

    if ((features_hi & (1u << (VIRTIO_F_VERSION_1_BIT - 32u))) == 0u) {
        micro_fail(NAME, "the device does not offer VIRTIO_F_VERSION_1, so its ring layout is the "
                         "legacy one and a modern driver must refuse it");
        micro_halt();
    }
    if ((features_lo & (1u << VIRTIO_NET_F_MAC_BIT)) == 0u) {
        micro_fail(NAME, "the device does not offer VIRTIO_NET_F_MAC, which means the driver is "
                         "entitled to invent its own address -- and hype's forwarding plane "
                         "identifies a guest by exactly that address");
        micro_halt();
    }

    /* Accept exactly those two. */
    mmio_w32(g_common_off + CFG_DRIVER_FEATURE_SELECT, 0u);
    mmio_w32(g_common_off + CFG_DRIVER_FEATURE, 1u << VIRTIO_NET_F_MAC_BIT);
    mmio_w32(g_common_off + CFG_DRIVER_FEATURE_SELECT, 1u);
    mmio_w32(g_common_off + CFG_DRIVER_FEATURE, 1u << (VIRTIO_F_VERSION_1_BIT - 32u));

    /* Both halves must have survived: they are written one at a time, and a device that assigned
     * rather than merged would have just erased the MAC bit. */
    mmio_w32(g_common_off + CFG_DRIVER_FEATURE_SELECT, 0u);
    if (mmio_r32(g_common_off + CFG_DRIVER_FEATURE) != (1u << VIRTIO_NET_F_MAC_BIT)) {
        micro_fail(NAME, "the low feature word did not survive writing the high one -- the device "
                         "is assigning where it must merge, so no driver can negotiate two words");
        micro_halt();
    }

    mmio_w8(g_common_off + CFG_DEVICE_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    status = mmio_r8(g_common_off + CFG_DEVICE_STATUS);
    if ((status & VIRTIO_STATUS_FEATURES_OK) == 0u) {
        micro_fail(NAME, "the device cleared FEATURES_OK -- it rejected the feature set the driver "
                         "accepted, and the spec says stop here");
        micro_halt();
    }

    nq = mmio_r16(g_common_off + CFG_NUM_QUEUES);
    micro_puts("micro/" NAME ": num_queues ");
    micro_put_uint(nq);
    micro_puts("\n");
    if (nq < 2u) {
        micro_fail(NAME, "fewer than 2 virtqueues -- a NIC needs receive AND transmit, and with one "
                         "queue traffic can only ever move in a single direction");
        micro_halt();
    }

    /* The MAC, out of the device-config region the capability chain pointed at. */
    for (i = 0; i < 6u; i++) {
        mac[i] = mmio_r8(g_device_off + i);
    }
    micro_puts("micro/" NAME ": mac ");
    for (i = 0; i < 6u; i++) {
        micro_put_hex(mac[i]);
        micro_puts(i == 5u ? "\n" : ":");
    }
    if (mac[0] == 0u && mac[1] == 0u && mac[2] == 0u && mac[3] == 0u && mac[4] == 0u &&
        mac[5] == 0u) {
        micro_fail(NAME, "the MAC is all zeros, which is not a usable source address -- frames from "
                         "this guest would be discarded by the first thing that saw them");
        micro_halt();
    }
    if ((mac[0] & 0x01u) != 0u) {
        micro_fail(NAME, "the MAC has the group bit set, so it is a multicast address and cannot be "
                         "a unicast sender");
        micro_halt();
    }

    /* Each queue's own doorbell slot. Equal values would mean one shared doorbell. */
    mmio_w16(g_common_off + CFG_QUEUE_SELECT, VQ_RX);
    rx_notify = mmio_r16(g_common_off + CFG_QUEUE_NOTIFY_OFF);
    mmio_w16(g_common_off + CFG_QUEUE_SELECT, VQ_TX);
    tx_notify = mmio_r16(g_common_off + CFG_QUEUE_NOTIFY_OFF);
    micro_puts("micro/" NAME ": notify_off rx=");
    micro_put_uint(rx_notify);
    micro_puts(" tx=");
    micro_put_uint(tx_notify);
    micro_puts(" multiplier=");
    micro_put_uint(g_notify_mult);
    micro_puts("\n");
    if (rx_notify == tx_notify) {
        micro_fail(NAME, "both queues report the same notify_off, so a transmit kick and a receive "
                         "kick land on one address and the device cannot tell them apart");
        micro_halt();
    }

    /* Rings. Zeroed first: a stale used index from a previous run would make an unmoved ring look
     * like a completed transmit, which is the shape of a test that passes without the device doing
     * anything (#535's first run reported PASS from a guest entered at the wrong address). */
    tx_desc = (virtq_desc_t *)(uintptr_t)TX_DESC_GPA;
    tx_avail = (volatile uint16_t *)(uintptr_t)TX_AVAIL_GPA;
    tx_used = (volatile uint16_t *)(uintptr_t)TX_USED_GPA;
    rx_desc = (virtq_desc_t *)(uintptr_t)RX_DESC_GPA;
    rx_avail = (volatile uint16_t *)(uintptr_t)RX_AVAIL_GPA;
    frame = (uint8_t *)(uintptr_t)TX_FRAME_GPA;

    for (i = 0; i < QUEUE_SIZE * sizeof(virtq_desc_t); i++) {
        ((uint8_t *)tx_desc)[i] = 0;
        ((uint8_t *)rx_desc)[i] = 0;
    }
    for (i = 0; i < 4u + 2u * QUEUE_SIZE + 2u; i++) {
        ((volatile uint8_t *)tx_avail)[i] = 0;
        ((volatile uint8_t *)rx_avail)[i] = 0;
    }
    for (i = 0; i < 4u + 8u * QUEUE_SIZE + 2u; i++) {
        ((volatile uint8_t *)tx_used)[i] = 0;
        ((volatile uint8_t *)(uintptr_t)RX_USED_GPA)[i] = 0;
    }

    queue_setup(VQ_RX, RX_DESC_GPA, RX_AVAIL_GPA, RX_USED_GPA);
    queue_setup(VQ_TX, TX_DESC_GPA, TX_AVAIL_GPA, TX_USED_GPA);

    /* Each queue must have kept its OWN addresses. A device that ignored queue_select would have
     * both pointing at whichever was programmed last, and would look healthy until traffic went
     * the other way. */
    if (check_queue_readback(VQ_RX, RX_DESC_GPA, RX_AVAIL_GPA, RX_USED_GPA) != 0) {
        micro_fail(NAME, "the receive queue's ring addresses did not read back -- the queue "
                         "registers are not per-queue, so both queues share one ring");
        micro_halt();
    }
    if (check_queue_readback(VQ_TX, TX_DESC_GPA, TX_AVAIL_GPA, TX_USED_GPA) != 0) {
        micro_fail(NAME, "the transmit queue's ring addresses did not read back -- see the receive "
                         "queue message; the two queues are aliased");
        micro_halt();
    }

    mmio_w8(g_common_off + CFG_DEVICE_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK |
                VIRTIO_STATUS_DRIVER_OK);

    /* Post one receive buffer, as a driver does before it expects anything. Nothing should complete
     * it: no frame can arrive while there is no forwarding plane, and a used entry appearing here
     * would mean the device completed a receive descriptor it had nothing to put in. */
    rx_desc[0].addr = RX_BUF_GPA;
    rx_desc[0].len = 2048u;
    rx_desc[0].flags = VIRTQ_DESC_F_WRITE;
    rx_desc[0].next = 0u;
    rx_avail[2] = 0u;      /* ring[0] = descriptor 0 */
    rx_avail[1] = 1u;      /* idx */

    /* One frame: a 12-byte virtio-net header the device must SKIP, then an Ethernet frame. The
     * header bytes are 0xEE so that a device including them in the payload would be visible. */
    for (i = 0; i < NET_HDR_LEN; i++) {
        frame[i] = 0xEEu;
    }
    /* Destination: broadcast. Source: our own MAC. EtherType 0x0806 is deliberately NOT ARP -- this
     * frame is never meant to be interpreted, only carried, and using a real ARP opcode would
     * invite a future reader to expect a reply. */
    for (i = 0; i < 6u; i++) {
        frame[NET_HDR_LEN + i] = 0xFFu;
        frame[NET_HDR_LEN + 6u + i] = mac[i];
    }
    frame[NET_HDR_LEN + 12u] = 0x08u;
    frame[NET_HDR_LEN + 13u] = 0x06u;
    for (i = 0; i < 46u; i++) {
        frame[NET_HDR_LEN + 14u + i] = (uint8_t)(0x30u + i);
    }

    tx_desc[0].addr = TX_FRAME_GPA;
    tx_desc[0].len = NET_HDR_LEN + 60u;
    tx_desc[0].flags = 0u;
    tx_desc[0].next = 0u;
    tx_avail[2] = 0u;
    tx_avail[1] = 1u;

    /* Ring the TRANSMIT doorbell specifically: notify_off scaled by the multiplier the capability
     * advertised. Using the receive slot here would drain nothing and the test would report a
     * device that never completes -- which is why the two offsets were checked as distinct above. */
    mmio_w32(g_notify_off + (uint32_t)tx_notify * g_notify_mult, VQ_TX);

    if (tx_used[1] != 1u) {
        micro_puts("micro/" NAME ": tx used idx ");
        micro_put_uint(tx_used[1]);
        micro_puts("\n");
        micro_fail(NAME, "the transmit descriptor was not completed -- the device took the doorbell "
                         "and left the driver's descriptor in the ring, which stalls the transmit "
                         "queue for good");
        micro_halt();
    }
    if (*(volatile uint32_t *)(uintptr_t)(TX_USED_GPA + 4u) != 0u) {
        micro_fail(NAME, "the completed used element names the wrong descriptor");
        micro_halt();
    }

    /* A completed transmit raises the queue interrupt, and the ISR register is read-to-clear. */
    {
        uint8_t isr = mmio_r8(g_isr_off);
        if ((isr & 0x1u) == 0u) {
            micro_fail(NAME, "no queue interrupt after the transmit completed -- a real driver waits "
                             "on this and would never reclaim the descriptor");
            micro_halt();
        }
        if ((mmio_r8(g_isr_off) & 0x1u) != 0u) {
            micro_fail(NAME, "the ISR register is not read-to-clear, so a driver would see one "
                             "interrupt forever");
            micro_halt();
        }
    }

    /* And nothing completed a receive descriptor, because nothing could have. */
    if (*(volatile uint16_t *)(uintptr_t)(RX_USED_GPA + 2u) != 0u) {
        micro_fail(NAME, "a receive descriptor was completed with no frame to deliver -- the device "
                         "handed the driver a buffer it never filled");
        micro_halt();
    }

    micro_puts("micro/" NAME ": device found by class, regions discovered through the capability "
               "chain, both queues live, one frame transmitted and completed. No wire yet -- "
               "forwarding is #83/#85.\n");
    micro_pass(NAME);
    micro_halt();
}
