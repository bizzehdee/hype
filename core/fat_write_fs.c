#include "fat_write_fs.h"
#include "fat_write.h"

#define SECSZ HYPE_BLK_SECTOR_SIZE
#define FAT32_EOC_MIN 0x0FFFFFF8u /* entry >= this in a chain means end-of-chain */
#define DIRENT_SIZE 32u
#define UNKNOWN 0xFFFFFFFFu
#define FIRST_CLUSTER_GUARD_SEED 0xA5C37E19u

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

#ifndef HYPE_596_JOURNAL
/* Production build: the write wrapper is a pure pass-through. */
static int hype_596_wr(hype_fat32_fs_t *fs, uint64_t lba, uint32_t count, const void *src) {
    return fs->write(fs->ctx, lba, count, src);
}
#else
/*
 * #596 diagnostic build (-DHYPE_596_JOURNAL): an in-RAM ring journal of every mutation this
 * writer performs, plus a publish-time chain audit. On the FIRST publish whose FAT chain is
 * shorter than the size being published, the journal and a cache-vs-medium comparison of the
 * divergent FAT sector are dumped over RAW SERIAL (hype_serial_print -- deliberately NOT
 * hype_debug_print: routing diagnostics through the logbuf perturbs the exact writer under
 * observation, which is how two earlier probe rounds contaminated their own evidence).
 */
#include "serial.h"

#define J_N 16384u
typedef struct { uint8_t tag; uint32_t a, b, c; } hype_596_j_t;
static hype_596_j_t g_j[J_N];
static uint32_t g_jseq;
static int g_jdumped;

/* Which core produced each record: two different IDs interleaved on one file is the #239 guard
 * failing; one ID with nested patterns is reentrancy on that core. CPUID.1:EBX[31:24]. */
static uint32_t hype_596_apic_id(void) {
    uint32_t ebx;
    __asm__ volatile("cpuid" : "=b"(ebx) : "a"(1u) : "ecx", "edx");
    return ebx >> 24;
}

static void jrec(uint8_t tag, uint32_t a, uint32_t b, uint32_t c) {
    hype_596_j_t *e = &g_j[g_jseq % J_N];
    (void)c;
    e->tag = tag; e->a = a; e->b = b; e->c = hype_596_apic_id();
    g_jseq++;
}

/* #596: generic journal note for other modules (log_sink) -- tag + two values, core-stamped. */
void hype_596_note(uint32_t tag, uint32_t a, uint32_t b);
void hype_596_note(uint32_t tag, uint32_t a, uint32_t b) { jrec((uint8_t)tag, a, b, 0); }

static int hype_596_wr(hype_fat32_fs_t *fs, uint64_t lba, uint32_t count, const void *src) {
    int rc = fs->write(fs->ctx, lba, count, src);
    /* 'w' = any sector write (lba, count, ok). FAT-range detection scans these. */
    jrec(rc == 0 ? 'w' : 'X', (uint32_t)lba, count, (uint32_t)(uintptr_t)fs);
    return rc;
}

static void hype_596_dump_journal(void) {
    uint32_t n = (g_jseq < J_N) ? g_jseq : J_N;
    uint32_t first = g_jseq - n;
    uint32_t s;
    hype_serial_print("[596j] journal: %u total events, dumping last %u (seq %u..%u)\n",
                      g_jseq, n, first, g_jseq - 1u);
    for (s = first; s < g_jseq; s++) {
        const hype_596_j_t *e = &g_j[s % J_N];
        hype_serial_print("[596j] %u %c %u %u %u\n", s, e->tag, e->a, e->b, e->c);
    }
    hype_serial_print("[596j] journal end\n");
}

/* Publish-time audit: walk the chain the medium+cache actually hold and compare with the size
 * about to be published. Runs on every flush_metadata; dumps once. */
static int fat_get(hype_fat32_fs_t *fs, uint32_t cl, uint32_t *out); /* fwd */
static void hype_596_check(hype_fat32_wfile_t *f, uint32_t sz) {
    hype_fat32_fs_t *fs = f->fs;
    uint64_t cb = (uint64_t)fs->spc * SECSZ;
    uint64_t need = ((uint64_t)sz + cb - 1u) / cb;
    uint32_t cl = f->first_cluster, count = 0, ent = 0;
    uint32_t soff, i;
    uint8_t raw[SECSZ];

    if (g_jdumped || sz == 0u || cl < 2u) return;
    for (;;) {
        count++;
        if (fat_get(fs, cl, &ent) != 0) return;
        if (ent >= FAT32_EOC_MIN || count > need + 4u) break;
        if (ent < 2u || ent > fs->max_cluster) break; /* free/reserved mid-chain: also broken */
        cl = ent;
    }
    if ((uint64_t)count >= need && ent >= FAT32_EOC_MIN) return; /* healthy */

    g_jdumped = 1;
    hype_serial_print("[596j] SHORT CHAIN at publish: file '%c%c%c%c%c%c%c%c.%c%c%c' first=%u "
                      "size=%u need=%llu walked=%u end_cl=%u end_entry=0x%x mem_tail=%u "
                      "dirent_lba=%llu\n",
                      f->name11[0], f->name11[1], f->name11[2], f->name11[3], f->name11[4],
                      f->name11[5], f->name11[6], f->name11[7], f->name11[8], f->name11[9],
                      f->name11[10], f->first_cluster, sz, (unsigned long long)need, count, cl,
                      ent, f->tail_cluster, (unsigned long long)f->dirent_lba);

    /* The walk parked the shared cache on end_cl's FAT sector. Re-read that sector RAW from the
     * medium and print any entry where cache and medium disagree -- a nonempty list is a lost or
     * reverted sector write; an empty list means cache==medium and the loss happened earlier. */
    soff = cl / HYPE_FAT32_ENTRIES_PER_SECTOR;
    if (fs->fat_cache_valid && fs->fat_cache_off == soff &&
        fs->read(fs->ctx, (uint64_t)fs->reserved + soff, 1u, raw) == 0) {
        int diffs = 0;
        for (i = 0; i < HYPE_FAT32_ENTRIES_PER_SECTOR; i++) {
            uint32_t cv = hype_fat32_entry_get(fs->fat_cache, i);
            uint32_t mv = hype_fat32_entry_get(raw, i);
            if (cv != mv) {
                hype_serial_print("[596j] CACHE!=MEDIUM fatsec=%u entry=%u (cl=%u) cache=0x%x "
                                  "medium=0x%x\n", soff, i,
                                  soff * HYPE_FAT32_ENTRIES_PER_SECTOR + i, cv, mv);
                diffs = 1;
            }
        }
        if (!diffs) hype_serial_print("[596j] cache==medium for fatsec=%u\n", soff);
    }
    /* Any journalled write that landed inside FAT copy 0 but was NOT a fat_set ('S' precedes its
     * own 'w' from inside fat_set, so an unpaired 'w' in FAT range is a stray writer). */
    hype_serial_print("[596j] fat0 range: lba %u..%u\n", fs->reserved, fs->reserved + fs->fat_size);
    hype_596_dump_journal();
}
#endif /* HYPE_596_JOURNAL */

/* The first cluster is immutable after the first allocation. The guard binds
 * it to this file's directory slot and short name, so a stray assignment of a
 * neighbouring writer's first_cluster fails closed instead of cross-linking
 * the two files. This is deliberately independent of the FAT chain itself:
 * the AMD #377 capture contained both valid chains, but VM1's directory entry
 * published VM0's root. */
static uint32_t first_cluster_guard(const hype_fat32_wfile_t *f, uint32_t first) {
    uint32_t v = FIRST_CLUSTER_GUARD_SEED ^ first ^ (uint32_t)f->dirent_lba ^
                 (uint32_t)(f->dirent_lba >> 32) ^ f->dirent_off;
    unsigned int i;
    for (i = 0; i < 11u; i++) v = (v << 5) ^ (v >> 27) ^ f->name11[i];
    return v;
}

static void first_cluster_set(hype_fat32_wfile_t *f, uint32_t first) {
    f->first_cluster = first;
    f->first_cluster_guard = first_cluster_guard(f, first);
}

static int first_cluster_valid(const hype_fat32_wfile_t *f) {
    return f->first_cluster_guard == first_cluster_guard(f, f->first_cluster);
}

static uint64_t cluster_lba(const hype_fat32_fs_t *fs, uint32_t cl) {
    return (uint64_t)fs->data_start + (uint64_t)(cl - 2u) * fs->spc;
}

/* Refresh advisory allocation state before this writer changes it. Several
 * log sinks mount one volume independently, so a value cached at mount may be
 * stale even though all operations are serialized on the BSP. Do not refresh
 * while this instance has uncommitted changes from the same public operation. */
static void fsinfo_refresh(hype_fat32_fs_t *fs) {
    uint8_t fsi[SECSZ];
    uint32_t next;
    if (fs->fsinfo_dirty || fs->fsinfo_sector == 0u) return;
    /*
     * #584: never re-read the hint once this mount has allocated something.
     *
     * The hint is a starting point for a scan, and after the first allocation this mount's own
     * cursor is the better one -- it knows what it handed out, and the medium may not. Re-reading it
     * is how the allocator came to scan BACKWARDS into clusters it had already given away, which
     * with a stale FAT read produced two log files sharing 60 clusters. See allocated_any in the
     * header for the measurement.
     *
     * This is the whole fix: the free/used test still consults the FAT, so on-medium corruption
     * stays detectable (the #382 chain checks depend on that) and nothing shadows or second-guesses
     * a read. Only the scan's STARTING POINT stops going backwards.
     */
    if (fs->allocated_any) return;
    if (fs->read(fs->ctx, fs->fsinfo_sector, 1u, fsi) != 0 ||
        rd32(fsi + 0) != 0x41615252u) {
        fs->free_count = UNKNOWN;
        return;
    }
    fs->free_count = rd32(fsi + 0x1E8);
    next = rd32(fsi + 0x1EC);
    fs->next_free = (next >= 2u && next <= fs->max_cluster) ? next : 2u;
}

static int fat_cache_load(hype_fat32_fs_t *fs, uint32_t off) {
    uint64_t slba;
    if (fs->fat_cache_valid && fs->fat_cache_off == off) return 0;
    slba = (uint64_t)fs->reserved + off;
    if (fs->read(fs->ctx, slba, 1u, fs->fat_cache) != 0) {
        fs->fat_cache_valid = 0;
        return -1;
    }
    fs->fat_cache_off = off;
    fs->fat_cache_valid = 1;
    return 0;
}

/* Read a single FAT entry from this mount's authoritative FAT-sector view. */
static int fat_get(hype_fat32_fs_t *fs, uint32_t cl, uint32_t *out) {
    uint32_t off = cl / HYPE_FAT32_ENTRIES_PER_SECTOR;
    if (fat_cache_load(fs, off) != 0) return -1;
    *out = hype_fat32_entry_get(fs->fat_cache, cl % HYPE_FAT32_ENTRIES_PER_SECTOR);
    return 0;
}

/*
 * Write a FAT entry into every FAT copy from one authoritative sector image.
 * Reading each copy independently before modifying it allowed a stale medium
 * read to resurrect an older allocation map. The cache is updated first and
 * the same complete sector is then written to every copy.
 */
static int fat_set(hype_fat32_fs_t *fs, uint32_t cl, uint32_t val) {
    unsigned int idx = cl % HYPE_FAT32_ENTRIES_PER_SECTOR;
    uint32_t off = cl / HYPE_FAT32_ENTRIES_PER_SECTOR;
    unsigned int copy;
    if (fat_cache_load(fs, off) != 0) return -1;
#ifdef HYPE_596_JOURNAL
    jrec('S', cl, val, (uint32_t)(uintptr_t)fs);
#endif
    hype_fat32_entry_set(fs->fat_cache, idx, val);
    for (copy = 0; copy < fs->num_fats; copy++) {
        uint64_t slba = (uint64_t)fs->reserved + (uint64_t)copy * fs->fat_size + off;
        if (hype_596_wr(fs, slba, 1u, fs->fat_cache) != 0) {
            fs->fat_cache_valid = 0;
            return -1;
        }
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
        if (hype_596_wr(fs, cluster_lba(fs, cl) + s, 1u, sec) != 0) return -1;
    }
    return 0;
}

/* Allocate one free cluster, mark it end-of-chain, and update the free hints.
 * Returns 0 and the cluster in *out, or -1 if the volume is full. */
static int alloc_cluster(hype_fat32_fs_t *fs, uint32_t *out) {
    uint32_t start;
    uint32_t scanned = 0;
    uint32_t total = fs->max_cluster - 2u + 1u;
    uint32_t cl;

    fsinfo_refresh(fs);
    start = (fs->next_free >= 2u && fs->next_free <= fs->max_cluster) ? fs->next_free : 2u;
    cl = start;
    while (scanned < total) {
        uint32_t v;
        if (fat_get(fs, cl, &v) != 0) return -1;
        if (v == 0u) {
            if (fat_set(fs, cl, FAT32_EOC_MIN | 0x7u) != 0) return -1; /* 0x0FFFFFFF EOC */
            fs->next_free = (cl + 1u > fs->max_cluster) ? 2u : (cl + 1u);
            if (fs->free_count != UNKNOWN && fs->free_count != 0u) fs->free_count--;
            fs->fsinfo_dirty = 1;
            fs->allocated_any = 1; /* #584: our cursor is authoritative from here on */
#ifdef HYPE_596_JOURNAL
            jrec('A', cl, fs->next_free, fs->free_count);
#endif
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
    fsinfo_refresh(fs);
    while (cl >= 2u && cl <= fs->max_cluster && guard <= fs->max_cluster) {
        uint32_t next;
        if (fat_get(fs, cl, &next) != 0) return -1;
        if (fat_set(fs, cl, 0u) != 0) return -1;
        if (fs->free_count != UNKNOWN) fs->free_count++;
        if (fs->next_free == 0u || cl < fs->next_free) fs->next_free = cl;
        fs->fsinfo_dirty = 1;
        if (next >= FAT32_EOC_MIN) break;
        cl = next;
        guard++;
    }
    return 0;
}

int hype_fat32_fs_mount(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
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
    out->sync = (hype_blk_sync_fn)0;
    out->ctx = ctx;
    out->spc = bpb[0x0D];
    out->reserved = rd16(bpb + 0x0E);
    out->num_fats = bpb[0x10];
    out->fat_size = rd32(bpb + 0x24);
    out->root_cluster = rd32(bpb + 0x2C);
    out->fsinfo_sector = rd16(bpb + 0x30);
    out->fsinfo_dirty = 0;
    out->allocated_any = 0; /* #584: the disk hint is trusted until we allocate our first cluster */
    out->fat_cache_off = 0u;
    out->fat_cache_valid = 0;
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
#ifdef HYPE_596_JOURNAL
    /* Build-variant gate: this line on serial proves the journal build is the one running. */
    hype_serial_print("[596j] JOURNAL BUILD: fat32 mount fs=%u reserved=%u fat_size=%u spc=%u "
                      "anchor=0x%llx\n", (uint32_t)(uintptr_t)out, out->reserved, out->fat_size,
                      out->spc, (unsigned long long)(uintptr_t)&hype_fat32_fs_mount);
#endif
    return 0;
}

/* Flush the free-cluster counters to FSInfo (best-effort: a volume without a
 * usable FSInfo simply keeps none). */
static void fsinfo_flush(hype_fat32_fs_t *fs) {
    if (fs->fsinfo_sector != 0u && fs->fsinfo_dirty) {
        uint8_t fsi[SECSZ];
        if (fs->read(fs->ctx, fs->fsinfo_sector, 1u, fsi) == 0 &&
            hype_fat32_fsinfo_set(fsi, fs->free_count, fs->next_free) == 0) {
            if (hype_596_wr(fs, fs->fsinfo_sector, 1u, fsi) == 0) {
                fs->fsinfo_dirty = 0;
            }
        }
    }
}

/* Names match if their 11-byte 8.3 fields are byte-identical. */
static int name_eq(const uint8_t *a, const uint8_t *b) {
    unsigned int i;
    for (i = 0; i < 11u; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Write the file's current first-cluster + size into its directory entry, and
 * flush the free-cluster counters to FSInfo. */
static int flush_metadata(hype_fat32_wfile_t *f, int durable, int truncate_root) {
    hype_fat32_fs_t *fs = f->fs;
    uint8_t sec[SECSZ];
    uint8_t ent[DIRENT_SIZE];
    uint32_t sz = (f->size > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)f->size;
    uint32_t disk_first;

    if (!first_cluster_valid(f) || f->dirent_off > SECSZ - DIRENT_SIZE) {
        f->last_error = HYPE_FAT32_WFILE_ERR_IDENTITY;
        return -1;
    }

    /* #377: a WRITE(10) completion does not guarantee that the stick made the
     * preceding FAT link durable. When a cluster chain grew, establish that
     * ordering before publishing a size that reaches the new cluster. */
    if (durable && fs->sync != (hype_blk_sync_fn)0 && fs->sync(fs->ctx) != 0) return -1;
    if (fs->read(fs->ctx, f->dirent_lba, 1u, sec) != 0) return -1;
    if (!name_eq(sec + f->dirent_off, f->name11)) {
        f->last_error = HYPE_FAT32_WFILE_ERR_IDENTITY;
        return -1;
    }
    disk_first = hype_fat_dirent_cluster(sec + f->dirent_off);
    /* A file's chain root never changes during append. Preserve and verify the
     * already-published value instead of blindly replacing it from mutable RAM.
     * Zero is allowed only before the first non-empty metadata commit. */
    if (!truncate_root && disk_first != 0u && disk_first != f->first_cluster) {
        f->last_error = HYPE_FAT32_WFILE_ERR_IDENTITY;
        return -1;
    }
    if (!truncate_root && hype_fat_dirent_size(sec + f->dirent_off) != 0u && disk_first == 0u) {
        f->last_error = HYPE_FAT32_WFILE_ERR_IDENTITY;
        return -1;
    }
#ifdef HYPE_596_JOURNAL
    jrec('P', f->first_cluster, sz, f->tail_cluster);
    hype_596_check(f, sz);
#endif
    hype_fat_dirent_build(ent, f->name11, HYPE_FAT_ATTR_ARCHIVE,
                          (!truncate_root && disk_first != 0u) ? disk_first : f->first_cluster, sz,
                          &f->fs->now);
    bcopy(sec + f->dirent_off, ent, DIRENT_SIZE);
    if (hype_596_wr(fs, f->dirent_lba, 1u, sec) != 0) return -1;

    fsinfo_flush(fs);
    /* Persist the directory entry too. Losing it leaves a safely shorter file;
     * completing it makes the whole cluster-extension transaction durable. */
    if (durable && fs->sync != (hype_blk_sync_fn)0 && fs->sync(fs->ctx) != 0) return -1;
    return 0;
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
    return hype_596_wr(fs, lba, 1u, sec);
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
    uint32_t parent, ei = 0;
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
        out->fs = fs;
        bcopy(out->name11, loc.ent, 11u);
        out->first_cluster = 0u;
        out->tail_cluster = 0u;
        out->size = 0u;
        out->seek_index = 0u;
        out->seek_cluster = 0u;
        out->last_error = HYPE_FAT32_WFILE_ERR_NONE;
        if (dirent_pos(fs, parent, loc.ent_index, &out->dirent_lba, &out->dirent_off) != 0) {
            return -1;
        }
        first_cluster_set(out, 0u);
        return flush_metadata(out, 1, 1);
    }
    /* Fresh empty files are chainless. Allocating here leaves a zero-length
     * file with an excess cluster, which fsck must later truncate. The first
     * append allocates the first data cluster and publishes it with the size. */
    {
        uint8_t ent[DIRENT_SIZE];
        uint8_t dummy11[11];
        unsigned int i;
        for (i = 0; i < 11u; i++) dummy11[i] = ' '; /* insert_entry overwrites it */
        hype_fat_dirent_build(ent, dummy11, HYPE_FAT_ATTR_ARCHIVE, 0u, 0u, &fs->now);
        if (insert_entry(fs, parent, leafbuf, ent, &ei) != 0) return -1;
    }
    {
        /* insert_entry chose the short name (possibly a ~N tail): read it back
         * so flush_metadata keeps rewriting the same entry. */
        uint8_t placed[DIRENT_SIZE];
        if (dirent_read(fs, parent, ei, placed) != 0) return -1;
        out->fs = fs;
        bcopy(out->name11, placed, 11u);
        out->first_cluster = 0u;
        out->tail_cluster = 0u;
        out->size = 0u;
        out->seek_index = 0u;
        out->seek_cluster = 0u;
        out->last_error = HYPE_FAT32_WFILE_ERR_NONE;
        if (dirent_pos(fs, parent, ei, &out->dirent_lba, &out->dirent_off) != 0) return -1;
        first_cluster_set(out, 0u);
    }
    return flush_metadata(out, 1, 0);
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
    if (hype_596_wr(fs, cluster_lba(fs, dcl), 1u, sec) != 0) return -1;

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
            if (hype_596_wr(fs, cluster_lba(fs, dcl), 1u, sec) != 0) return -1;
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
    int extended = 0;

    f->last_error = HYPE_FAT32_WFILE_ERR_NONE;
    if (!first_cluster_valid(f)) {
        f->last_error = HYPE_FAT32_WFILE_ERR_IDENTITY;
        return -1;
    }

    while (len > 0u) {
        uint64_t oic = f->size % cluster_bytes;
        unsigned int sic, bis, n;
        uint64_t lba;

        if (f->size == 0u && f->first_cluster < 2u) {
            uint32_t ncl;
            if (alloc_cluster(fs, &ncl) != 0) return -1;
            first_cluster_set(f, ncl);
            f->tail_cluster = ncl;
            extended = 1;
        } else if (!cluster_ok(fs, f->tail_cluster)) {
            return -1;
        } else if (oic == 0u && f->size > 0u) {
            /* Filled the current cluster exactly -- extend the chain. */
            uint32_t ncl;
            if (alloc_cluster(fs, &ncl) != 0) return -1;
            if (fat_set(fs, f->tail_cluster, ncl) != 0) return -1;
            f->tail_cluster = ncl;
            extended = 1;
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
            if (hype_596_wr(fs, lba, 1u, sec) != 0) return -1;
        } else {
            /*
             * #374: the block callbacks accept a sector count, and the USB
             * backend can transfer up to 64 KiB per command. Sending every
             * full sector separately forced one USB command per 512 bytes.
             * Coalesce the contiguous full sectors left in this cluster. A
             * cluster boundary still returns to the top of the loop so the
             * next FAT link is allocated and committed before data reaches it.
             */
            unsigned int sectors = len / SECSZ;
            unsigned int in_cluster = fs->spc - sic;
            if (sectors > in_cluster) sectors = in_cluster;
            if (sectors == 0u) sectors = 1u;
            n = sectors * SECSZ;
            if (hype_596_wr(fs, lba, sectors, src) != 0) return -1;
        }
        src += n;
        len -= n;
        f->size += n;
    }
    return flush_metadata(f, extended, 0);
}


/* ---- #382: open-existing + random-position read/write with allocation ---- */

/* FAT[1] ClnShutBitMask: bit CLEAR while the volume has in-flight changes.
 * Within the 28 valid FAT32 entry bits, so fat_set() carries it intact. */
#define FAT32_CLNSHUT 0x08000000u

static uint64_t cluster_bytes_of(const hype_fat32_fs_t *fs) {
    return (uint64_t)fs->spc * SECSZ;
}

static int volume_set_dirty(hype_fat32_fs_t *fs, int dirty) {
    uint32_t v, nv;
    if (fat_get(fs, 1u, &v) != 0) return -1;
    nv = dirty ? (v & ~FAT32_CLNSHUT) : (v | FAT32_CLNSHUT);
    if (nv == v) return 0;
    return fat_set(fs, 1u, nv);
}

/*
 * Walks and validates the complete chain against the recorded size (the
 * DIR_FileSize rule: every cluster through the size belongs to the chain,
 * and none beyond it -- FAT32 cannot represent an internal hole, so a short
 * chain is corruption and a long one is what fsck reports as an allocation/
 * size mismatch). Bounded by `need`, so a loop or a cross-link back into
 * this chain fails as "longer than the size justifies" without a visited
 * set. Refuses free (0), reserved (1) and out-of-range clusters.
 */
static int chain_measure(hype_fat32_fs_t *fs, uint32_t first, uint64_t size,
                         uint32_t *out_count, uint32_t *out_tail) {
    uint64_t need = (size + cluster_bytes_of(fs) - 1u) / cluster_bytes_of(fs);
    uint32_t cl = first, count = 0, tail = 0;

    if (first == 0u) {
        if (size != 0u) return -1; /* a non-empty file must have a chain */
        *out_count = 0u;
        *out_tail = 0u;
        return 0;
    }
    if (!cluster_ok(fs, cl)) return -1;
    for (;;) {
        uint32_t next;
        count++;
        if ((uint64_t)count > need) return -1; /* loop, cross-link, or slack clusters */
        tail = cl;
        if (fat_get(fs, cl, &next) != 0) return -1;
        if (next >= FAT32_EOC_MIN) break;
        if (!cluster_ok(fs, next)) return -1; /* free, reserved or out of range */
        cl = next;
    }
    if ((uint64_t)count < need) return -1; /* shorter than the recorded size */
    *out_count = count;
    *out_tail = tail;
    return 0;
}

/* Cluster at chain index `index`, through the handle's seek cache so a
 * sequential scan is O(n) overall instead of per call. */
static int chain_cluster_at(hype_fat32_wfile_t *f, uint32_t index, uint32_t *out) {
    hype_fat32_fs_t *fs = f->fs;
    uint32_t cl, i;
    if (!cluster_ok(fs, f->first_cluster)) return -1;
    if (f->seek_cluster >= 2u && f->seek_index <= index && cluster_ok(fs, f->seek_cluster)) {
        cl = f->seek_cluster;
        i = f->seek_index;
    } else {
        cl = f->first_cluster;
        i = 0u;
    }
    for (; i < index; i++) {
        uint32_t next;
        if (fat_get(fs, cl, &next) != 0) return -1;
        if (next >= FAT32_EOC_MIN || !cluster_ok(fs, next)) return -1;
        cl = next;
    }
    f->seek_index = index;
    f->seek_cluster = cl;
    *out = cl;
    return 0;
}

/*
 * One transfer loop for all three byte-range operations over the chain:
 * read (rbuf), write (wbuf), or zero-fill (both NULL). Whole aligned sectors
 * move in bulk runs bounded by the cluster; ragged edges bounce through one
 * sector (read-modify-write when writing). Pure data path -- no metadata.
 */
static int span_io(hype_fat32_wfile_t *f, uint64_t off, uint8_t *rbuf, const uint8_t *wbuf,
                   uint64_t len, uint64_t limit) {
    hype_fat32_fs_t *fs = f->fs;
    uint64_t cb = cluster_bytes_of(fs);
    static const uint8_t zsec[SECSZ]; /* all-zero source for gap fill */

    if (off + len < off || off + len > limit) return -1;
    while (len > 0u) {
        uint32_t cl;
        uint64_t oic = off % cb; /* offset in cluster */
        unsigned int sic = (unsigned int)(oic / SECSZ);
        unsigned int bis = (unsigned int)(oic % SECSZ);
        uint64_t lba;
        unsigned int n;

        if (chain_cluster_at(f, (uint32_t)(off / cb), &cl) != 0) return -1;
        lba = cluster_lba(fs, cl) + sic;

        if (bis == 0u && len >= SECSZ) {
            /* bulk full sectors, bounded by this cluster */
            unsigned int sectors = (unsigned int)(len / SECSZ);
            unsigned int in_cluster = fs->spc - sic;
            if (sectors > in_cluster) sectors = in_cluster;
            n = sectors * SECSZ;
            if (rbuf != 0) {
                if (fs->read(fs->ctx, lba, sectors, rbuf) != 0) return -1;
            } else if (wbuf != 0) {
                if (hype_596_wr(fs, lba, sectors, wbuf) != 0) return -1;
            } else {
                unsigned int si;
                for (si = 0; si < sectors; si++) {
                    if (hype_596_wr(fs, lba + si, 1u, zsec) != 0) return -1;
                }
            }
        } else {
            /* ragged head/tail: one bounced sector */
            uint8_t sec[SECSZ];
            n = SECSZ - bis;
            if ((uint64_t)n > len) n = (unsigned int)len;
            if (rbuf != 0) {
                if (fs->read(fs->ctx, lba, 1u, sec) != 0) return -1;
                bcopy(rbuf, sec + bis, n);
            } else {
                if (fs->read(fs->ctx, lba, 1u, sec) != 0) return -1;
                if (wbuf != 0) {
                    bcopy(sec + bis, wbuf, n);
                } else {
                    bzero(sec + bis, n);
                }
                if (hype_596_wr(fs, lba, 1u, sec) != 0) return -1;
            }
        }
        off += n;
        len -= n;
        if (rbuf != 0) rbuf += n;
        if (wbuf != 0) wbuf += n;
    }
    return 0;
}

int hype_fat32_open(hype_fat32_fs_t *fs, const char *path, hype_fat32_wfile_t *out) {
    char leafbuf[HYPE_FAT_MAX_LFN + 1u];
    hype_fat32_dloc_t loc;
    unsigned int leaf;
    uint32_t parent, first, count, tail;
    uint64_t size;
    int rc;

    if (path_split(path, &leaf) != 0 || leaf_name(path, leaf, leafbuf, sizeof leafbuf) != 0) {
        return -1;
    }
    if (resolve_parent(fs, path, leaf, 0u, &parent) != 0) return -1;
    rc = dir_find(fs, parent, leafbuf, &loc);
    if (rc != 1) return -1; /* open never creates */
    if (loc.ent[11] & HYPE_FAT_ATTR_DIRECTORY) return -1;

    first = hype_fat_dirent_cluster(loc.ent);
    size = hype_fat_dirent_size(loc.ent);
    if (chain_measure(fs, first, size, &count, &tail) != 0) return -1;

    out->fs = fs;
    bcopy(out->name11, loc.ent, 11u);
    out->size = size;
    out->tail_cluster = tail;
    out->seek_index = 0u;
    out->seek_cluster = (first >= 2u) ? first : 0u;
    out->last_error = HYPE_FAT32_WFILE_ERR_NONE;
    if (dirent_pos(fs, parent, loc.ent_index, &out->dirent_lba, &out->dirent_off) != 0) return -1;
    first_cluster_set(out, first);
    return 0;
}

int hype_fat32_read_at(hype_fat32_wfile_t *f, uint64_t offset, void *out, unsigned int len) {
    if (len == 0u) return 0;
    return span_io(f, offset, (uint8_t *)out, 0, len, f->size);
}

/* Undo a partial growth: free the clusters allocated this call, restore the
 * old chain terminator (or the empty file), and put the handle back. Returns
 * 0 when the volume was fully restored, -1 when the undo itself failed --
 * the volume dirty flag is then left SET, honestly. */
/* #464: count rollbacks that could not complete. Read by the diagnostics so a volume left
 * needing repair is visible in the log rather than only in the next mount's failure. */
static unsigned long long g_fat_rollback_failures;
void hype_fat_write_note_rollback_failure(void) { g_fat_rollback_failures++; }
unsigned long long hype_fat_write_rollback_failures(void) { return g_fat_rollback_failures; }

/*
 * #464: what does the entry ON THE MEDIUM claim right now? The rollback has to decide whether
 * cutting the chain is safe, and that turns on what was actually published -- not on what a
 * failed flush intended. A flush refused before writing (the identity check, or a failed read)
 * leaves the old size published and the new clusters safe to reclaim; a flush that failed while
 * writing may have left the larger size behind, and then the chain must stay.
 */
static int entry_claims_at_most(hype_fat32_wfile_t *f, uint64_t bytes) {
    hype_fat32_fs_t *fs = f->fs;
    uint8_t sec[SECSZ];
    if (f->dirent_off > SECSZ - DIRENT_SIZE) return 0;
    if (fs->read(fs->ctx, f->dirent_lba, 1u, sec) != 0) return 0;
    return ((uint64_t)hype_fat_dirent_size(sec + f->dirent_off) <= bytes) ? 1 : 0;
}

static int growth_rollback(hype_fat32_wfile_t *f, uint32_t first_new, uint32_t old_tail,
                           uint64_t old_size) {
    hype_fat32_fs_t *fs = f->fs;
    int ok = 0;
    int entry_shrunk = 0;
    /*
     * #464: SHRINK THE DIRECTORY ENTRY BEFORE SHRINKING THE CHAIN.
     *
     * This used to free the new clusters first and restore f->size only in memory, never
     * rewriting the entry. On the last call site that is reached AFTER flush_metadata() has
     * already published the larger size to disk and then failed partway -- so the sequence was:
     * entry says `end`, chain is cut back to `old_size`, entry never corrected. That leaves a
     * directory entry whose size exceeds its cluster chain, which is exactly what Linux reports
     * as "fat_bmap_cluster: request beyond EOF" before applying errors=remount-ro.
     *
     * Restoring the size on disk first means the entry never describes more data than the chain
     * holds at any intermediate point, including across a power cut. The opposite ordering -- a
     * chain longer than the entry -- is harmless: it is a few leaked clusters that fsck
     * reclaims, and the file simply reads short.
     */
    /*
     * The restore must not depend on the barrier. flush_metadata(durable=1) issues its barrier
     * BEFORE rewriting the entry, so when the rollback was caused by a barrier that keeps
     * failing -- #516 found this stick rejects SYNCHRONIZE CACHE(10) outright -- the restore
     * aborted before touching the entry, and the chain was freed anyway. That reproduced the
     * exact entry-beyond-chain state this function exists to prevent, which is why the first
     * cut of this fix did not close the issue. Shrinking needs no preceding barrier: ordering
     * only matters when publishing a size that reaches clusters the medium may not have linked.
     */
    f->size = old_size;
    if (first_cluster_valid(f)) {
        if (flush_metadata(f, 0, 0) != 0) {
            ok = -1;
        } else if (fs->sync != (hype_blk_sync_fn)0 && fs->sync(fs->ctx) != 0) {
            ok = -1; /* the shrink is written but not known durable */
        }
    }
    /*
     * Cut the chain only once the medium itself shows an entry within the old size -- read back,
     * not inferred from what the restore attempt returned. If it does not, leave the clusters
     * linked: a chain longer than its entry is leaked space fsck reclaims and a file that reads
     * short, while a chain shorter than its entry is the volume Linux refuses. Given the choice,
     * leak.
     */
    entry_shrunk = entry_claims_at_most(f, old_size);
    if (entry_shrunk && first_new >= 2u) {
        ok |= free_chain(fs, first_new);
        if (old_tail >= 2u) {
            ok |= fat_set(fs, old_tail, FAT32_EOC_MIN | 0x7u);
        } else {
            first_cluster_set(f, 0u); /* the file was empty: it stays empty */
        }
    }
    f->size = old_size;
    f->tail_cluster = old_tail;
    f->seek_index = 0u;
    f->seek_cluster = (f->first_cluster >= 2u) ? f->first_cluster : 0u;
    fsinfo_flush(fs);
    if (ok == 0) ok |= volume_set_dirty(fs, 0);
    if (ok != 0) {
        /*
         * #464: say so. Every call site discards this return with (void), which is defensible
         * -- the write has already failed and there is nothing better to return -- but a
         * rollback that ITSELF failed leaves the volume dirty and possibly inconsistent, and
         * that is exactly the state the operator needs to know about: it is the difference
         * between "this write did not happen" and "run fsck before trusting this volume".
         * Silence here is how a stick that needs repairing looks identical to one that does not.
         */
        hype_fat_write_note_rollback_failure();
    }
    return ok ? -1 : 0;
}

int hype_fat32_write_at(hype_fat32_wfile_t *f, uint64_t offset, const void *data,
                        unsigned int len) {
    hype_fat32_fs_t *fs = f->fs;
    uint64_t cb = cluster_bytes_of(fs);
    uint64_t end, old_size, old_cap;
    uint32_t old_count, old_tail, need, i;
    uint32_t first_new = 0u, prev;

    f->last_error = HYPE_FAT32_WFILE_ERR_NONE;
    if (len == 0u) return 0;
    if (offset + len < offset) return -1;
    if (!first_cluster_valid(f)) {
        f->last_error = HYPE_FAT32_WFILE_ERR_IDENTITY;
        return -1;
    }
    end = offset + len;

    if (end <= f->size) {
        /* In place: pure data writes, no metadata touched (the #204/#199
         * discipline that makes this safe on a volume the host OS knows). */
        return span_io(f, offset, 0, (const uint8_t *)data, len, f->size);
    }

    /* Growth. Re-measure the chain now rather than trusting open-time state:
     * appends may have extended it since, and the measure re-applies every
     * corruption refusal before this call commits to changing the volume. */
    old_size = f->size;
    if (chain_measure(fs, f->first_cluster, old_size, &old_count, &old_tail) != 0) return -1;
    old_cap = (uint64_t)old_count * cb;
    need = (uint32_t)((end + cb - 1u) / cb);
    if ((end + cb - 1u) / cb > 0x0FFFFFF5u) return -1; /* beyond what FAT32 can chain */

    if (volume_set_dirty(fs, 1) != 0) return -1;

    /* 1. Extend the chain, zeroing any fresh cluster that will hold gap
     *    bytes (cluster start before `offset`) BEFORE it becomes reachable.
     *    Clusters fully covered by the incoming data skip the zero pass. */
    prev = old_tail;
    for (i = old_count; i < need; i++) {
        uint32_t ncl;
        uint64_t cl_start = (uint64_t)i * cb;
        if (alloc_cluster(fs, &ncl) != 0) {
            (void)growth_rollback(f, first_new, old_tail, old_size);
            return -1; /* volume full (or FAT I/O failed) -- file unchanged */
        }
        if (cl_start < offset && cluster_zero(fs, ncl) != 0) {
            (void)fat_set(fs, ncl, 0u); /* orphan the never-linked cluster */
            (void)growth_rollback(f, first_new, old_tail, old_size);
            return -1;
        }
        if (prev >= 2u) {
            if (fat_set(fs, prev, ncl) != 0) {
                /* The link may be half-written (cache/copy 0 carry it, a later
                 * copy failed). Free the never-published cluster AND restore
                 * prev's terminator explicitly: when this was the FIRST link,
                 * first_new is still 0, so growth_rollback alone would leave
                 * the old tail pointing at a freed cluster. */
                (void)fat_set(fs, ncl, 0u);
                (void)fat_set(fs, prev, FAT32_EOC_MIN | 0x7u);
                (void)growth_rollback(f, first_new, old_tail, old_size);
                return -1;
            }
        } else {
            first_cluster_set(f, ncl); /* the file was empty */
        }
        if (first_new == 0u) first_new = ncl;
        prev = ncl;
    }
    f->tail_cluster = prev;

    /* 2. Zero the logical gap inside the OLD allocation: the bytes past the
     *    recorded size in already-allocated clusters were never written and
     *    hold stale media contents. */
    if (offset > old_size && old_count > 0u) {
        uint64_t zto = (offset < old_cap) ? offset : old_cap;
        if (zto > old_size && span_io(f, old_size, 0, 0, zto - old_size, end) != 0) {
            (void)growth_rollback(f, first_new, old_tail, old_size);
            return -1;
        }
    }

    /* 3. The data itself. */
    if (span_io(f, offset, 0, (const uint8_t *)data, len, end) != 0) {
        (void)growth_rollback(f, first_new, old_tail, old_size);
        return -1;
    }

    /* 4. Publish: barrier, directory size, FSInfo, barrier (flush_metadata),
     *    then retire the dirty flag. A crash before this point leaves the old
     *    size over a longer chain -- a safely shorter file. */
    f->size = end;
    if (flush_metadata(f, 1, 0) != 0) {
        (void)growth_rollback(f, first_new, old_tail, old_size);
        return -1;
    }
    return volume_set_dirty(fs, 0);
}

void hype_fat32_fs_set_sync(hype_fat32_fs_t *fs, hype_blk_sync_fn sync) {
    if (fs != (hype_fat32_fs_t *)0) fs->sync = sync;
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
