#ifndef HYPE_CORE_E1000_DEV_RING_H
#define HYPE_CORE_E1000_DEV_RING_H

#include <stdint.h>

#include "guest_mem.h"
#include "virtio_net_ring.h" /* hype_virtio_net_tx_fn, hype_virtio_net_ring_stats_t */
#include "../devices/e1000_dev.h"

/*
 * NET-3 (#82): walking the guest's e1000 descriptor rings.
 *
 * Separate from devices/e1000_dev.c for the same reason core/virtio_net_ring.c is separate from
 * devices/virtio_net.c: that file is the register plane and knows nothing about guest memory, this
 * one dereferences guest-supplied addresses and so needs the bounds-checked gpa map (VALID-1/2).
 *
 * IT SHARES THE SINK SIGNATURE AND THE COUNTERS with virtio-net, deliberately. The forwarding plane
 * above does not care which NIC a frame came from -- proxy ARP, NAT and peer rules are identical --
 * so making the two frontends present the same shape is what lets one plane serve both. Two
 * implementations is also what plan.md §10 says earns an interface, so hype_guest_nic_ops_t exists
 * (core/guest_nic.h) and both of these sit behind it.
 *
 * THE RING MODEL IS NOT VIRTIO'S, and the difference is the part that gets written wrong. virtio has
 * an available ring and a used ring, both driver-published. e1000 has ONE ring per direction with a
 * HEAD the hardware owns and a TAIL the driver owns:
 *
 *   transmit: the driver fills descriptors and advances TDT. Hardware consumes from TDH to TDT,
 *             writes DD back into each descriptor's status, and advances TDH.
 *   receive:  the driver posts empty buffers and advances RDT. Hardware fills from RDH, sets
 *             DD|EOP and the length, and advances RDH. RDH == RDT means NO buffers are posted.
 *
 * So the emptiness test is inverted relative to virtio, and getting it backwards produces a NIC that
 * appears to work while transmitting the same descriptor forever.
 */

/*
 * Drains the transmit ring: for every descriptor between TDH and TDT, gathers the frame and hands it
 * to `sink`, sets DD, and advances TDH.
 *
 * `scratch` is the caller's per-VM gather buffer, not a static here -- the #343/#557 reasoning in
 * core/virtio_net_ring.h applies identically. Returns the number of descriptors completed, or -1 if
 * the ring could not be walked at all (nothing consumed).
 */
int hype_e1000_dev_drain_tx(hype_e1000_dev_t *dev, const hype_gpa_map_t *map,
                            hype_virtio_net_tx_fn sink, void *user, uint8_t *scratch,
                            unsigned int scratch_len, hype_virtio_net_ring_stats_t *stats);

/*
 * Places one received frame into the descriptor at RDH and advances it.
 *
 * Returns 1 when delivered, 0 when the guest has posted no buffer (RDH == RDT -- a normal condition
 * between driver polls, not an error), and -1 when the ring could not be walked.
 */
int hype_e1000_dev_deliver_rx(hype_e1000_dev_t *dev, const hype_gpa_map_t *map,
                              const uint8_t *frame, unsigned int len,
                              hype_virtio_net_ring_stats_t *stats);

#endif /* HYPE_CORE_E1000_DEV_RING_H */
