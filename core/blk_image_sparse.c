#include "blk_image_sparse.h"

#include "ticket_lock.h"

/*
 * #506: guest block backend over a sparse file's hype_file_rmap_t. See blk_image_sparse.h and
 * plan.md decision 69 for why this exists beside, not inside, hype_blk_image_t.
 */

/* One process-wide growth lock (plan.md decision 69): growth is rare next to steady-state I/O,
 * so a single lock is simpler than one per volume and costs nothing worth avoiding. */
static volatile unsigned int g_grow_lock_next;
static volatile unsigned int g_grow_lock_owner;

/* Copies `src` into `dst` field-by-field (whole-struct assignment of an array-holding struct
 * emits a memcpy call the freestanding build cannot link), folding `partition_lba` into every
 * DATA/UNWRITTEN range's start_lba so the injected read_sectors/write_sectors callbacks -- which
 * speak DISK-ABSOLUTE LBAs, same convention as hype_blk_image_t -- can be handed straight to
 * hype_file_rmap_read_at/write_at without those functions needing to know about partitions. */
static void load_rmap(const hype_file_rmap_t *src, uint64_t partition_lba, hype_file_rmap_t *dst) {
    unsigned i;
    dst->count = src->count;
    dst->size_bytes = src->size_bytes;
    dst->too_fragmented = src->too_fragmented;
    for (i = 0; i < src->count; i++) {
        dst->ranges[i].kind = src->ranges[i].kind;
        dst->ranges[i].sector_count = src->ranges[i].sector_count;
        dst->ranges[i].start_lba = src->ranges[i].start_lba;
        if (src->ranges[i].kind != (uint32_t)HYPE_RANGE_HOLE) {
            dst->ranges[i].start_lba += partition_lba;
        }
    }
}

static int sparse_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    hype_blk_image_sparse_t *img = (hype_blk_image_sparse_t *)ctx;
    return hype_file_rmap_read_at(&img->map, img->read_sectors, img->hw,
                                  lba * (uint64_t)HYPE_BLK_SECTOR_SIZE, dst,
                                  (unsigned int)((uint64_t)count * HYPE_BLK_SECTOR_SIZE));
}

/* Allocates and writes `len` bytes at file byte `offset` through the growth handle, then
 * refreshes the cached map so the new region joins the fast DATA path. See
 * hype_blk_image_sparse_t::grow_broken for what happens if the refresh itself fails. */
static int grow_and_write(hype_blk_image_sparse_t *img, uint64_t offset, const void *src,
                          unsigned int len) {
    hype_file_rmap_t fresh;
    int rc;

    if (img->grow_fs == 0 || img->grow_broken) {
        return -1;
    }
    hype_ticket_lock_acquire(&g_grow_lock_next, &g_grow_lock_owner);
    rc = hype_fs_write_at(img->grow_file, offset, src, len);
    if (rc == 0) {
        if (hype_fs_map_ranges(img->grow_fs, img->grow_path, &fresh) == 0) {
            load_rmap(&fresh, img->partition_lba, &img->map);
        } else {
            /* The bytes are safely on the medium -- write_at's own crash-safety already
             * covered them -- but this backend can no longer trust where anything is.
             * Refuse every future write rather than risk serving a stale map. */
            img->grow_broken = 1;
            rc = -1;
        }
    }
    hype_ticket_lock_release(&g_grow_lock_owner);
    return rc;
}

static int sparse_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    hype_blk_image_sparse_t *img = (hype_blk_image_sparse_t *)ctx;
    const uint8_t *s = (const uint8_t *)src;
    uint64_t offset = lba * (uint64_t)HYPE_BLK_SECTOR_SIZE;
    uint64_t remaining = (uint64_t)count * HYPE_BLK_SECTOR_SIZE;

    while (remaining > 0u) {
        hype_range_kind_t kind;
        uint64_t range_lba;
        uint32_t head;
        uint64_t run;
        uint64_t chunk;

        if (hype_file_rmap_locate(&img->map, offset, &kind, &range_lba, &head, &run) != 0) {
            return -1;
        }
        chunk = (run < remaining) ? run : remaining;
        if (kind == HYPE_RANGE_DATA) {
            /* write_sectors is guaranteed non-NULL here: sparse_write is only ever installed as
             * be->write when write_sectors != 0 (see hype_blk_image_sparse_init below). */
            if (hype_file_rmap_write_at(&img->map, img->read_sectors, img->write_sectors, img->hw,
                                        offset, s, (unsigned int)chunk) != 0) {
                return -1;
            }
        } else if (kind == HYPE_RANGE_HOLE) {
            if (grow_and_write(img, offset, s, (unsigned int)chunk) != 0) {
                return -1;
            }
            /* grow_and_write() just replaced img->map wholesale; head/run computed above are
             * stale for anything past this chunk, which is exactly why the loop re-locates
             * from scratch on its next iteration instead of trusting them further. */
        } else {
            return -1; /* UNWRITTEN: this layer cannot fake a valid-length advance */
        }
        offset += chunk;
        s += chunk;
        remaining -= chunk;
    }
    return 0;
}

int hype_blk_image_sparse_init(hype_blk_image_sparse_t *img, hype_blk_backend_t *be,
                               const hype_file_rmap_t *rmap, uint64_t partition_lba,
                               hype_blk_read_fn read_sectors, hype_blk_write_fn write_sectors,
                               void *hw, hype_fs_t *grow_fs, hype_fs_file_t *grow_file,
                               const char *grow_path) {
    uint64_t capacity;
    int have_growth = (grow_fs != 0 || grow_file != 0 || grow_path != 0);

    if (img == 0 || be == 0 || rmap == 0 || read_sectors == 0) {
        return -1;
    }
    if (have_growth && (grow_fs == 0 || grow_file == 0 || grow_path == 0)) {
        return -1; /* the growth handle triple is all-or-nothing */
    }
    if (have_growth && (hype_fs_caps(grow_fs) & HYPE_FS_CAP_WRITE_GROW) == 0u) {
        return -1; /* caught at setup, not at the first guest write into a hole */
    }
    capacity = rmap->size_bytes / HYPE_BLK_SECTOR_SIZE;
    if (capacity == 0u) {
        return -1;
    }

    load_rmap(rmap, partition_lba, &img->map);
    img->partition_lba = partition_lba;
    img->read_sectors = read_sectors;
    img->write_sectors = write_sectors;
    img->hw = hw;
    img->grow_fs = grow_fs;
    img->grow_file = grow_file;
    img->grow_path = grow_path;
    img->grow_broken = 0;

    be->read = sparse_read;
    be->write = (write_sectors != 0) ? sparse_write : 0;
    be->writev = 0;
    be->ctx = img;
    be->total_sectors = capacity;
    return 0;
}
