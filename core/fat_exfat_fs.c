#include "fat_exfat_fs.h"
#include "lebytes.h"

#define SECSZ HYPE_BLK_SECTOR_SIZE
#define ENTSZ HYPE_EXFAT_ENTRY_SIZE
#define FAT_ENTRIES_PER_SECTOR (SECSZ / 4u)
#define BITMAP_BITS_PER_SECTOR (SECSZ * 8u)
#define EOC_MARK 0xFFFFFFFFu

/* exFAT reserves the first 24 sectors of a volume for the main + backup boot
 * regions (12 each), so nothing else may start below that. */
#define BOOT_REGION_SECTORS 24u
#define BACKUP_BOOT_LBA 12u

/* Upper bound on how far any directory / chain walk will go before giving up, so
 * a corrupt (self-referential) FAT chain cannot wedge the hypervisor. */
#define WALK_GUARD (1u << 22)

/* The largest entry set hype builds: File + Stream + 17 File Name entries. */
#define MAX_SET_ENTRIES (HYPE_EXFAT_MAX_SECONDARY + 1u)

static void bcopy(uint8_t *dst, const uint8_t *src, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}
static void bzero(uint8_t *dst, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        dst[i] = 0u;
    }
}

/* ---- cluster + FAT plumbing ---- */

static int cluster_valid(const hype_exfat_fs_t *fs, uint32_t cl) {
    return (cl >= 2u && cl <= fs->cluster_count + 1u) ? 1 : 0;
}

static uint64_t clba(const hype_exfat_fs_t *fs, uint32_t cl) {
    return hype_exfat_cluster_lba(fs->heap_lba, fs->spc, cl);
}

static uint64_t cluster_bytes(const hype_exfat_fs_t *fs) {
    return (uint64_t)fs->spc * SECSZ;
}

/* Clusters needed to hold `bytes`. */
static uint64_t clusters_for(const hype_exfat_fs_t *fs, uint64_t bytes) {
    uint64_t cb = cluster_bytes(fs);
    return (bytes + cb - 1u) / cb;
}

/*
 * #645: load this mount's authoritative view of FAT sector `off` (the FAT-LBA-
 * relative sector index), re-reading from the medium only when a different
 * sector is wanted. Every fat_get/fat_set on one mounted volume goes through
 * this one cached image, exactly as core/fat_write_fs.c's fat_cache_load does
 * for FAT32 -- so a cluster this mount has already written can never read back
 * as something else because the medium served a stale copy.
 */
static int fat_cache_load(hype_exfat_fs_t *fs, uint32_t off) {
    uint64_t slba;
    if (fs->fat_cache_valid && fs->fat_cache_off == off) {
        return 0;
    }
    slba = (uint64_t)fs->fat_lba + off;
    if (fs->read(fs->ctx, slba, 1u, fs->fat_cache) != 0) {
        fs->fat_cache_valid = 0;
        return -1;
    }
    fs->fat_cache_off = off;
    fs->fat_cache_valid = 1;
    return 0;
}

static int fat_get(hype_exfat_fs_t *fs, uint32_t cl, uint32_t *out) {
    uint32_t off = cl / FAT_ENTRIES_PER_SECTOR;
    if (!cluster_valid(fs, cl)) {
        return -1;
    }
    if (fat_cache_load(fs, off) != 0) {
        return -1;
    }
    *out = hype_rd32(fs->fat_cache + (cl % FAT_ENTRIES_PER_SECTOR) * 4u);
    return 0;
}

/*
 * `cl` is always a cluster the caller has already validated -- either freshly
 * allocated or reached through a chain walk that range-checked it -- and only the
 * mutating entry points, which require a write callback, get here.
 *
 * #645: the cache is updated in place and the SAME image is what the next
 * fat_get() reads back, so a stale medium read can never resurrect an older
 * FAT entry within this mount. A write failure invalidates the cache instead
 * of leaving it holding a value that never reached the medium.
 */
static int fat_set(hype_exfat_fs_t *fs, uint32_t cl, uint32_t val) {
    uint32_t off = cl / FAT_ENTRIES_PER_SECTOR;
    if (fat_cache_load(fs, off) != 0) {
        return -1;
    }
    hype_wr32(fs->fat_cache + (cl % FAT_ENTRIES_PER_SECTOR) * 4u, val);
    if (fs->write(fs->ctx, (uint64_t)fs->fat_lba + off, 1u, fs->fat_cache) != 0) {
        fs->fat_cache_valid = 0;
        return -1;
    }
    return 0;
}

/* ---- allocation bitmap ---- */

/* #645: the bitmap's counterpart to fat_cache_load -- `lba` is the absolute
 * sector LBA (the bitmap, unlike the FAT, is addressed by plain sector
 * arithmetic off bitmap_lba, per decision #24), so it is used as the cache key
 * directly. */
static int bitmap_cache_load(hype_exfat_fs_t *fs, uint64_t lba) {
    if (fs->bitmap_cache_valid && fs->bitmap_cache_off == lba) {
        return 0;
    }
    if (fs->read(fs->ctx, lba, 1u, fs->bitmap_cache) != 0) {
        fs->bitmap_cache_valid = 0;
        return -1;
    }
    fs->bitmap_cache_off = lba;
    fs->bitmap_cache_valid = 1;
    return 0;
}

/* Clears one cluster's bitmap bit (allocation goes through alloc_cluster, which
 * already has the right bitmap sector in hand). As with fat_set, `cl` has been
 * range-checked by the caller. */
static int bitmap_release(hype_exfat_fs_t *fs, uint32_t cl) {
    uint64_t lba;
    unsigned int bit;
    hype_exfat_bitmap_location(cl, fs->bitmap_lba, &lba, &bit);
    if (bitmap_cache_load(fs, lba) != 0) {
        return -1;
    }
    hype_exfat_bitmap_set(fs->bitmap_cache, bit, 0);
    if (fs->write(fs->ctx, lba, 1u, fs->bitmap_cache) != 0) {
        fs->bitmap_cache_valid = 0;
        return -1;
    }
    return 0;
}

/* "EXFAT   " -- all eight bytes, so a volume whose name merely starts with those
 * five characters is not mistaken for exFAT. */
static int boot_signature_ok(const uint8_t *sec) {
    static const char sig[8] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};
    unsigned int i;
    for (i = 0; i < 8u; i++) {
        if (sec[3u + i] != (uint8_t)sig[i]) {
            return 0;
        }
    }
    return 1;
}

/*
 * Rewrites VolumeFlags (and, when asked, PercentInUse) in one boot sector.
 * Those two fields sit at the only boot-sector offsets the boot-region checksum
 * deliberately excludes, which is exactly why they can be updated in place
 * without recomputing sector 11.
 */
static int boot_sector_flags(hype_exfat_fs_t *fs, uint64_t lba, int dirty, int refresh_percent) {
    uint8_t sec[SECSZ];
    uint16_t flags;

    if (fs->write == 0) {
        return -1;
    }
    if (fs->read(fs->ctx, lba, 1u, sec) != 0 || !boot_signature_ok(sec)) {
        return -1;
    }
    flags = hype_rd16(sec + 0x6A);
    if (dirty) {
        flags = (uint16_t)(flags | HYPE_EXFAT_VOLUME_DIRTY);
    } else {
        flags = (uint16_t)(flags & (uint16_t)~(uint16_t)HYPE_EXFAT_VOLUME_DIRTY);
    }
    hype_wr16(sec + 0x6A, flags);
    if (refresh_percent && fs->used_clusters != HYPE_EXFAT_USED_UNKNOWN) {
        /* used_clusters is counted out of the bitmap, so it cannot exceed
         * cluster_count and the quotient cannot exceed 100. */
        sec[0x70] = (uint8_t)(((uint64_t)fs->used_clusters * 100u) / fs->cluster_count);
    }
    return fs->write(fs->ctx, lba, 1u, sec);
}

/* Records VolumeDirty on the medium before the first structural change, so an
 * interrupted write is visible to the next mounter rather than silently
 * inconsistent. Also refreshes PercentInUse when `refresh_percent` is set. */
static int boot_flags_write(hype_exfat_fs_t *fs, int dirty, int refresh_percent) {
    if (boot_sector_flags(fs, 0u, dirty, refresh_percent) != 0) {
        return -1;
    }
    /* The backup boot region is optional: a volume without a usable one still
     * gets its main copy flushed. */
    (void)boot_sector_flags(fs, BACKUP_BOOT_LBA, dirty, refresh_percent);
    return 0;
}

static int mark_dirty(hype_exfat_fs_t *fs) {
    if (fs->dirty) {
        return 0;
    }
    if (boot_flags_write(fs, 1, 0) != 0) {
        return -1;
    }
    fs->dirty = 1u;
    return 0;
}

int hype_exfat_fs_sync(hype_exfat_fs_t *fs) {
    if (boot_flags_write(fs, 0, 1) != 0) {
        return -1;
    }
    fs->dirty = 0u;
    return 0;
}

void hype_exfat_fs_set_sync(hype_exfat_fs_t *fs, hype_blk_sync_fn sync) {
    if (fs != (hype_exfat_fs_t *)0) {
        fs->sync = sync;
    }
}

/*
 * Allocates one free cluster: claims its bitmap bit, marks its FAT entry
 * end-of-chain, and advances the search hint. Scans the bitmap a sector at a
 * time (not a bit at a time) starting from the hint, wrapping once.
 *
 * Only reached from the mutating entry points, each of which has already
 * established that `write` is non-NULL; mount guarantees cluster_count >= 1, so
 * `sectors` is always at least 1.
 */
static int alloc_cluster(hype_exfat_fs_t *fs, uint32_t *out) {
    uint32_t sectors = (fs->cluster_count + BITMAP_BITS_PER_SECTOR - 1u) / BITMAP_BITS_PER_SECTOR;
    uint32_t start_bit = fs->next_free - 2u; /* next_free is kept in range below */
    uint32_t pass;

    for (pass = 0; pass <= sectors; pass++) {
        uint32_t s = (start_bit / BITMAP_BITS_PER_SECTOR + pass) % sectors;
        unsigned int from = (pass == 0u) ? (start_bit % BITMAP_BITS_PER_SECTOR) : 0u;
        unsigned int limit = BITMAP_BITS_PER_SECTOR;
        uint64_t lba = fs->bitmap_lba + s;
        if (s == sectors - 1u) {
            limit = fs->cluster_count - s * BITMAP_BITS_PER_SECTOR;
        }
        /* `from` is always inside `limit`: next_free never exceeds the last valid
         * cluster, so its bit index never reaches the end of its own sector. */
        if (bitmap_cache_load(fs, lba) != 0) {
            return -1;
        }
        for (;;) {
            unsigned int bit;
            uint32_t cl, fv;
            if (hype_exfat_bitmap_find_free(fs->bitmap_cache, from, limit, &bit) != 0) {
                break; /* no more clear bits in this sector */
            }
            cl = 2u + s * BITMAP_BITS_PER_SECTOR + bit;
            /*
             * #645: the bitmap used to be the ONLY source of truth an allocation
             * consulted. A bitmap that has drifted from the FAT -- its
             * chain-of-record -- because of a stale medium read, or simple
             * corruption, would then hand out a cluster something else still
             * chains through. Cross-check the FAT before committing to this
             * cluster; a non-zero entry means fail CLOSED (skip it and keep
             * scanning), never hand it out anyway.
             */
            if (fat_get(fs, cl, &fv) != 0) {
                return -1;
            }
            if (fv != 0u) {
                from = bit + 1u;
                continue;
            }
            hype_exfat_bitmap_set(fs->bitmap_cache, bit, 1);
            if (fs->write(fs->ctx, lba, 1u, fs->bitmap_cache) != 0) {
                fs->bitmap_cache_valid = 0;
                return -1;
            }
            if (fat_set(fs, cl, EOC_MARK) != 0) {
                return -1;
            }
            if (fs->used_clusters != HYPE_EXFAT_USED_UNKNOWN) {
                fs->used_clusters++;
            }
            fs->next_free = (cl + 1u > fs->cluster_count + 1u) ? 2u : (cl + 1u);
            *out = cl;
            return 0;
        }
    }
    return -1; /* volume full */
}

static int free_cluster(hype_exfat_fs_t *fs, uint32_t cl) {
    if (fat_set(fs, cl, 0u) != 0) {
        return -1;
    }
    if (bitmap_release(fs, cl) != 0) {
        return -1;
    }
    if (fs->used_clusters != HYPE_EXFAT_USED_UNKNOWN && fs->used_clusters != 0u) {
        fs->used_clusters--;
    }
    if (cl < fs->next_free) {
        fs->next_free = cl;
    }
    return 0;
}

/* Releases a whole allocation. A contiguous (NoFatChain) allocation has no FAT
 * chain to follow, so its length comes from the byte size instead. */
static int free_allocation(hype_exfat_fs_t *fs, uint32_t first, int contiguous, uint64_t size) {
    if (first == 0u) {
        return 0;
    }
    if (contiguous) {
        /* set_read has already established that this run stays inside the heap. */
        uint64_t n = clusters_for(fs, size);
        uint64_t i;
        for (i = 0; i < n; i++) {
            if (free_cluster(fs, first + (uint32_t)i) != 0) {
                return -1;
            }
        }
        return 0;
    }
    {
        uint32_t cl = first;
        unsigned int guard = 0;
        while (cluster_valid(fs, cl) && guard++ < WALK_GUARD) {
            uint32_t next;
            if (fat_get(fs, cl, &next) != 0) {
                return -1;
            }
            if (free_cluster(fs, cl) != 0) {
                return -1;
            }
            if (next >= HYPE_EXFAT_EOC) {
                return 0;
            }
            cl = next;
        }
        return -1; /* chain left the volume or looped */
    }
}

/* ---- directory-entry access ---- */

/* Cluster at index `index` of an allocation. */
static int chain_cluster_at(hype_exfat_fs_t *fs, uint32_t first, int contiguous, uint32_t index,
                            uint32_t *out) {
    if (contiguous) {
        uint32_t cl = first + index;
        if (!cluster_valid(fs, cl)) {
            return -1; /* the run does not extend this far */
        }
        *out = cl;
        return 0;
    }
    {
        uint32_t cl = first;
        uint32_t i;
        for (i = 0; i < index; i++) {
            uint32_t next;
            if (fat_get(fs, cl, &next) != 0 || next >= HYPE_EXFAT_EOC) {
                return -1;
            }
            cl = next;
        }
        if (!cluster_valid(fs, cl)) {
            return -1;
        }
        *out = cl;
        return 0;
    }
}

/*
 * #647: walks and validates a FAT-chained allocation against `size` -- the DataLength rule: a
 * chain shorter than ceil(size / cluster_bytes) is corruption (exFAT has no representation for an
 * internal hole either, same as FAT32), and a chain longer than that is a loop, a cross-link, or
 * slack clusters. Bounded by the size-derived need, not WALK_GUARD, so a corrupt chain fails after
 * `need` steps rather than iterating up to WALK_GUARD times -- no visited set required, since a
 * loop or a cross-link back into any chain simply never reaches end-of-chain within `need` steps.
 * Refuses a free (0), reserved (1), bad (0xFFFFFFF7) or out-of-heap cluster mid-chain via the same
 * cluster_valid() range check chain_cluster_at() uses. Returns the tail cluster in *out_tail.
 * Mirrors FAT32's chain_measure (core/fat_write_fs.c).
 */
static int chain_measure(hype_exfat_fs_t *fs, uint32_t first, uint64_t size, uint32_t *out_tail) {
    uint64_t need = clusters_for(fs, size);
    uint32_t cl = first;
    uint32_t tail = 0u;
    uint64_t count = 0u;

    if (first == 0u) {
        if (size != 0u) {
            return -1; /* a non-empty file must have a chain */
        }
        *out_tail = 0u;
        return 0;
    }
    if (!cluster_valid(fs, cl)) {
        return -1;
    }
    for (;;) {
        uint32_t next;
        count++;
        if (count > need) {
            return -1; /* loop, cross-link, or slack clusters */
        }
        tail = cl;
        if (fat_get(fs, cl, &next) != 0) {
            return -1;
        }
        if (next >= HYPE_EXFAT_EOC) {
            break;
        }
        if (!cluster_valid(fs, next)) {
            return -1; /* free, reserved, bad, or out of the heap */
        }
        cl = next;
    }
    if (count < need) {
        return -1; /* shorter than the recorded size */
    }
    *out_tail = tail;
    return 0;
}

/*
 * #647: the contiguous (NoFatChain) counterpart of chain_measure. set_read() already range-checks
 * the whole run against the heap; this additionally refuses a run where any cluster's allocation-
 * bitmap bit reads clear, so a contiguous stream this mount never actually allocated (or one whose
 * bitmap has drifted, the #645 class of disagreement) is not accepted as this file's data.
 */
static int contiguous_run_all_used(hype_exfat_fs_t *fs, uint32_t first, uint64_t size) {
    uint64_t n = clusters_for(fs, size);
    uint64_t i;

    for (i = 0; i < n; i++) {
        uint32_t cl = first + (uint32_t)i;
        uint64_t lba;
        unsigned int bit;
        hype_exfat_bitmap_location(cl, fs->bitmap_lba, &lba, &bit);
        if (bitmap_cache_load(fs, lba) != 0) {
            return -1;
        }
        if (!hype_exfat_bitmap_get(fs->bitmap_cache, bit)) {
            return -1; /* marked free: not a genuine allocation */
        }
    }
    return 0;
}

static int entry_lba(hype_exfat_fs_t *fs, uint32_t dir_first, int dir_contig, uint32_t ei,
                     uint64_t *out_lba, unsigned int *out_off) {
    uint32_t ci, sic, cl;
    hype_exfat_entry_pos(ei, fs->spc, &ci, &sic, out_off);
    if (chain_cluster_at(fs, dir_first, dir_contig, ci, &cl) != 0) {
        return -1;
    }
    *out_lba = clba(fs, cl) + sic;
    return 0;
}

static int entry_read(hype_exfat_fs_t *fs, uint32_t dir_first, int dir_contig, uint32_t ei,
                      uint8_t ent[ENTSZ]) {
    uint8_t sec[SECSZ];
    uint64_t lba;
    unsigned int off;
    if (entry_lba(fs, dir_first, dir_contig, ei, &lba, &off) != 0) {
        return -1;
    }
    if (fs->read(fs->ctx, lba, 1u, sec) != 0) {
        return -1;
    }
    bcopy(ent, sec + off, ENTSZ);
    return 0;
}

static int entry_write(hype_exfat_fs_t *fs, uint32_t dir_first, int dir_contig, uint32_t ei,
                       const uint8_t ent[ENTSZ]) {
    uint8_t sec[SECSZ];
    uint64_t lba;
    unsigned int off;
    if (entry_lba(fs, dir_first, dir_contig, ei, &lba, &off) != 0) {
        return -1;
    }
    if (fs->read(fs->ctx, lba, 1u, sec) != 0) {
        return -1;
    }
    bcopy(sec + off, ent, ENTSZ);
    return fs->write(fs->ctx, lba, 1u, sec);
}

/* ---- entry sets ---- */

/* Context for the shared hype_exfat_set_read() entry-fetch callback. */
typedef struct {
    hype_exfat_fs_t *fs;
    uint32_t dir_first;
    int dir_contig;
} set_ctx_t;

static int set_ctx_read(void *ctx, uint32_t ei, uint8_t ent[ENTSZ]) {
    set_ctx_t *c = (set_ctx_t *)ctx;
    return entry_read(c->fs, c->dir_first, c->dir_contig, ei, ent);
}

/*
 * Reads the entry set whose File entry sits at `ei` (structural validation --
 * checksum, entry types, name length -- in hype_exfat_set_read), then range-checks
 * its allocation against this volume's cluster heap. Returns 0 on a set that is
 * safe to act on, -1 otherwise.
 */
static int set_read(hype_exfat_fs_t *fs, uint32_t dir_first, int dir_contig, uint32_t ei,
                    hype_exfat_set_t *set) {
    set_ctx_t ctx;
    ctx.fs = fs;
    ctx.dir_first = dir_first;
    ctx.dir_contig = dir_contig;
    if (hype_exfat_set_read(set_ctx_read, &ctx, ei, set) != 0) {
        return -1;
    }
    /* An allocation must lie inside the cluster heap. A zero first cluster is
     * only legal for a zero-length stream. */
    if (set->first_cluster == 0u) {
        if (set->data_length != 0u) {
            return -1;
        }
    } else {
        uint64_t need = clusters_for(fs, set->data_length);
        if (!cluster_valid(fs, set->first_cluster)) {
            return -1;
        }
        if (need > (uint64_t)fs->cluster_count) {
            return -1;
        }
        if (set->contiguous &&
            (uint64_t)set->first_cluster + need > (uint64_t)fs->cluster_count + 2u) {
            return -1;
        }
    }
    return 0;
}

/*
 * Rewrites the stream entry's allocation fields and the set's checksum.
 *
 * #648: `durable` is set by the caller exactly when this call extended the
 * allocation -- a new cluster's FAT link must be durable BEFORE the entry set
 * that exposes it commits (plan.md decision 56, the same ordering #377 gave
 * FAT32's flush_metadata). The barrier brackets the whole entry-set update:
 * once before the first write, once after the last, matching
 * core/fat_write_fs.c:405-408 and :439. A call that only advances
 * ValidDataLength inside an already-published allocation stays non-durable.
 */
static int set_flush(hype_exfat_wfile_t *f, int durable) {
    hype_exfat_fs_t *fs = f->fs;
    uint8_t ent[ENTSZ];
    uint16_t sum = 0u;
    unsigned int k;

    if (durable && fs->sync != (hype_blk_sync_fn)0 && fs->sync(fs->ctx) != 0) {
        return -1;
    }

    /* set_read (via lookup) and create both establish that set_index + 1 is this
     * set's Stream Extension entry before a handle exists at all. */
    if (entry_read(fs, f->dir_cluster, f->dir_contiguous, f->set_index + 1u, ent) != 0) {
        return -1;
    }
    ent[1] = (uint8_t)(HYPE_EXFAT_FLAG_ALLOC_POSSIBLE |
                       (f->contiguous ? HYPE_EXFAT_FLAG_NO_FAT_CHAIN : 0u));
    if (f->valid > f->size) {
        return -1; /* never publish a valid prefix past the data length */
    }
    hype_wr64(ent + 8, f->valid); /* ValidDataLength (#383) */
    hype_wr32(ent + 20, f->first_cluster);
    hype_wr64(ent + 24, f->size);
    if (entry_write(fs, f->dir_cluster, f->dir_contiguous, f->set_index + 1u, ent) != 0) {
        return -1;
    }
    for (k = 0; k <= f->secondary; k++) {
        if (entry_read(fs, f->dir_cluster, f->dir_contiguous, f->set_index + k, ent) != 0) {
            return -1;
        }
        sum = hype_exfat_set_checksum_update(sum, k * ENTSZ, ent, ENTSZ);
    }
    if (entry_read(fs, f->dir_cluster, f->dir_contiguous, f->set_index, ent) != 0) {
        return -1;
    }
    hype_exfat_file_entry_set_checksum(ent, sum);
    if (entry_write(fs, f->dir_cluster, f->dir_contiguous, f->set_index, ent) != 0) {
        return -1;
    }
    if (durable && fs->sync != (hype_blk_sync_fn)0 && fs->sync(fs->ctx) != 0) {
        return -1;
    }
    return 0;
}

/* ---- mount ---- */

/*
 * Reads a whole stream (bitmap / up-case table) sector by sector, following its
 * FAT chain, and reports whether the clusters turned out physically contiguous.
 * Neither structure has a NoFatChain flag of its own -- both are always
 * described by the FAT -- so contiguity has to be measured, not assumed.
 */
static int stream_scan(hype_exfat_fs_t *fs, uint32_t first, uint64_t bytes, int want_upcase,
                       int *out_contiguous) {
    uint64_t remaining = bytes;
    uint32_t cl = first;
    uint32_t prev = 0u;
    unsigned int guard = 0;
    int contiguous = 1;

    /* `bytes` is non-zero: the caller validates each structure's DataLength
     * against its minimum before scanning it. */
    while (remaining > 0u && guard++ < WALK_GUARD) {
        unsigned int s;
        if (!cluster_valid(fs, cl)) {
            return -1;
        }
        if (prev != 0u && cl != prev + 1u) {
            contiguous = 0;
        }
        for (s = 0; s < fs->spc && remaining > 0u; s++) {
            uint8_t sec[SECSZ];
            unsigned int n = (remaining < SECSZ) ? (unsigned int)remaining : SECSZ;
            if (fs->read(fs->ctx, clba(fs, cl) + s, 1u, sec) != 0) {
                return -1;
            }
            if (want_upcase) {
                hype_exfat_upcase_feed(&fs->upcase, sec, n);
            }
            remaining -= n;
        }
        if (remaining == 0u) {
            break;
        }
        prev = cl;
        if (fat_get(fs, cl, &cl) != 0 || cl >= HYPE_EXFAT_EOC) {
            return -1; /* the stream ends before its DataLength says it should */
        }
    }
    if (remaining != 0u) {
        return -1; /* the walk guard tripped: a chain long enough to be a loop */
    }
    if (out_contiguous) {
        *out_contiguous = contiguous;
    }
    return 0;
}

/* Counts allocated clusters straight out of the bitmap, so PercentInUse can be
 * kept honest. Left unknown on a bitmap too large to sweep cheaply. */
static void count_used(hype_exfat_fs_t *fs) {
    uint32_t sectors = (fs->cluster_count + BITMAP_BITS_PER_SECTOR - 1u) / BITMAP_BITS_PER_SECTOR;
    uint32_t s;
    uint32_t used = 0u;

    fs->used_clusters = HYPE_EXFAT_USED_UNKNOWN;
    if (sectors > HYPE_EXFAT_MAX_BITMAP_SCAN) {
        return; /* too large to sweep cheaply: leave the total unknown */
    }
    for (s = 0; s < sectors; s++) {
        uint8_t sec[SECSZ];
        unsigned int bits = BITMAP_BITS_PER_SECTOR;
        if (s == sectors - 1u) {
            bits = fs->cluster_count - s * BITMAP_BITS_PER_SECTOR;
        }
        if (fs->read(fs->ctx, fs->bitmap_lba + s, 1u, sec) != 0) {
            return;
        }
        used += hype_exfat_bitmap_count(sec, bits);
    }
    fs->used_clusters = used;
}

int hype_exfat_fs_mount(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                        hype_exfat_fs_t *out) {
    uint8_t boot[SECSZ];
    uint8_t num_fats;
    uint16_t volume_flags;
    uint32_t bitmap_cluster = 0u;
    uint64_t bitmap_bytes = 0u;
    uint32_t upcase_checksum = 0u;
    uint32_t ei;
    int have_bitmap = 0;
    int have_upcase = 0;
    int contiguous = 0;

    /* Same reason as the FAT32 mount: an uninitialised snapshot would produce
     * random timestamps in entry sets. Invalid until set_time() supplies one. */
    out->now.year = 0;

    if (read(ctx, 0u, 1u, boot) != 0) {
        return -1;
    }
    if (!boot_signature_ok(boot)) {
        return -1;
    }
    if (boot[0x6C] != 9u) {
        return -1; /* only 512-byte logical sectors */
    }
    if (boot[0x6D] > 16u) {
        return -1; /* > 32 MiB clusters: implausible, and keeps 1u << shift sane */
    }
    num_fats = boot[0x6E];
    if (num_fats != 1u && num_fats != 2u) {
        return -1;
    }
    volume_flags = hype_rd16(boot + 0x6A);

    out->read = read;
    out->write = write;
    out->sync = (hype_blk_sync_fn)0;
    out->ctx = ctx;
    out->volume_length = hype_rd64(boot + 0x48);
    out->fat_length = hype_rd32(boot + 0x54);
    out->heap_lba = hype_rd32(boot + 0x58);
    out->cluster_count = hype_rd32(boot + 0x5C);
    out->root_cluster = hype_rd32(boot + 0x60);
    out->spc = 1u << boot[0x6D];
    out->next_free = 2u;
    out->used_clusters = HYPE_EXFAT_USED_UNKNOWN;
    out->dirty = (uint8_t)((volume_flags & HYPE_EXFAT_VOLUME_DIRTY) ? 1u : 0u);
    out->fat_cache_valid = 0;
    out->fat_cache_off = 0u;
    out->bitmap_cache_valid = 0;
    out->bitmap_cache_off = 0u;
    hype_exfat_upcase_reset(&out->upcase);

    /* With two FATs, VolumeFlags bit 0 selects the live one; reading the stale
     * copy would follow chains that no longer exist. */
    out->fat_lba = hype_rd32(boot + 0x50);
    if (num_fats == 2u && (volume_flags & 0x0001u) != 0u) {
        out->fat_lba += out->fat_length;
    }

    if (out->fat_length == 0u || hype_rd32(boot + 0x50) < BOOT_REGION_SECTORS) {
        return -1;
    }
    if (out->heap_lba < hype_rd32(boot + 0x50) + (uint32_t)num_fats * out->fat_length) {
        return -1; /* the heap would overlap the FAT region */
    }
    if (out->cluster_count == 0u || out->cluster_count > 0xFFFFFFF4u) {
        return -1;
    }
    /* The FAT has to be able to address every cluster (entries 0 and 1 are
     * reserved), otherwise a chain walk would read another structure's bytes. */
    if ((uint64_t)out->fat_length * FAT_ENTRIES_PER_SECTOR < (uint64_t)out->cluster_count + 2u) {
        return -1;
    }
    if ((uint64_t)out->heap_lba + (uint64_t)out->cluster_count * out->spc > out->volume_length) {
        return -1;
    }
    if (!cluster_valid(out, out->root_cluster)) {
        return -1;
    }

    /*
     * The root directory carries the volume's critical structures. Both are
     * mandatory: without the bitmap nothing can be allocated, and without a
     * checksum-verified up-case table names cannot be compared or hashed the way
     * every other exFAT implementation would.
     */
    for (ei = 0; ei < WALK_GUARD; ei++) {
        uint8_t ent[ENTSZ];
        if (entry_read(out, out->root_cluster, 0, ei, ent) != 0) {
            break; /* end of the root directory's chain */
        }
        if (ent[0] == 0x00u) {
            break; /* end-of-directory marker */
        }
        if ((ent[0] & HYPE_EXFAT_ENT_INUSE) == 0u) {
            continue; /* deleted entry (mkfs.exfat leaves an unused 0x20 here) */
        }
        if (ent[0] == HYPE_EXFAT_ENT_BITMAP && !have_bitmap) {
            /* With two FATs there are two bitmaps; BitmapFlags bit 0 names which
             * FAT a bitmap belongs to, so take the one matching the active FAT. */
            unsigned int which = (unsigned int)(ent[1] & 0x01u);
            unsigned int active = (num_fats == 2u) ? (unsigned int)(volume_flags & 0x0001u) : 0u;
            if (which != active) {
                continue;
            }
            bitmap_cluster = hype_rd32(ent + 20);
            bitmap_bytes = hype_rd64(ent + 24);
            have_bitmap = 1;
            continue;
        }
        if (ent[0] == HYPE_EXFAT_ENT_UPCASE && !have_upcase) {
            out->upcase_cluster = hype_rd32(ent + 20);
            out->upcase_bytes = hype_rd64(ent + 24);
            upcase_checksum = hype_rd32(ent + 4);
            have_upcase = 1;
            continue;
        }
    }
    if (!have_bitmap || !have_upcase) {
        return -1;
    }
    if (bitmap_bytes < ((uint64_t)out->cluster_count + 7u) / 8u) {
        return -1; /* the bitmap cannot describe every cluster */
    }
    out->bitmap_bytes = bitmap_bytes;
    /* bitmap_lba is only meaningful once the bitmap is known contiguous, which
     * stream_scan below establishes; set it first so it can be scanned. */
    out->bitmap_lba = clba(out, bitmap_cluster);
    if (stream_scan(out, bitmap_cluster, bitmap_bytes, 0, &contiguous) != 0) {
        return -1;
    }
    if (!contiguous) {
        return -1; /* a fragmented allocation bitmap: no formatter produces one,
                    * and hype indexes it by plain sector arithmetic */
    }
    if (out->upcase_bytes < 2u || out->upcase_bytes > 0x20000u || (out->upcase_bytes & 1u) != 0u) {
        return -1; /* an up-case table is a whole number of 16-bit entries */
    }
    if (stream_scan(out, out->upcase_cluster, out->upcase_bytes, 1, 0) != 0) {
        return -1;
    }
    if (out->upcase.malformed || out->upcase.checksum != upcase_checksum) {
        return -1; /* refuse a table hype would up-case names differently from */
    }
    count_used(out);
    return 0;
}

/* ---- name handling ---- */

static unsigned int path_component(const char *path, unsigned int *pos, uint16_t *out,
                                   unsigned int cap, int *out_overflow) {
    unsigned int n = 0;
    *out_overflow = 0;
    while (path[*pos] == '\\' || path[*pos] == '/') {
        (*pos)++;
    }
    while (path[*pos] != '\0' && path[*pos] != '\\' && path[*pos] != '/') {
        if (n < cap) {
            out[n] = (uint16_t)(unsigned char)path[*pos];
        } else {
            *out_overflow = 1;
        }
        n++;
        (*pos)++;
    }
    return n;
}

/* Case-insensitive UTF-16 comparison through the volume's up-case table. */
static int name_eq(const hype_exfat_fs_t *fs, const uint16_t *a, const uint16_t *b,
                   unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        if (hype_exfat_upcase(&fs->upcase, a[i]) != hype_exfat_upcase(&fs->upcase, b[i])) {
            return 0;
        }
    }
    return 1;
}

/*
 * Finds `name` in the directory starting at `dir_first`. Returns 1 and fills
 * *out_index / *set on a match, 0 if the directory ends without one, -1 on a
 * read error. Entry sets that fail validation are skipped -- a single corrupt
 * set must not hide the rest of the directory.
 */
static int dir_find(hype_exfat_fs_t *fs, uint32_t dir_first, int dir_contig, const uint16_t *name,
                    unsigned int nlen, uint32_t *out_index, hype_exfat_set_t *set) {
    uint32_t ei = 0;
    unsigned int guard = 0;

    while (guard++ < WALK_GUARD) {
        uint8_t ent[ENTSZ];
        if (entry_read(fs, dir_first, dir_contig, ei, ent) != 0) {
            return 0; /* ran off the end of the directory's allocation */
        }
        if (ent[0] == 0x00u) {
            return 0;
        }
        if ((ent[0] & HYPE_EXFAT_ENT_INUSE) == 0u || ent[0] != HYPE_EXFAT_ENT_FILE) {
            ei++;
            continue;
        }
        if (set_read(fs, dir_first, dir_contig, ei, set) != 0) {
            ei++; /* malformed set: step over its File entry and keep looking */
            continue;
        }
        if (set->name_length == nlen && name_eq(fs, set->name, name, nlen)) {
            *out_index = ei;
            return 1;
        }
        ei += 1u + set->secondary;
    }
    return -1;
}

/* Fills a handle from a located entry set. */
static void wfile_from_set(hype_exfat_wfile_t *f, hype_exfat_fs_t *fs, uint32_t dir_first,
                           int dir_contig, uint32_t ei, const hype_exfat_set_t *set) {
    f->fs = fs;
    f->dir_cluster = dir_first;
    f->dir_contiguous = (uint8_t)(dir_contig ? 1 : 0);
    f->set_index = ei;
    f->secondary = set->secondary;
    f->first_cluster = set->first_cluster;
    f->tail_cluster = 0u; /* resolved lazily: walking a long chain is not free */
    f->size = set->data_length;
    f->valid = set->valid_length;
    f->contiguous = set->contiguous;
    f->is_dir = (uint8_t)((set->attributes & HYPE_EXFAT_ATTR_DIRECTORY) ? 1 : 0);
    f->seek_index = 0u;
    f->seek_cluster = set->first_cluster;
}

int hype_exfat_lookup(hype_exfat_fs_t *fs, const char *path, int want_dir,
                      hype_exfat_wfile_t *out) {
    uint16_t comp[HYPE_EXFAT_MAX_NAME];
    hype_exfat_set_t set;
    uint32_t dir_first = fs->root_cluster;
    int dir_contig = 0; /* the root directory has no stream entry: always chained */
    unsigned int pos = 0;

    for (;;) {
        unsigned int nlen;
        uint32_t ei = 0;
        int overflow = 0;
        int rc;
        int last;

        nlen = path_component(path, &pos, comp, HYPE_EXFAT_MAX_NAME, &overflow);
        if (nlen == 0u || overflow) {
            return -1; /* empty path component, or a name longer than exFAT allows */
        }
        last = (path[pos] == '\0') ? 1 : 0;
        if (!last) {
            unsigned int peek = pos;
            uint16_t tmp[1];
            int ovf2 = 0;
            /* A trailing separator ("\dir\") still means the component is last. */
            last = (path_component(path, &peek, tmp, 1u, &ovf2) == 0u) ? 1 : 0;
        }

        rc = dir_find(fs, dir_first, dir_contig, comp, nlen, &ei, &set);
        if (rc <= 0) {
            return -1;
        }
        if (last) {
            int is_dir = (set.attributes & HYPE_EXFAT_ATTR_DIRECTORY) ? 1 : 0;
            uint32_t tail = 0u;
            if (is_dir != (want_dir ? 1 : 0)) {
                return -1;
            }
            /*
             * #647: a FILE handle is writable random-I/O, exactly like FAT32's hype_fat32_open, so
             * its complete chain is validated against DataLength before a handle is returned --
             * refusing a short chain (would fail mid-read/write at an arbitrary offset instead of
             * at open), a long chain (a loop or cross-link, walked to WALK_GUARD otherwise), and a
             * contiguous run with a cluster the bitmap says is not actually allocated. Directories
             * are addressed through their own already-bounded walks (dir_find, dir_scan_slots,
             * dir_is_empty), so this does not apply to them.
             */
            if (!is_dir) {
                if (set.contiguous) {
                    if (contiguous_run_all_used(fs, set.first_cluster, set.data_length) != 0) {
                        return -1;
                    }
                    tail = (set.first_cluster == 0u)
                               ? 0u
                               : set.first_cluster +
                                     (uint32_t)(clusters_for(fs, set.data_length) - 1u);
                } else if (chain_measure(fs, set.first_cluster, set.data_length, &tail) != 0) {
                    return -1;
                }
            }
            wfile_from_set(out, fs, dir_first, dir_contig, ei, &set);
            if (!is_dir) {
                out->tail_cluster = tail; /* already resolved: no lazy walk needed */
            }
            return 0;
        }
        if ((set.attributes & HYPE_EXFAT_ATTR_DIRECTORY) == 0u) {
            return -1; /* a non-final component is not a directory */
        }
        /* set_read has already range-checked this allocation against the heap. */
        dir_first = set.first_cluster;
        dir_contig = set.contiguous;
    }
}

/* ---- directory references ---- */

/*
 * A directory hype is about to insert into, scan, or grow -- together with
 * where that directory's OWN entry set lives. Growing a subdirectory changes
 * its DataLength (and, if its allocation was NoFatChain, its flags), and both
 * live in its parent's entry set; the root directory is described only by the
 * boot sector and has no such set, hence `has_owner`.
 */
typedef struct {
    uint32_t first;    /* first cluster of the directory itself */
    uint64_t size;     /* its DataLength (unused for the root directory) */
    uint8_t contiguous; /* 1 == NoFatChain */
    uint8_t has_owner; /* 0 == the root directory */
    uint32_t owner_dir; /* directory holding this directory's entry set */
    uint8_t owner_contig;
    uint32_t owner_set; /* entry index of its File entry there */
    uint8_t owner_secondary;
} dirref_t;

static void dirref_root(const hype_exfat_fs_t *fs, dirref_t *d) {
    d->first = fs->root_cluster;
    d->size = 0u;
    d->contiguous = 0u;
    d->has_owner = 0u;
    d->owner_dir = 0u;
    d->owner_contig = 0u;
    d->owner_set = 0u;
    d->owner_secondary = 0u;
}

/* Writes the directory's current allocation back into its own entry set (a
 * no-op for the root, which has none). */
static int dirref_flush(hype_exfat_fs_t *fs, const dirref_t *d) {
    hype_exfat_wfile_t f;
    if (!d->has_owner) {
        return 0;
    }
    f.fs = fs;
    f.dir_cluster = d->owner_dir;
    f.dir_contiguous = d->owner_contig;
    f.set_index = d->owner_set;
    f.secondary = d->owner_secondary;
    f.first_cluster = d->first;
    f.size = d->size;
    /* A directory has no ValidDataLength concept of its own -- every byte of
     * its allocation is meaningful -- so this is always the whole size. Left
     * unset, set_flush()'s `valid > size` guard reads uninitialised stack. */
    f.valid = d->size;
    f.contiguous = d->contiguous;
    /* A directory's own allocation growth is out of #648's scope (the ticket
     * covers file DataLength publication); non-durable preserves prior
     * behaviour here. */
    return set_flush(&f, 0);
}

/* ---- path handling ----
 *
 * Every mutating entry point takes a full path. path_split finds the final
 * component; resolve_parent walks everything before it. Names being LOOKED UP
 * (unlink/rmdir/rename's source, and existence checks) are widened as-is, the
 * same way lookup treats them; names being WRITTEN go through name_prepare,
 * which enforces exFAT's character rules and the volume's up-case coverage.
 */

/* Byte offset of the final path component. -1 if there is none ("", "\"). A
 * trailing separator is accepted and ignored. */
static int path_split(const char *path, unsigned int *out_leaf) {
    unsigned int i;
    unsigned int leaf = 0;
    int have = 0;
    int in_sep = 1;
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '\\' || path[i] == '/') {
            in_sep = 1;
        } else {
            if (in_sep) {
                leaf = i;
                have = 1;
            }
            in_sep = 0;
        }
    }
    if (!have) {
        return -1;
    }
    *out_leaf = leaf;
    return 0;
}

/* Copies the final component (which starts at byte `leaf`) into a
 * NUL-terminated buffer for the validating name encoder. */
static int leaf_string(const char *path, unsigned int leaf, char *out, unsigned int cap) {
    unsigned int n = 0;
    while (path[leaf + n] != '\0' && path[leaf + n] != '\\' && path[leaf + n] != '/') {
        if (n + 1u >= cap) {
            return -1;
        }
        out[n] = path[leaf + n];
        n++;
    }
    out[n] = '\0';
    return 0;
}

/* Widens the final component for a directory search (no validation: this is a
 * name that already exists on the medium). Returns the length, 0 on error. */
static unsigned int leaf_component(const char *path, unsigned int leaf,
                                   uint16_t out[HYPE_EXFAT_MAX_NAME]) {
    unsigned int pos = leaf;
    int overflow = 0;
    unsigned int n = path_component(path, &pos, out, HYPE_EXFAT_MAX_NAME, &overflow);
    return overflow ? 0u : n;
}

/*
 * Resolves the parent directory of the entry `path` names: walks every
 * component before the final one (at byte `leaf`). When `forbid` is a non-zero
 * cluster, a walk that passes through the directory starting at that cluster
 * fails -- rename uses this to refuse moving a directory into itself or a
 * descendant of itself. Returns 0 on success, -1 otherwise.
 */
static int resolve_parent(hype_exfat_fs_t *fs, const char *path, unsigned int leaf,
                          uint32_t forbid, dirref_t *dir) {
    dirref_root(fs, dir);
    {
        unsigned int pos = 0;
        for (;;) {
            uint16_t comp[HYPE_EXFAT_MAX_NAME];
            hype_exfat_set_t set;
            unsigned int nlen;
            int overflow = 0;
            uint32_t ei = 0;

            while (path[pos] == '\\' || path[pos] == '/') {
                pos++;
            }
            if (pos >= leaf) {
                return 0; /* everything before the final component resolved */
            }
            nlen = path_component(path, &pos, comp, HYPE_EXFAT_MAX_NAME, &overflow);
            if (nlen == 0u || overflow) {
                return -1;
            }
            if (dir_find(fs, dir->first, dir->contiguous, comp, nlen, &ei, &set) != 1) {
                return -1;
            }
            if ((set.attributes & HYPE_EXFAT_ATTR_DIRECTORY) == 0u) {
                return -1; /* a non-final component is not a directory */
            }
            if (forbid != 0u && set.first_cluster == forbid) {
                return -1;
            }
            /* The current directory becomes the owner of the next one. */
            dir->owner_dir = dir->first;
            dir->owner_contig = dir->contiguous;
            dir->owner_set = ei;
            dir->owner_secondary = set.secondary;
            dir->has_owner = 1u;
            dir->first = set.first_cluster;
            dir->size = set.data_length;
            dir->contiguous = set.contiguous;
        }
    }
}

/* Encodes and validates a name hype is about to WRITE: exFAT's forbidden
 * characters, and exact up-case coverage -- a NameHash built on a guessed fold
 * would disagree with every other implementation's. */
static int name_prepare(hype_exfat_fs_t *fs, const char *name,
                        uint16_t chars[HYPE_EXFAT_MAX_NAME], unsigned int *out_len,
                        uint16_t *out_hash) {
    uint16_t upcased[HYPE_EXFAT_MAX_NAME];
    int nlen_signed = hype_exfat_name_to_utf16(name, chars, HYPE_EXFAT_MAX_NAME);
    unsigned int i, nlen;
    if (nlen_signed <= 0) {
        return -1;
    }
    nlen = (unsigned int)nlen_signed;
    for (i = 0; i < nlen; i++) {
        if (!hype_exfat_upcase_exact(&fs->upcase, chars[i])) {
            return -1; /* the volume's table does not cover this character, so the
                        * NameHash hype wrote would not match another driver's */
        }
        upcased[i] = hype_exfat_upcase(&fs->upcase, chars[i]);
    }
    *out_hash = hype_exfat_name_hash_update(0u, upcased, nlen);
    *out_len = nlen;
    return 0;
}

/* ---- entry-set placement, removal ---- */

/* Retires a whole entry set: clears the InUse bit on EVERY entry of the set,
 * not only the primary, so no stale secondary is left looking like part of a
 * neighbouring set. */
static int set_delete(hype_exfat_fs_t *fs, uint32_t dir_first, int dir_contig, uint32_t ei,
                      uint8_t secondary) {
    unsigned int k;
    for (k = 0; k <= secondary; k++) {
        uint8_t ent[ENTSZ];
        if (entry_read(fs, dir_first, dir_contig, ei + k, ent) != 0) {
            return -1;
        }
        ent[0] = (uint8_t)(ent[0] & (uint8_t)~(uint8_t)HYPE_EXFAT_ENT_INUSE);
        if (entry_write(fs, dir_first, dir_contig, ei + k, ent) != 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Scans a directory for `need` consecutive unused entry slots. An exFAT entry set
 * must be contiguous, so a run that would spill past the directory's current
 * allocation does not count. Returns 1 found, 0 not found, -1 on I/O error.
 */
static int dir_scan_slots(hype_exfat_fs_t *fs, const dirref_t *d, unsigned int need,
                          uint32_t *out_index) {
    uint32_t entries_per_cluster = fs->spc * HYPE_EXFAT_ENTRIES_PER_SECTOR;
    uint32_t clusters = 0;
    uint32_t capacity;
    uint32_t ei;
    uint32_t run_start = 0;
    unsigned int run = 0;

    if (d->contiguous) {
        /* A NoFatChain directory's extent comes from its DataLength, which its
         * own entry set carries (range-checked by set_read on the way here). */
        clusters = (uint32_t)clusters_for(fs, d->size);
    } else {
        uint32_t cl = d->first;
        unsigned int guard = 0;
        while (guard++ < WALK_GUARD) {
            uint32_t next;
            if (fat_get(fs, cl, &next) != 0) {
                return -1;
            }
            clusters++;
            if (next >= HYPE_EXFAT_EOC) {
                break;
            }
            cl = next;
        }
        if (guard >= WALK_GUARD) {
            return -1; /* the directory's chain loops */
        }
    }
    capacity = clusters * entries_per_cluster;

    for (ei = 0; ei < capacity; ei++) {
        uint8_t ent[ENTSZ];
        if (entry_read(fs, d->first, d->contiguous, ei, ent) != 0) {
            return -1;
        }
        if (ent[0] == 0x00u || (ent[0] & HYPE_EXFAT_ENT_INUSE) == 0u) {
            if (run == 0u) {
                run_start = ei;
            }
            run++;
            if (run >= need) {
                *out_index = run_start;
                return 1;
            }
            continue;
        }
        run = 0u;
        if (ent[0] == HYPE_EXFAT_ENT_FILE && ent[1] >= 1u && ent[1] <= HYPE_EXFAT_MAX_SECONDARY) {
            ei += ent[1]; /* the for-loop's ++ takes us past the whole set */
        }
    }
    return 0;
}

/*
 * Appends one zeroed cluster to a directory. A zeroed entry reads as the
 * end-of-directory marker, which is exactly what a fresh cluster must look
 * like. A NoFatChain directory gets its FAT chain materialised first (the next
 * physical cluster cannot be assumed free), which -- like the growth itself --
 * must be reflected in the directory's own entry set, so both changes are
 * flushed to its owner as they happen.
 */
static int dir_grow(hype_exfat_fs_t *fs, dirref_t *d) {
    uint32_t newcl;
    uint32_t cl = d->first;
    uint32_t last = d->first;
    unsigned int guard = 0;
    uint8_t zero[SECSZ];
    unsigned int s;

    if (d->contiguous) {
        uint64_t n = clusters_for(fs, d->size);
        uint64_t i;
        if (n == 0u || !cluster_valid(fs, d->first + (uint32_t)(n - 1u))) {
            return -1;
        }
        for (i = 0; i < n; i++) {
            uint32_t c = d->first + (uint32_t)i;
            if (fat_set(fs, c, (i + 1u < n) ? (c + 1u) : EOC_MARK) != 0) {
                return -1;
            }
        }
        d->contiguous = 0u;
        if (dirref_flush(fs, d) != 0) {
            return -1; /* the flags byte must never claim NoFatChain again */
        }
    }
    while (guard++ < WALK_GUARD) {
        uint32_t next;
        if (fat_get(fs, cl, &next) != 0) {
            return -1;
        }
        last = cl;
        if (next >= HYPE_EXFAT_EOC) {
            break;
        }
        cl = next;
    }
    if (guard >= WALK_GUARD) {
        return -1;
    }
    if (alloc_cluster(fs, &newcl) != 0) {
        return -1;
    }
    bzero(zero, SECSZ);
    for (s = 0; s < fs->spc; s++) {
        if (fs->write(fs->ctx, clba(fs, newcl) + s, 1u, zero) != 0) {
            return -1;
        }
    }
    if (fat_set(fs, last, newcl) != 0) {
        return -1;
    }
    d->size += cluster_bytes(fs);
    return dirref_flush(fs, d);
}

/*
 * Finds `need` consecutive unused directory-entry slots, growing the directory a
 * cluster at a time until they fit. The attempt cap bounds the work: one grow is
 * enough unless the entry set is larger than a whole cluster's worth of entries,
 * which needs a name of well over 200 characters on a 512-byte cluster.
 */
static int dir_find_slots(hype_exfat_fs_t *fs, dirref_t *d, unsigned int need,
                          uint32_t *out_index) {
    unsigned int attempt;
    for (attempt = 0; attempt < 4u; attempt++) {
        int rc = dir_scan_slots(fs, d, need, out_index);
        if (rc < 0) {
            return -1;
        }
        if (rc == 1) {
            return 0;
        }
        if (dir_grow(fs, d) != 0) {
            return -1;
        }
    }
    return -1;
}

/* Builds a complete entry set in `entries` so its checksum covers exactly the
 * bytes that land on the medium, then writes it at `ei`. The File and Stream
 * entries are already in place; this fills the name entries and the checksum. */
static int set_place(hype_exfat_fs_t *fs, const dirref_t *d, uint32_t ei, uint8_t *entries,
                     const uint16_t *chars, unsigned int nlen, unsigned int need) {
    unsigned int name_entries = need - 2u;
    unsigned int k;
    uint16_t sum;

    for (k = 0; k < name_entries; k++) {
        unsigned int off = k * HYPE_EXFAT_NAME_CHARS_PER_ENTRY;
        unsigned int count = nlen - off;
        if (count > HYPE_EXFAT_NAME_CHARS_PER_ENTRY) {
            count = HYPE_EXFAT_NAME_CHARS_PER_ENTRY;
        }
        hype_exfat_name_entry(entries + (2u + k) * ENTSZ, chars + off, count);
    }
    sum = hype_exfat_set_checksum_update(0u, 0u, entries, need * ENTSZ);
    hype_exfat_file_entry_set_checksum(entries, sum);

    for (k = 0; k < need; k++) {
        if (entry_write(fs, d->first, d->contiguous, ei + k, entries + k * ENTSZ) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- create ---- */

int hype_exfat_create(hype_exfat_fs_t *fs, const char *path, hype_exfat_wfile_t *out) {
    uint16_t chars[HYPE_EXFAT_MAX_NAME];
    char leafbuf[HYPE_EXFAT_MAX_NAME + 1u];
    uint8_t entries[MAX_SET_ENTRIES * ENTSZ];
    dirref_t dir;
    hype_exfat_set_t set;
    unsigned int leaf, nlen, name_entries, need;
    uint16_t hash = 0u;
    uint32_t ei = 0;
    int rc;

    if (fs->write == 0) {
        return -1;
    }
    if (path_split(path, &leaf) != 0 || leaf_string(path, leaf, leafbuf, sizeof leafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, path, leaf, 0u, &dir) != 0) {
        return -1;
    }
    if (name_prepare(fs, leafbuf, chars, &nlen, &hash) != 0) {
        return -1;
    }
    name_entries = (nlen + HYPE_EXFAT_NAME_CHARS_PER_ENTRY - 1u) / HYPE_EXFAT_NAME_CHARS_PER_ENTRY;
    need = 2u + name_entries;

    if (mark_dirty(fs) != 0) {
        return -1;
    }

    /* An existing file of the same name is truncated. Its name is identical, so
     * the replacement set is normally exactly the same shape and the slot can be
     * reused in place; a set carrying extra secondary entries hype does not
     * generate is retired wholesale instead, so no stale entry is left inside a
     * set whose checksum no longer covers it. */
    rc = dir_find(fs, dir.first, dir.contiguous, chars, nlen, &ei, &set);
    if (rc < 0) {
        return -1;
    }
    if (rc == 1) {
        if ((set.attributes & HYPE_EXFAT_ATTR_DIRECTORY) != 0u) {
            return -1; /* refuse to clobber a directory */
        }
        if (free_allocation(fs, set.first_cluster, set.contiguous, set.data_length) != 0) {
            return -1;
        }
        if (set.secondary != (uint8_t)(need - 1u)) {
            if (set_delete(fs, dir.first, dir.contiguous, ei, set.secondary) != 0) {
                return -1;
            }
            rc = 0;
        }
    }
    if (rc != 1 && dir_find_slots(fs, &dir, need, &ei) != 0) {
        return -1;
    }

    hype_exfat_file_entry(entries, HYPE_EXFAT_ATTR_ARCHIVE, (uint8_t)(need - 1u), &fs->now);
    hype_exfat_stream_entry(entries + ENTSZ, nlen, hash, 0u, 0u, 0u, 0);
    if (set_place(fs, &dir, ei, entries, chars, nlen, need) != 0) {
        return -1;
    }

    out->fs = fs;
    out->dir_cluster = dir.first;
    out->dir_contiguous = dir.contiguous;
    out->set_index = ei;
    out->secondary = (uint8_t)(need - 1u);
    out->first_cluster = 0u;
    out->tail_cluster = 0u;
    out->size = 0u;
    out->valid = 0u;
    out->contiguous = 0u;
    out->is_dir = 0u;
    out->seek_index = 0u;
    out->seek_cluster = 0u;
    return 0;
}

/* ---- unlink, mkdir, rmdir, rename (#246) ---- */

int hype_exfat_unlink(hype_exfat_fs_t *fs, const char *path) {
    uint16_t comp[HYPE_EXFAT_MAX_NAME];
    dirref_t dir;
    hype_exfat_set_t set;
    unsigned int leaf, nlen;
    uint32_t ei = 0;

    if (fs->write == 0) {
        return -1;
    }
    if (path_split(path, &leaf) != 0 || resolve_parent(fs, path, leaf, 0u, &dir) != 0) {
        return -1;
    }
    nlen = leaf_component(path, leaf, comp);
    if (nlen == 0u) {
        return -1;
    }
    if (dir_find(fs, dir.first, dir.contiguous, comp, nlen, &ei, &set) != 1) {
        return -1;
    }
    if ((set.attributes & HYPE_EXFAT_ATTR_DIRECTORY) != 0u) {
        return -1; /* directories go through rmdir, which checks emptiness */
    }
    if (mark_dirty(fs) != 0) {
        return -1;
    }
    if (free_allocation(fs, set.first_cluster, set.contiguous, set.data_length) != 0) {
        return -1;
    }
    return set_delete(fs, dir.first, dir.contiguous, ei, set.secondary);
}

int hype_exfat_mkdir(hype_exfat_fs_t *fs, const char *path) {
    uint16_t chars[HYPE_EXFAT_MAX_NAME];
    char leafbuf[HYPE_EXFAT_MAX_NAME + 1u];
    uint8_t entries[MAX_SET_ENTRIES * ENTSZ];
    uint8_t zero[SECSZ];
    dirref_t dir;
    hype_exfat_set_t set;
    unsigned int leaf, nlen, need, s;
    uint16_t hash = 0u;
    uint32_t ei = 0;
    uint32_t cl = 0;

    if (fs->write == 0) {
        return -1;
    }
    if (path_split(path, &leaf) != 0 || leaf_string(path, leaf, leafbuf, sizeof leafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, path, leaf, 0u, &dir) != 0) {
        return -1;
    }
    if (name_prepare(fs, leafbuf, chars, &nlen, &hash) != 0) {
        return -1;
    }
    if (dir_find(fs, dir.first, dir.contiguous, chars, nlen, &ei, &set) != 0) {
        return -1; /* an existing entry of EITHER kind, or an I/O error */
    }
    need = 2u + (nlen + HYPE_EXFAT_NAME_CHARS_PER_ENTRY - 1u) / HYPE_EXFAT_NAME_CHARS_PER_ENTRY;

    if (mark_dirty(fs) != 0) {
        return -1;
    }
    /* Place the entry set's slots before allocating the directory's cluster:
     * failing between the two leaks at worst directory SLACK (extra room in the
     * parent), never an allocated cluster nothing references. */
    if (dir_find_slots(fs, &dir, need, &ei) != 0) {
        return -1;
    }
    /*
     * One zeroed whole cluster: an exFAT directory has no '.'/'..' entries, its
     * unused entries must read 0x00 (the end-of-directory marker -- garbage here
     * would misparse), and its DataLength is a whole multiple of the cluster
     * size. alloc_cluster has already made its FAT entry end-of-chain.
     */
    if (alloc_cluster(fs, &cl) != 0) {
        return -1;
    }
    bzero(zero, SECSZ);
    for (s = 0; s < fs->spc; s++) {
        if (fs->write(fs->ctx, clba(fs, cl) + s, 1u, zero) != 0) {
            return -1;
        }
    }

    hype_exfat_file_entry(entries, HYPE_EXFAT_ATTR_DIRECTORY, (uint8_t)(need - 1u), &fs->now);
    hype_exfat_stream_entry(entries + ENTSZ, nlen, hash, cluster_bytes(fs), cl, cluster_bytes(fs),
                            0);
    return set_place(fs, &dir, ei, entries, chars, nlen, need);
}

/*
 * 1 == the directory holds no in-use entry of any type, 0 == something is
 * still there, -1 on a sector-read failure (which must not read as "empty":
 * removing a directory that still has entries orphans them). Bounded by the
 * allocation itself: DataLength for a NoFatChain directory, the FAT chain (with
 * the usual loop guard) otherwise.
 */
static int dir_is_empty(hype_exfat_fs_t *fs, uint32_t first, int contiguous, uint64_t size) {
    uint64_t cap = contiguous
                       ? clusters_for(fs, size) * (uint64_t)(fs->spc * HYPE_EXFAT_ENTRIES_PER_SECTOR)
                       : (uint64_t)WALK_GUARD;
    uint64_t ei;
    for (ei = 0; ei < cap; ei++) {
        uint8_t sec[SECSZ];
        uint64_t lba;
        unsigned int off;
        if (entry_lba(fs, first, contiguous, (uint32_t)ei, &lba, &off) != 0) {
            return 1; /* past the end of the allocation */
        }
        if (fs->read(fs->ctx, lba, 1u, sec) != 0) {
            return -1;
        }
        if (sec[off] == 0x00u) {
            return 1; /* end-of-directory marker */
        }
        if ((sec[off] & HYPE_EXFAT_ENT_INUSE) != 0u) {
            return 0;
        }
    }
    return -1; /* the walk guard tripped: never treat a looping chain as empty */
}

int hype_exfat_rmdir(hype_exfat_fs_t *fs, const char *path) {
    uint16_t comp[HYPE_EXFAT_MAX_NAME];
    dirref_t dir;
    hype_exfat_set_t set;
    unsigned int leaf, nlen;
    uint32_t ei = 0;

    if (fs->write == 0) {
        return -1;
    }
    /* The root directory has no final component, so path_split refuses it. */
    if (path_split(path, &leaf) != 0 || resolve_parent(fs, path, leaf, 0u, &dir) != 0) {
        return -1;
    }
    nlen = leaf_component(path, leaf, comp);
    if (nlen == 0u) {
        return -1;
    }
    if (dir_find(fs, dir.first, dir.contiguous, comp, nlen, &ei, &set) != 1) {
        return -1;
    }
    if ((set.attributes & HYPE_EXFAT_ATTR_DIRECTORY) == 0u) {
        return -1;
    }
    if (dir_is_empty(fs, set.first_cluster, set.contiguous, set.data_length) != 1) {
        return -1;
    }
    if (mark_dirty(fs) != 0) {
        return -1;
    }
    if (free_allocation(fs, set.first_cluster, set.contiguous, set.data_length) != 0) {
        return -1;
    }
    return set_delete(fs, dir.first, dir.contiguous, ei, set.secondary);
}

int hype_exfat_rename(hype_exfat_fs_t *fs, const char *from, const char *to) {
    uint16_t fcomp[HYPE_EXFAT_MAX_NAME];
    uint16_t tchars[HYPE_EXFAT_MAX_NAME];
    char leafbuf[HYPE_EXFAT_MAX_NAME + 1u];
    uint8_t entries[MAX_SET_ENTRIES * ENTSZ];
    dirref_t fdir, tdir;
    hype_exfat_set_t fset, tset;
    unsigned int fleaf, tleaf, fnlen, tnlen, need;
    uint16_t hash = 0u;
    uint32_t fei = 0;
    uint32_t tei = 0;
    uint32_t forbid = 0;

    if (fs->write == 0) {
        return -1;
    }
    /* The source, which must exist. */
    if (path_split(from, &fleaf) != 0 || resolve_parent(fs, from, fleaf, 0u, &fdir) != 0) {
        return -1;
    }
    fnlen = leaf_component(from, fleaf, fcomp);
    if (fnlen == 0u) {
        return -1;
    }
    if (dir_find(fs, fdir.first, fdir.contiguous, fcomp, fnlen, &fei, &fset) != 1) {
        return -1;
    }
    /*
     * The destination parent, which must not be reached THROUGH the source: a
     * directory moved into its own subtree becomes an unreachable cycle. Every
     * directory on the destination walk is checked against the source's first
     * cluster, which is exactly the set of ancestors the target would have.
     */
    if ((fset.attributes & HYPE_EXFAT_ATTR_DIRECTORY) != 0u) {
        forbid = fset.first_cluster;
    }
    if (path_split(to, &tleaf) != 0 || leaf_string(to, tleaf, leafbuf, sizeof leafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, to, tleaf, forbid, &tdir) != 0) {
        return -1;
    }
    if (name_prepare(fs, leafbuf, tchars, &tnlen, &hash) != 0) {
        return -1;
    }
    /* Rename never replaces. NOTE this also refuses a pure case change of the
     * same name -- the case-insensitive search finds the source itself. */
    if (dir_find(fs, tdir.first, tdir.contiguous, tchars, tnlen, &tei, &tset) != 0) {
        return -1;
    }
    need = 2u + (tnlen + HYPE_EXFAT_NAME_CHARS_PER_ENTRY - 1u) / HYPE_EXFAT_NAME_CHARS_PER_ENTRY;

    if (mark_dirty(fs) != 0) {
        return -1;
    }
    /*
     * The original File entry carries the attributes and timestamps, which a
     * rename preserves; only its SecondaryCount (the name may need a different
     * number of entries) and checksum change. The Stream entry is rebuilt
     * around the SAME allocation -- ValidDataLength included, which append never
     * leaves short but another writer may have.
     */
    if (entry_read(fs, fdir.first, fdir.contiguous, fei, entries) != 0) {
        return -1;
    }
    entries[1] = (uint8_t)(need - 1u);
    entries[2] = 0u;
    entries[3] = 0u;
    hype_exfat_stream_entry(entries + ENTSZ, tnlen, hash, fset.valid_length, fset.first_cluster,
                            fset.data_length, fset.contiguous ? 1 : 0);
    /* The new set is written before the old one is retired, so an interruption
     * leaves the entry findable under at least one of its names. Growing the
     * destination directory never moves existing entries, so the source set's
     * index stays valid even when both are the same directory. */
    if (dir_find_slots(fs, &tdir, need, &tei) != 0) {
        return -1;
    }
    if (set_place(fs, &tdir, tei, entries, tchars, tnlen, need) != 0) {
        return -1;
    }
    return set_delete(fs, fdir.first, fdir.contiguous, fei, fset.secondary);
}

/* ---- data access ---- */

/* Cluster holding chain index `index`, using and refreshing the seek cache. */
static int file_cluster_at(hype_exfat_wfile_t *f, uint32_t index, uint32_t *out) {
    hype_exfat_fs_t *fs = f->fs;
    uint32_t cl;
    uint32_t i;

    /* Reached only for an offset inside the file's size, which set_read has
     * already tied to a non-zero first cluster. */
    if (f->contiguous) {
        return chain_cluster_at(fs, f->first_cluster, 1, index, out);
    }
    if (f->seek_cluster != 0u && f->seek_index <= index) {
        cl = f->seek_cluster;
        i = f->seek_index;
    } else {
        cl = f->first_cluster;
        i = 0u;
    }
    while (i < index) {
        uint32_t next;
        if (fat_get(fs, cl, &next) != 0 || next >= HYPE_EXFAT_EOC) {
            return -1;
        }
        cl = next;
        i++;
    }
    if (!cluster_valid(fs, cl)) {
        return -1;
    }
    f->seek_index = i;
    f->seek_cluster = cl;
    *out = cl;
    return 0;
}

/*
 * Shared body of read_at / write_at: both walk the same offset arithmetic.
 * Exactly one of `rbuf` (read into) and `wbuf` (write from) is non-NULL.
 */
static int file_rw_at(hype_exfat_wfile_t *f, uint64_t offset, uint8_t *rbuf, const uint8_t *wbuf,
                      unsigned int len) {
    hype_exfat_fs_t *fs = f->fs;
    int writing = (wbuf != 0);
    uint64_t cb = cluster_bytes(fs);

    if (writing && fs->write == 0) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }
    /* The whole range must already exist: this path never grows a file, and an
     * unchecked offset+len is exactly the guest-supplied-length class of bug
     * AGENTS.md forbids. */
    if (offset > f->size || (uint64_t)len > f->size - offset) {
        return -1;
    }
    if (writing && mark_dirty(fs) != 0) {
        return -1;
    }
    while (len > 0u) {
        uint32_t cl;
        uint64_t within = offset % cb;
        unsigned int sic = (unsigned int)(within / SECSZ);
        unsigned int bis = (unsigned int)(within % SECSZ);
        unsigned int n = SECSZ - bis;
        uint64_t lba;
        /* The chain index fits in 32 bits: set_read bounded the file's size to
         * the volume's cluster count, which is itself a 32-bit quantity. */
        if (file_cluster_at(f, (uint32_t)(offset / cb), &cl) != 0) {
            return -1;
        }
        lba = clba(fs, cl) + sic;
        if (n > len) {
            n = len;
        }
        if (!writing) {
            uint8_t sec[SECSZ];
            if (fs->read(fs->ctx, lba, 1u, sec) != 0) {
                return -1;
            }
            bcopy(rbuf, sec + bis, n);
            rbuf += n;
        } else if (bis != 0u || n < SECSZ) {
            uint8_t sec[SECSZ];
            if (fs->read(fs->ctx, lba, 1u, sec) != 0) {
                return -1;
            }
            bcopy(sec + bis, wbuf, n);
            if (fs->write(fs->ctx, lba, 1u, sec) != 0) {
                return -1;
            }
            wbuf += n;
        } else {
            if (fs->write(fs->ctx, lba, 1u, wbuf) != 0) {
                return -1;
            }
            wbuf += n;
        }
        offset += n;
        len -= n;
    }
    return 0;
}

static int chain_materialise(hype_exfat_wfile_t *f);
static int resolve_tail(hype_exfat_wfile_t *f);
static int zero_span(hype_exfat_wfile_t *f, uint64_t offset, uint64_t len);

/* #510: a rollback that could not push the restored entry set back to the medium. Zero on every
 * healthy run; nonzero says the volume may hold an entry claiming freed clusters. Mirrors
 * hype_fat_write_rollback_failures(). */
static unsigned long long g_exfat_rollback_failures;
unsigned long long hype_exfat_write_rollback_failures(void) { return g_exfat_rollback_failures; }

/*
 * #517: what does the entry set ON THE MEDIUM claim right now? The rollback has to decide whether
 * cutting the chain is safe, and that turns on what was actually published, not on what a failed
 * flush intended. Reads DataLength straight back out of the Stream Extension entry.
 *
 * A read that fails answers "no": unable to confirm is not the same as confirmed, and the safe
 * direction under doubt is to leave the clusters alone.
 */
static int entry_set_claims_at_most(hype_exfat_wfile_t *f, uint64_t bytes) {
    uint8_t ent[ENTSZ];
    if (entry_read(f->fs, f->dir_cluster, f->dir_contiguous, f->set_index + 1u, ent) != 0) {
        return 0;
    }
    return (hype_rd64(ent + 24) <= bytes) ? 1 : 0;
}

int hype_exfat_write_at(hype_exfat_wfile_t *f, uint64_t offset, const void *data,
                        unsigned int len) {
    hype_exfat_fs_t *fs = f->fs;
    uint64_t end = offset + len;
    uint64_t old_size, old_valid;
    uint32_t old_tail = 0u, first_new = 0u;
    uint8_t old_contig;

    if (data == 0 || f->is_dir) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }
    if (end < offset) {
        return -1;
    }
    if (end <= f->valid) {
        /* wholly inside the initialized prefix: pure in-place data */
        return file_rw_at(f, offset, 0, (const uint8_t *)data, len);
    }
    if (fs->write == 0) {
        return -1;
    }
    if (mark_dirty(fs) != 0) {
        return -1;
    }
    old_size = f->size;
    old_valid = f->valid;
    old_contig = f->contiguous;

    /* 1. Grow the allocation through the new end (exFAT cannot represent a
     *    missing cluster inside DataLength). */
    if (end > f->size) {
        uint64_t cb = cluster_bytes(fs);
        uint64_t need = clusters_for(fs, end);
        uint64_t have = (f->first_cluster == 0u) ? 0u : clusters_for(fs, f->size);

        if (have > 0u) {
            /*
             * #647: re-validate before trusting the chain enough to extend it. hype_exfat_lookup
             * validated once, at open; another writer sharing this mount (or corruption) could
             * have moved the allocation since -- exactly the reason core/fat_write_fs.c:1446
             * re-runs chain_measure at the top of FAT32's growth path rather than trusting
             * open-time state.
             */
            if (f->contiguous) {
                if (contiguous_run_all_used(fs, f->first_cluster, f->size) != 0) {
                    return -1;
                }
            } else {
                uint32_t measured_tail;
                if (chain_measure(fs, f->first_cluster, f->size, &measured_tail) != 0) {
                    return -1;
                }
                f->tail_cluster = measured_tail;
            }
            if (chain_materialise(f) != 0) {
                return -1;
            }
            if (resolve_tail(f) != 0) {
                return -1;
            }
            old_tail = f->tail_cluster;
        }
        while (have < need) {
            uint32_t cl;
            if (alloc_cluster(fs, &cl) != 0) {
                goto rollback;
            }
            if (f->first_cluster == 0u) {
                f->first_cluster = cl;
                f->contiguous = 0u;
                f->seek_index = 0u;
                f->seek_cluster = cl;
            } else if (fat_set(fs, f->tail_cluster, cl) != 0) {
                (void)free_cluster(fs, cl);
                goto rollback;
            }
            if (first_new == 0u) {
                first_new = cl;
            }
            f->tail_cluster = cl;
            have++;
        }
        f->size = end;
        (void)cb;
    }

    /* 2. Zero the never-written gap on the medium BEFORE anything makes it
     *    reachable: [old_valid, offset) inside the (possibly new) allocation. */
    if (offset > old_valid) {
        if (zero_span(f, old_valid, offset - old_valid) != 0) {
            goto rollback;
        }
    }

    /* 3. The data itself. */
    if (file_rw_at(f, offset, 0, (const uint8_t *)data, len) != 0) {
        goto rollback;
    }

    /* 4. Publish: DataLength + ValidDataLength + first cluster + checksum in
     *    one entry-set update, after the bytes are on the medium. #648:
     *    durable exactly when this call allocated a cluster (first_new != 0) --
     *    a call that only advances ValidDataLength inside the existing
     *    allocation needs no barrier. */
    f->valid = end;
    if (set_flush(f, first_new != 0u) != 0) {
        goto rollback;
    }
    return 0;

rollback:
    /*
     * #510: SHRINK THE ENTRY SET BEFORE SHRINKING THE CHAIN -- the same ordering #464 fixed in
     * the FAT32 writer. The old order restored size/valid in MEMORY ONLY and never re-flushed
     * the entry set, so a set_flush() that reached the medium and then failed (partial entry
     * write, failed barrier) left the on-disk DataLength claiming clusters this rollback was
     * about to free. fsck reads that as corruption and Linux remounts the volume read-only.
     *
     * So: restore the in-memory shape first, push it back to the medium, and only then free
     * the allocation. At every intermediate point -- including across a power cut -- the
     * on-disk entry set never describes more bytes than its chain holds. If even the
     * restoring flush fails the medium is wedged beyond ordering fixes; counted, not hidden.
     */
    if (f->first_cluster != 0u && old_size == 0u && first_new == f->first_cluster) {
        f->first_cluster = 0u;
        f->tail_cluster = 0u;
    } else {
        f->tail_cluster = old_tail;
    }
    f->size = old_size;
    f->valid = old_valid;
    f->contiguous = (old_contig && first_new == 0u) ? old_contig : f->contiguous;
    f->seek_index = 0u;
    f->seek_cluster = f->first_cluster;
    /*
     * #648/#517 (mirrors core/fat_write_fs.c:1366-1374's growth_rollback): the restore must not
     * depend on the barrier. A persistently failing barrier must not stop the entry set from being
     * pushed back to its old, safe shape -- shrinking never reaches a cluster the medium has not
     * already linked, so it needs no preceding barrier.
     */
    if (set_flush(f, 0) != 0) {
        g_exfat_rollback_failures++;
    }
    /*
     * #517: cut the chain only once the MEDIUM shows an entry set within the old size -- read back,
     * not inferred from what set_flush() returned. #510 fixed the order but still freed the
     * clusters whether or not the restore had landed, so a failed entry-set write left DataLength
     * claiming clusters the next lines released: the same unmountable shape #464 produced on FAT32.
     *
     * If it cannot be confirmed, leave the clusters linked. A chain longer than its entry set is
     * leaked space a repair tool reclaims and a file that reads short; the reverse is a volume the
     * host refuses. Given the choice, leak.
     */
    if (!entry_set_claims_at_most(f, old_size)) {
        g_exfat_rollback_failures++;
        return -1;
    }
    if (first_new != 0u) {
        uint32_t cl = first_new;
        uint32_t guard = 0;
        while (cluster_valid(fs, cl) && guard++ <= fs->cluster_count) {
            uint32_t next;
            if (fat_get(fs, cl, &next) != 0) {
                break;
            }
            (void)free_cluster(fs, cl);
            if (next >= EOC_MARK || next == 0u) {
                break;
            }
            cl = next;
        }
        if (old_tail != 0u) {
            (void)fat_set(fs, old_tail, EOC_MARK);
        }
    }
    return -1;
}

int hype_exfat_read_at(hype_exfat_wfile_t *f, uint64_t offset, void *out, unsigned int len) {
    uint8_t *dst = (uint8_t *)out;
    unsigned int from_media = len;
    if (out == 0) {
        return -1;
    }
    if (offset > f->size || (uint64_t)len > f->size - offset) {
        return -1;
    }
    /* #383: the allocation past ValidDataLength was never written -- reading
     * the medium there would hand out stale bytes. Synthesize zeros. */
    if (offset >= f->valid) {
        from_media = 0;
    } else if (offset + len > f->valid) {
        from_media = (unsigned int)(f->valid - offset);
    }
    if (from_media > 0u && file_rw_at(f, offset, dst, 0, from_media) != 0) {
        return -1;
    }
    bzero(dst + from_media, len - from_media);
    return 0;
}

/* Zero `len` bytes of the ALLOCATION at byte `offset`, bounded by f->size
 * (which the caller has already advanced when growing). Chunked through a
 * static zero buffer -- BSP-serialized, like every writer here. */
static int zero_span(hype_exfat_wfile_t *f, uint64_t offset, uint64_t len) {
    static const uint8_t zeros[2048];
    while (len > 0u) {
        unsigned int n = (len > sizeof zeros) ? (unsigned int)sizeof zeros : (unsigned int)len;
        if (file_rw_at(f, offset, 0, zeros, n) != 0) {
            return -1;
        }
        offset += n;
        len -= n;
    }
    return 0;
}

/* ---- append ---- */

/*
 * Turns a contiguous (NoFatChain) allocation into a real FAT chain, so it can be
 * extended into whatever free cluster is available rather than requiring the
 * next physical one to be free.
 */
static int chain_materialise(hype_exfat_wfile_t *f) {
    hype_exfat_fs_t *fs = f->fs;
    uint64_t n = clusters_for(fs, f->size);
    uint64_t i;

    if (!f->contiguous) {
        return 0; /* already a FAT chain */
    }
    if (!cluster_valid(fs, f->first_cluster + (uint32_t)(n - 1u))) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        uint32_t cl = f->first_cluster + (uint32_t)i;
        uint32_t val = (i + 1u < n) ? (cl + 1u) : EOC_MARK;
        if (fat_set(fs, cl, val) != 0) {
            return -1;
        }
    }
    f->contiguous = 0u;
    f->tail_cluster = f->first_cluster + (uint32_t)(n - 1u);
    return 0;
}

/* Resolves (and caches) the last cluster of the allocation. */
static int resolve_tail(hype_exfat_wfile_t *f) {
    hype_exfat_fs_t *fs = f->fs;
    uint64_t n = clusters_for(fs, f->size);
    uint32_t cl;
    unsigned int guard = 0;

    if (f->tail_cluster != 0u) {
        return 0;
    }
    /* Only called with a non-empty allocation, so n >= 1. */
    if (f->contiguous) {
        /* set_read bounded a contiguous run to the heap, so its last cluster is
         * inside it. */
        f->tail_cluster = f->first_cluster + (uint32_t)(n - 1u);
        return 0;
    }
    cl = f->first_cluster;
    while (guard++ < WALK_GUARD) {
        uint32_t next;
        if (fat_get(fs, cl, &next) != 0) {
            return -1;
        }
        if (next >= HYPE_EXFAT_EOC) {
            f->tail_cluster = cl;
            return 0;
        }
        cl = next;
    }
    return -1;
}

int hype_exfat_append(hype_exfat_wfile_t *f, const void *data, unsigned int len) {
    hype_exfat_fs_t *fs = f->fs;
    const uint8_t *src = (const uint8_t *)data;
    uint64_t cb = cluster_bytes(fs);
    int grew = 0; /* #648: did THIS call allocate a cluster? -> the entry-set
                   * publication that follows needs a durability barrier. */

    if (fs->write == 0 || f->is_dir) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }
    if (mark_dirty(fs) != 0) {
        return -1;
    }
    while (len > 0u) {
        uint64_t within = f->size % cb;
        unsigned int sic, bis, n;
        uint64_t lba;

        if (f->first_cluster == 0u) {
            uint32_t cl;
            if (alloc_cluster(fs, &cl) != 0) {
                return -1;
            }
            f->first_cluster = cl;
            f->tail_cluster = cl;
            f->contiguous = 0u;
            f->seek_index = 0u;
            f->seek_cluster = cl;
            grew = 1;
        } else if (within == 0u) {
            /* The last cluster is exactly full: extend the chain. */
            uint32_t cl;
            if (chain_materialise(f) != 0) {
                return -1;
            }
            if (resolve_tail(f) != 0) {
                return -1;
            }
            if (alloc_cluster(fs, &cl) != 0) {
                return -1;
            }
            if (fat_set(fs, f->tail_cluster, cl) != 0) {
                return -1;
            }
            f->tail_cluster = cl;
            grew = 1;
        } else if (resolve_tail(f) != 0) {
            return -1;
        }

        sic = (unsigned int)(within / SECSZ);
        bis = (unsigned int)(within % SECSZ);
        lba = clba(fs, f->tail_cluster) + sic;
        n = SECSZ - bis;
        if (n > len) {
            n = len;
        }
        if (bis != 0u || n < SECSZ) {
            uint8_t sec[SECSZ];
            if (fs->read(fs->ctx, lba, 1u, sec) != 0) {
                return -1;
            }
            bcopy(sec + bis, src, n);
            if (fs->write(fs->ctx, lba, 1u, sec) != 0) {
                return -1;
            }
        } else if (fs->write(fs->ctx, lba, 1u, src) != 0) {
            return -1;
        }
        src += n;
        len -= n;
        f->size += n;
    }
    f->valid = f->size; /* an append writes every byte through the new end */
    return set_flush(f, grew);
}

void hype_exfat_fs_set_time(hype_exfat_fs_t *fs, const hype_rtc_time_t *now) {
    if (fs == 0) {
        return;
    }
    if (now == 0) {
        fs->now.year = 0; /* invalid -> encoders emit the 1980 epoch */
        return;
    }
    fs->now.year = now->year;
    fs->now.month = now->month;
    fs->now.day = now->day;
    fs->now.hour = now->hour;
    fs->now.minute = now->minute;
    fs->now.second = now->second;
}
