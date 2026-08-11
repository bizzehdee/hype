#ifndef HYPE_CORE_BLK_IMAGE_H
#define HYPE_CORE_BLK_IMAGE_H

#include <stdint.h>

#include "blk_backend.h"
#include "blk_io.h" /* hype_file_map_t: the shared extent contract (#292) */

/*
 * M5-8 (#199): a RAW disk-image FILE on a host filesystem as a guest block
 * backend -- read AND write, so guest writes persist to the file.
 *
 * The trick is that this needs no filesystem writer at all. Every host-FS
 * resolver in the tree -- FAT32 and exFAT (#181, core/fat.c) and ext2/3/4
 * (#203, core/ext.c) -- produces the SAME hype_file_map_t extent list, so a
 * pre-allocated image file is just a scatter list of disk sectors. Mapping a
 * guest LBA through that list gives a host LBA, and the transfer is then plain
 * block I/O through the injected host read/write pair (AHCI / NVMe / USB via
 * blk_phys). One backend therefore covers a raw image on ANY of the three
 * filesystems, and the guest gets the same device whichever it is.
 *
 * Because the file's extents never move, this path writes NO filesystem
 * metadata: no directory entry, no size, no bitmap, no journal. That is what
 * makes persisting guest writes post-ExitBootServices safe on a volume the host
 * OS also knows about -- and it is why the image must be FULLY ALLOCATED before
 * hype sees it (tools/make-disk-image.sh, #90, creates and verifies exactly
 * that). A sparse hole would be a sector the filesystem has not assigned, and
 * this layer cannot assign one.
 *
 * Capacity is the file's own size, floored to whole sectors; every guest
 * LBA+count is bounds-checked against it by hype_blk_backend_read/write before
 * reaching here (VALID-3), and each transfer is additionally split at extent
 * boundaries, so a guest can never be handed sectors outside its own image.
 *
 * Pure logic over injected callbacks -- unit-tested against synthetic extent
 * lists with no disk, and validated end-to-end against real FAT32/exFAT/ext
 * images.
 */

/* The injected host block I/O pair operates in DISK-ABSOLUTE LBAs (the same
 * shape blk_phys hands out); the shared callback types are LBA-space-neutral
 * (see core/blk_io.h), so the space is documented here, where it is decided. */

typedef struct {
    hype_file_map_t map;    /* the image file's extents, VOLUME-relative */
    uint64_t partition_lba; /* added to every extent LBA to get disk-absolute */
    hype_blk_read_fn read_sectors;
    hype_blk_write_fn write_sectors; /* NULL => read-only backend */
    void *hw;
} hype_blk_image_t;

/*
 * Wires `be` to the image described by `map` (extents volume-relative;
 * `partition_lba` is the volume's first disk LBA, 0 for an unpartitioned
 * medium). Capacity comes from map->size_bytes floored to whole sectors.
 * `write_sectors` NULL leaves be->write NULL, so the dispatcher rejects guest
 * writes. Returns 0, or -1 if the map is unusable (no extents for a non-empty
 * file, extents shorter than the file's size, or a zero-sector capacity) --
 * refused rather than silently serving a short disk.
 */
int hype_blk_image_init(hype_blk_image_t *img, hype_blk_backend_t *be,
                        const hype_file_map_t *map, uint64_t partition_lba,
                        hype_blk_read_fn read_sectors,
                        hype_blk_write_fn write_sectors, void *hw);

/*
 * Maps file-relative sector `fsec` to its disk-absolute LBA and reports how
 * many sectors stay contiguous from there (so a caller can transfer that much
 * in one command). Returns 0, or -1 if `fsec` is past the mapped extents.
 * Pure; exposed for tests and for callers that want to do their own I/O.
 */
int hype_blk_image_locate(const hype_blk_image_t *img, uint64_t fsec, uint64_t *out_lba,
                          uint64_t *out_run);

#endif /* HYPE_CORE_BLK_IMAGE_H */
