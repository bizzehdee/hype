#include "fat_write_fs.h"
#include "fat_write.h"

#define SECSZ HYPE_FAT_SECTOR_SIZE
#define FAT32_EOC_MIN 0x0FFFFFF8u /* entry >= this in a chain means end-of-chain */
#define DIRENT_SIZE 32u
#define UNKNOWN 0xFFFFFFFFu

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void bcopy(uint8_t *dst, const uint8_t *src, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}
static void bzero(uint8_t *dst, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) dst[i] = 0u;
}

static uint64_t cluster_lba(const hype_fat32_fs_t *fs, uint32_t cl) {
    return (uint64_t)fs->data_start + (uint64_t)(cl - 2u) * fs->spc;
}

/* Read a single FAT entry (from FAT copy 0). */
static int fat_get(hype_fat32_fs_t *fs, uint32_t cl, uint32_t *out) {
    uint8_t sec[SECSZ];
    uint64_t slba = (uint64_t)fs->reserved + cl / HYPE_FAT32_ENTRIES_PER_SECTOR;
    if (fs->read(fs->ctx, slba, 1u, sec) != 0) return -1;
    *out = hype_fat32_entry_get(sec, cl % HYPE_FAT32_ENTRIES_PER_SECTOR);
    return 0;
}

/* Write a FAT entry into every FAT copy (read-modify-write, reserved nibble kept). */
static int fat_set(hype_fat32_fs_t *fs, uint32_t cl, uint32_t val) {
    uint8_t sec[SECSZ];
    unsigned int idx = cl % HYPE_FAT32_ENTRIES_PER_SECTOR;
    uint32_t off = cl / HYPE_FAT32_ENTRIES_PER_SECTOR;
    unsigned int copy;
    for (copy = 0; copy < fs->num_fats; copy++) {
        uint64_t slba = (uint64_t)fs->reserved + (uint64_t)copy * fs->fat_size + off;
        if (fs->read(fs->ctx, slba, 1u, sec) != 0) return -1;
        hype_fat32_entry_set(sec, idx, val);
        if (fs->write(fs->ctx, slba, 1u, sec) != 0) return -1;
    }
    return 0;
}

/* Zero every sector of a data cluster (used when a fresh directory cluster is
 * allocated so its entries read as end-of-directory terminators). */
static int cluster_zero(hype_fat32_fs_t *fs, uint32_t cl) {
    uint8_t sec[SECSZ];
    unsigned int s;
    bzero(sec, SECSZ);
    for (s = 0; s < fs->spc; s++) {
        if (fs->write(fs->ctx, cluster_lba(fs, cl) + s, 1u, sec) != 0) return -1;
    }
    return 0;
}

/* Allocate one free cluster, mark it end-of-chain, and update the free hints.
 * Returns 0 and the cluster in *out, or -1 if the volume is full. */
static int alloc_cluster(hype_fat32_fs_t *fs, uint32_t *out) {
    uint32_t start = (fs->next_free >= 2u && fs->next_free <= fs->max_cluster) ? fs->next_free : 2u;
    uint32_t scanned = 0;
    uint32_t total = fs->max_cluster - 2u + 1u;
    uint32_t cl = start;

    while (scanned < total) {
        uint32_t v;
        if (fat_get(fs, cl, &v) != 0) return -1;
        if (v == 0u) {
            if (fat_set(fs, cl, FAT32_EOC_MIN | 0x7u) != 0) return -1; /* 0x0FFFFFFF EOC */
            fs->next_free = (cl + 1u > fs->max_cluster) ? 2u : (cl + 1u);
            if (fs->free_count != UNKNOWN && fs->free_count != 0u) fs->free_count--;
            *out = cl;
            return 0;
        }
        cl = (cl + 1u > fs->max_cluster) ? 2u : (cl + 1u);
        scanned++;
    }
    return -1; /* full */
}

/* Free an entire cluster chain (used to truncate an existing file). */
static int free_chain(hype_fat32_fs_t *fs, uint32_t first) {
    uint32_t cl = first;
    unsigned int guard = 0;
    while (cl >= 2u && cl <= fs->max_cluster && guard <= fs->max_cluster) {
        uint32_t next;
        if (fat_get(fs, cl, &next) != 0) return -1;
        if (fat_set(fs, cl, 0u) != 0) return -1;
        if (fs->free_count != UNKNOWN) fs->free_count++;
        if (fs->next_free == 0u || cl < fs->next_free) fs->next_free = cl;
        if (next >= FAT32_EOC_MIN) break;
        cl = next;
        guard++;
    }
    return 0;
}

int hype_fat32_fs_mount(hype_fat_read_fn read, hype_fat_write_fn write, void *ctx,
                        hype_fat32_fs_t *out) {
    uint8_t bpb[SECSZ];
    uint32_t total_sectors, data_sectors, data_clusters, fat_capacity;

    /* Invalidate the timestamp snapshot FIRST: a caller with a stack-allocated
     * hype_fat32_fs_t would otherwise stamp directory entries from uninitialised
     * memory, i.e. random dates. Callers opt in via hype_fat32_fs_set_time(). */
    out->now.year = 0;
    if (read(ctx, 0u, 1u, bpb) != 0) return -1;
    if (rd16(bpb + 0x0B) != SECSZ) return -1;      /* bytes/sector must be 512 */
    if (rd16(bpb + 0x16) != 0u || rd32(bpb + 0x24) == 0u) return -1; /* FAT16 shape / no FAT32 size */

    out->read = read;
    out->write = write;
    out->ctx = ctx;
    out->spc = bpb[0x0D];
    out->reserved = rd16(bpb + 0x0E);
    out->num_fats = bpb[0x10];
    out->fat_size = rd32(bpb + 0x24);
    out->root_cluster = rd32(bpb + 0x2C);
    out->fsinfo_sector = rd16(bpb + 0x30);
    if (out->spc == 0u || out->reserved == 0u || out->num_fats == 0u || out->root_cluster < 2u) {
        return -1;
    }
    out->data_start = out->reserved + out->num_fats * out->fat_size;

    total_sectors = rd16(bpb + 0x13);
    if (total_sectors == 0u) total_sectors = rd32(bpb + 0x20);
    if (total_sectors <= out->data_start) return -1;
    data_sectors = total_sectors - out->data_start;
    data_clusters = data_sectors / out->spc;
    fat_capacity = out->fat_size * HYPE_FAT32_ENTRIES_PER_SECTOR; /* entries the FAT can address */
    out->max_cluster = data_clusters + 1u; /* clusters are numbered from 2 */
    if (out->max_cluster > fat_capacity - 1u) out->max_cluster = fat_capacity - 1u;
    if (out->max_cluster < 2u) return -1;

    /* FSInfo free-cluster accounting (treat a missing/invalid FSInfo as unknown). */
    out->free_count = UNKNOWN;
    out->next_free = 2u;
    if (out->fsinfo_sector != 0u) {
        uint8_t fsi[SECSZ];
        if (read(ctx, out->fsinfo_sector, 1u, fsi) == 0 && rd32(fsi + 0) == 0x41615252u) {
            out->free_count = rd32(fsi + 0x1E8);
            out->next_free = rd32(fsi + 0x1EC);
            if (out->next_free < 2u || out->next_free > out->max_cluster) out->next_free = 2u;
        }
    }
    return 0;
}

/* Flush the free-cluster counters to FSInfo (best-effort: a volume without a
 * usable FSInfo simply keeps none). */
static void fsinfo_flush(hype_fat32_fs_t *fs) {
    if (fs->fsinfo_sector != 0u) {
        uint8_t fsi[SECSZ];
        if (fs->read(fs->ctx, fs->fsinfo_sector, 1u, fsi) == 0 &&
            hype_fat32_fsinfo_set(fsi, fs->free_count, fs->next_free) == 0) {
            (void)fs->write(fs->ctx, fs->fsinfo_sector, 1u, fsi);
        }
    }
}

/* Write the file's current first-cluster + size into its directory entry, and
 * flush the free-cluster counters to FSInfo. */
static int flush_metadata(hype_fat32_wfile_t *f) {
    hype_fat32_fs_t *fs = f->fs;
    uint8_t sec[SECSZ];
    uint8_t ent[DIRENT_SIZE];
    uint32_t sz = (f->size > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)f->size;

    if (fs->read(fs->ctx, f->dirent_lba, 1u, sec) != 0) return -1;
    hype_fat_dirent_build(ent, f->name11, HYPE_FAT_ATTR_ARCHIVE, f->first_cluster, sz,
                          &f->fs->now);
    bcopy(sec + f->dirent_off, ent, DIRENT_SIZE);
    if (fs->write(fs->ctx, f->dirent_lba, 1u, sec) != 0) return -1;

    fsinfo_flush(fs);
    return 0;
}

/* Names match if their 11-byte 8.3 fields are byte-identical. */
static int name_eq(const uint8_t *a, const uint8_t *b) {
    unsigned int i;
    for (i = 0; i < 11u; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* ---- directory-entry addressing (#247) ----
 *
 * Directory entries are addressed by a flat index into the directory's cluster
 * chain, the same scheme the exFAT layer uses. Bounded walks everywhere: a
 * corrupt (looping) chain must fail, not spin.
 */

static int cluster_ok(const hype_fat32_fs_t *fs, uint32_t cl) {
    return (cl >= 2u && cl <= fs->max_cluster) ? 1 : 0;
}

/* Cluster at chain index `ci` of the directory starting at `first`. */
static int dir_cluster_at(hype_fat32_fs_t *fs, uint32_t first, uint32_t ci, uint32_t *out) {
    uint32_t cl = first;
    uint32_t i;
    if (!cluster_ok(fs, cl)) return -1;
    for (i = 0; i < ci; i++) {
        uint32_t next;
        if (fat_get(fs, cl, &next) != 0) return -1;
        if (next >= FAT32_EOC_MIN || !cluster_ok(fs, next)) return -1;
        cl = next;
    }
    *out = cl;
    return 0;
}

static int dirent_pos(hype_fat32_fs_t *fs, uint32_t dir_first, uint32_t ei, uint64_t *out_lba,
                      unsigned int *out_off) {
    uint32_t epc = fs->spc * (SECSZ / DIRENT_SIZE);
    uint32_t within = ei % epc;
    uint32_t cl;
    if (dir_cluster_at(fs, dir_first, ei / epc, &cl) != 0) return -1;
    *out_lba = cluster_lba(fs, cl) + within / (SECSZ / DIRENT_SIZE);
    *out_off = (within % (SECSZ / DIRENT_SIZE)) * DIRENT_SIZE;
    return 0;
}

/* Entries the directory's CURRENT allocation holds; -1 for a broken or
 * looping chain. Every walk below is bounded by this, so a corrupt chain
 * fails in one cluster-count walk instead of DIR_GUARD quadratic reads. */
static int dir_capacity(hype_fat32_fs_t *fs, uint32_t first, uint32_t *out_entries) {
    uint32_t cl = first;
    uint32_t clusters = 0;
    uint32_t guard = 0;
    if (!cluster_ok(fs, cl)) return -1;
    while (guard++ <= fs->max_cluster) {
        uint32_t next;
        clusters++;
        if (fat_get(fs, cl, &next) != 0) return -1;
        if (next >= FAT32_EOC_MIN) {
            *out_entries = clusters * fs->spc * (SECSZ / DIRENT_SIZE);
            return 0;
        }
        if (!cluster_ok(fs, next)) return -1;
        cl = next;
    }
    return -1; /* the chain loops */
}

static int dirent_read(hype_fat32_fs_t *fs, uint32_t dir_first, uint32_t ei,
                       uint8_t ent[DIRENT_SIZE]) {
    uint8_t sec[SECSZ];
    uint64_t lba;
    unsigned int off;
    if (dirent_pos(fs, dir_first, ei, &lba, &off) != 0) return -1;
    if (fs->read(fs->ctx, lba, 1u, sec) != 0) return -1;
    bcopy(ent, sec + off, DIRENT_SIZE);
    return 0;
}

static int dirent_write(hype_fat32_fs_t *fs, uint32_t dir_first, uint32_t ei,
                        const uint8_t ent[DIRENT_SIZE]) {
    uint8_t sec[SECSZ];
    uint64_t lba;
    unsigned int off;
    if (dirent_pos(fs, dir_first, ei, &lba, &off) != 0) return -1;
    if (fs->read(fs->ctx, lba, 1u, sec) != 0) return -1;
    bcopy(sec + off, ent, DIRENT_SIZE);
    return fs->write(fs->ctx, lba, 1u, sec);
}

/* ---- name matching ---- */

/* Case-insensitive ASCII comparison against a NUL-terminated accumulated LFN. */
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int ci_eq(const char *a, const char *b) {
    unsigned int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (lower(a[i]) != lower(b[i])) return 0;
        i++;
    }
    return a[i] == b[i];
}

/* 1 if `name` has 8.3 SHAPE ignoring case, so it may legitimately match a
 * short name directly (e.g. the query "hypefull.log" for "HYPEFULL LOG"). */
static int name_83_shape(const char *name) {
    char up[13];
    unsigned int i;
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if (i >= sizeof up - 1u) return 0; /* longer than 8+1+3 can never be 8.3 */
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        up[i] = c;
    }
    up[i] = '\0';
    return hype_fat_name_is_83(up);
}

/* A located directory entry: the 8.3 entry itself plus the extent of its
 * validated LFN run (run_start == ent_index when there is none). */
typedef struct {
    uint32_t ent_index;
    uint32_t run_start;
    uint8_t ent[DIRENT_SIZE];
} hype_fat32_dloc_t;

/*
 * Finds `name` in the directory starting at cluster `dir_first`, matching
 * either its accumulated LFN (case-insensitive, checksum-validated against the
 * short name) or -- for a query that has 8.3 shape -- the short name itself.
 * Returns 1 found, 0 not found, -1 on a broken walk.
 */
static int dir_find(hype_fat32_fs_t *fs, uint32_t dir_first, const char *name,
                    hype_fat32_dloc_t *out) {
    char lfn[HYPE_FAT_MAX_LFN + 1u];
    uint8_t short11[11];
    int short_ok = name_83_shape(name);
    uint32_t ei;
    uint32_t run_start = 0;
    int run_active = 0;        /* collecting a plausible LFN run */
    unsigned int expected = 0; /* next (descending) sequence number wanted */
    uint8_t run_chk = 0;

    uint32_t cap = 0;

    if (dir_capacity(fs, dir_first, &cap) != 0) return -1;
    if (short_ok) hype_fat_shortname_83(name, short11);
    lfn[0] = '\0';

    for (ei = 0; ei < cap; ei++) {
        uint8_t ent[DIRENT_SIZE];
        if (dirent_read(fs, dir_first, ei, ent) != 0) return -1; /* real I/O error */
        if (ent[0] == 0x00u) return 0;                          /* end-of-directory */
        if (ent[0] == 0xE5u) { run_active = 0; continue; }      /* deleted */
        if ((ent[11] & 0x3Fu) == HYPE_FAT_ATTR_LFN) {
            unsigned int seq = (unsigned int)(ent[0] & 0x3Fu);
            if (ent[0] & HYPE_FAT_LFN_LAST) {
                unsigned int i;
                for (i = 0; i < sizeof lfn; i++) lfn[i] = '\0';
                if (seq == 0u || hype_fat_lfn_entry_chars(ent, lfn) != seq) {
                    run_active = 0;
                    continue;
                }
                run_active = 1;
                run_start = ei;
                run_chk = ent[13];
                expected = seq - 1u;
            } else if (run_active && expected != 0u && seq == expected && ent[13] == run_chk) {
                (void)hype_fat_lfn_entry_chars(ent, lfn);
                expected = seq - 1u;
            } else {
                run_active = 0; /* out-of-order or foreign piece: not a valid run */
            }
            continue;
        }
        if (ent[11] & HYPE_FAT_ATTR_VOLUME_ID) { run_active = 0; continue; }
        {
            /* The run names this entry only if it counted down to sequence 1
             * immediately before it AND its checksum ties it to this short
             * name (FAT spec: an orphaned run must be ignored). */
            int have_lfn = (run_active && expected == 0u &&
                            hype_fat_shortname_checksum(ent) == run_chk);
            int match = (have_lfn && ci_eq(lfn, name)) ||
                        (short_ok && name_eq(ent, short11));
            run_active = 0;
            if (match) {
                out->ent_index = ei;
                out->run_start = have_lfn ? run_start : ei;
                bcopy(out->ent, ent, DIRENT_SIZE);
                return 1;
            }
        }
    }
    return 0; /* scanned the whole allocation without a match */
}

/* Appends one zeroed cluster to a directory chain. */
static int dir_grow(hype_fat32_fs_t *fs, uint32_t dir_first) {
    uint32_t cl = dir_first, last = dir_first, newcl;
    uint32_t guard = 0;
    while (guard++ <= fs->max_cluster) {
        uint32_t next;
        if (fat_get(fs, cl, &next) != 0) return -1;
        last = cl;
        if (next >= FAT32_EOC_MIN) break;
        if (!cluster_ok(fs, next)) return -1;
        cl = next;
    }
    if (guard > fs->max_cluster + 1u) return -1; /* looping chain */
    if (alloc_cluster(fs, &newcl) != 0) return -1;
    if (cluster_zero(fs, newcl) != 0) return -1;
    return fat_set(fs, last, newcl);
}

/*
 * Finds `need` CONSECUTIVE free (never-used or deleted) entry slots -- an LFN
 * run plus its 8.3 entry must be contiguous -- growing the directory a cluster
 * at a time when it is full. In-use entries are stepped over one at a time; a
 * free run may span a cluster seam.
 */
static int find_slots(hype_fat32_fs_t *fs, uint32_t dir_first, unsigned int need,
                      uint32_t *out_start) {
    unsigned int grows = 0;
    uint32_t ei = 0;
    uint32_t cap = 0;
    uint32_t run_begin = 0;
    unsigned int run = 0;

    if (dir_capacity(fs, dir_first, &cap) != 0) return -1;
    for (;;) {
        uint8_t ent[DIRENT_SIZE];
        if (ei >= cap) {
            /* End of the allocation: grow (bounded -- a 20-entry run needs at
             * most two fresh 512-byte clusters on spc == 1). */
            if (grows++ >= 4u) return -1;
            if (dir_grow(fs, dir_first) != 0) return -1;
            if (dir_capacity(fs, dir_first, &cap) != 0) return -1;
            continue; /* same index, now inside the new cluster */
        }
        if (dirent_read(fs, dir_first, ei, ent) != 0) return -1;
        if (ent[0] == 0x00u || ent[0] == 0xE5u) {
            if (run == 0u) run_begin = ei;
            run++;
            if (run >= need) {
                *out_start = run_begin;
                return 0;
            }
        } else {
            run = 0;
        }
        ei++;
    }
}

/* Marks every entry of [start, end] deleted (0xE5) -- an entry and its LFN run. */
static int run_delete(hype_fat32_fs_t *fs, uint32_t dir_first, uint32_t start, uint32_t end) {
    uint32_t ei;
    for (ei = start; ei <= end; ei++) {
        uint8_t ent[DIRENT_SIZE];
        if (dirent_read(fs, dir_first, ei, ent) != 0) return -1;
        ent[0] = 0xE5u;
        if (dirent_write(fs, dir_first, ei, ent) != 0) return -1;
    }
    return 0;
}

/* ---- paths ---- */

/* Byte offset of the final path component; -1 if there is none ("", "\"). */
static int path_split(const char *path, unsigned int *out_leaf) {
    unsigned int i, leaf = 0;
    int have = 0, in_sep = 1;
    for (i = 0; path[i] != '\0'; i++) {
        if (path[i] == '\\' || path[i] == '/') {
            in_sep = 1;
        } else {
            if (in_sep) { leaf = i; have = 1; }
            in_sep = 0;
        }
    }
    if (!have) return -1;
    *out_leaf = leaf;
    return 0;
}

/* Copies the component starting at `pos` into a NUL-terminated buffer,
 * advancing `pos` past it. Returns its length, 0 for none/over-long. */
static unsigned int component(const char *path, unsigned int *pos, char *out, unsigned int cap) {
    unsigned int n = 0;
    while (path[*pos] == '\\' || path[*pos] == '/') (*pos)++;
    while (path[*pos] != '\0' && path[*pos] != '\\' && path[*pos] != '/') {
        if (n + 1u >= cap) return 0;
        out[n++] = path[*pos];
        (*pos)++;
    }
    out[n] = '\0';
    return n;
}

/*
 * Resolves the parent directory of the entry `path` names (final component at
 * byte `leaf`), starting from the root. When `forbid` is a non-zero cluster,
 * any directory reached on the walk with that first cluster fails it -- rename
 * uses this to refuse moving a directory into its own subtree. Fills the
 * parent's first cluster into *out_first.
 */
static int resolve_parent(hype_fat32_fs_t *fs, const char *path, unsigned int leaf,
                          uint32_t forbid, uint32_t *out_first) {
    uint32_t cur = fs->root_cluster;
    unsigned int pos = 0;
    for (;;) {
        char comp[HYPE_FAT_MAX_LFN + 1u];
        hype_fat32_dloc_t loc;
        uint32_t cl;
        while (path[pos] == '\\' || path[pos] == '/') pos++;
        if (pos >= leaf) {
            *out_first = cur;
            return 0;
        }
        if (component(path, &pos, comp, sizeof comp) == 0u) return -1;
        if (dir_find(fs, cur, comp, &loc) != 1) return -1;
        if ((loc.ent[11] & HYPE_FAT_ATTR_DIRECTORY) == 0u) return -1;
        cl = hype_fat_dirent_cluster(loc.ent);
        if (!cluster_ok(fs, cl)) return -1;
        if (forbid != 0u && cl == forbid) return -1;
        cur = cl;
    }
}

/* Widens the final component into a buffer (no walk). */
static int leaf_name(const char *path, unsigned int leaf, char *out, unsigned int cap) {
    unsigned int pos = leaf;
    return component(path, &pos, out, cap) == 0u ? -1 : 0;
}

/* ---- entry insertion (LFN generation) ---- */

/* 1 if the 11-byte short name is already taken anywhere in the directory. */
static int short_taken(hype_fat32_fs_t *fs, uint32_t dir_first, const uint8_t name11[11]) {
    uint32_t ei;
    uint32_t cap = 0;
    if (dir_capacity(fs, dir_first, &cap) != 0) {
        return 1; /* a broken directory: claim taken, forcing the caller to fail */
    }
    for (ei = 0; ei < cap; ei++) {
        uint8_t ent[DIRENT_SIZE];
        if (dirent_read(fs, dir_first, ei, ent) != 0) return 1;
        if (ent[0] == 0x00u) return 0;
        if (ent[0] == 0xE5u || (ent[11] & 0x3Fu) == HYPE_FAT_ATTR_LFN ||
            (ent[11] & HYPE_FAT_ATTR_VOLUME_ID)) {
            continue;
        }
        if (name_eq(ent, name11)) return 1;
    }
    return 0;
}

/*
 * Inserts `template_ent` (its name field is overwritten) into the directory
 * under `name`: a strict 8.3 name is written directly with no LFN run; any
 * other valid name gets a run of LFN entries over a collision-avoided "~N"
 * short name. Returns 0 and the 8.3 entry's index in *out_ei.
 */
static int insert_entry(hype_fat32_fs_t *fs, uint32_t dir_first, const char *name,
                        const uint8_t template_ent[DIRENT_SIZE], uint32_t *out_ei) {
    uint8_t name11[11];
    uint8_t ent[DIRENT_SIZE];
    unsigned int nlen = 0, lfn_n = 0, k;
    uint32_t start = 0;

    if (!hype_fat_name_valid(name)) return -1;
    while (name[nlen] != '\0') nlen++;

    if (hype_fat_name_is_83(name)) {
        hype_fat_shortname_83(name, name11);
    } else {
        unsigned int n;
        for (n = 1;; n++) {
            if (hype_fat_shortname_tail(name, n, name11) != 0) return -1;
            if (!short_taken(fs, dir_first, name11)) break;
            if (n >= 1000u) return -1; /* a directory of a thousand collisions */
        }
        lfn_n = (nlen + HYPE_FAT_LFN_CHARS - 1u) / HYPE_FAT_LFN_CHARS;
    }

    if (find_slots(fs, dir_first, lfn_n + 1u, &start) != 0) return -1;
    if (lfn_n != 0u) {
        uint8_t chk = hype_fat_shortname_checksum(name11);
        for (k = 0; k < lfn_n; k++) {
            /* Physical order is logically-last-first: sequence lfn_n first. */
            unsigned int seq = lfn_n - k;
            hype_fat_lfn_entry_build(ent, name, nlen, seq, k == 0u, chk);
            if (dirent_write(fs, dir_first, start + k, ent) != 0) return -1;
        }
    }
    bcopy(ent, template_ent, DIRENT_SIZE);
    bcopy(ent, name11, 11u);
    if (dirent_write(fs, dir_first, start + lfn_n, ent) != 0) return -1;
    *out_ei = start + lfn_n;
    return 0;
}

/* ---- create (now path-based), unlink, mkdir, rmdir, rename (#247) ---- */

int hype_fat32_create(hype_fat32_fs_t *fs, const char *path, hype_fat32_wfile_t *out) {
    char leafbuf[HYPE_FAT_MAX_LFN + 1u];
    hype_fat32_dloc_t loc;
    unsigned int leaf;
    uint32_t parent, first_cl = 0, ei = 0;
    int rc;

    if (path_split(path, &leaf) != 0 || leaf_name(path, leaf, leafbuf, sizeof leafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, path, leaf, 0u, &parent) != 0) return -1;
    if (!hype_fat_name_valid(leafbuf)) return -1;

    rc = dir_find(fs, parent, leafbuf, &loc);
    if (rc < 0) return -1;
    if (rc == 1) {
        /* Truncate in place: the slot -- LFN run included -- stays as it is,
         * only the chain is freed and the entry rewritten by flush_metadata. */
        uint32_t old;
        if (loc.ent[11] & HYPE_FAT_ATTR_DIRECTORY) return -1; /* never clobber a directory */
        old = hype_fat_dirent_cluster(loc.ent);
        if (old >= 2u && free_chain(fs, old) != 0) return -1;
        if (alloc_cluster(fs, &first_cl) != 0) return -1;
        out->fs = fs;
        bcopy(out->name11, loc.ent, 11u);
        out->first_cluster = first_cl;
        out->tail_cluster = first_cl;
        out->size = 0u;
        if (dirent_pos(fs, parent, loc.ent_index, &out->dirent_lba, &out->dirent_off) != 0) {
            return -1;
        }
        return flush_metadata(out);
    }
    /* Fresh file: allocate its first data cluster, then place the entry. */
    if (alloc_cluster(fs, &first_cl) != 0) return -1;
    {
        uint8_t ent[DIRENT_SIZE];
        uint8_t dummy11[11];
        unsigned int i;
        for (i = 0; i < 11u; i++) dummy11[i] = ' '; /* insert_entry overwrites it */
        hype_fat_dirent_build(ent, dummy11, HYPE_FAT_ATTR_ARCHIVE, first_cl, 0u, &fs->now);
        if (insert_entry(fs, parent, leafbuf, ent, &ei) != 0) return -1;
    }
    {
        /* insert_entry chose the short name (possibly a ~N tail): read it back
         * so flush_metadata keeps rewriting the same entry. */
        uint8_t placed[DIRENT_SIZE];
        if (dirent_read(fs, parent, ei, placed) != 0) return -1;
        out->fs = fs;
        bcopy(out->name11, placed, 11u);
        out->first_cluster = first_cl;
        out->tail_cluster = first_cl;
        out->size = 0u;
        if (dirent_pos(fs, parent, ei, &out->dirent_lba, &out->dirent_off) != 0) return -1;
    }
    return flush_metadata(out);
}

int hype_fat32_unlink(hype_fat32_fs_t *fs, const char *path) {
    char leafbuf[HYPE_FAT_MAX_LFN + 1u];
    hype_fat32_dloc_t loc;
    unsigned int leaf;
    uint32_t parent, cl;

    if (path_split(path, &leaf) != 0 || leaf_name(path, leaf, leafbuf, sizeof leafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, path, leaf, 0u, &parent) != 0) return -1;
    if (dir_find(fs, parent, leafbuf, &loc) != 1) return -1;
    if (loc.ent[11] & HYPE_FAT_ATTR_DIRECTORY) return -1; /* directories go through rmdir */
    cl = hype_fat_dirent_cluster(loc.ent);
    if (cl >= 2u && free_chain(fs, cl) != 0) return -1;
    if (run_delete(fs, parent, loc.run_start, loc.ent_index) != 0) return -1;
    fsinfo_flush(fs);
    return 0;
}

int hype_fat32_mkdir(hype_fat32_fs_t *fs, const char *path) {
    char leafbuf[HYPE_FAT_MAX_LFN + 1u];
    hype_fat32_dloc_t loc;
    uint8_t sec[SECSZ];
    uint8_t ent[DIRENT_SIZE];
    uint8_t dot11[11];
    unsigned int leaf, i;
    uint32_t parent, dcl, ei = 0;

    if (path_split(path, &leaf) != 0 || leaf_name(path, leaf, leafbuf, sizeof leafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, path, leaf, 0u, &parent) != 0) return -1;
    if (!hype_fat_name_valid(leafbuf)) return -1;
    if (dir_find(fs, parent, leafbuf, &loc) != 0) return -1; /* exists (either kind), or error */

    if (alloc_cluster(fs, &dcl) != 0) return -1;
    if (cluster_zero(fs, dcl) != 0) return -1;

    /*
     * '.' and '..' as the first two entries (FAT spec) -- exFAT has no such
     * entries, so this is FAT32-only work. '..' holds the parent's first
     * cluster, and 0 -- not the root's real cluster number -- when the parent
     * IS the root.
     */
    for (i = 0; i < 11u; i++) dot11[i] = ' ';
    dot11[0] = '.';
    if (fs->read(fs->ctx, cluster_lba(fs, dcl), 1u, sec) != 0) return -1;
    hype_fat_dirent_build(ent, dot11, HYPE_FAT_ATTR_DIRECTORY, dcl, 0u, &fs->now);
    bcopy(sec + 0, ent, DIRENT_SIZE);
    dot11[1] = '.';
    hype_fat_dirent_build(ent, dot11, HYPE_FAT_ATTR_DIRECTORY,
                          (parent == fs->root_cluster) ? 0u : parent, 0u, &fs->now);
    bcopy(sec + DIRENT_SIZE, ent, DIRENT_SIZE);
    if (fs->write(fs->ctx, cluster_lba(fs, dcl), 1u, sec) != 0) return -1;

    {
        uint8_t dummy11[11];
        for (i = 0; i < 11u; i++) dummy11[i] = ' ';
        hype_fat_dirent_build(ent, dummy11, HYPE_FAT_ATTR_DIRECTORY, dcl, 0u, &fs->now);
        if (insert_entry(fs, parent, leafbuf, ent, &ei) != 0) return -1;
    }
    fsinfo_flush(fs);
    return 0;
}

/*
 * 1 == nothing but '.', '..' and deleted slots; 0 == something else is there
 * (a valid LFN run included -- deleting a directory that still carries entries
 * would orphan them); -1 on a sector-read failure, which must never read as
 * "empty".
 */
static int dir_is_empty(hype_fat32_fs_t *fs, uint32_t first) {
    uint32_t ei;
    uint32_t cap = 0;
    if (dir_capacity(fs, first, &cap) != 0) {
        return -1; /* never treat a broken or looping chain as empty */
    }
    for (ei = 0; ei < cap; ei++) {
        uint8_t sec[SECSZ];
        uint64_t lba;
        unsigned int off;
        const uint8_t *ent;
        if (dirent_pos(fs, first, ei, &lba, &off) != 0) return -1;
        if (fs->read(fs->ctx, lba, 1u, sec) != 0) return -1;
        ent = sec + off;
        if (ent[0] == 0x00u) return 1;
        if (ent[0] == 0xE5u) continue;
        if (ent[0] == '.' && (ent[11] & HYPE_FAT_ATTR_DIRECTORY)) continue; /* '.' / '..' */
        return 0;
    }
    return 1; /* the whole allocation is deleted slots: empty */
}

int hype_fat32_rmdir(hype_fat32_fs_t *fs, const char *path) {
    char leafbuf[HYPE_FAT_MAX_LFN + 1u];
    hype_fat32_dloc_t loc;
    unsigned int leaf;
    uint32_t parent, cl;

    /* The root has no final component, so path_split refuses it. */
    if (path_split(path, &leaf) != 0 || leaf_name(path, leaf, leafbuf, sizeof leafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, path, leaf, 0u, &parent) != 0) return -1;
    if (dir_find(fs, parent, leafbuf, &loc) != 1) return -1;
    if ((loc.ent[11] & HYPE_FAT_ATTR_DIRECTORY) == 0u) return -1;
    cl = hype_fat_dirent_cluster(loc.ent);
    if (!cluster_ok(fs, cl)) return -1;
    if (dir_is_empty(fs, cl) != 1) return -1;
    if (free_chain(fs, cl) != 0) return -1;
    if (run_delete(fs, parent, loc.run_start, loc.ent_index) != 0) return -1;
    fsinfo_flush(fs);
    return 0;
}

int hype_fat32_rename(hype_fat32_fs_t *fs, const char *from, const char *to) {
    char fleafbuf[HYPE_FAT_MAX_LFN + 1u];
    char tleafbuf[HYPE_FAT_MAX_LFN + 1u];
    hype_fat32_dloc_t floc, tloc;
    unsigned int fleaf, tleaf;
    uint32_t fparent, tparent, tei = 0, forbid = 0;
    int is_dir;

    /* The source, which must exist. */
    if (path_split(from, &fleaf) != 0 || leaf_name(from, fleaf, fleafbuf, sizeof fleafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, from, fleaf, 0u, &fparent) != 0) return -1;
    if (dir_find(fs, fparent, fleafbuf, &floc) != 1) return -1;
    is_dir = (floc.ent[11] & HYPE_FAT_ATTR_DIRECTORY) ? 1 : 0;

    /* The destination parent, never reached THROUGH the source: a directory
     * moved into its own subtree becomes an unreachable cycle. */
    if (is_dir) forbid = hype_fat_dirent_cluster(floc.ent);
    if (path_split(to, &tleaf) != 0 || leaf_name(to, tleaf, tleafbuf, sizeof tleafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, to, tleaf, forbid, &tparent) != 0) return -1;
    if (!hype_fat_name_valid(tleafbuf)) return -1;
    /* Rename never replaces. NOTE this also refuses a pure case change of the
     * same name -- the case-insensitive search finds the source itself. */
    if (dir_find(fs, tparent, tleafbuf, &tloc) != 0) return -1;

    /* The entry keeps everything -- attributes, timestamps, chain, size --
     * except its name; written under the new name BEFORE the old one is
     * deleted, so an interruption leaves it findable under at least one. */
    if (insert_entry(fs, tparent, tleafbuf, floc.ent, &tei) != 0) return -1;

    if (is_dir && tparent != fparent) {
        /* A moved directory's '..' must point at its NEW parent (0 == root). */
        uint8_t sec[SECSZ];
        uint32_t dcl = hype_fat_dirent_cluster(floc.ent);
        if (!cluster_ok(fs, dcl)) return -1;
        if (fs->read(fs->ctx, cluster_lba(fs, dcl), 1u, sec) != 0) return -1;
        if (sec[DIRENT_SIZE] == '.' && sec[DIRENT_SIZE + 1u] == '.') {
            hype_fat_dirent_set_cluster(sec + DIRENT_SIZE,
                                        (tparent == fs->root_cluster) ? 0u : tparent);
            if (fs->write(fs->ctx, cluster_lba(fs, dcl), 1u, sec) != 0) return -1;
        }
    }

    if (run_delete(fs, fparent, floc.run_start, floc.ent_index) != 0) return -1;
    fsinfo_flush(fs);
    return 0;
}

int hype_fat32_append(hype_fat32_wfile_t *f, const void *data, unsigned int len) {
    hype_fat32_fs_t *fs = f->fs;
    const uint8_t *src = (const uint8_t *)data;
    uint64_t cluster_bytes = (uint64_t)fs->spc * SECSZ;

    while (len > 0u) {
        uint64_t oic = f->size % cluster_bytes;
        unsigned int sic, bis, n;
        uint64_t lba;

        if (oic == 0u && f->size > 0u) {
            /* Filled the current cluster exactly -- extend the chain. */
            uint32_t ncl;
            if (alloc_cluster(fs, &ncl) != 0) return -1;
            if (fat_set(fs, f->tail_cluster, ncl) != 0) return -1;
            f->tail_cluster = ncl;
        }
        sic = (unsigned int)(oic / SECSZ);
        bis = (unsigned int)(oic % SECSZ);
        lba = cluster_lba(fs, f->tail_cluster) + sic;
        n = SECSZ - bis;
        if (n > len) n = len;

        if (bis != 0u || n < SECSZ) {
            uint8_t sec[SECSZ];
            if (fs->read(fs->ctx, lba, 1u, sec) != 0) return -1;
            bcopy(sec + bis, src, n);
            if (fs->write(fs->ctx, lba, 1u, sec) != 0) return -1;
        } else {
            if (fs->write(fs->ctx, lba, 1u, src) != 0) return -1;
        }
        src += n;
        len -= n;
        f->size += n;
    }
    return flush_metadata(f);
}

void hype_fat32_fs_set_time(hype_fat32_fs_t *fs, const hype_rtc_time_t *now) {
    if (fs == 0) {
        return;
    }
    if (now == 0) {
        fs->now.year = 0; /* invalid -> encoders emit the old zero timestamps */
        return;
    }
    /* Field-by-field: whole-struct assignment of anything containing an array
     * emits a memcpy call, which does not exist in this freestanding build.
     * hype_rtc_time_t has no array today, but copying explicitly keeps that
     * true if a field is ever added. */
    fs->now.year = now->year;
    fs->now.month = now->month;
    fs->now.day = now->day;
    fs->now.hour = now->hour;
    fs->now.minute = now->minute;
    fs->now.second = now->second;
}
