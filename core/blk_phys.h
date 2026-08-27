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
/*
 * #295: one hw call carrying a whole segment list as ONE device command (multi-PRDT on AHCI). The
 * caller (phys_writev) has already ensured the batch fits the limits the hw layer declared at init
 * (writev_max_segs / writev_max_sectors), so an impl may build the command without re-deciding.
 */
typedef int (*hype_blk_phys_writev_fn)(void *hw, uint64_t lba, const hype_blk_seg_t *segs,
                                       uint32_t nsegs);

typedef struct {
    hype_blk_phys_read_fn read_sectors;
    hype_blk_phys_write_fn write_sectors; /* NULL => read-only backend */
    /*
     * #295: optional vectored write, with the limits ONE hw command can carry. Both limits are
     * plain numbers so this module stays bus-agnostic: AHCI's are its PRDT slot count and its
     * 4 MiB total-per-command ceiling; a bus with no vectored win leaves the fn NULL and the
     * backend's writev slot stays NULL too (the dispatcher then falls back to scalar writes).
     */
    hype_blk_phys_writev_fn writev_sectors;
    uint32_t writev_max_segs;    /* most segments one hw command carries */
    uint32_t writev_max_sectors; /* most total sectors one hw command carries */
    void *hw;                             /* opaque, passed to the callbacks */
    /*
     * #332: disk-absolute LBA of sector 0 of the SCOPE. 0 for a whole-disk target; the partition's
     * first LBA for a partition-scoped one.
     *
     * The confinement property is the reason it lives here rather than being filtered in the adapter:
     * hype_blk_backend_read/write already bounds-checks the guest's LBA+count against
     * be->total_sectors. Set total_sectors to the PARTITION's size and add this base, and the guest
     * is confined to that partition BY THE CHECK THAT ALREADY EXISTS -- no new trust boundary, and no
     * second place to forget one.
     */
    uint64_t base_lba;
    /*
     * #747: the device behind this backend has been unplugged.
     *
     * Kept HERE, in the chunker, because it is the one layer both physical paths funnel
     * through: AHCI/NVMe reach it directly and USB MSC reaches it via hype_blk_usb_init(),
     * which wires its callbacks into this same struct. One flag, checked in one place, for
     * every physical-backed disk, log sink and guest volume -- #576's rule applied before
     * it can be broken rather than after.
     *
     * STICKY. A re-plug does NOT clear it: a half-written cluster chain is not made good by
     * the device coming back, and resuming onto it is how #596's broken chain would be
     * extended rather than noticed. Clearing it is an explicit operator action (attach).
     */
    int departed;
} hype_blk_phys_t;

/*
 * Wires `be` to a physical backend of `total_sectors`, driven by the injected
 * per-chunk sector callbacks over `hw`. `write_sectors` NULL makes the backend
 * read-only (be->write stays NULL, so a guest write is rejected by the
 * dispatcher). Pure.
 *
 * Whole-disk form: equivalent to hype_blk_phys_init_scoped() with base_lba 0.
 */
void hype_blk_phys_init(hype_blk_phys_t *p, hype_blk_backend_t *be,
                        hype_blk_phys_read_fn read_sectors, hype_blk_phys_write_fn write_sectors,
                        void *hw, uint64_t total_sectors);

/*
 * #332: partition-scoped form. `base_lba` is the partition's first disk-absolute LBA and
 * `sector_count` its length, which becomes the backend's total_sectors -- so the guest sees a disk of
 * exactly that size and cannot address outside it (see hype_blk_phys_t.base_lba).
 *
 * This is what turns "dedicate a whole drive to hype" into "give hype the spare partition you already
 * have" (plan.md §6d).
 */
void hype_blk_phys_init_scoped(hype_blk_phys_t *p, hype_blk_backend_t *be,
                               hype_blk_phys_read_fn read_sectors,
                               hype_blk_phys_write_fn write_sectors, void *hw, uint64_t base_lba,
                               uint64_t sector_count);

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

/*
 * #295: arm the vectored write path. Call AFTER hype_blk_phys_init(_scoped); sets be->writev so
 * hype_blk_backend_writev() reaches phys_writev, which batches the segment list into hw commands
 * of at most `max_segs` segments and `max_sectors` total sectors each (a single segment larger
 * than `max_sectors` goes through the ordinary chunked scalar path instead). No-op when
 * `writev_sectors` is NULL or the backend is read-only.
 */
void hype_blk_phys_enable_writev(hype_blk_phys_t *p, hype_blk_backend_t *be,
                                 hype_blk_phys_writev_fn writev_sectors, uint32_t max_segs,
                                 uint32_t max_sectors);

#endif /* HYPE_CORE_BLK_PHYS_H */

/*
 * #747: the device behind this backend is gone. Every subsequent read, write and writev
 * fails with HYPE_BLK_ERR_GONE without touching the hardware.
 *
 * Idempotent, and safe to call from the USB departure path with I/O in flight: the flag is
 * only ever set, and each entry point tests it before it touches `hw`.
 */
void hype_blk_phys_mark_departed(hype_blk_phys_t *p);

/* #747: has it? 0 when `p` is NULL, so a caller with no physical backend reads as present. */
int hype_blk_phys_is_departed(const hype_blk_phys_t *p);
