#ifndef HYPE_CORE_BLK_IMAGE_SPARSE_H
#define HYPE_CORE_BLK_IMAGE_SPARSE_H

#include <stdint.h>

#include "blk_backend.h"
#include "file_range.h" /* hype_file_rmap_t: the sparse-aware logical range contract (#381) */
#include "fs_ops.h"      /* hype_fs_t / hype_fs_file_t: the growth handle */

/*
 * #506 (plan.md decision 69): a file-backed guest disk that can be SPARSE -- the
 * growth-capable sibling of core/blk_image.h's hype_blk_image_t.
 *
 * hype_blk_image_t deliberately never touches a filesystem writer: its hype_file_map_t is a
 * flat, physical-only extent list, resolved once, and that is exactly what makes it safe to
 * use post-ExitBootServices on a fully-preallocated image. This backend is for the OTHER
 * case: a sparse image, where "not yet allocated" is a normal, expected state rather than
 * corruption. It is built on hype_file_rmap_t (#381) instead of reinventing hole semantics --
 * the SAME sparse-aware contract NTFS and ext already use for their own internals.
 *
 * A guest READ into a HOLE never touches the filesystem writer: hype_file_rmap_read_at()
 * synthesizes zeros directly. A guest WRITE into a HOLE calls hype_fs_write_at() on the
 * injected growth handle (fs + open file), which allocates AND writes the guest's actual
 * bytes in ONE filesystem-level call -- so the crash-safety story is whatever that
 * filesystem's own writer already guarantees (journaled on ext, #385), never a second,
 * home-grown allocate-then-fill sequence. The range map is then refreshed from the
 * filesystem so the newly-allocated region joins the fast DATA path for later access.
 * A write into an UNWRITTEN range is refused, never promoted to a growth attempt -- this
 * layer cannot fake a valid-length advance, and in practice a sparse image created by hype
 * (#507) never produces UNWRITTEN ranges (only ext-style HOLE).
 *
 * A disk without a growth handle (grow_fs == 0) is a READ-ONLY sparse view: a write into a
 * HOLE is refused rather than silently discarded or guessed at. That is a real, intentional
 * configuration, not a placeholder.
 *
 * Growth is serialised by one process-wide ticket lock (core/ticket_lock.c, SMP-7 #191): two
 * vCPUs growing two different sparse images that happen to share a host volume's allocation
 * metadata is the hazard, and growth is rare enough next to steady-state I/O that a single
 * global lock costs nothing worth avoiding by scoping it per-volume.
 *
 * Pure logic over injected callbacks, like hype_blk_image_t: unit-tested against synthetic
 * range maps and a fake filesystem, no real disk needed.
 */

typedef struct {
    hype_file_rmap_t map;   /* DATA/UNWRITTEN ranges carry DISK-ABSOLUTE start_lba (partition_lba
                              * already folded in whenever the map is loaded/refreshed) */
    uint64_t partition_lba; /* added to every DATA/UNWRITTEN range's start_lba on (re)load */
    hype_blk_read_fn read_sectors;
    hype_blk_write_fn write_sectors; /* NULL => read-only backend */
    void *hw;

    /* The growth handle. NULL grow_fs means "no growth capability": a write into a HOLE is
     * refused. grow_path must stay valid for the backend's whole lifetime -- it is re-resolved
     * on every growth, never copied; callers pass a stable buffer, matching how mkdisk/VM-setup
     * already keep paths in fixed storage. */
    hype_fs_t *grow_fs;
    hype_fs_file_t *grow_file;
    const char *grow_path;

    /* Set once a post-growth hype_fs_map_ranges() refresh fails after the underlying
     * hype_fs_write_at() already succeeded: the guest's bytes are safely on the medium (that
     * call's own crash-safety already covered them), but this backend's cached map can no
     * longer be trusted to say where anything is. Once set, every further write is refused --
     * conservative, since guessing at a stale map risks handing a later guest read stale or
     * wrong data instead of the zeros/real-bytes contract this backend otherwise guarantees.
     * Reads keep working off the last-known-good map. */
    int grow_broken;
} hype_blk_image_sparse_t;

/*
 * Wires `be` to the sparse image described by `rmap` (ranges VOLUME-relative, as every
 * resolver produces them -- `partition_lba` is folded in here, once, same convention as
 * hype_blk_image_init). Capacity comes from rmap->size_bytes floored to whole sectors -- the
 * file's LOGICAL size, not the sum of its allocated ranges, so a freshly-created sparse image
 * with zero DATA ranges still reports its full virtual size to the guest.
 *
 * `grow_fs`/`grow_file`/`grow_path` may all be 0 together for a read-only sparse view (a write
 * into a HOLE is then refused). If any one is non-zero, all three must be, and the filesystem
 * behind them must have HYPE_FS_CAP_WRITE_GROW -- checked here and refused otherwise, so a
 * misconfigured sparse disk is caught at VM setup, not discovered at the first guest write into
 * a hole.
 *
 * Returns 0, or -1 on: img/be/rmap/read_sectors NULL, a zero-sector capacity, or an
 * inconsistent (partially-NULL) growth-handle triple.
 */
int hype_blk_image_sparse_init(hype_blk_image_sparse_t *img, hype_blk_backend_t *be,
                               const hype_file_rmap_t *rmap, uint64_t partition_lba,
                               hype_blk_read_fn read_sectors, hype_blk_write_fn write_sectors,
                               void *hw, hype_fs_t *grow_fs, hype_fs_file_t *grow_file,
                               const char *grow_path);

#endif /* HYPE_CORE_BLK_IMAGE_SPARSE_H */
