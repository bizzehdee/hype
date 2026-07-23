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
    if (fat_capacity >= 1u && out->max_cluster > fat_capacity - 1u) out->max_cluster = fat_capacity - 1u;
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

/* Write the file's current first-cluster + size into its directory entry, and
 * flush the free-cluster counters to FSInfo. */
static int flush_metadata(hype_fat32_wfile_t *f) {
    hype_fat32_fs_t *fs = f->fs;
    uint8_t sec[SECSZ];
    uint8_t ent[DIRENT_SIZE];
    uint32_t sz = (f->size > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)f->size;

    if (fs->read(fs->ctx, f->dirent_lba, 1u, sec) != 0) return -1;
    hype_fat_dirent_build(ent, f->name11, HYPE_FAT_ATTR_ARCHIVE, f->first_cluster, sz);
    bcopy(sec + f->dirent_off, ent, DIRENT_SIZE);
    if (fs->write(fs->ctx, f->dirent_lba, 1u, sec) != 0) return -1;

    if (fs->fsinfo_sector != 0u) {
        uint8_t fsi[SECSZ];
        if (fs->read(fs->ctx, fs->fsinfo_sector, 1u, fsi) == 0 &&
            hype_fat32_fsinfo_set(fsi, fs->free_count, fs->next_free) == 0) {
            (void)fs->write(fs->ctx, fs->fsinfo_sector, 1u, fsi);
        }
    }
    return 0;
}

/* Names match if their 11-byte 8.3 fields are byte-identical. */
static int name_eq(const uint8_t *a, const uint8_t *b) {
    unsigned int i;
    for (i = 0; i < 11u; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int hype_fat32_create(hype_fat32_fs_t *fs, const char *name, hype_fat32_wfile_t *out) {
    uint8_t name11[11];
    uint8_t sec[SECSZ];
    uint32_t cl, last_root = fs->root_cluster;
    uint64_t free_lba = 0;      /* first reusable dirent slot found */
    unsigned int free_off = 0;
    int have_free = 0;
    uint64_t use_lba = 0;
    unsigned int use_off = 0;
    uint32_t first_cl = 0;

    hype_fat_shortname_83(name, name11);

    /* Walk the root directory: find an existing entry (to truncate) or a slot. */
    cl = fs->root_cluster;
    while (cl >= 2u && cl <= fs->max_cluster) {
        unsigned int s;
        for (s = 0; s < fs->spc; s++) {
            uint64_t lba = cluster_lba(fs, cl) + s;
            unsigned int e;
            if (fs->read(fs->ctx, lba, 1u, sec) != 0) return -1;
            for (e = 0; e < SECSZ / DIRENT_SIZE; e++) {
                uint8_t *ent = sec + e * DIRENT_SIZE;
                uint8_t b0 = ent[0];
                if (b0 == 0x00u) { /* end-of-directory terminator */
                    if (!have_free) { free_lba = lba; free_off = e * DIRENT_SIZE; have_free = 1; }
                    goto scanned;
                }
                if (b0 == 0xE5u) { /* deleted slot */
                    if (!have_free) { free_lba = lba; free_off = e * DIRENT_SIZE; have_free = 1; }
                    continue;
                }
                if (ent[11] == 0x0Fu) continue; /* LFN component */
                if (name_eq(ent, name11)) {     /* existing file -> truncate + reuse slot */
                    uint32_t old = hype_fat_dirent_cluster(ent);
                    if (old >= 2u) { if (free_chain(fs, old) != 0) return -1; }
                    use_lba = lba;
                    use_off = e * DIRENT_SIZE;
                    goto placed;
                }
            }
        }
        last_root = cl;
        if (fat_get(fs, cl, &cl) != 0) return -1;
    }
scanned:
    if (have_free) {
        use_lba = free_lba;
        use_off = free_off;
    } else {
        /* Root directory is full -- grow it by one cluster. */
        uint32_t newcl;
        if (alloc_cluster(fs, &newcl) != 0) return -1;
        if (cluster_zero(fs, newcl) != 0) return -1;
        if (fat_set(fs, last_root, newcl) != 0) return -1;
        use_lba = cluster_lba(fs, newcl);
        use_off = 0u;
    }
placed:
    /* Allocate the file's first data cluster and write its directory entry. */
    if (alloc_cluster(fs, &first_cl) != 0) return -1;

    out->fs = fs;
    bcopy(out->name11, name11, 11u);
    out->first_cluster = first_cl;
    out->tail_cluster = first_cl;
    out->size = 0u;
    out->dirent_lba = use_lba;
    out->dirent_off = use_off;
    return flush_metadata(out);
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
