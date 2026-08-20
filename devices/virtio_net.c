#include "virtio_net.h"

static void reset_negotiation_state(hype_virtio_net_t *dev) {
    unsigned int q;

    dev->device_feature_select = 0;
    dev->driver_feature_select = 0;
    dev->driver_features = 0;
    dev->device_status = 0;
    dev->queue_select = 0;
    dev->isr_status = 0;
    for (q = 0; q < HYPE_VIRTIO_NET_NUM_QUEUES; q++) {
        dev->vq[q].size = HYPE_VIRTIO_NET_QUEUE_SIZE_MAX;
        dev->vq[q].enable = 0;
        dev->vq[q].desc = 0;
        dev->vq[q].driver = 0;
        dev->vq[q].device = 0;
        dev->vq[q].last_avail_idx = 0;
    }
}

static int mac_is_usable(const uint8_t *mac) {
    unsigned int i;
    unsigned int zeros = 0;
    unsigned int ones = 0;

    for (i = 0; i < HYPE_VIRTIO_NET_MAC_BYTES; i++) {
        if (mac[i] == 0x00u) {
            zeros++;
        }
        if (mac[i] == 0xFFu) {
            ones++;
        }
    }
    /*
     * All-zero is the "no address" placeholder and all-ones is the broadcast address. Neither is a
     * legal SOURCE address, so a guest given one transmits frames the first switch discards. This
     * mirrors the same refusal in core/e1000.c's MAC decode, deliberately: a bad address is worth
     * refusing at every point it can enter, because the symptom downstream is "the network does not
     * work" with nothing pointing at the address.
     */
    if (zeros == HYPE_VIRTIO_NET_MAC_BYTES || ones == HYPE_VIRTIO_NET_MAC_BYTES) {
        return 0;
    }
    /* A multicast source address is equally not a unicast sender. Bit 0 of octet 0 is the
     * group bit. */
    if ((mac[0] & 0x01u) != 0u) {
        return 0;
    }
    return 1;
}

int hype_virtio_net_set_mac(hype_virtio_net_t *dev, const uint8_t *mac) {
    unsigned int i;

    if (dev == 0 || mac == 0) {
        return -1;
    }
    if (!mac_is_usable(mac)) {
        return -1;
    }
    for (i = 0; i < HYPE_VIRTIO_NET_MAC_BYTES; i++) {
        dev->mac[i] = mac[i];
    }
    return 0;
}

void hype_virtio_net_reset(hype_virtio_net_t *dev, const uint8_t *mac) {
    if (dev == 0) {
        return;
    }
    reset_negotiation_state(dev);
    /*
     * The MAC is set HERE and not in reset_negotiation_state(), which runs again every time a driver
     * writes device_status = 0. It is device identity, not negotiation state -- the same reasoning
     * that keeps virtio-blk's serial out of its reset path (#310). A NIC whose MAC changed under a
     * driver reset would look to the guest like the card had been swapped, and to hype's NAT plane
     * like a different guest.
     */
    if (mac != 0) {
        (void)hype_virtio_net_set_mac(dev, mac);
    }
    /* #372's permissive default, for the same reason as virtio-blk: the microtests drive this model
     * with no PCI at all, so a device that started with bus mastering off would be inert in exactly
     * the environment built to test it. The live path mirrors the guest's real bit in. */
    dev->bus_master = 1;
}

void hype_virtio_net_set_bus_master(hype_virtio_net_t *dev, int enabled) {
    if (dev == 0) {
        return;
    }
    dev->bus_master = (enabled != 0) ? 1 : 0;
}

/* A queue_select past the last queue is legal for a driver to write and reads back all-zero, so
 * every register accessor funnels through this rather than indexing on trust. */
static const hype_virtio_net_vq_t *sel_ro(const hype_virtio_net_t *dev) {
    if (dev->queue_select >= HYPE_VIRTIO_NET_NUM_QUEUES) {
        return 0;
    }
    return &dev->vq[dev->queue_select];
}

static hype_virtio_net_vq_t *sel_rw(hype_virtio_net_t *dev) {
    if (dev->queue_select >= HYPE_VIRTIO_NET_NUM_QUEUES) {
        return 0;
    }
    return &dev->vq[dev->queue_select];
}

int hype_virtio_net_common_cfg_read(const hype_virtio_net_t *dev, uint32_t offset,
                                    uint8_t size_bytes, uint32_t *out_value) {
    const hype_virtio_net_vq_t *vq;

    if (dev == 0 || out_value == 0) {
        return -1;
    }
    if (offset >= HYPE_VIRTIO_COMMON_CFG_SIZE) {
        return -1;
    }
    vq = sel_ro(dev);

    switch (offset) {
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT:
            if (size_bytes != 4u) return -1;
            *out_value = dev->device_feature_select;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE:
            if (size_bytes != 4u) return -1;
            if (dev->device_feature_select == 0u) {
                *out_value = 1u << HYPE_VIRTIO_NET_F_MAC_BIT;
            } else if (dev->device_feature_select == 1u) {
                *out_value = 1u << (HYPE_VIRTIO_F_VERSION_1_BIT - 32u);
            } else {
                *out_value = 0u;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT:
            if (size_bytes != 4u) return -1;
            *out_value = dev->driver_feature_select;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE:
            if (size_bytes != 4u) return -1;
            if (dev->driver_feature_select == 0u) {
                *out_value = (uint32_t)(dev->driver_features & 0xFFFFFFFFu);
            } else if (dev->driver_feature_select == 1u) {
                *out_value = (uint32_t)(dev->driver_features >> 32);
            } else {
                *out_value = 0u;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG:
            if (size_bytes != 2u) return -1;
            *out_value = 0xFFFFu; /* NO_VECTOR -- MSI-X not modeled */
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES:
            if (size_bytes != 2u) return -1;
            *out_value = HYPE_VIRTIO_NET_NUM_QUEUES;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS:
            if (size_bytes != 1u) return -1;
            *out_value = dev->device_status;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION:
            if (size_bytes != 1u) return -1;
            /* The device config (the MAC) never changes after reset, so the generation counter is
             * permanently 0 -- a driver re-reading it can never see a torn value. */
            *out_value = 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT:
            if (size_bytes != 2u) return -1;
            *out_value = dev->queue_select;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE:
            if (size_bytes != 2u) return -1;
            *out_value = (vq != 0) ? vq->size : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR:
            if (size_bytes != 2u) return -1;
            *out_value = 0xFFFFu;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE:
            if (size_bytes != 2u) return -1;
            *out_value = (vq != 0) ? vq->enable : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF:
            if (size_bytes != 2u) return -1;
            /*
             * One notify slot per queue: queue N notifies at notify_off N, scaled by the
             * capability's notify_off_multiplier. Returning 0 for every queue -- which a
             * single-queue device can do -- would make both queues share one doorbell address, and
             * hype could then not tell a transmit notify from a receive one.
             */
            *out_value = (vq != 0) ? dev->queue_select : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO:
            if (size_bytes != 4u) return -1;
            *out_value = (vq != 0) ? (uint32_t)(vq->desc & 0xFFFFFFFFu) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI:
            if (size_bytes != 4u) return -1;
            *out_value = (vq != 0) ? (uint32_t)(vq->desc >> 32) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO:
            if (size_bytes != 4u) return -1;
            *out_value = (vq != 0) ? (uint32_t)(vq->driver & 0xFFFFFFFFu) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI:
            if (size_bytes != 4u) return -1;
            *out_value = (vq != 0) ? (uint32_t)(vq->driver >> 32) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO:
            if (size_bytes != 4u) return -1;
            *out_value = (vq != 0) ? (uint32_t)(vq->device & 0xFFFFFFFFu) : 0u;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI:
            if (size_bytes != 4u) return -1;
            *out_value = (vq != 0) ? (uint32_t)(vq->device >> 32) : 0u;
            return 0;
        default:
            return -1;
    }
}

int hype_virtio_net_common_cfg_write(hype_virtio_net_t *dev, uint32_t offset, uint8_t size_bytes,
                                     uint32_t value) {
    hype_virtio_net_vq_t *vq;

    if (dev == 0) {
        return -1;
    }
    if (offset >= HYPE_VIRTIO_COMMON_CFG_SIZE) {
        return -1;
    }
    vq = sel_rw(dev);

    switch (offset) {
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT:
            if (size_bytes != 4u) return -1;
            dev->device_feature_select = value;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT:
            if (size_bytes != 4u) return -1;
            dev->driver_feature_select = value;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE:
            if (size_bytes != 4u) return -1;
            /* Each half is written independently and both must survive, so this merges rather than
             * assigns -- a driver writes select=0/value, then select=1/value. */
            if (dev->driver_feature_select == 0u) {
                dev->driver_features =
                    (dev->driver_features & 0xFFFFFFFF00000000ull) | (uint64_t)value;
            } else if (dev->driver_feature_select == 1u) {
                dev->driver_features =
                    (dev->driver_features & 0x00000000FFFFFFFFull) | ((uint64_t)value << 32);
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS:
            if (size_bytes != 1u) return -1;
            /* Writing 0 is the spec's device reset (4.1.4.3.1), and it must clear everything a
             * driver negotiated -- not just the status byte. */
            if ((value & 0xFFu) == 0u) {
                reset_negotiation_state(dev);
            } else {
                dev->device_status = (uint8_t)(value & 0xFFu);
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT:
            if (size_bytes != 2u) return -1;
            /* Out-of-range selects are RETAINED rather than clamped: the driver must read back
             * queue_size == 0 to learn the queue does not exist, and clamping to a real queue would
             * answer as though it did. */
            dev->queue_select = (uint16_t)(value & 0xFFFFu);
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE:
            if (size_bytes != 2u) return -1;
            if (vq == 0) return 0;
            {
                uint16_t want = (uint16_t)(value & 0xFFFFu);
                /* A driver may only shrink the queue. Clamp rather than refuse, which is what the
                 * spec expects of a device whose maximum is lower than the request. */
                vq->size = (want > HYPE_VIRTIO_NET_QUEUE_SIZE_MAX) ? HYPE_VIRTIO_NET_QUEUE_SIZE_MAX
                                                                   : want;
            }
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE:
            if (size_bytes != 2u) return -1;
            if (vq == 0) return 0;
            vq->enable = (uint16_t)(value & 0xFFFFu);
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR:
        case HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG:
            /* Accepted and discarded: MSI-X is not modeled, and the read side already answers
             * NO_VECTOR, so a driver that writes a vector here is told it did not take. */
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO:
            if (size_bytes != 4u) return -1;
            if (vq == 0) return 0;
            vq->desc = (vq->desc & 0xFFFFFFFF00000000ull) | (uint64_t)value;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI:
            if (size_bytes != 4u) return -1;
            if (vq == 0) return 0;
            vq->desc = (vq->desc & 0x00000000FFFFFFFFull) | ((uint64_t)value << 32);
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO:
            if (size_bytes != 4u) return -1;
            if (vq == 0) return 0;
            vq->driver = (vq->driver & 0xFFFFFFFF00000000ull) | (uint64_t)value;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI:
            if (size_bytes != 4u) return -1;
            if (vq == 0) return 0;
            vq->driver = (vq->driver & 0x00000000FFFFFFFFull) | ((uint64_t)value << 32);
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO:
            if (size_bytes != 4u) return -1;
            if (vq == 0) return 0;
            vq->device = (vq->device & 0xFFFFFFFF00000000ull) | (uint64_t)value;
            return 0;
        case HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI:
            if (size_bytes != 4u) return -1;
            if (vq == 0) return 0;
            vq->device = (vq->device & 0x00000000FFFFFFFFull) | ((uint64_t)value << 32);
            return 0;
        default:
            /* Read-only registers (device_feature, num_queues, config_generation, queue_notify_off)
             * ignore writes rather than failing: the spec makes them read-only, and a driver that
             * writes one is not owed an error the transport has no way to report. */
            return 0;
    }
}

int hype_virtio_net_device_cfg_read(const hype_virtio_net_t *dev, uint32_t offset,
                                    uint8_t size_bytes, uint32_t *out_value) {
    uint32_t v = 0;
    unsigned int i;

    if (dev == 0 || out_value == 0) {
        return -1;
    }
    if (size_bytes != 1u && size_bytes != 2u && size_bytes != 4u) {
        return -1;
    }
    /* The whole access must lie inside the config, not just its first byte -- a 4-byte read at
     * offset 4 would otherwise return two MAC bytes and two bytes of nothing. */
    if (offset >= HYPE_VIRTIO_NET_CFG_SIZE ||
        (uint32_t)(offset + size_bytes) > HYPE_VIRTIO_NET_CFG_SIZE) {
        return -1;
    }
    for (i = 0; i < size_bytes; i++) {
        v |= (uint32_t)dev->mac[offset + i] << (8u * i);
    }
    *out_value = v;
    return 0;
}

uint8_t hype_virtio_net_isr_read(hype_virtio_net_t *dev) {
    uint8_t v;

    if (dev == 0) {
        return 0;
    }
    v = dev->isr_status;
    dev->isr_status = 0; /* read-to-clear, spec 4.1.4.5 */
    return v;
}

void hype_virtio_net_raise_queue_interrupt(hype_virtio_net_t *dev) {
    if (dev == 0) {
        return;
    }
    dev->isr_status |= 0x1u; /* bit 0: queue interrupt */
}

int hype_virtio_net_is_queue_ready(const hype_virtio_net_t *dev, unsigned int queue) {
    const hype_virtio_net_vq_t *vq;

    if (dev == 0 || queue >= HYPE_VIRTIO_NET_NUM_QUEUES) {
        return 0;
    }
    vq = &dev->vq[queue];
    /*
     * Every clause is something the DRIVER does, and a processor that skipped any one of them would
     * be walking a ring the guest has not finished publishing:
     *   DRIVER_OK        the driver says the device may start
     *   VERSION_1        modern layout agreed; without it the ring format is the legacy one
     *   enable           this queue specifically is live
     *   size, desc       the ring exists and has an address
     *   driver, device   the avail and used rings have addresses too. virtio-blk checks only desc,
     *                    which is enough for a device that only ever reads a request; a NIC writes
     *                    the used ring on every frame in both directions, so a zero there is a write
     *                    to guest-physical 0.
     *   bus_master       the guest left the device able to reach memory at all (#372)
     */
    return (dev->device_status & HYPE_VIRTIO_STATUS_DRIVER_OK) != 0 &&
           (dev->driver_features & (1ull << HYPE_VIRTIO_F_VERSION_1_BIT)) != 0 &&
           vq->enable != 0 && vq->size != 0 && vq->desc != 0 && vq->driver != 0 &&
           vq->device != 0 && dev->bus_master != 0;
}

unsigned int hype_virtio_net_hdr_len(const hype_virtio_net_t *dev) {
    if (dev == 0) {
        return HYPE_VIRTIO_NET_HDR_LEN_MODERN;
    }
    if ((dev->driver_features & (1ull << HYPE_VIRTIO_F_VERSION_1_BIT)) != 0) {
        return HYPE_VIRTIO_NET_HDR_LEN_MODERN;
    }
    return HYPE_VIRTIO_NET_HDR_LEN_LEGACY;
}
