#include "e1000_dev_ring.h"

static uint64_t xlate(const hype_gpa_map_t *map, uint64_t gpa, uint64_t len) {
    if (map == 0) {
        return gpa; /* identity-mapped caller (the microtests) */
    }
    return hype_gpa_to_host(map, gpa, len);
}

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    unsigned int i;
    for (i = 0; i < 8u; i++) {
        v |= (uint64_t)p[i] << (8u * i);
    }
    return v;
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint64_t ring_base(uint32_t lo, uint32_t hi) {
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* A legacy descriptor: addr(8) length(2) cksum_off(1) cmd(1) status(1) cksum_start(1) special(2). */
#define D_LENGTH 8u
#define D_CMD 11u
#define D_STATUS 12u

int hype_e1000_dev_drain_tx(hype_e1000_dev_t *dev, const hype_gpa_map_t *map,
                            hype_virtio_net_tx_fn sink, void *user, uint8_t *scratch,
                            unsigned int scratch_len, hype_virtio_net_ring_stats_t *stats) {
    unsigned int count;
    uint64_t base;
    uint8_t *ring;
    unsigned int completed = 0;
    unsigned int guard;

    if (dev == 0 || sink == 0 || scratch == 0) {
        return -1;
    }
    if (scratch_len < HYPE_VIRTIO_NET_MAX_FRAME_LEN) {
        /* Refused, not truncated: a clipped frame is a corrupt one the far end reports as a checksum
         * error somewhere else entirely. */
        return -1;
    }
    if (!hype_e1000_dev_tx_ready(dev)) {
        return -1;
    }
    count = hype_e1000_dev_tx_count(dev);
    base = ring_base(dev->tdbal, dev->tdbah);
    /* The WHOLE ring is translated once, so a ring running off the end of guest RAM is refused here
     * rather than part-way through a walk with descriptors already consumed. */
    ring = (uint8_t *)(uintptr_t)xlate(map, base, (uint64_t)count * HYPE_E1000_DESC_BYTES);
    if (ring == 0) {
        return -1;
    }
    if (dev->tdt >= count) {
        /*
         * A tail outside the ring. Refused rather than masked: masking would silently transmit from
         * a descriptor the driver never meant, and a driver that wrote a bad tail has a bug worth
         * seeing rather than absorbing.
         */
        return -1;
    }

    /* HEAD chases TAIL. The ring is empty when they are equal, which is the OPPOSITE of virtio's
     * avail ring -- there, equal means the device has caught up with a monotonically growing index. */
    for (guard = 0; guard < count && dev->tdh != dev->tdt; guard++) {
        uint8_t *d = ring + (uint64_t)dev->tdh * HYPE_E1000_DESC_BYTES;
        uint64_t addr = rd64(d);
        uint16_t dlen = rd16(d + D_LENGTH);
        uint8_t cmd = d[D_CMD];

        if (stats != 0) {
            stats->tx_chains++;
        }
        if (dlen == 0u || dlen > scratch_len) {
            if (stats != 0) {
                stats->tx_bad_desc++;
            }
        } else {
            const uint8_t *src = (const uint8_t *)(uintptr_t)xlate(map, addr, dlen);
            if (src == 0) {
                if (stats != 0) {
                    stats->tx_bad_desc++;
                }
            } else {
                unsigned int i;
                for (i = 0; i < dlen; i++) {
                    scratch[i] = src[i];
                }
                /*
                 * ONE DESCRIPTOR PER FRAME. A driver may split a frame across descriptors and mark
                 * only the last with EOP; this model does not gather, so a non-EOP descriptor is
                 * counted as unusable rather than silently transmitted as a short frame. Linux's
                 * e1000 driver sends single-descriptor frames for anything under a page, which is
                 * every frame at a 1500-byte MTU -- so this is a real limit that the common path
                 * does not hit, and it is counted rather than hidden.
                 */
                if ((cmd & HYPE_E1000_TXD_CMD_EOP) == 0u) {
                    if (stats != 0) {
                        stats->tx_bad_desc++;
                    }
                } else {
                    if (stats != 0) {
                        stats->tx_frames++;
                    }
                    if (sink(user, scratch, dlen) != 0 && stats != 0) {
                        stats->tx_dropped++;
                    }
                }
            }
        }

        /*
         * DD written back on EVERY path, usable descriptor or not, and only when the driver asked for
         * it with RS. A device that withheld DD because it disliked a descriptor would stall the
         * driver's transmit queue permanently -- the operator sees a hung network rather than a
         * dropped packet.
         */
        if ((cmd & HYPE_E1000_TXD_CMD_RS) != 0u) {
            d[D_STATUS] |= HYPE_E1000_TXD_STA_DD;
        }
        dev->tdh = (dev->tdh + 1u) % count;
        completed++;
    }

    if (completed > 0u) {
        (void)hype_e1000_dev_raise(dev, HYPE_E1000_ICR_TXDW);
    }
    return (int)completed;
}

int hype_e1000_dev_deliver_rx(hype_e1000_dev_t *dev, const hype_gpa_map_t *map,
                              const uint8_t *frame, unsigned int len,
                              hype_virtio_net_ring_stats_t *stats) {
    unsigned int count;
    uint64_t base;
    uint8_t *ring;
    uint8_t *d;
    uint64_t addr;
    uint8_t *dst;
    unsigned int i;

    if (dev == 0 || frame == 0 || len == 0u || len > HYPE_VIRTIO_NET_MAX_FRAME_LEN) {
        return -1;
    }
    if (!hype_e1000_dev_rx_ready(dev)) {
        return -1;
    }
    count = hype_e1000_dev_rx_count(dev);
    base = ring_base(dev->rdbal, dev->rdbah);
    ring = (uint8_t *)(uintptr_t)xlate(map, base, (uint64_t)count * HYPE_E1000_DESC_BYTES);
    if (ring == 0) {
        return -1;
    }
    if (dev->rdt >= count) {
        return -1;
    }
    if (dev->rdh == dev->rdt) {
        /*
         * NO BUFFER POSTED. Equal pointers mean the driver has not made anything available, which is
         * normal between polls -- reported as 0 rather than an error so the caller drops the frame
         * exactly as a real NIC does with an empty ring.
         *
         * Note this is the inverse of the transmit test above, on the same equality. Transmit is
         * empty when the driver has queued nothing; receive is empty when the driver has posted
         * nothing. Reading one as the other produces a NIC that either never transmits or claims a
         * buffer it does not have.
         */
        if (stats != 0) {
            stats->rx_no_buffer++;
        }
        return 0;
    }

    d = ring + (uint64_t)dev->rdh * HYPE_E1000_DESC_BYTES;
    addr = rd64(d);
    /*
     * The buffer size is not in the descriptor -- it comes from RCTL.BSIZE, which this model does not
     * decode. HYPE_E1000_BUF_BYTES (2048) is the size every driver programs for a 1500-byte MTU and
     * is the part's own default, so it is what the translation is bounds-checked against: a buffer
     * shorter than that fails the check and the frame is dropped rather than overrunning it.
     */
    dst = (uint8_t *)(uintptr_t)xlate(map, addr, HYPE_E1000_BUF_BYTES);
    if (dst == 0) {
        if (stats != 0) {
            stats->rx_no_buffer++;
        }
        /* The descriptor is still completed with zero length, so the ring keeps moving -- withholding
         * it would stall the driver's receive path over one bad buffer address. */
        wr16(d + D_LENGTH, 0u);
        d[D_STATUS] |= HYPE_E1000_RXD_STA_DD | HYPE_E1000_RXD_STA_EOP;
        dev->rdh = (dev->rdh + 1u) % count;
        (void)hype_e1000_dev_raise(dev, HYPE_E1000_ICR_RXT0);
        return 0;
    }

    for (i = 0; i < len; i++) {
        dst[i] = frame[i];
    }
    /* Length first, then the status that makes it valid: the driver reads DD to decide the length
     * means something, and the other order hands it a length it may read before it was written. */
    wr16(d + D_LENGTH, (uint16_t)len);
    d[D_STATUS] |= HYPE_E1000_RXD_STA_DD | HYPE_E1000_RXD_STA_EOP;
    dev->rdh = (dev->rdh + 1u) % count;
    if (stats != 0) {
        stats->rx_delivered++;
    }
    (void)hype_e1000_dev_raise(dev, HYPE_E1000_ICR_RXT0);
    return 1;
}
