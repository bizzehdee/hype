#ifndef HYPE_CORE_BLK_PHYS_H
#define HYPE_CORE_BLK_PHYS_H

#include <stdint.h>
#include "blk_backend.h"

/*
 * M10-3 (§6d): the physical-disk implementation of the blk_backend vtable. A
 * guest's writes/reads go straight to a real drive via the host AHCI driver
 * (core/ahci_host), bounds-checked against the drive's *actual* capacity
 * (VALID-3) -- higher stakes than the file backend since a miss here touches
 * real hardware.
 *
 * The guest-supplied LBA+count is bounds-checked centrally by
 * hype_blk_backend_read/write() against total_sectors (the disk's real
 * capacity from hype_ahci_host_parse_identify, #122). This module then splits
 * the validated transfer into chunks the underlying HBA can do in one command
 * (one AHCI PRDT entry = 4 MiB = 8192 sectors).
 *
 * The chunking logic is pure and dependency-injected: the per-chunk sector I/O
 * is a pair of injected callbacks (a fake in tests; the ahci_host MMIO path at
 * runtime -- see hype_blk_phys_ahci_init in the coverage-exempt shim).
 */

/* One AHCI PRDT entry moves at most 4 MiB; cap each hw transfer there. */
#define HYPE_BLK_PHYS_MAX_CHUNK 8192u

typedef int (*hype_blk_phys_read_fn)(void *hw, uint64_t lba, uint32_t count, void *buf);
typedef int (*hype_blk_phys_write_fn)(void *hw, uint64_t lba, uint32_t count, const void *buf);

typedef struct {
    hype_blk_phys_read_fn read_sectors;
    hype_blk_phys_write_fn write_sectors; /* NULL => read-only backend */
    void *hw;                             /* opaque, passed to the callbacks */
} hype_blk_phys_t;

/*
 * Wires `be` to a physical backend of `total_sectors`, driven by the injected
 * per-chunk sector callbacks over `hw`. `write_sectors` NULL makes the backend
 * read-only (be->write stays NULL, so a guest write is rejected by the
 * dispatcher). Pure.
 */
void hype_blk_phys_init(hype_blk_phys_t *p, hype_blk_backend_t *be,
                        hype_blk_phys_read_fn read_sectors, hype_blk_phys_write_fn write_sectors,
                        void *hw, uint64_t total_sectors);

/* --- runtime AHCI binding (coverage-exempt shim; real MMIO via ahci_host) --- */

/* Owns the (ABAR, port) the adapter callbacks read. Caller-allocated, must
 * outlive the backend. */
typedef struct {
    uint64_t abar_phys;
    unsigned port;
} hype_blk_phys_ahci_t;

/*
 * Convenience wiring for a real disk: binds `be` to `port` of the HBA at
 * `abar_phys` (already hype_ahci_host_init'd), with `total_sectors` the disk's
 * real IDENTIFY capacity. read/write go through hype_ahci_host_read/write.
 * The port must already be initialised. DESTRUCTIVE on write -- see
 * hype_ahci_host_write's contract re: the §6d safety guard.
 */
void hype_blk_phys_ahci_init(hype_blk_phys_t *p, hype_blk_phys_ahci_t *hw, hype_blk_backend_t *be,
                             uint64_t abar_phys, unsigned port, uint64_t total_sectors);

/* M10-1c (#197): NVMe counterpart. Owns the controller BAR0 the adapter
 * callbacks drive. Caller-allocated, must outlive the backend. */
typedef struct {
    uint64_t abar_phys; /* NVMe BAR0 (already hype_nvme_host_init'd) */
} hype_blk_phys_nvme_t;

/*
 * Binds `be` to an NVMe namespace-1 disk at `abar_phys` (already
 * hype_nvme_host_init'd), with `total_sectors` the namespace's real IDENTIFY
 * capacity. read/write go through hype_nvme_host_read/write. DESTRUCTIVE on
 * write -- same §6d/phys_guard contract as the AHCI path.
 */
void hype_blk_phys_nvme_init(hype_blk_phys_t *p, hype_blk_phys_nvme_t *hw, hype_blk_backend_t *be,
                             uint64_t abar_phys, uint64_t total_sectors);

#endif /* HYPE_CORE_BLK_PHYS_H */
