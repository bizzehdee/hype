#include "virtio_net_ring.h"

/* virtq_desc: addr(8) len(4) flags(2) next(2). */
#define DESC_BYTES 16u

static uint64_t xlate(const hype_gpa_map_t *map, uint64_t gpa, uint64_t len) {
    if (map == 0) {
        return gpa; /* identity-mapped caller (the microtests) */
    }
    return hype_gpa_to_host(map, gpa, len);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} desc_t;

/* Reads descriptor `idx` out of guest memory. Returns 0 on success. The index is masked by the
 * caller; this checks only that the descriptor itself is reachable. */
static int read_desc(const hype_gpa_map_t *map, uint64_t desc_table, uint16_t idx, desc_t *out) {
    const uint8_t *p =
        (const uint8_t *)(uintptr_t)xlate(map, desc_table + (uint64_t)idx * DESC_BYTES, DESC_BYTES);
    if (p == 0) {
        return -1;
    }
    out->addr = rd64(p);
    out->len = rd32(p + 8);
    out->flags = rd16(p + 12);
    out->next = rd16(p + 14);
    return 0;
}

/* Both rings, resolved once per drain. Returns 0 on success. */
typedef struct {
    const uint8_t *avail;
    uint8_t *used;
    uint16_t qsz;
} rings_t;

static int map_rings(const hype_virtio_net_vq_t *vq, const hype_gpa_map_t *map, rings_t *out) {
    uint16_t qsz = vq->size;

    if (qsz == 0u) {
        /*
         * UNREACHABLE through either public entry point today: both call
         * hype_virtio_net_is_queue_ready() first, which already refuses size 0. It stays because
         * the ring index arithmetic below is `% qsz`, so a future caller that skipped the readiness
         * check would not get a wrong answer -- it would take a divide-by-zero #DE inside the
         * hypervisor. A guard whose cost is one comparison and whose absence is a host fault is
         * worth keeping even when no current path can reach it.
         */
        return -1;
    }
    /* avail: flags(2) idx(2) ring(2*qsz) used_event(2); used: flags(2) idx(2) elem(8*qsz)
     * avail_event(2). Translated as WHOLE ranges, so a ring that runs off the end of guest RAM is
     * refused here rather than part-way through a walk with descriptors already consumed. */
    out->avail = (const uint8_t *)(uintptr_t)xlate(map, vq->driver, 4u + 2u * (uint64_t)qsz + 2u);
    out->used = (uint8_t *)(uintptr_t)xlate(map, vq->device, 4u + 8u * (uint64_t)qsz + 2u);
    if (out->avail == 0 || out->used == 0) {
        return -1;
    }
    out->qsz = qsz;
    return 0;
}

static void complete(rings_t *r, uint16_t head, uint32_t written) {
    uint16_t used_idx = rd16(r->used + 2);
    uint32_t elem_off = 4u + 8u * (uint32_t)(used_idx % r->qsz);

    wr32(r->used + elem_off, head);
    wr32(r->used + elem_off + 4, written);
    /* The index is published LAST, after the element it refers to, because the guest reads the
     * index to decide whether the element is valid. The other order hands the driver an element it
     * may read before it was written. */
    wr16(r->used + 2, (uint16_t)(used_idx + 1u));
}

int hype_virtio_net_drain_tx(hype_virtio_net_t *dev, const hype_gpa_map_t *map,
                            hype_virtio_net_tx_fn sink, void *user, uint8_t *scratch,
                            unsigned int scratch_len, hype_virtio_net_ring_stats_t *stats) {
    hype_virtio_net_vq_t *vq;
    rings_t r;
    uint16_t avail_idx;
    unsigned int hdr_len;
    int completed = 0;

    if (dev == 0 || sink == 0 || scratch == 0) {
        return -1;
    }
    if (scratch_len < HYPE_VIRTIO_NET_MAX_FRAME_LEN) {
        /* Refused, not truncated: a short buffer would silently clip frames, and a clipped frame is
         * a corrupt one that the far end reports as a checksum error somewhere else entirely. */
        return -1;
    }
    if (!hype_virtio_net_is_queue_ready(dev, HYPE_VIRTIO_NET_VQ_TX)) {
        return -1;
    }
    vq = &dev->vq[HYPE_VIRTIO_NET_VQ_TX];
    if (map_rings(vq, map, &r) != 0) {
        return -1;
    }
    hdr_len = hype_virtio_net_hdr_len(dev);
    avail_idx = rd16(r.avail + 2);

    while (vq->last_avail_idx != avail_idx) {
        uint16_t head = rd16(r.avail + 4 + 2 * (uint32_t)(vq->last_avail_idx % r.qsz));
        uint16_t cur = head;
        unsigned int guard = 0;
        unsigned int gathered = 0;
        unsigned int skipped = 0;
        int chain_ok = 1;

        if (stats != 0) {
            stats->tx_chains++;
        }
        if (head >= r.qsz) {
            /* A descriptor index outside the ring. The chain is unusable, but it must still be
             * completed or the driver waits on it forever -- so it is completed with zero bytes,
             * which is what a real device does with a descriptor it cannot honour. */
            if (stats != 0) {
                stats->tx_bad_desc++;
            }
            complete(&r, head, 0);
            vq->last_avail_idx++;
            completed++;
            continue;
        }

        /* Walk the chain, gathering payload past the virtio-net header. The guard bounds the walk
         * at the ring size: a `next` chain that loops would otherwise spin here forever, and the
         * guest controls every link in it. */
        while (guard <= r.qsz) {
            desc_t d;
            const uint8_t *src;
            unsigned int take;
            unsigned int off = 0;

            if (cur >= r.qsz || read_desc(map, vq->desc, cur, &d) != 0) {
                chain_ok = 0;
                break;
            }
            if (d.len != 0u) {
                src = (const uint8_t *)(uintptr_t)xlate(map, d.addr, d.len);
                if (src == 0) {
                    chain_ok = 0;
                    break;
                }
                /* The header sits at the front of the chain and is not part of the frame. It may
                 * span descriptors, so what is skipped is counted across the whole chain rather
                 * than assumed to be the first descriptor exactly. */
                if (skipped < hdr_len) {
                    unsigned int need = hdr_len - skipped;
                    unsigned int drop = (d.len < need) ? d.len : need;
                    skipped += drop;
                    off = drop;
                }
                take = d.len - off;
                if (take > 0u) {
                    unsigned int room = (gathered < scratch_len) ? (scratch_len - gathered) : 0u;
                    if (take > room) {
                        /* Oversized: dropped rather than clipped, for the same reason the short
                         * scratch buffer is refused above. */
                        chain_ok = 0;
                        break;
                    }
                    {
                        unsigned int i;
                        for (i = 0; i < take; i++) {
                            scratch[gathered + i] = src[off + i];
                        }
                    }
                    gathered += take;
                }
            }
            if ((d.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0u) {
                break;
            }
            cur = d.next;
            guard++;
        }
        if (guard > r.qsz) {
            chain_ok = 0; /* the chain did not terminate within the ring */
        }

        if (!chain_ok) {
            if (stats != 0) {
                stats->tx_bad_desc++;
            }
        } else if (gathered == 0u) {
            /* Header with no payload. Not an error and not a frame -- counted as neither, and
             * completed so the driver's descriptor comes back. */
        } else {
            if (stats != 0) {
                stats->tx_frames++;
            }
            if (sink(user, scratch, gathered) != 0 && stats != 0) {
                stats->tx_dropped++;
            }
        }

        /* Completed on EVERY path, valid chain or not. A device that withholds a descriptor because
         * it disliked its contents stops the driver's transmit queue permanently, and the operator
         * sees a hung network rather than a dropped packet. */
        complete(&r, head, 0);
        vq->last_avail_idx++;
        completed++;
    }

    if (completed > 0) {
        hype_virtio_net_raise_queue_interrupt(dev);
    }
    return completed;
}

int hype_virtio_net_deliver_rx(hype_virtio_net_t *dev, const hype_gpa_map_t *map,
                              const uint8_t *frame, unsigned int len,
                              hype_virtio_net_ring_stats_t *stats) {
    hype_virtio_net_vq_t *vq;
    rings_t r;
    uint16_t avail_idx;
    unsigned int hdr_len;
    uint16_t head;
    uint16_t cur;
    unsigned int guard = 0;
    unsigned int written = 0;
    unsigned int hdr_done = 0;
    unsigned int frame_done = 0;

    if (dev == 0 || frame == 0 || len == 0u || len > HYPE_VIRTIO_NET_MAX_FRAME_LEN) {
        return -1;
    }
    if (!hype_virtio_net_is_queue_ready(dev, HYPE_VIRTIO_NET_VQ_RX)) {
        return -1;
    }
    vq = &dev->vq[HYPE_VIRTIO_NET_VQ_RX];
    if (map_rings(vq, map, &r) != 0) {
        return -1;
    }
    hdr_len = hype_virtio_net_hdr_len(dev);
    avail_idx = rd16(r.avail + 2);

    if (vq->last_avail_idx == avail_idx) {
        /* No receive buffer posted. NORMAL: a driver between polls has an empty ring, so this is
         * reported as 0 rather than an error, and the caller drops the frame exactly as a real NIC
         * does when its ring is empty. */
        if (stats != 0) {
            stats->rx_no_buffer++;
        }
        return 0;
    }

    head = rd16(r.avail + 4 + 2 * (uint32_t)(vq->last_avail_idx % r.qsz));
    if (head >= r.qsz) {
        if (stats != 0) {
            stats->rx_no_buffer++;
        }
        complete(&r, head, 0);
        vq->last_avail_idx++;
        return 0;
    }
    cur = head;

    while (guard <= r.qsz) {
        desc_t d;
        uint8_t *dst;
        unsigned int room;
        unsigned int at = 0;

        if (cur >= r.qsz || read_desc(map, vq->desc, cur, &d) != 0) {
            break;
        }
        /* A receive descriptor must be device-writable. One that is not cannot hold a frame, and
         * writing it anyway would corrupt whatever the driver put there. */
        if ((d.flags & HYPE_VIRTQ_DESC_F_WRITE) == 0u) {
            break;
        }
        if (d.len != 0u) {
            dst = (uint8_t *)(uintptr_t)xlate(map, d.addr, d.len);
            if (dst == 0) {
                break;
            }
            room = d.len;
            /* The header goes in first, zeroed: no offloads are negotiated, so every field
             * (flags, gso_type, hdr_len, gso_size, csum_start, csum_offset, num_buffers) is 0 --
             * and num_buffers of 0 would be wrong, so it is set to 1 below. */
            while (hdr_done < hdr_len && at < room) {
                dst[at] = 0;
                at++;
                hdr_done++;
                written++;
            }
            /* num_buffers (the last 2 bytes of the modern 12-byte header) is 1: this frame occupies
             * exactly one chain. Zero would tell the driver the frame spans no buffers. */
            if (hdr_len == HYPE_VIRTIO_NET_HDR_LEN_MODERN && hdr_done == hdr_len &&
                at >= 2u && written == hdr_len) {
                dst[at - 2] = 1;
                dst[at - 1] = 0;
            }
            while (frame_done < len && at < room) {
                dst[at] = frame[frame_done];
                at++;
                frame_done++;
                written++;
            }
        }
        if (frame_done >= len) {
            break;
        }
        if ((d.flags & HYPE_VIRTQ_DESC_F_NEXT) == 0u) {
            break;
        }
        cur = d.next;
        guard++;
    }

    if (frame_done < len) {
        /* The posted buffer was too small for the frame. The descriptor is completed with what fits
         * -- withholding it would stall the driver's receive ring -- but the frame is counted as
         * dropped rather than delivered, because a partial frame is not a delivered one. */
        if (stats != 0) {
            stats->rx_no_buffer++;
        }
        complete(&r, head, 0);
        vq->last_avail_idx++;
        hype_virtio_net_raise_queue_interrupt(dev);
        return 0;
    }

    complete(&r, head, written);
    vq->last_avail_idx++;
    if (stats != 0) {
        stats->rx_delivered++;
    }
    hype_virtio_net_raise_queue_interrupt(dev);
    return 1;
}
