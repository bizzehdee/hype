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

static hype_blk_wstats_t g_wstats;
static uint64_t (*g_wstats_now)(void);

void hype_blk_wstats_set_clock(uint64_t (*now)(void)) {
    g_wstats_now = now;
}

hype_blk_wstats_t *hype_blk_wstats(void) {
    return &g_wstats;
}

unsigned hype_blk_wstats_bucket(uint32_t count) {
    if (count <= 1u) {
        return 0u;
    }
    if (count < 8u) {
        return 1u;
    }
    if (count < 32u) {
        return 2u;
    }
    if (count < 128u) {
        return 3u;
    }
    if (count < 1024u) {
        return 4u;
    }
    return 5u;
}

void hype_blk_wstats_reset(hype_blk_wstats_t *s) {
    unsigned i;
    if (s == (hype_blk_wstats_t *)0) {
        return;
    }
    s->writes = 0;
    s->sectors = 0;
    s->first_tsc = 0;
    s->max_count = 0;
    for (i = 0; i < HYPE_BLK_WSTATS_BUCKETS; i++) {
        s->hist[i] = 0;
    }
    s->vec_writes = 0;
    s->vec_segs = 0;
    s->vec_max_segs = 0;
}

void hype_blk_wstats_record(hype_blk_wstats_t *s, uint32_t count) {
    if (s == (hype_blk_wstats_t *)0) {
        return;
    }
    s->writes++;
    if (s->writes == 1u && g_wstats_now != (uint64_t (*)(void))0) {
        s->first_tsc = g_wstats_now();
    }
    s->sectors += (uint64_t)count;
    if (count > s->max_count) {
        s->max_count = count;
    }
    s->hist[hype_blk_wstats_bucket(count)]++;
}

uint64_t hype_blk_wstats_kbps(const hype_blk_wstats_t *s, uint64_t elapsed_ms) {
    if (s == (const hype_blk_wstats_t *)0 || elapsed_ms == 0u) {
        return 0u;
    }
    /* sectors * 512 bytes / 1024 = sectors/2 KB; scaled by 1000 ms/s. */
    return (s->sectors * 1000ULL) / (elapsed_ms * 2ULL);
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
    {
        int rc = be->write(be->ctx, lba, count, buf);
        if (rc == 0) {
            hype_blk_wstats_record(&g_wstats, count); /* #265: successful writes only */
        }
        return rc;
    }
}

int hype_blk_backend_writev(const hype_blk_backend_t *be, uint64_t lba,
                            const hype_blk_seg_t *segs, uint32_t nsegs) {
    uint64_t total = 0;
    uint32_t i;

    if (be == (const hype_blk_backend_t *)0 ||
        be->write == (int (*)(void *, uint64_t, uint32_t, const void *))0 ||
        segs == (const hype_blk_seg_t *)0 || nsegs == 0u) {
        return -1; /* NULL write => read-only backend, even for the vectored path */
    }
    /*
     * Validate the WHOLE list before any byte moves. The sum is 64-bit and each segment is
     * re-checked against it, so a list built to wrap 32 bits cannot read as small: 2^32 sectors is
     * 2 TiB, and total_sectors bounds it far earlier for any real backend, but the guard is
     * arithmetic rather than an assumption about capacities.
     */
    for (i = 0; i < nsegs; i++) {
        if (segs[i].count == 0u) {
            return -1; /* a real transfer is always >= 1 sector (same rule as the scalar path) */
        }
        total += (uint64_t)segs[i].count;
    }
    if (total > 0xFFFFFFFFu || !hype_blk_range_in_bounds(be->total_sectors, lba, (uint32_t)total)) {
        return -1;
    }

    if (be->writev != (int (*)(void *, uint64_t, const hype_blk_seg_t *, uint32_t))0) {
        int rc = be->writev(be->ctx, lba, segs, nsegs);
        if (rc == 0) {
            hype_blk_wstats_record(&g_wstats, (uint32_t)total);
            g_wstats.vec_writes++;
            g_wstats.vec_segs += (uint64_t)nsegs;
            if (nsegs > g_wstats.vec_max_segs) {
                g_wstats.vec_max_segs = nsegs;
            }
        }
        return rc;
    }

    /* Fallback: N ordinary writes, recorded individually so the histogram reflects the commands
     * the backend actually saw. A failure mid-list leaves earlier segments written, exactly as a
     * device error mid-transfer does -- the caller fails the whole request. */
    for (i = 0; i < nsegs; i++) {
        if (be->write(be->ctx, lba, segs[i].count, segs[i].buf) != 0) {
            return -1;
        }
        hype_blk_wstats_record(&g_wstats, segs[i].count);
        lba += (uint64_t)segs[i].count;
    }
    return 0;
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
    be->writev = 0; /* #295: a memcpy backend gains nothing from vectoring */
    be->ctx = f;
    be->total_sectors = f->total_sectors;
}
