#ifndef HYPE_CORE_GUEST_NIC_H
#define HYPE_CORE_GUEST_NIC_H

#include <stdint.h>

#include "guest_mem.h"
#include "virtio_net_ring.h"

/*
 * NET-2/NET-3 (#81/#82): the one interface the forwarding plane sees, whichever NIC a guest has.
 *
 * WHY THIS EXISTS NOW AND NOT BEFORE. plan.md §10's rule is explicit: "per-type interfaces earn
 * their place with concrete implementations, not in anticipation" -- abstract a type when it has two
 * or three real implementations that genuinely share shape. virtio-net alone did not earn one, and
 * #81 deliberately called the device's functions directly. e1000 is the second, and the shape they
 * share is exact: hand me a frame for the guest, drain the frames the guest queued, tell me its MAC.
 *
 * WHAT IT BUYS, concretely. The forwarding plane is where the interesting logic is -- proxy ARP,
 * address learning, the on-link check, NAPT, the peer mailbox -- and none of it depends on which NIC
 * the guest has. Without this indirection every one of those sites would need a two-way branch, and
 * a future third frontend would mean finding all of them. With it, the plane is written once and the
 * frontend is a pointer.
 *
 * The `void *dev` is the frontend's own state. It is not type-checked, which is the cost of a vtable
 * in C; what keeps it honest is that the ops pointer and the dev pointer are set together in one
 * place per frontend and never separately.
 */

typedef struct {
    /* The frontend's name, for log lines. A diagnostic that says "the NIC" when a host has two kinds
     * of guest NIC is a diagnostic that cannot be acted on. */
    const char *name;

    /*
     * Put one frame in the guest's receive path. Returns 1 delivered, 0 no buffer available (a
     * NORMAL condition between driver polls, not an error -- the caller drops the frame exactly as a
     * real NIC does), -1 the ring could not be walked.
     */
    int (*deliver_rx)(void *dev, const hype_gpa_map_t *map, const uint8_t *frame, unsigned int len,
                      hype_virtio_net_ring_stats_t *stats);

    /*
     * Drain everything the guest has queued for transmit, handing each frame to `sink`. Returns the
     * number of descriptors completed, or -1 if the ring could not be walked at all.
     *
     * `scratch` is the CALLER'S per-VM buffer. Both frontends refuse a buffer smaller than a maximum
     * frame rather than truncating, and neither uses a static -- this runs concurrently on one core
     * per VM, which is #343 for the data and #557 for the counters.
     */
    int (*drain_tx)(void *dev, const hype_gpa_map_t *map, hype_virtio_net_tx_fn sink, void *user,
                    uint8_t *scratch, unsigned int scratch_len,
                    hype_virtio_net_ring_stats_t *stats);

    /* Copies the guest's MAC out. Returns 0 on success. The forwarding plane needs it to build the
     * Ethernet header of an inbound frame. */
    int (*mac)(void *dev, uint8_t out[6]);
} hype_guest_nic_ops_t;

/*
 * The two frontends. Selected per VM from `os_hint`, mirroring §6a's storage split exactly: Windows
 * gets the device it has an inbox driver for, Linux and BSD get virtio.
 */
extern const hype_guest_nic_ops_t hype_guest_nic_virtio;
extern const hype_guest_nic_ops_t hype_guest_nic_e1000;

#endif /* HYPE_CORE_GUEST_NIC_H */
