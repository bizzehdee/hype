#include "fat_exfat_fs.h"

#define SECSZ HYPE_FAT_SECTOR_SIZE
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

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}
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

static int fat_get(hype_exfat_fs_t *fs, uint32_t cl, uint32_t *out) {
    uint8_t sec[SECSZ];
    uint64_t slba = (uint64_t)fs->fat_lba + cl / FAT_ENTRIES_PER_SECTOR;
    if (!cluster_valid(fs, cl)) {
        return -1;
    }
    if (fs->read(fs->ctx, slba, 1u, sec) != 0) {
        return -1;
    }
    *out = rd32(sec + (cl % FAT_ENTRIES_PER_SECTOR) * 4u);
    return 0;
}

/* `cl` is always a cluster the caller has already validated -- either freshly
 * allocated or reached through a chain walk that range-checked it -- and only the
 * mutating entry points, which require a write callback, get here. */
static int fat_set(hype_exfat_fs_t *fs, uint32_t cl, uint32_t val) {
    uint8_t sec[SECSZ];
    uint64_t slba = (uint64_t)fs->fat_lba + cl / FAT_ENTRIES_PER_SECTOR;
    if (fs->read(fs->ctx, slba, 1u, sec) != 0) {
        return -1;
    }
    wr32(sec + (cl % FAT_ENTRIES_PER_SECTOR) * 4u, val);
    return fs->write(fs->ctx, slba, 1u, sec);
}

/* ---- allocation bitmap ---- */

/* Clears one cluster's bitmap bit (allocation goes through alloc_cluster, which
 * already has the right bitmap sector in hand). As with fat_set, `cl` has been
 * range-checked by the caller. */
static int bitmap_release(hype_exfat_fs_t *fs, uint32_t cl) {
    uint8_t sec[SECSZ];
    uint64_t lba;
    unsigned int bit;
    hype_exfat_bitmap_location(cl, fs->bitmap_lba, &lba, &bit);
    if (fs->read(fs->ctx, lba, 1u, sec) != 0) {
        return -1;
    }
    hype_exfat_bitmap_set(sec, bit, 0);
    return fs->write(fs->ctx, lba, 1u, sec);
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
    flags = rd16(sec + 0x6A);
    if (dirty) {
        flags = (uint16_t)(flags | HYPE_EXFAT_VOLUME_DIRTY);
    } else {
        flags = (uint16_t)(flags & (uint16_t)~(uint16_t)HYPE_EXFAT_VOLUME_DIRTY);
    }
    wr16(sec + 0x6A, flags);
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
        uint8_t sec[SECSZ];
        uint32_t s = (start_bit / BITMAP_BITS_PER_SECTOR + pass) % sectors;
        unsigned int from = (pass == 0u) ? (start_bit % BITMAP_BITS_PER_SECTOR) : 0u;
        unsigned int limit = BITMAP_BITS_PER_SECTOR;
        unsigned int bit;
        if (s == sectors - 1u) {
            limit = fs->cluster_count - s * BITMAP_BITS_PER_SECTOR;
        }
        /* `from` is always inside `limit`: next_free never exceeds the last valid
         * cluster, so its bit index never reaches the end of its own sector. */
        if (fs->read(fs->ctx, fs->bitmap_lba + s, 1u, sec) != 0) {
            return -1;
        }
        if (hype_exfat_bitmap_find_free(sec, from, limit, &bit) != 0) {
            continue;
        }
        {
            uint32_t cl = 2u + s * BITMAP_BITS_PER_SECTOR + bit;
            hype_exfat_bitmap_set(sec, bit, 1);
            if (fs->write(fs->ctx, fs->bitmap_lba + s, 1u, sec) != 0) {
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

/* Rewrites the stream entry's allocation fields and the set's checksum. */
static int set_flush(hype_exfat_wfile_t *f) {
    hype_exfat_fs_t *fs = f->fs;
    uint8_t ent[ENTSZ];
    uint16_t sum = 0u;
    unsigned int k;

    /* set_read (via lookup) and create both establish that set_index + 1 is this
     * set's Stream Extension entry before a handle exists at all. */
    if (entry_read(fs, f->dir_cluster, f->dir_contiguous, f->set_index + 1u, ent) != 0) {
        return -1;
    }
    ent[1] = (uint8_t)(HYPE_EXFAT_FLAG_ALLOC_POSSIBLE |
                       (f->contiguous ? HYPE_EXFAT_FLAG_NO_FAT_CHAIN : 0u));
    wr64(ent + 8, f->size); /* ValidDataLength: hype only ever writes full data */
    wr32(ent + 20, f->first_cluster);
    wr64(ent + 24, f->size);
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
    return entry_write(fs, f->dir_cluster, f->dir_contiguous, f->set_index, ent);
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

int hype_exfat_fs_mount(hype_fat_read_fn read, hype_fat_write_fn write, void *ctx,
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
    volume_flags = rd16(boot + 0x6A);

    out->read = read;
    out->write = write;
    out->ctx = ctx;
    out->volume_length = rd64(boot + 0x48);
    out->fat_length = rd32(boot + 0x54);
    out->heap_lba = rd32(boot + 0x58);
    out->cluster_count = rd32(boot + 0x5C);
    out->root_cluster = rd32(boot + 0x60);
    out->spc = 1u << boot[0x6D];
    out->next_free = 2u;
    out->used_clusters = HYPE_EXFAT_USED_UNKNOWN;
    out->dirty = (uint8_t)((volume_flags & HYPE_EXFAT_VOLUME_DIRTY) ? 1u : 0u);
    hype_exfat_upcase_reset(&out->upcase);

    /* With two FATs, VolumeFlags bit 0 selects the live one; reading the stale
     * copy would follow chains that no longer exist. */
    out->fat_lba = rd32(boot + 0x50);
    if (num_fats == 2u && (volume_flags & 0x0001u) != 0u) {
        out->fat_lba += out->fat_length;
    }

    if (out->fat_length == 0u || rd32(boot + 0x50) < BOOT_REGION_SECTORS) {
        return -1;
    }
    if (out->heap_lba < rd32(boot + 0x50) + (uint32_t)num_fats * out->fat_length) {
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
            bitmap_cluster = rd32(ent + 20);
            bitmap_bytes = rd64(ent + 24);
            have_bitmap = 1;
            continue;
        }
        if (ent[0] == HYPE_EXFAT_ENT_UPCASE && !have_upcase) {
            out->upcase_cluster = rd32(ent + 20);
            out->upcase_bytes = rd64(ent + 24);
            upcase_checksum = rd32(ent + 4);
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
            if (is_dir != (want_dir ? 1 : 0)) {
                return -1;
            }
            wfile_from_set(out, fs, dir_first, dir_contig, ei, &set);
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

/* ---- create ---- */

/*
 * Scans a directory for `need` consecutive unused entry slots. An exFAT entry set
 * must be contiguous, so a run that would spill past the directory's current
 * allocation does not count. Returns 1 found, 0 not found, -1 on I/O error.
 */
static int dir_scan_slots(hype_exfat_fs_t *fs, uint32_t dir_first, unsigned int need,
                          uint32_t *out_index) {
    uint32_t entries_per_cluster = fs->spc * HYPE_EXFAT_ENTRIES_PER_SECTOR;
    uint32_t clusters = 0;
    uint32_t cl = dir_first;
    uint32_t capacity;
    uint32_t ei;
    uint32_t run_start = 0;
    unsigned int run = 0;
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
    capacity = clusters * entries_per_cluster;

    for (ei = 0; ei < capacity; ei++) {
        uint8_t ent[ENTSZ];
        if (entry_read(fs, dir_first, 0, ei, ent) != 0) {
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

/* Appends one zeroed cluster to a FAT-chained directory. A zeroed entry reads as
 * the end-of-directory marker, which is exactly what a fresh cluster must look
 * like. */
static int dir_grow(hype_exfat_fs_t *fs, uint32_t dir_first) {
    uint32_t newcl;
    uint32_t cl = dir_first;
    uint32_t last = dir_first;
    unsigned int guard = 0;
    uint8_t zero[SECSZ];
    unsigned int s;

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
    return fat_set(fs, last, newcl);
}

/*
 * Finds `need` consecutive unused directory-entry slots, growing the directory a
 * cluster at a time until they fit. Only ever called for the root directory,
 * whose allocation is always FAT-chained. The attempt cap bounds the work: one
 * grow is enough unless the entry set is larger than a whole cluster's worth of
 * entries, which needs a name of well over 200 characters on a 512-byte cluster.
 */
static int dir_find_slots(hype_exfat_fs_t *fs, uint32_t dir_first, unsigned int need,
                          uint32_t *out_index) {
    unsigned int attempt;
    for (attempt = 0; attempt < 4u; attempt++) {
        int rc = dir_scan_slots(fs, dir_first, need, out_index);
        if (rc < 0) {
            return -1;
        }
        if (rc == 1) {
            return 0;
        }
        if (dir_grow(fs, dir_first) != 0) {
            return -1;
        }
    }
    return -1;
}

int hype_exfat_create(hype_exfat_fs_t *fs, const char *name, hype_exfat_wfile_t *out) {
    uint16_t chars[HYPE_EXFAT_MAX_NAME];
    uint16_t upcased[HYPE_EXFAT_MAX_NAME];
    uint8_t entries[MAX_SET_ENTRIES * ENTSZ];
    hype_exfat_set_t set;
    int nlen_signed;
    unsigned int nlen, name_entries, need, i, k;
    uint16_t hash = 0u;
    uint16_t sum = 0u;
    uint32_t ei = 0;
    int rc;

    if (fs->write == 0) {
        return -1;
    }
    nlen_signed = hype_exfat_name_to_utf16(name, chars, HYPE_EXFAT_MAX_NAME);
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
    hash = hype_exfat_name_hash_update(0u, upcased, nlen);
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
    rc = dir_find(fs, fs->root_cluster, 0, chars, nlen, &ei, &set);
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
            for (k = 0; k <= set.secondary; k++) {
                uint8_t ent[ENTSZ];
                if (entry_read(fs, fs->root_cluster, 0, ei + k, ent) != 0) {
                    return -1;
                }
                ent[0] = (uint8_t)(ent[0] & (uint8_t)~(uint8_t)HYPE_EXFAT_ENT_INUSE);
                if (entry_write(fs, fs->root_cluster, 0, ei + k, ent) != 0) {
                    return -1;
                }
            }
            rc = 0;
        }
    }
    if (rc != 1 && dir_find_slots(fs, fs->root_cluster, need, &ei) != 0) {
        return -1;
    }

    /* Build the whole set in memory so its checksum covers exactly the bytes
     * that land on the medium. */
    hype_exfat_file_entry(entries, HYPE_EXFAT_ATTR_ARCHIVE, (uint8_t)(need - 1u));
    hype_exfat_stream_entry(entries + ENTSZ, nlen, hash, 0u, 0u, 0u, 0);
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
        if (entry_write(fs, fs->root_cluster, 0, ei + k, entries + k * ENTSZ) != 0) {
            return -1;
        }
    }

    out->fs = fs;
    out->dir_cluster = fs->root_cluster;
    out->dir_contiguous = 0u;
    out->set_index = ei;
    out->secondary = (uint8_t)(need - 1u);
    out->first_cluster = 0u;
    out->tail_cluster = 0u;
    out->size = 0u;
    out->contiguous = 0u;
    out->is_dir = 0u;
    out->seek_index = 0u;
    out->seek_cluster = 0u;
    return 0;
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

int hype_exfat_write_at(hype_exfat_wfile_t *f, uint64_t offset, const void *data,
                        unsigned int len) {
    if (data == 0) {
        return -1;
    }
    return file_rw_at(f, offset, 0, (const uint8_t *)data, len);
}

int hype_exfat_read_at(hype_exfat_wfile_t *f, uint64_t offset, void *out, unsigned int len) {
    if (out == 0) {
        return -1;
    }
    return file_rw_at(f, offset, (uint8_t *)out, 0, len);
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
    return set_flush(f);
}
