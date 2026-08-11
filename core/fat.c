#include "fat.h"
#include "lebytes.h"
#include "fat_exfat.h" /* shared exFAT layout constants + entry-set validation */

/* ---- little-endian field readers over a 512-byte sector buffer ---- */

/* ASCII lowercase for case-insensitive name matching. */
static char lc(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Case-insensitive compare of `n` chars. Returns 1 if equal. */
static int ci_eq(const char *a, const char *b, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) {
        if (lc(a[i]) != lc(b[i])) {
            return 0;
        }
    }
    return 1;
}

static unsigned str_len(const char *s) {
    unsigned n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/*
 * Path component iterator. On entry *pos indexes into path; skips leading
 * separators, then copies the next component into comp[] (up to cap-1 chars,
 * NUL-terminated) and advances *pos past it. Returns the component length, or 0
 * when no more components remain.
 */
static unsigned next_component(const char *path, unsigned *pos, char *comp, unsigned cap) {
    unsigned n = 0;
    while (path[*pos] == '\\' || path[*pos] == '/') {
        (*pos)++;
    }
    while (path[*pos] != '\0' && path[*pos] != '\\' && path[*pos] != '/') {
        if (n < cap - 1u) {
            comp[n] = path[*pos];
        }
        n++;
        (*pos)++;
    }
    comp[(n < cap - 1u) ? n : (cap - 1u)] = '\0';
    return n;
}

/* Build the "BASE.EXT" string (uppercase, spaces trimmed) from an 11-byte 8.3
 * directory name field into dst (>=13 bytes). */
static void short_name(const uint8_t *name11, char *dst) {
    unsigned di = 0;
    unsigned i;
    for (i = 0; i < 8u && name11[i] != ' '; i++) {
        dst[di++] = (char)name11[i];
    }
    if (name11[8] != ' ') {
        dst[di++] = '.';
        for (i = 8u; i < 11u && name11[i] != ' '; i++) {
            dst[di++] = (char)name11[i];
        }
    }
    dst[di] = '\0';
}

/* Extract the 13 UTF-16 chars of one LFN entry into lfn[] at slot (seq-1)*13,
 * taking the low byte only (ASCII approximation). Stops copying a name at a
 * 0x0000 unit. `seq` is 1-based (the entry's ordinal). */
static void lfn_piece(const uint8_t *ent, unsigned seq, char *lfn, unsigned lfn_cap) {
    static const unsigned off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    unsigned base = (seq - 1u) * 13u;
    unsigned i;
    for (i = 0; i < 13u; i++) {
        uint16_t u = hype_rd16(ent + off[i]);
        unsigned idx = base + i;
        if (idx >= lfn_cap - 1u) {
            break;
        }
        lfn[idx] = (u == 0u || u == 0xFFFFu) ? '\0' : (char)(u & 0xFFu);
    }
}

/* ================================ FAT32 ================================ */

typedef struct {
    hype_blk_read_fn read;
    void *ctx;
    uint32_t spc;         /* sectors per cluster */
    uint32_t reserved;    /* reserved sector count */
    uint32_t fat_start;   /* == reserved */
    uint32_t fat_size;    /* sectors per FAT */
    uint32_t data_start;  /* first data sector (cluster 2) */
    uint32_t root_cluster;
} fat32_vol_t;

#define FAT32_EOC 0x0FFFFFF8u /* >= this in a chain entry means end-of-chain */
#define FAT_ATTR_DIR 0x10u
#define FAT_ATTR_VOLUME_ID 0x08u
#define FAT_ATTR_LFN 0x0Fu

static uint64_t cluster_lba(const fat32_vol_t *v, uint32_t cl) {
    return (uint64_t)v->data_start + (uint64_t)(cl - 2u) * v->spc;
}

/* Next cluster in the chain, or a value >= FAT32_EOC on end/error. */
/*
 * #347: one cached FAT sector, owned by the CALLER's stack (never file-global -- two resolves
 * sharing it silently is the multi-VM singleton hazard). One FAT sector holds 128 entries, and
 * a chain walk visits them nearly sequentially -- so caching the last sector turns one read per
 * CLUSTER into roughly one per 128. Measured on real hardware (#346's counters): an 800MB ISO's
 * resolve was 52k+ single-sector USB reads (~30s, budget-expired); with the cache it is ~500.
 */
typedef struct {
    uint32_t lba;
    int valid;
    uint8_t sec[HYPE_BLK_SECTOR_SIZE];
} fat32_cache_t;

static uint32_t fat32_next_cached(const fat32_vol_t *v, uint32_t cl, fat32_cache_t *c) {
    uint32_t byte = cl * 4u;
    uint32_t fat_sec = v->fat_start + byte / HYPE_BLK_SECTOR_SIZE;
    uint32_t within = byte % HYPE_BLK_SECTOR_SIZE;
    if (!c->valid || c->lba != fat_sec) {
        if (v->read(v->ctx, fat_sec, 1u, c->sec) != 0) {
            c->valid = 0;
            return FAT32_EOC;
        }
        c->lba = fat_sec;
        c->valid = 1;
    }
    return hype_rd32(c->sec + within) & 0x0FFFFFFFu;
}

static uint32_t fat32_next(const fat32_vol_t *v, uint32_t cl) {
    fat32_cache_t c;
    c.valid = 0;
    return fat32_next_cached(v, cl, &c);
}

/*
 * Scan directory starting at cluster `dir_cl` for `comp` (len `clen`). On match
 * fills *first_cl, *attr, *size and returns 1; returns 0 if not found, -1 on a
 * read error.
 */
static int fat32_find_in_dir(const fat32_vol_t *v, uint32_t dir_cl, const char *comp,
                             unsigned clen, uint32_t *first_cl, uint8_t *attr, uint32_t *size) {
    char lfn[256];
    char sname[16];
    int have_lfn = 0;
    unsigned guard = 0;

    lfn[0] = '\0';
    while (dir_cl < FAT32_EOC && guard++ < (1u << 20)) {
        uint32_t s;
        for (s = 0; s < v->spc; s++) {
            uint8_t sec[HYPE_BLK_SECTOR_SIZE];
            unsigned e;
            if (v->read(v->ctx, cluster_lba(v, dir_cl) + s, 1u, sec) != 0) {
                return -1;
            }
            for (e = 0; e < HYPE_BLK_SECTOR_SIZE; e += 32u) {
                const uint8_t *ent = sec + e;
                uint8_t a = ent[0x0B];
                if (ent[0] == 0x00u) {
                    return 0; /* end of directory */
                }
                if (ent[0] == 0xE5u) {
                    have_lfn = 0;
                    lfn[0] = '\0';
                    continue; /* deleted */
                }
                if (a == FAT_ATTR_LFN) {
                    unsigned seq = ent[0] & 0x1Fu;
                    if (seq >= 1u) {
                        /* lfn_piece clamps its writes to the buffer, so an
                         * out-of-range sequence number can't overflow lfn[]. */
                        lfn_piece(ent, seq, lfn, sizeof(lfn));
                        have_lfn = 1;
                    }
                    continue;
                }
                if ((a & FAT_ATTR_VOLUME_ID) != 0u) {
                    have_lfn = 0;
                    lfn[0] = '\0';
                    continue; /* volume label / not a file-or-dir */
                }
                short_name(ent, sname);
                {
                    int matched = 0;
                    if (have_lfn && str_len(lfn) == clen && ci_eq(lfn, comp, clen)) {
                        matched = 1;
                    } else if (str_len(sname) == clen && ci_eq(sname, comp, clen)) {
                        matched = 1;
                    }
                    if (matched) {
                        *first_cl = ((uint32_t)hype_rd16(ent + 0x14) << 16) | hype_rd16(ent + 0x1A);
                        *attr = a;
                        *size = hype_rd32(ent + 0x1C);
                        return 1;
                    }
                }
                have_lfn = 0;
                lfn[0] = '\0';
            }
        }
        dir_cl = fat32_next(v, dir_cl);
    }
    return 0;
}

/* Follow the file's cluster chain, coalescing on-disk-consecutive clusters into
 * runs, into out->extents. Trims the last extent to the exact file size. */
static int fat32_build_extents(const fat32_vol_t *v, uint32_t first_cl, uint32_t size,
                               hype_file_map_t *out) {
    uint32_t cl = first_cl;
    uint64_t sectors_per_cluster = v->spc;
    unsigned guard = 0;
    fat32_cache_t fc;
    fc.valid = 0;

    out->count = 0;
    out->size_bytes = size;
    if (size == 0u) {
        return 0; /* empty file: zero extents */
    }
    while (cl >= 2u && cl < FAT32_EOC && guard++ < (1u << 20)) {
        uint64_t lba = cluster_lba(v, cl);
        if (out->count > 0u) {
            hype_file_extent_t *last = &out->extents[out->count - 1u];
            if (last->start_lba + last->sector_count == lba) {
                last->sector_count += sectors_per_cluster; /* contiguous: extend */
                cl = fat32_next_cached(v, cl, &fc);
                continue;
            }
        }
        if (out->count >= HYPE_FILE_MAX_EXTENTS) {
            /* #366: distinguishable from every other -1. The caller cannot otherwise tell "too
             * fragmented to map" from "no such file", and those need different advice. */
            out->too_fragmented = 1;
            return -1;
        }
        out->extents[out->count].start_lba = lba;
        out->extents[out->count].sector_count = sectors_per_cluster;
        out->count++;
        cl = fat32_next_cached(v, cl, &fc);
    }

    /* Trim the trailing extent to the exact file length (clusters are rounded
     * up; the ISO stream must not read past size_bytes). */
    {
        uint64_t total_sectors = ((uint64_t)size + HYPE_BLK_SECTOR_SIZE - 1u) / HYPE_BLK_SECTOR_SIZE;
        uint64_t acc = 0;
        unsigned i;
        for (i = 0; i < out->count; i++) {
            uint64_t remain = total_sectors - acc;
            if (out->extents[i].sector_count >= remain) {
                out->extents[i].sector_count = remain;
                out->count = i + 1u;
                acc = total_sectors;
                break;
            }
            acc += out->extents[i].sector_count;
        }
        /* A chain that ran out (bad first cluster, a chain shorter than the
         * recorded size, a loop trapped by the guard) must not be reported as
         * success with a short extent list -- the caller would read whatever
         * happens to live past the end of the data it did get. */
        if (acc < total_sectors) {
            return -1;
        }
    }
    return 0;
}

int hype_fat32_resolve(hype_blk_read_fn read, void *ctx, const char *path, hype_file_map_t *out) {
    /* #366: cleared HERE, not at the extent-walk, because a path that fails earlier (no such
     * file, bad volume) returns before ever reaching it -- and would then inherit the previous
     * call's reason. That would tell the operator to defragment a stick that simply does not have
     * the file on it. Caught by a test, not on hardware. */
    if (out != 0) out->too_fragmented = 0;
    uint8_t bpb[HYPE_BLK_SECTOR_SIZE];
    fat32_vol_t v;
    uint32_t dir_cl;
    unsigned pos = 0;
    char comp[128];
    char next[128];
    unsigned clen;

    if (read(ctx, 0u, 1u, bpb) != 0) {
        return -1;
    }
    if (hype_rd16(bpb + 0x0B) != HYPE_BLK_SECTOR_SIZE) {
        return -1; /* only 512-byte logical sectors supported */
    }
    /* FAT32 has a zero 16-bit FATSz (0x16) and a nonzero 32-bit FATSz (0x24). */
    if (hype_rd16(bpb + 0x16) != 0u || hype_rd32(bpb + 0x24) == 0u) {
        return -1;
    }
    v.read = read;
    v.ctx = ctx;
    v.spc = bpb[0x0D];
    v.reserved = hype_rd16(bpb + 0x0E);
    v.fat_start = v.reserved;
    v.fat_size = hype_rd32(bpb + 0x24);
    v.root_cluster = hype_rd32(bpb + 0x2C);
    if (v.spc == 0u || v.reserved == 0u || v.root_cluster < 2u) {
        return -1;
    }
    v.data_start = v.reserved + (uint32_t)bpb[0x10] * v.fat_size;

    /* Walk the path. Every component but the last must be a directory. */
    clen = next_component(path, &pos, comp, sizeof(comp));
    if (clen == 0u) {
        return -1; /* empty path */
    }
    dir_cl = v.root_cluster;
    for (;;) {
        uint32_t first_cl = 0;
        uint8_t attr = 0;
        uint32_t size = 0;
        unsigned nlen = next_component(path, &pos, next, sizeof(next));
        int is_last = (nlen == 0u);
        int rc = fat32_find_in_dir(&v, dir_cl, comp, clen, &first_cl, &attr, &size);
        if (rc <= 0) {
            return -1; /* not found or read error */
        }
        if (is_last) {
            if ((attr & FAT_ATTR_DIR) != 0u) {
                return -1; /* path names a directory, not a file */
            }
            return fat32_build_extents(&v, first_cl, size, out);
        }
        if ((attr & FAT_ATTR_DIR) == 0u) {
            return -1; /* a non-final component is not a directory */
        }
        dir_cl = first_cl;
        {
            unsigned i;
            for (i = 0; i <= nlen && i < sizeof(comp); i++) {
                comp[i] = next[i];
            }
            clen = nlen;
        }
    }
}

/* ================================ exFAT ================================ */

/*
 * exFAT layout (Microsoft exFAT spec, §3.1 Main Boot Sector): VolumeLength
 * (0x48, u64 sectors), FatOffset (0x50, u32 sectors), FatLength (0x54, u32
 * sectors), ClusterHeapOffset (0x58, u32 sectors), ClusterCount (0x5C, u32),
 * FirstClusterOfRootDirectory (0x60, u32), VolumeFlags (0x6A, u16),
 * BytesPerSectorShift (0x6C, u8), SectorsPerClusterShift (0x6D, u8),
 * NumberOfFats (0x6E, u8).
 *
 * Directory entries are 32 bytes and come in *sets*: a File entry (0x85), a
 * Stream Extension entry (0xC0, carrying the first cluster, the data length and
 * the NoFatChain flag) and one or more File Name entries (0xC1, 15 UTF-16 code
 * units each), covered as a whole by the File entry's SetChecksum. Reading and
 * validating a set is shared with the writer (core/fat_exfat.c); this file adds
 * the read-only path-walk and extent construction on top.
 */
typedef struct {
    hype_blk_read_fn read;
    void *ctx;
    uint32_t fat_lba;         /* first sector of the ACTIVE FAT */
    uint32_t fat_length;      /* sectors per FAT */
    uint32_t cluster_heap;    /* first sector of the cluster heap (cluster 2) */
    uint32_t cluster_count;   /* clusters in the heap; valid are 2..count+1 */
    uint32_t root_cluster;
    uint32_t sec_per_cluster; /* 1 << SectorsPerClusterShift */
} exfat_vol_t;

static int exfat_cluster_ok(const exfat_vol_t *v, uint32_t cl) {
    return (cl >= 2u && cl <= v->cluster_count + 1u) ? 1 : 0;
}

static uint64_t exfat_cluster_lba(const exfat_vol_t *v, uint32_t cl) {
    return hype_exfat_cluster_lba(v->cluster_heap, v->sec_per_cluster, cl);
}

/* Next cluster in a chain, or >= HYPE_EXFAT_EOC on end-of-chain / error. */
static uint32_t exfat_next(const exfat_vol_t *v, uint32_t cl) {
    uint8_t sec[HYPE_BLK_SECTOR_SIZE];
    uint32_t byte = cl * 4u;
    uint32_t fat_sec = v->fat_lba + byte / HYPE_BLK_SECTOR_SIZE;
    uint32_t within = byte % HYPE_BLK_SECTOR_SIZE;
    if (!exfat_cluster_ok(v, cl)) {
        return HYPE_EXFAT_EOC;
    }
    if (v->read(v->ctx, fat_sec, 1u, sec) != 0) {
        return HYPE_EXFAT_EOC;
    }
    return hype_rd32(sec + within);
}

/*
 * Reads the 32-byte directory entry at index `ei` of the directory whose
 * allocation starts at `dir_cl`. `dir_contig` says the directory is NoFatChain,
 * in which case its clusters are consecutive and the FAT holds nothing for them
 * -- walking the FAT there would follow zeroes off the end of the directory. That
 * is a real case (formatters mark contiguous directories NoFatChain), so the flag
 * has to be honoured, not assumed clear.
 * Returns 0 ok, -1 on error / past the end of the allocation.
 */
static int exfat_read_entry(const exfat_vol_t *v, uint32_t dir_cl, int dir_contig, uint32_t ei,
                            uint8_t ent[32]) {
    uint32_t cluster_index, sec_in_cluster;
    unsigned off_in_sector;
    uint8_t sec[HYPE_BLK_SECTOR_SIZE];
    uint32_t cl = dir_cl;
    unsigned i;

    hype_exfat_entry_pos(ei, v->sec_per_cluster, &cluster_index, &sec_in_cluster, &off_in_sector);
    if (dir_contig) {
        cl = dir_cl + cluster_index;
        if (cl < dir_cl || !exfat_cluster_ok(v, cl)) {
            return -1;
        }
    } else {
        unsigned guard = 0;
        while (cluster_index-- > 0u) {
            cl = exfat_next(v, cl);
            if (!exfat_cluster_ok(v, cl) || guard++ > (1u << 20)) {
                return -1;
            }
        }
    }
    if (!exfat_cluster_ok(v, cl)) {
        return -1;
    }
    if (v->read(v->ctx, exfat_cluster_lba(v, cl) + sec_in_cluster, 1u, sec) != 0) {
        return -1;
    }
    for (i = 0; i < 32u; i++) {
        ent[i] = sec[off_in_sector + i];
    }
    return 0;
}

/* Context for the shared hype_exfat_set_read() entry fetcher. */
typedef struct {
    const exfat_vol_t *v;
    uint32_t dir_cl;
    int dir_contig;
} exfat_walk_t;

static int exfat_walk_entry(void *ctx, uint32_t ei, uint8_t ent[32]) {
    exfat_walk_t *w = (exfat_walk_t *)ctx;
    return exfat_read_entry(w->v, w->dir_cl, w->dir_contig, ei, ent);
}

/*
 * Range-checks a validated entry set's allocation against the volume, so a
 * corrupt or hostile directory cannot aim the caller's reads outside the cluster
 * heap. Returns 1 if the set is safe to act on.
 */
static int exfat_set_in_range(const exfat_vol_t *v, const hype_exfat_set_t *set) {
    uint64_t bytes_per_cluster = (uint64_t)v->sec_per_cluster * HYPE_BLK_SECTOR_SIZE;
    uint64_t need;

    if (set->first_cluster == 0u) {
        return (set->data_length == 0u) ? 1 : 0;
    }
    if (!exfat_cluster_ok(v, set->first_cluster)) {
        return 0;
    }
    need = (set->data_length + bytes_per_cluster - 1u) / bytes_per_cluster;
    if (need > (uint64_t)v->cluster_count) {
        return 0;
    }
    if (set->contiguous &&
        (uint64_t)set->first_cluster + need > (uint64_t)v->cluster_count + 2u) {
        return 0;
    }
    return 1;
}

/* Builds the extent list for a validated, in-range stream. */
static int exfat_build_extents(const exfat_vol_t *v, const hype_exfat_set_t *set,
                               hype_file_map_t *out) {
    uint64_t total_sectors =
        (set->data_length + HYPE_BLK_SECTOR_SIZE - 1u) / HYPE_BLK_SECTOR_SIZE;
    uint64_t acc = 0;
    uint32_t cl = set->first_cluster;
    unsigned guard = 0;

    out->count = 0;
    out->size_bytes = set->data_length;
    if (set->data_length == 0u) {
        return 0; /* empty file: zero extents */
    }
    if (set->contiguous) {
        /* One extent: the common case for a file written in one go. */
        out->extents[0].start_lba = exfat_cluster_lba(v, cl);
        out->extents[0].sector_count = total_sectors;
        out->count = 1u;
        return 0;
    }
    while (exfat_cluster_ok(v, cl) && acc < total_sectors && guard++ < (1u << 20)) {
        uint64_t lba = exfat_cluster_lba(v, cl);
        uint64_t remain = total_sectors - acc;
        uint64_t this_sectors = (remain < v->sec_per_cluster) ? remain : v->sec_per_cluster;
        if (out->count > 0u) {
            hype_file_extent_t *last = &out->extents[out->count - 1u];
            if (last->start_lba + last->sector_count == lba) {
                last->sector_count += this_sectors; /* on-disk consecutive: extend */
                acc += this_sectors;
                cl = exfat_next(v, cl);
                continue;
            }
        }
        if (out->count >= HYPE_FILE_MAX_EXTENTS) {
            /* #366: distinguishable from every other -1. The caller cannot otherwise tell "too
             * fragmented to map" from "no such file", and those need different advice. */
            out->too_fragmented = 1;
            return -1;
        }
        out->extents[out->count].start_lba = lba;
        out->extents[out->count].sector_count = this_sectors;
        out->count++;
        acc += this_sectors;
        cl = exfat_next(v, cl);
    }
    if (acc < total_sectors) {
        return -1; /* the chain is shorter than DataLength claims: refuse it rather
                    * than hand back extents that stop early */
    }
    return 0;
}

/*
 * Scans one directory for `comp` (an ASCII component of length `clen`). Returns 1
 * and fills *set on a match, 0 if the directory ends without one, -1 on a read
 * error. Entry sets that fail validation are stepped over rather than trusted.
 */
static int exfat_find_in_dir(const exfat_vol_t *v, uint32_t dir_cl, int dir_contig,
                             const char *comp, unsigned clen, hype_exfat_set_t *set) {
    exfat_walk_t walk;
    uint32_t ei = 0;
    unsigned guard = 0;

    walk.v = v;
    walk.dir_cl = dir_cl;
    walk.dir_contig = dir_contig;

    while (guard++ < (1u << 22)) {
        uint8_t ent[32];
        if (exfat_read_entry(v, dir_cl, dir_contig, ei, ent) != 0) {
            return 0; /* ran off the end of the directory's allocation */
        }
        if (ent[0] == 0x00u) {
            return 0; /* end-of-directory marker */
        }
        /* An entry with the InUse bit clear is deleted; anything that is not a
         * File entry (the volume label, the allocation bitmap, the up-case table,
         * a not-in-use volume GUID) is simply not a name. */
        if ((ent[0] & HYPE_EXFAT_ENT_INUSE) == 0u || ent[0] != HYPE_EXFAT_ENT_FILE) {
            ei++;
            continue;
        }
        if (hype_exfat_set_read(exfat_walk_entry, &walk, ei, set) != 0 ||
            !exfat_set_in_range(v, set)) {
            ei++; /* malformed set: step over its File entry and keep looking */
            continue;
        }
        if (set->name_length == clen) {
            unsigned i;
            int eq = 1;
            for (i = 0; i < clen; i++) {
                /* ASCII-fold both sides. The volume's up-case table is not read
                 * here (that needs the writer's mount); for ASCII names -- every
                 * name hype resolves -- folding a-z to A-Z is exactly what the
                 * reference table does. */
                if (lc((char)(set->name[i] & 0xFFu)) != lc(comp[i]) ||
                    (set->name[i] & 0xFF00u) != 0u) {
                    eq = 0;
                    break;
                }
            }
            if (eq) {
                return 1;
            }
        }
        ei += 1u + set->secondary; /* skip the whole set */
    }
    return -1;
}

int hype_exfat_resolve(hype_blk_read_fn read, void *ctx, const char *path, hype_file_map_t *out) {
    if (out != 0) out->too_fragmented = 0; /* #366: see hype_fat32_resolve */
    uint8_t boot[HYPE_BLK_SECTOR_SIZE];
    exfat_vol_t v;
    hype_exfat_set_t set;
    uint32_t dir_cl;
    int dir_contig = 0; /* the root directory has no Stream entry: always FAT-chained */
    unsigned pos = 0;
    char comp[128];
    uint8_t num_fats;
    uint16_t volume_flags;
    uint32_t fat_offset;

    if (read(ctx, 0u, 1u, boot) != 0) {
        return -1;
    }
    /* "EXFAT   " signature at offset 3 -- all eight bytes, so a volume whose name
     * merely starts with those five characters is not mistaken for exFAT. */
    if (boot[3] != 'E' || boot[4] != 'X' || boot[5] != 'F' || boot[6] != 'A' || boot[7] != 'T' ||
        boot[8] != ' ' || boot[9] != ' ' || boot[10] != ' ') {
        return -1;
    }
    if (boot[0x6C] != 9u) {
        return -1; /* BytesPerSectorShift must be 9 (512) */
    }
    if (boot[0x6D] > 16u) {
        return -1; /* implausible SectorsPerClusterShift (>32 MiB clusters); also
                    * keeps the 1u << shift below well-defined */
    }
    num_fats = boot[0x6E];
    if (num_fats != 1u && num_fats != 2u) {
        return -1;
    }
    volume_flags = hype_rd16(boot + 0x6A);
    fat_offset = hype_rd32(boot + 0x50);

    v.read = read;
    v.ctx = ctx;
    v.fat_length = hype_rd32(boot + 0x54);
    v.cluster_heap = hype_rd32(boot + 0x58);
    v.cluster_count = hype_rd32(boot + 0x5C);
    v.root_cluster = hype_rd32(boot + 0x60);
    v.sec_per_cluster = 1u << boot[0x6D];
    /* With two FATs, VolumeFlags bit 0 (ActiveFat) says which copy is live;
     * reading the stale one would follow chains that no longer exist. */
    v.fat_lba = fat_offset;
    if (num_fats == 2u && (volume_flags & 0x0001u) != 0u) {
        v.fat_lba += v.fat_length;
    }

    if (fat_offset < 24u || v.fat_length == 0u) {
        return -1; /* the first 24 sectors are the two boot regions */
    }
    if (v.cluster_heap < fat_offset + (uint32_t)num_fats * v.fat_length) {
        return -1; /* the cluster heap would overlap the FAT region */
    }
    if (v.cluster_count == 0u || v.cluster_count > 0xFFFFFFF4u) {
        return -1;
    }
    /* The FAT must be able to address every cluster (entries 0 and 1 are
     * reserved), or a chain walk would read another structure's bytes. */
    if ((uint64_t)v.fat_length * (HYPE_BLK_SECTOR_SIZE / 4u) < (uint64_t)v.cluster_count + 2u) {
        return -1;
    }
    if ((uint64_t)v.cluster_heap + (uint64_t)v.cluster_count * v.sec_per_cluster >
        hype_rd64(boot + 0x48)) {
        return -1; /* the heap would run past the end of the volume */
    }
    if (!exfat_cluster_ok(&v, v.root_cluster)) {
        return -1;
    }

    dir_cl = v.root_cluster;
    for (;;) {
        unsigned clen = next_component(path, &pos, comp, sizeof(comp));
        int at_end;
        int rc;

        if (clen == 0u || clen >= sizeof(comp)) {
            return -1; /* empty path, or a component longer than we can hold */
        }
        /* Peek whether this is the last component (a trailing separator still
         * means it is). */
        {
            unsigned save = pos;
            char tmp[8];
            at_end = (next_component(path, &save, tmp, sizeof(tmp)) == 0u);
        }

        rc = exfat_find_in_dir(&v, dir_cl, dir_contig, comp, clen, &set);
        if (rc <= 0) {
            return -1; /* not found or read error */
        }
        if (at_end) {
            if ((set.attributes & HYPE_EXFAT_ATTR_DIRECTORY) != 0u) {
                return -1; /* path names a directory, not a file */
            }
            return exfat_build_extents(&v, &set, out);
        }
        if ((set.attributes & HYPE_EXFAT_ATTR_DIRECTORY) == 0u) {
            return -1; /* a non-final component is not a directory */
        }
        dir_cl = set.first_cluster;
        dir_contig = set.contiguous;
        if (!exfat_cluster_ok(&v, dir_cl)) {
            return -1;
        }
    }
}
