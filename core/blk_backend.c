#include "blk_backend.h"

int hype_blk_range_in_bounds(uint64_t total_sectors, uint64_t lba, uint32_t count) {
    uint64_t end;

    if (count == 0u) {
        return 0; /* a real transfer is always >= 1 sector */
    }
    end = lba + (uint64_t)count;
    if (end < lba) {
        return 0; /* lba + count overflowed 64 bits */
    }
    return end <= total_sectors;
}

int hype_blk_backend_read(const hype_blk_backend_t *be, uint64_t lba, uint32_t count, void *buf) {
    if (be == (const hype_blk_backend_t *)0 || be->read == (int (*)(void *, uint64_t, uint32_t, void *))0) {
        return -1;
    }
    if (!hype_blk_range_in_bounds(be->total_sectors, lba, count)) {
        return -1;
    }
    return be->read(be->ctx, lba, count, buf);
}

int hype_blk_backend_write(const hype_blk_backend_t *be, uint64_t lba, uint32_t count,
                           const void *buf) {
    if (be == (const hype_blk_backend_t *)0 ||
        be->write == (int (*)(void *, uint64_t, uint32_t, const void *))0) {
        return -1; /* NULL write => read-only backend */
    }
    if (!hype_blk_range_in_bounds(be->total_sectors, lba, count)) {
        return -1;
    }
    return be->write(be->ctx, lba, count, buf);
}

/* --- file-backed implementation (a raw disk image in a host buffer) --- */

/* Both impls run only on ranges the dispatcher already validated against
 * f->total_sectors (== be->total_sectors), so the byte offset is in bounds. */
static int file_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    hype_blk_file_t *f = (hype_blk_file_t *)ctx;
    const uint8_t *src = f->base + lba * HYPE_BLK_SECTOR_SIZE;
    uint8_t *dst = (uint8_t *)buf;
    uint64_t n = (uint64_t)count * HYPE_BLK_SECTOR_SIZE;
    uint64_t i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    return 0;
}

static int file_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    hype_blk_file_t *f = (hype_blk_file_t *)ctx;
    uint8_t *dst = f->base + lba * HYPE_BLK_SECTOR_SIZE;
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t n = (uint64_t)count * HYPE_BLK_SECTOR_SIZE;
    uint64_t i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    return 0;
}

void hype_blk_file_init(hype_blk_file_t *f, hype_blk_backend_t *be, uint8_t *base,
                        uint64_t size_bytes) {
    f->base = base;
    f->total_sectors = size_bytes / HYPE_BLK_SECTOR_SIZE;

    be->read = file_read;
    be->write = file_write;
    be->ctx = f;
    be->total_sectors = f->total_sectors;
}
