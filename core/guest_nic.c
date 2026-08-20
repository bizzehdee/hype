#include "guest_nic.h"

#include "e1000_dev_ring.h"

/*
 * The two frontends behind hype_guest_nic_ops_t. Thin by design: each entry either forwards to its
 * frontend's own function or adapts a signature. Anything cleverer here would be logic that belongs
 * to one frontend living in the file that is supposed to make them interchangeable.
 */

static int vnet_deliver(void *dev, const hype_gpa_map_t *map, const uint8_t *frame,
                        unsigned int len, hype_virtio_net_ring_stats_t *stats) {
    return hype_virtio_net_deliver_rx((hype_virtio_net_t *)dev, map, frame, len, stats);
}

static int vnet_drain(void *dev, const hype_gpa_map_t *map, hype_virtio_net_tx_fn sink, void *user,
                      uint8_t *scratch, unsigned int scratch_len,
                      hype_virtio_net_ring_stats_t *stats) {
    return hype_virtio_net_drain_tx((hype_virtio_net_t *)dev, map, sink, user, scratch, scratch_len,
                                    stats);
}

static int vnet_mac(void *dev, uint8_t out[6]) {
    const hype_virtio_net_t *d = (const hype_virtio_net_t *)dev;
    unsigned int i;

    if (d == 0 || out == 0) {
        return -1;
    }
    for (i = 0; i < 6u; i++) {
        out[i] = d->mac[i];
    }
    return 0;
}

static int e1000_deliver(void *dev, const hype_gpa_map_t *map, const uint8_t *frame,
                         unsigned int len, hype_virtio_net_ring_stats_t *stats) {
    return hype_e1000_dev_deliver_rx((hype_e1000_dev_t *)dev, map, frame, len, stats);
}

static int e1000_drain(void *dev, const hype_gpa_map_t *map, hype_virtio_net_tx_fn sink, void *user,
                       uint8_t *scratch, unsigned int scratch_len,
                       hype_virtio_net_ring_stats_t *stats) {
    return hype_e1000_dev_drain_tx((hype_e1000_dev_t *)dev, map, sink, user, scratch, scratch_len,
                                   stats);
}

static int e1000_mac(void *dev, uint8_t out[6]) {
    const hype_e1000_dev_t *d = (const hype_e1000_dev_t *)dev;
    unsigned int i;

    if (d == 0 || out == 0) {
        return -1;
    }
    for (i = 0; i < 6u; i++) {
        out[i] = d->mac[i];
    }
    return 0;
}

const hype_guest_nic_ops_t hype_guest_nic_virtio = {
    "virtio-net", vnet_deliver, vnet_drain, vnet_mac
};

const hype_guest_nic_ops_t hype_guest_nic_e1000 = {
    "e1000", e1000_deliver, e1000_drain, e1000_mac
};
