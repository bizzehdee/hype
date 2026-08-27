#include "blk_phys.h"

/* Split a validated [lba, lba+count) transfer into <=MAX_CHUNK-sector hw calls.
 * The dispatcher (hype_blk_backend_read/write) has already bounds-checked the
 * whole range against total_sectors, so every chunk is in bounds too.
 *
 * #332: `lba` arrives SCOPE-relative and base_lba makes it disk-absolute. Added once, before the
 * chunk loop -- adding it per chunk would double-count on the second iteration. */
void hype_blk_phys_mark_departed(hype_blk_phys_t *p) {
    if (p != (hype_blk_phys_t *)0) {
        p->departed = 1;
    }
}

int hype_blk_phys_is_departed(const hype_blk_phys_t *p) {
    return (p != (const hype_blk_phys_t *)0) && p->departed;
}

static int phys_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    hype_blk_phys_t *p = (hype_blk_phys_t *)ctx;
    uint8_t *dst = (uint8_t *)buf;

    /* #747: before anything touches the hardware. A read against a departed device is not
     * a slow read, it is a read that will never complete -- and the hw layer's own timeout
     * is measured in seconds, per request, from the guest dispatch loop. */
    if (p->departed) {
        return HYPE_BLK_ERR_GONE;
    }
    lba += p->base_lba;
    while (count > 0u) {
        uint32_t chunk = (count > HYPE_BLK_PHYS_MAX_CHUNK) ? HYPE_BLK_PHYS_MAX_CHUNK : count;
        if (p->read_sectors(p->hw, lba, chunk, dst) != 0) {
            return -1;
        }
        lba += chunk;
        dst += (uint64_t)chunk * HYPE_BLK_SECTOR_SIZE;
        count -= chunk;
    }
    return 0;
}

static int phys_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    hype_blk_phys_t *p = (hype_blk_phys_t *)ctx;
    const uint8_t *src = (const uint8_t *)buf;

    if (p->departed) {
        return HYPE_BLK_ERR_GONE; /* #747 */
    }
    lba += p->base_lba;
    while (count > 0u) {
        uint32_t chunk = (count > HYPE_BLK_PHYS_MAX_CHUNK) ? HYPE_BLK_PHYS_MAX_CHUNK : count;
        if (p->write_sectors(p->hw, lba, chunk, src) != 0) {
            return -1;
        }
        lba += chunk;
        src += (uint64_t)chunk * HYPE_BLK_SECTOR_SIZE;
        count -= chunk;
    }
    return 0;
}

/*
 * #295: the batching half of the vectored write path, pure and injected like the chunk loops
 * above. Contiguity and direction are fixed by the caller's contract (one writev = one contiguous
 * run), so the only decisions left are the hw limits -- take segments while BOTH hold:
 *
 *   - the batch stays within writev_max_segs entries, and
 *   - its total stays within writev_max_sectors.
 *
 * A single segment larger than writev_max_sectors cannot ride any batch; it takes the ordinary
 * chunked scalar path (phys_write's loop, via write_sectors) and the batching resumes after it.
 * base_lba is added ONCE up front, same as phys_read/phys_write -- the #332 lesson.
 */
static int phys_writev(void *ctx, uint64_t lba, const hype_blk_seg_t *segs, uint32_t nsegs) {
    hype_blk_phys_t *p = (hype_blk_phys_t *)ctx;
    uint32_t i = 0;

    if (p->departed) {
        return HYPE_BLK_ERR_GONE; /* #747 */
    }
    lba += p->base_lba;
    while (i < nsegs) {
        uint32_t take = 0;
        uint64_t batch_sectors = 0;

        while (i + take < nsegs && take < p->writev_max_segs &&
               batch_sectors + (uint64_t)segs[i + take].count <= (uint64_t)p->writev_max_sectors) {
            batch_sectors += (uint64_t)segs[i + take].count;
            take++;
        }
        if (take == 0u) {
            /* segs[i] alone exceeds one command's total: chunked scalar path. */
            const uint8_t *src = (const uint8_t *)segs[i].buf;
            uint64_t seg_lba = lba;
            uint32_t left = segs[i].count;
            while (left > 0u) {
                uint32_t chunk = (left > HYPE_BLK_PHYS_MAX_CHUNK) ? HYPE_BLK_PHYS_MAX_CHUNK : left;
                if (p->write_sectors(p->hw, seg_lba, chunk, src) != 0) {
                    return -1;
                }
                seg_lba += chunk;
                src += (uint64_t)chunk * HYPE_BLK_SECTOR_SIZE;
                left -= chunk;
            }
            lba += (uint64_t)segs[i].count;
            i++;
            continue;
        }
        if (take == 1u) {
            /* One segment is one buffer: the scalar hw call is the same command with less setup. */
            if (p->write_sectors(p->hw, lba, segs[i].count, segs[i].buf) != 0) {
                return -1;
            }
        } else if (p->writev_sectors(p->hw, lba, segs + i, take) != 0) {
            return -1;
        }
        lba += batch_sectors;
        i += take;
    }
    return 0;
}

void hype_blk_phys_enable_writev(hype_blk_phys_t *p, hype_blk_backend_t *be,
                                 hype_blk_phys_writev_fn writev_sectors, uint32_t max_segs,
                                 uint32_t max_sectors) {
    if (writev_sectors == (hype_blk_phys_writev_fn)0 || max_segs == 0u || max_sectors == 0u ||
        be->write == (int (*)(void *, uint64_t, uint32_t, const void *))0) {
        return; /* nothing to arm, or a read-only backend -- writev stays NULL */
    }
    p->writev_sectors = writev_sectors;
    p->writev_max_segs = max_segs;
    p->writev_max_sectors = max_sectors;
    be->writev = phys_writev;
}

void hype_blk_phys_init(hype_blk_phys_t *p, hype_blk_backend_t *be,
                        hype_blk_phys_read_fn read_sectors, hype_blk_phys_write_fn write_sectors,
                        void *hw, uint64_t total_sectors) {
    /* One implementation: a whole-disk target is just base_lba 0. */
    hype_blk_phys_init_scoped(p, be, read_sectors, write_sectors, hw, 0u, total_sectors);
}

void hype_blk_phys_init_scoped(hype_blk_phys_t *p, hype_blk_backend_t *be,
                               hype_blk_phys_read_fn read_sectors,
                               hype_blk_phys_write_fn write_sectors, void *hw, uint64_t base_lba,
                               uint64_t sector_count) {
    p->base_lba = base_lba;
    p->read_sectors = read_sectors;
    p->write_sectors = write_sectors;
    p->writev_sectors = 0; /* #295: armed separately by hype_blk_phys_enable_writev */
    p->writev_max_segs = 0;
    p->writev_max_sectors = 0;
    p->hw = hw;
    /* #747: explicitly, not by trusting the caller's allocation. A backend re-init over a
     * struct that had departed must come back present, or an `attach` after a re-plug would
     * silently refuse every I/O -- and #359 is the ticket for what a recycled slot inherits
     * when a field is left to whatever was there before. */
    p->departed = 0;

    be->read = phys_read;
    be->write = (write_sectors != (hype_blk_phys_write_fn)0) ? phys_write : (int (*)(void *, uint64_t, uint32_t, const void *))0;
    be->writev = 0; /* #295: armed separately by hype_blk_phys_enable_writev */
    be->ctx = p;
    /* The partition's length, NOT the disk's: this is the value the dispatcher bounds-checks the
     * guest against, and it is what confines the guest to the partition. */
    be->total_sectors = sector_count;
}
