#ifndef HYPE_DEVICES_VIRTIO_NET_H
#define HYPE_DEVICES_VIRTIO_NET_H

#include <stdint.h>

#include "virtio_blk.h"

/*
 * NET-2 (#81): the guest-facing virtio-net device, register plane and negotiation state.
 *
 * This is the DEVICE, not the network. It owns what a driver can see and write: the virtio-pci
 * common configuration, the per-queue addresses a driver publishes, the MAC hype hands the guest,
 * and the ISR byte. Moving frames between these queues and anywhere else is the NAT plane's job
 * (#83) and the peer-forwarding plane's (#85) -- kept out of here so this file stays pure and
 * unit-testable, the same division devices/virtio_blk.c keeps against core/blk_backend.c.
 *
 * WHY THE COMMON-CFG CONSTANTS COME FROM virtio_blk.h. The virtio-pci transport layout
 * (HYPE_VIRTIO_COMMON_CFG_*, HYPE_VIRTIO_F_VERSION_1_BIT, the descriptor flags) is the TRANSPORT
 * and is identical for every virtio device; only the device-type config differs. Redeclaring those
 * offsets here would be two statements of one fact, and the failure they produce when they drift is
 * a driver reading the wrong register with no error anywhere. If a third virtio device arrives, the
 * transport half earns its own header -- two implementations is not yet enough to move it (plan.md
 * decision #17), but three is.
 *
 * TWO QUEUES, WHICH IS THE REAL DIFFERENCE FROM virtio-blk. virtio-blk here is deliberately
 * single-queue, so its queue registers could ignore queue_select for anything but 0. A NIC cannot:
 * receive is queue 0 and transmit is queue 1 (spec 5.1.2), and a driver programs both. Every
 * queue register below is therefore indexed by queue_select, and a select beyond the last queue
 * reads back all-zero -- the spec's own "no such queue" convention.
 */

#define HYPE_VIRTIO_NET_PCI_VENDOR_ID 0x1AF4u
/*
 * 0x1041 = the modern (non-transitional) virtio-net ID: 0x1040 + virtio device type 1. Matches
 * this project's choice for virtio-blk, which uses 0x1042 = 0x1040 + type 2 rather than the
 * legacy 0x1001. A modern ID commits the device to VIRTIO_F_VERSION_1, which is why that bit is
 * the one feature offered below.
 */
#define HYPE_VIRTIO_NET_PCI_DEVICE_ID 0x1041u
#define HYPE_VIRTIO_NET_PCI_CLASS_BASE 0x02u      /* network controller */
#define HYPE_VIRTIO_NET_PCI_CLASS_SUB 0x00u       /* ethernet */
#define HYPE_VIRTIO_NET_PCI_CLASS_INTERFACE 0x00u

/*
 * VIRTIO_NET_F_MAC (spec 5.1.3). Offered because hype assigns the guest's MAC: without this bit a
 * driver is entitled to invent its own address, and the NAT plane's per-guest segment identifies
 * the guest by exactly that address. A guest choosing its own MAC would still work as a network
 * endpoint and would break hype's ability to tell two guests apart.
 */
#define HYPE_VIRTIO_NET_F_MAC_BIT 5u

/*
 * NOT offered, and each for a stated reason rather than by omission:
 *
 *   VIRTIO_NET_F_STATUS (16)  would let hype report carrier down when the host NIC has no link.
 *                             Useful, and deferred: without it a driver assumes the link is always
 *                             up, which is the SAFE default here -- a guest that believes the link
 *                             is down stops transmitting, so a bug in hype's carrier reporting
 *                             would present as a silently dead network.
 *   VIRTIO_NET_F_MRG_RXBUF (15)  receive into several chained buffers. Not needed while the MTU is
 *                             1500 and every receive buffer a driver posts is at least that; it
 *                             matters for large-receive-offload sized frames, which hype does not
 *                             do.
 *   VIRTIO_NET_F_CSUM (0) / GUEST_CSUM (1) / the TSO and UFO bits
 *                             offload. hype would have to compute or verify what it claimed to
 *                             offload, and the NAT plane already has to fix checksums up after
 *                             rewriting addresses (plan.md 6e). Claiming an offload hype does not
 *                             perform hands the guest a frame the wire will reject.
 *   VIRTIO_NET_F_CTRL_VQ (17) the control queue, and with it multiqueue, RX-mode and MAC-filter
 *                             programming. A third queue with a command language is its own slice.
 */

/* Receive is queue 0, transmit is queue 1 (spec 5.1.2). Named rather than numbered at the call
 * sites, because getting these the wrong way round produces a device that looks alive and never
 * moves a packet in one direction. */
#define HYPE_VIRTIO_NET_VQ_RX 0u
#define HYPE_VIRTIO_NET_VQ_TX 1u
#define HYPE_VIRTIO_NET_NUM_QUEUES 2u

#define HYPE_VIRTIO_NET_QUEUE_SIZE_MAX 256u

#define HYPE_VIRTIO_NET_MAC_BYTES 6u

/* Device-type config: just the MAC while F_MAC is the only device feature offered. The spec's
 * layout puts `status` at offset 6 and `max_virtqueue_pairs` at 8; both are absent because their
 * feature bits are not negotiated, so a read there is out of range rather than zero -- an absent
 * field and a field that reads zero are different answers, and `status` reading zero would mean
 * LINK DOWN. */
#define HYPE_VIRTIO_NET_CFG_SIZE HYPE_VIRTIO_NET_MAC_BYTES

/*
 * The virtio-net buffer header that prefixes every frame in both directions (spec 5.1.6.
 * `num_buffers` is present whenever VIRTIO_F_VERSION_1 is negotiated, not only under
 * MRG_RXBUF -- so a modern device's header is 12 bytes, not 10. Getting this wrong shifts every
 * frame by two bytes, which a guest reports as a storm of malformed packets rather than as a
 * header-length disagreement.
 */
#define HYPE_VIRTIO_NET_HDR_LEN_MODERN 12u
#define HYPE_VIRTIO_NET_HDR_LEN_LEGACY 10u

/* Largest Ethernet frame this device carries, header excluded: 1500 payload + 14 Ethernet. No
 * VLAN tag (hype does not tag, plan.md 6e defers VLANs) and no jumbo. */
#define HYPE_VIRTIO_NET_MAX_FRAME_LEN 1514u

typedef struct {
    uint16_t size;
    uint16_t enable;
    uint64_t desc;
    uint64_t driver;
    uint64_t device;
    /* How many avail-ring entries this device has consumed. Device-private bookkeeping, exactly as
     * in virtio-blk -- not a register, and not part of the wire format. */
    uint16_t last_avail_idx;
} hype_virtio_net_vq_t;

typedef struct {
    uint32_t device_feature_select;
    uint32_t driver_feature_select;
    uint64_t driver_features; /* accumulated across both 32-bit halves */
    uint8_t device_status;
    uint16_t queue_select;
    uint8_t isr_status;
    /* #372's rule, same as virtio-blk: the guest's PCI Bus Master Enable mirrored in, so this model
     * needs no PCI. A NIC reaches its rings and buffers by mastering the bus, so with the bit clear
     * a queue notify must do nothing. */
    int bus_master;
    uint8_t mac[HYPE_VIRTIO_NET_MAC_BYTES];
    hype_virtio_net_vq_t vq[HYPE_VIRTIO_NET_NUM_QUEUES];
} hype_virtio_net_t;

/*
 * Post-power-on state: status 0 so a driver must reset and renegotiate, both queues disabled with
 * size at this device's maximum, and `mac` installed as device IDENTITY.
 *
 * `mac` may be 0, which keeps whatever address is already there. A NULL MAC is not an error and is
 * not zeroed either: an all-zero Ethernet address is not a valid source address, so a device reset
 * that produced one would hand the guest an address the wire discards.
 */
void hype_virtio_net_reset(hype_virtio_net_t *dev, const uint8_t *mac);

/* Sets the MAC hype hands the guest. Refuses an all-zero and an all-ones address -- neither is a
 * usable unicast source, and refusing is better than a guest that transmits and is ignored. Returns
 * 0 when the address was taken, -1 when it was refused and the previous one kept. */
int hype_virtio_net_set_mac(hype_virtio_net_t *dev, const uint8_t *mac);

void hype_virtio_net_set_bus_master(hype_virtio_net_t *dev, int enabled);

/* The virtio-pci common configuration. `size_bytes` must match the register's width exactly, as in
 * virtio-blk: a driver reading a 4-byte register two bytes at a time is a driver bug hype should
 * report, not paper over. Returns 0 when handled, -1 otherwise. */
int hype_virtio_net_common_cfg_read(const hype_virtio_net_t *dev, uint32_t offset,
                                    uint8_t size_bytes, uint32_t *out_value);
int hype_virtio_net_common_cfg_write(hype_virtio_net_t *dev, uint32_t offset, uint8_t size_bytes,
                                     uint32_t value);

/* The device-type config: the MAC, byte-addressable, because a driver reads it a byte at a time. */
int hype_virtio_net_device_cfg_read(const hype_virtio_net_t *dev, uint32_t offset,
                                    uint8_t size_bytes, uint32_t *out_value);

/* Reads the ISR byte, which is READ-TO-CLEAR (spec 4.1.4.5) -- so this takes a mutable device. */
uint8_t hype_virtio_net_isr_read(hype_virtio_net_t *dev);

/* Raises the queue-interrupt bit in the ISR. The caller delivers the actual interrupt; this model
 * has no idea what a vector is. */
void hype_virtio_net_raise_queue_interrupt(hype_virtio_net_t *dev);

/*
 * Whether `queue` is usable for DMA right now: the driver has published all three ring addresses,
 * enabled it, negotiated VERSION_1, set DRIVER_OK, and the guest has left bus mastering on. Every
 * one of those is a thing a driver does, and a queue processor that skipped any of them would be
 * reading a ring the guest has not finished building.
 */
int hype_virtio_net_is_queue_ready(const hype_virtio_net_t *dev, unsigned int queue);

/* Header length implied by the negotiated features: 12 with VERSION_1, 10 without. */
unsigned int hype_virtio_net_hdr_len(const hype_virtio_net_t *dev);

#endif /* HYPE_DEVICES_VIRTIO_NET_H */
