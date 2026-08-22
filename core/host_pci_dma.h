#ifndef HYPE_CORE_HOST_PCI_DMA_H
#define HYPE_CORE_HOST_PCI_DMA_H

#include <stdint.h>

/*
 * HNET-2 (#399): shared host-PCI-device DMA rings + buffer pool, per plan.md
 * §10 decision 34 -- a facility grown from the NIC work but scoped to every
 * host PCI device driver (AHCI/NVMe/xHCI storage HBAs migrate onto it per
 * #426). NICs are the first consumer.
 *
 * hype is freestanding with no heap: every existing host driver (nvme_host_hw.c,
 * ahci_host_hw.c, xhci_hw.c) already gets its DMA memory from static, page/struct
 * -aligned BSS arrays -- that convention does not change here. What this module
 * shares is the two things every one of those drivers hand-rolls on top of its
 * own static storage: descriptor-ring INDEX MATH (advance/full/used, with or
 * without a phase/cycle tag), and a bounded BUFFER-SLOT allocator over a
 * caller-owned backing array. Both are pure logic over caller-supplied state,
 * so both are fully unit-tested on the host with no real MMIO involved.
 *
 * Two ring shapes are covered:
 *  - A plain producer/consumer ring (head, tail, capacity) -- the shape
 *    core/e1000.c's hype_e1000_ring_next/full/used already use for the guest-
 *    facing e1000 emulation. This module is that same math, generalized so a
 *    host driver does not re-derive it.
 *  - A phase-tagged consumer ring (index + a single bit that flips each time
 *    the ring wraps) -- the shape NVMe completion queues use (NVMe base spec
 *    §4.6: the Phase Tag flips on every wrap so the driver can tell a fresh
 *    completion from a stale one without a separate "valid" flag).
 *
 * Explicitly NOT covered: xHCI's Link-TRB producer ring. A Link TRB occupies
 * the last slot of the ring itself (xHCI spec §4.11.5.1) and its Toggle Cycle
 * bit flips the producer cycle on wrap -- the wrap consumes a slot the plain
 * ring math above does not know about. core/xhci_hw.c's own `ring_enqueue` +
 * `next_event` already implement that correctly, with real-hardware incident
 * history behind the details (see the comments there); #426 moves that logic
 * INTO this module as its own function (hype_dma_link_ring_enqueue /
 * hype_dma_event_ring_next below) rather than force-fitting it onto the plain
 * ring shape, which would silently drop the Link-TRB slot accounting.
 */

/* --- Plain producer/consumer ring index math --- */

/* Next index after `index` in a ring of `capacity` slots. capacity == 0 always
 * returns 0 (a zero-length ring has no valid index to advance to). */
unsigned int hype_dma_ring_advance(unsigned int index, unsigned int capacity);

/* 1 if the ring is full (tail one slot behind head, mod capacity) -- the
 * producer must not write `tail` until the consumer has moved `head` past it.
 * A zero-capacity ring is always reported full: it can hold nothing. */
int hype_dma_ring_full(unsigned int head, unsigned int tail, unsigned int capacity);

/* Number of occupied slots between `head` (oldest unconsumed) and `tail`
 * (next to be produced), mod capacity. 0 for a zero-capacity ring. */
unsigned int hype_dma_ring_used(unsigned int head, unsigned int tail, unsigned int capacity);

/*
 * Advances a phase-tagged consumer index: increments `*index` mod `capacity`,
 * and flips `*phase` exactly when the index wraps back to 0 (NVMe base spec
 * §4.6's Phase Tag rule -- CQE.P alternates each full pass of the queue so a
 * stale, not-yet-overwritten entry from the previous phase is distinguishable
 * from a fresh one). No-op if capacity == 0.
 */
void hype_dma_cqueue_advance(unsigned int *index, unsigned int *phase, unsigned int capacity);

/* --- Link-TRB producer ring (xHCI shape) --- */

/*
 * Advances a Link-TRB-terminated producer ring by one slot: if `*enqueue`
 * would land on the LAST slot (capacity - 1, reserved for the Link TRB), it
 * instead wraps to slot 0 and flips `*cycle` (Toggle Cycle semantics, xHCI
 * spec §4.11.5.1) -- otherwise it just increments. `capacity` is the ring's
 * full slot count INCLUDING the reserved Link TRB slot, matching how
 * core/xhci_hw.c sizes its rings today. No-op if capacity < 2 (a ring needs at
 * least one data slot plus the Link TRB).
 */
void hype_dma_link_ring_advance(unsigned int *enqueue, unsigned int *cycle, unsigned int capacity);

/* --- Bounded buffer-slot pool --- */

/* A caller-owned, page/struct-aligned backing array is carved into
 * `slot_count` fixed-size slots; this struct tracks which are in use with a
 * bitmap (capped at 64 slots -- every existing ring in this codebase is far
 * smaller, and a wider pool is a sign the caller wants a real ring, not a
 * free-form pool). Never owns or allocates the backing memory itself, per the
 * no-heap convention above. */
typedef struct {
    unsigned int slot_count;
    uint64_t used_bitmap; /* bit i set == slot i is allocated */
} hype_dma_pool_t;

/* Initializes an empty pool of `slot_count` slots (0..64). A count above 64 is
 * clamped to 64, since the bitmap cannot represent more. */
void hype_dma_pool_init(hype_dma_pool_t *pool, unsigned int slot_count);

/* Claims the lowest-numbered free slot, marks it used, returns its index,
 * or -1 if every slot is taken (or the pool has zero slots). */
int hype_dma_pool_alloc(hype_dma_pool_t *pool);

/* Releases slot `index` back to the pool. A double-free or an out-of-range
 * index is a no-op -- freeing what is not held must never corrupt another
 * caller's still-live slot. */
void hype_dma_pool_free(hype_dma_pool_t *pool, int index);

/* 1 if slot `index` is currently allocated, else 0 (including out-of-range). */
int hype_dma_pool_is_used(const hype_dma_pool_t *pool, int index);

/* Count of currently-allocated slots. */
unsigned int hype_dma_pool_used_count(const hype_dma_pool_t *pool);

#endif /* HYPE_CORE_HOST_PCI_DMA_H */
