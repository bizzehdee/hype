#include "blk_phys.h"

/* Split a validated [lba, lba+count) transfer into <=MAX_CHUNK-sector hw calls.
 * The dispatcher (hype_blk_backend_read/write) has already bounds-checked the
 * whole range against total_sectors, so every chunk is in bounds too.
 *
 * #332: `lba` arrives SCOPE-relative and base_lba makes it disk-absolute. Added once, before the
 * chunk loop -- adding it per chunk would double-count on the second iteration. */
static int phys_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    hype_blk_phys_t *p = (hype_blk_phys_t *)ctx;
    uint8_t *dst = (uint8_t *)buf;

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
    p->hw = hw;

    be->read = phys_read;
    be->write = (write_sectors != (hype_blk_phys_write_fn)0) ? phys_write : (int (*)(void *, uint64_t, uint32_t, const void *))0;
    be->ctx = p;
    /* The partition's length, NOT the disk's: this is the value the dispatcher bounds-checks the
     * guest against, and it is what confines the guest to the partition. */
    be->total_sectors = sector_count;
}
