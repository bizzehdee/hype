#include "blk_image.h"

/*
 * #199: guest block backend over a raw image file's resolved extents. See
 * blk_image.h for why this needs no filesystem writer.
 */

#define SECSZ HYPE_BLK_SECTOR_SIZE

int hype_blk_image_locate(const hype_blk_image_t *img, uint64_t fsec, uint64_t *out_lba,
                          uint64_t *out_run) {
    uint64_t before = 0;
    unsigned int x;

    for (x = 0; x < img->map.count; x++) {
        uint64_t n = img->map.extents[x].sector_count;
        if (fsec < before + n) {
            uint64_t into = fsec - before;
            *out_lba = img->partition_lba + img->map.extents[x].start_lba + into;
            *out_run = n - into;
            return 0;
        }
        before += n;
    }
    return -1; /* past the mapped extents */
}

/* Total sectors the extent list actually covers. */
static uint64_t mapped_sectors(const hype_file_map_t *map) {
    uint64_t total = 0;
    unsigned int x;
    for (x = 0; x < map->count; x++) {
        total += map->extents[x].sector_count;
    }
    return total;
}

/*
 * Shared body of the read/write impls. Walks the guest range extent by extent,
 * issuing one host command per contiguous run. The range is already bounds-
 * checked against capacity by hype_blk_backend_read/write, so a locate() miss
 * here would mean the extent list disagrees with the capacity init derived from
 * it -- treated as an error, never as a short transfer.
 */
static int image_rw(hype_blk_image_t *img, uint64_t lba, uint32_t count, uint8_t *rbuf,
                    const uint8_t *wbuf) {
    uint32_t done = 0;

    while (done < count) {
        uint64_t host_lba = 0;
        uint64_t run = 0;
        uint32_t n;

        if (hype_blk_image_locate(img, lba + done, &host_lba, &run) != 0) {
            return -1;
        }
        n = (uint64_t)(count - done) < run ? (count - done) : (uint32_t)run;
        if (rbuf != 0) {
            if (img->read_sectors(img->hw, host_lba, n, rbuf + (uint64_t)done * SECSZ) != 0) {
                return -1;
            }
        } else {
            if (img->write_sectors == 0) {
                return -1;
            }
            if (img->write_sectors(img->hw, host_lba, n, wbuf + (uint64_t)done * SECSZ) != 0) {
                return -1;
            }
        }
        done += n;
    }
    return 0;
}

static int image_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    return image_rw((hype_blk_image_t *)ctx, lba, count, (uint8_t *)buf, 0);
}

static int image_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    return image_rw((hype_blk_image_t *)ctx, lba, count, 0, (const uint8_t *)buf);
}

int hype_blk_image_init(hype_blk_image_t *img, hype_blk_backend_t *be, const hype_file_map_t *map,
                        uint64_t partition_lba, hype_blk_read_fn read_sectors,
                        hype_blk_write_fn write_sectors, void *hw) {
    uint64_t capacity;
    unsigned int x;

    if (img == 0 || be == 0 || map == 0 || read_sectors == 0) {
        return -1;
    }
    if (map->count > HYPE_FILE_MAX_EXTENTS) {
        return -1;
    }
    capacity = map->size_bytes / SECSZ; /* a trailing partial sector is unreachable */
    if (capacity == 0u) {
        return -1; /* nothing a guest could use as a disk */
    }
    /*
     * The extents must cover the whole file. A short list would otherwise serve
     * reads fine up to the gap and then fail mid-install, which is precisely the
     * late, confusing failure the prep tool (#90) exists to prevent.
     */
    if (mapped_sectors(map) < capacity) {
        return -1;
    }
    for (x = 0; x < map->count; x++) {
        if (map->extents[x].sector_count == 0u) {
            return -1; /* a zero-length run cannot be right */
        }
    }

    /* Copy the map field-by-field: whole-struct assignment of anything holding
     * an array emits a memcpy, which does not exist at EFI link time. */
    img->map.count = map->count;
    img->map.size_bytes = map->size_bytes;
    for (x = 0; x < map->count; x++) {
        img->map.extents[x].start_lba = map->extents[x].start_lba;
        img->map.extents[x].sector_count = map->extents[x].sector_count;
    }
    img->partition_lba = partition_lba;
    img->read_sectors = read_sectors;
    img->write_sectors = write_sectors;
    img->hw = hw;

    be->read = image_read;
    be->write = (write_sectors != 0) ? image_write : 0;
    be->writev = 0; /* #295: no vectored impl -- N scalar writes cost the same here */
    be->ctx = img;
    be->total_sectors = capacity;
    return 0;
}
