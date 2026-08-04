#ifndef HYPE_CORE_BLK_QCOW2_H
#define HYPE_CORE_BLK_QCOW2_H

#include <stdint.h>

#include "blk_backend.h"

/*
 * M5-9 (#200): QCOW2 as a guest block backend, read AND write.
 *
 * This is a FORMAT layer, not a storage layer: it stacks on any other
 * hype_blk_backend_t -- the raw image file on a host filesystem (#199), a physical
 * partition, or a test buffer -- and addresses it in that backend's own sectors. So it
 * inherits, unchanged, everything the layer below already got right: the VALID-3
 * bounds gate, extent mapping, and post-EBS write persistence. Nothing here touches a
 * filesystem or a controller.
 *
 * What qcow2 buys over raw (#199) is that a 40 GB guest disk occupies only the clusters
 * the guest has actually written, so a scratch disk fits on a USB stick. What it costs
 * is that every guest access needs an L1 and an L2 lookup, and every write to a new
 * region needs a cluster allocation plus a refcount update. That is the tradeoff this
 * file implements.
 *
 * Deliberately narrow, because the alternative to refusing is corrupting somebody's
 * disk image. Refused rather than guessed at:
 *
 *   - compressed clusters (no zlib in a freestanding EFI binary),
 *   - encryption of any kind,
 *   - a refcount_order other than 4 (16-bit refcounts) -- writing a refcount at the
 *     wrong width silently mangles the allocation map,
 *   - any qcow2 v3 incompatible_feature bit, which includes extended L2 entries and
 *     the "image is dirty/corrupt" markers,
 *   - a cluster whose COPIED bit is clear, i.e. one shared with a snapshot: writing it
 *     in place would change the snapshot too,
 *   - growing the refcount table, and allocating past the end of the underlying
 *     backend. Both mean "the image was created too small"; qemu-img sizes the table
 *     for the full virtual size up front, so hitting this is a prep-time mistake and
 *     must surface as one.
 *
 * Everything is done in 512-byte units against the backend below. That works because
 * every qcow2 structure that matters is cluster-aligned and the cluster size is at
 * least 512 bytes, so a metadata entry is always reachable by reading the one sector
 * that contains it. No cluster-sized buffer -- and therefore no allocator -- is needed
 * anywhere in this file.
 *
 * Pure logic over an injected backend: unit-tested against an in-memory qcow2 image the
 * tests assemble byte by byte, so the format handling is verified without a disk and
 * without qemu-img.
 */

/* The header's own magic, "QFI\xfb". */
#define HYPE_QCOW2_MAGIC 0x514649FBu

/* 512 B .. 2 MiB. The floor is the sector size (a smaller cluster could not be
 * addressed sector-wise); the ceiling is qemu's own supported maximum. */
#define HYPE_QCOW2_MIN_CLUSTER_BITS 9u
#define HYPE_QCOW2_MAX_CLUSTER_BITS 21u

typedef struct {
    /* The qcow2 FILE, addressed in its own 512-byte sectors. */
    hype_blk_backend_t *file;
    /*
     * Optional backing image, read-only, addressed in GUEST sectors -- a cluster this
     * image has not allocated reads through to it. NULL means unallocated clusters read
     * as zeros. If the header names a backing file and none is supplied, init refuses:
     * serving zeros where the chain has data would hand the guest a corrupt disk that
     * looks fine until it is mounted.
     */
    const hype_blk_backend_t *backing;

    uint32_t version;
    uint32_t cluster_bits;
    uint64_t cluster_size;
    uint32_t l2_bits;      /* cluster_bits - 3: L2 entries per table, as a shift */
    uint64_t virtual_size; /* bytes, from the header */
    uint64_t l1_offset;
    uint32_t l1_size; /* L1 entries */
    uint64_t refcount_table_offset;
    uint32_t refcount_table_clusters;

    /* Allocation cursor: one past the highest cluster the refcount map says is in
     * use, established by scanning the refcount blocks at init. */
    uint64_t next_free_cluster;
    uint64_t file_clusters; /* capacity of `file`, in clusters -- the allocation ceiling */

    /* Scratch for read-modify-write of a single metadata sector. In the struct rather
     * than on the stack because this path runs on a 4 KB EFI stack. */
    uint8_t sec[HYPE_BLK_SECTOR_SIZE];
} hype_qcow2_t;

/*
 * Parse `file`'s qcow2 header, validate it against everything listed above, establish
 * the allocation cursor, and wire `be` to this image. `be->total_sectors` becomes the
 * VIRTUAL size in sectors, so the guest sees the disk it was promised rather than the
 * bytes currently allocated.
 *
 * `be->write` is left NULL when `file->write` is NULL, so the dispatcher rejects guest
 * writes rather than this layer discovering it cannot persist them halfway through an
 * allocation.
 *
 * Returns 0, or -1 on any unreadable, unsupported or self-inconsistent image.
 */
int hype_qcow2_init(hype_qcow2_t *q, hype_blk_backend_t *be, hype_blk_backend_t *file,
                    const hype_blk_backend_t *backing);

/*
 * Translate a guest byte offset to its offset in the qcow2 file.
 *
 * Returns 1 and sets *out_file_offset when the cluster is allocated; 0 when it is not
 * (or is flagged all-zeroes), leaving *out_file_offset untouched; -1 on a read error or
 * an unsupported cluster (compressed). Exposed because it is where the format's
 * arithmetic lives, and therefore what the tests need to pin directly.
 */
int hype_qcow2_map(hype_qcow2_t *q, uint64_t guest_offset, uint64_t *out_file_offset);

#endif /* HYPE_CORE_BLK_QCOW2_H */
