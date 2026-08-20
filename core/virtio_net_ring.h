#ifndef HYPE_CORE_VIRTIO_NET_RING_H
#define HYPE_CORE_VIRTIO_NET_RING_H

#include <stdint.h>

#include "guest_mem.h"
#include "../devices/virtio_net.h"

/*
 * NET-2 (#81): walking the guest's transmit and receive virtqueues.
 *
 * WHY THIS IS A SEPARATE FILE FROM devices/virtio_net.c. That file is the register plane and knows
 * nothing about guest memory. This one dereferences guest-supplied addresses, so it needs the
 * bounds-checked gpa map (VALID-1 #53 / VALID-2 #54) and belongs where that dependency is visible.
 *
 * WHY IT IS NOT NEXT TO process_virtio_blk_queue(). That function lives in
 * arch/x86_64/svm/svm_vcpu.c, which is coverage-EXEMPT -- it is vendor-neutral code sitting in a
 * vendor file for historical reasons, and VMX calls into it across the arch boundary to reach it.
 * Putting the NIC's ring walk there would inherit the exemption and ship the trickiest arithmetic
 * in the network path with no unit tests at all. Here it is ordinary testable code.
 *
 * Every guest-supplied value is treated as hostile, because all of them are: the ring addresses
 * come from registers the guest wrote, the descriptor indices come from the ring, and the buffer
 * pointers and lengths come from the descriptors. Each is bounds-checked through `map` before it is
 * dereferenced, and each index is masked to the ring size.
 */

/* What a caller does with one outbound frame. Returns 0 if the frame was accepted (or deliberately
 * dropped); nonzero means "could not accept", and the descriptor is still returned to the guest --
 * a NIC that stops completing descriptors because the wire is busy wedges its driver, and a dropped
 * packet is what a real network does under load. */
typedef int (*hype_virtio_net_tx_fn)(void *user, const uint8_t *frame, unsigned int len);

typedef struct {
    unsigned long long tx_chains;      /* descriptor chains taken off the avail ring */
    unsigned long long tx_frames;      /* chains that carried a plausible frame */
    unsigned long long tx_dropped;     /* frames the sink refused or that were malformed */
    unsigned long long tx_bad_desc;    /* chains rejected before any byte was read */
    unsigned long long rx_delivered;   /* frames written into guest receive buffers */
    unsigned long long rx_no_buffer;   /* frames dropped because the guest posted none */
} hype_virtio_net_ring_stats_t;

/*
 * Drains the transmit queue: for every chain the driver has made available, gathers the frame
 * (skipping the virtio-net header) and hands it to `sink`, then returns the chain on the used ring.
 *
 * Returns the number of chains completed, or -1 if the queue could not be walked at all (a ring
 * address that does not translate, a zero-length ring). -1 means nothing was consumed and nothing
 * was completed, so the guest is not left believing a descriptor was taken.
 */
/*
 * `scratch` is where each outbound frame is gathered before the sink sees it, and it is the
 * CALLER'S buffer on purpose. A file-static here would be shared by every VM, and this runs
 * concurrently on one core per VM -- that is the #343 bug class for data and the #557 bug class for
 * diagnostics, and it has already cost this project two investigations. The caller holds the
 * per-VM device lock and owns per-VM storage, so the buffer's lifetime and exclusivity are
 * something it can actually guarantee. `scratch_len` under HYPE_VIRTIO_NET_MAX_FRAME_LEN is
 * refused rather than silently truncating frames.
 */
int hype_virtio_net_drain_tx(hype_virtio_net_t *dev, const hype_gpa_map_t *map,
                            hype_virtio_net_tx_fn sink, void *user, uint8_t *scratch,
                            unsigned int scratch_len, hype_virtio_net_ring_stats_t *stats);

/*
 * Places one received frame into the next available receive buffer, prefixing the virtio-net
 * header the driver expects, and completes that descriptor.
 *
 * Returns 1 when the frame was delivered, 0 when the guest had no buffer posted (the frame is the
 * caller's to drop -- this is a normal condition, not an error), and -1 when the queue could not be
 * walked. A 0 must NOT be treated as failure: a guest between NAPI polls legitimately has an empty
 * ring for short periods.
 */
int hype_virtio_net_deliver_rx(hype_virtio_net_t *dev, const hype_gpa_map_t *map,
                              const uint8_t *frame, unsigned int len,
                              hype_virtio_net_ring_stats_t *stats);

#endif /* HYPE_CORE_VIRTIO_NET_RING_H */
