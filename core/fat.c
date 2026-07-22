#include "fat.h"

/* ---- little-endian field readers over a 512-byte sector buffer ---- */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

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
        uint16_t u = rd16(ent + off[i]);
        unsigned idx = base + i;
        if (idx >= lfn_cap - 1u) {
            break;
        }
        lfn[idx] = (u == 0u || u == 0xFFFFu) ? '\0' : (char)(u & 0xFFu);
    }
}

/* ================================ FAT32 ================================ */

typedef struct {
    hype_fat_read_fn read;
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
static uint32_t fat32_next(const fat32_vol_t *v, uint32_t cl) {
    uint8_t sec[HYPE_FAT_SECTOR_SIZE];
    uint32_t byte = cl * 4u;
    uint32_t fat_sec = v->fat_start + byte / HYPE_FAT_SECTOR_SIZE;
    uint32_t within = byte % HYPE_FAT_SECTOR_SIZE;
    if (v->read(v->ctx, fat_sec, 1u, sec) != 0) {
        return FAT32_EOC;
    }
    return rd32(sec + within) & 0x0FFFFFFFu;
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
            uint8_t sec[HYPE_FAT_SECTOR_SIZE];
            unsigned e;
            if (v->read(v->ctx, cluster_lba(v, dir_cl) + s, 1u, sec) != 0) {
                return -1;
            }
            for (e = 0; e < HYPE_FAT_SECTOR_SIZE; e += 32u) {
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
                        *first_cl = ((uint32_t)rd16(ent + 0x14) << 16) | rd16(ent + 0x1A);
                        *attr = a;
                        *size = rd32(ent + 0x1C);
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
                               hype_fat_file_t *out) {
    uint32_t cl = first_cl;
    uint64_t sectors_per_cluster = v->spc;
    unsigned guard = 0;

    out->count = 0;
    out->size_bytes = size;
    if (size == 0u) {
        return 0; /* empty file: zero extents */
    }
    while (cl >= 2u && cl < FAT32_EOC && guard++ < (1u << 20)) {
        uint64_t lba = cluster_lba(v, cl);
        if (out->count > 0u) {
            hype_fat_extent_t *last = &out->extents[out->count - 1u];
            if (last->start_lba + last->sector_count == lba) {
                last->sector_count += sectors_per_cluster; /* contiguous: extend */
                cl = fat32_next(v, cl);
                continue;
            }
        }
        if (out->count >= HYPE_FAT_MAX_EXTENTS) {
            return -1; /* too fragmented for our fixed extent table */
        }
        out->extents[out->count].start_lba = lba;
        out->extents[out->count].sector_count = sectors_per_cluster;
        out->count++;
        cl = fat32_next(v, cl);
    }

    /* Trim the trailing extent to the exact file length (clusters are rounded
     * up; the ISO stream must not read past size_bytes). */
    {
        uint64_t total_sectors = ((uint64_t)size + HYPE_FAT_SECTOR_SIZE - 1u) / HYPE_FAT_SECTOR_SIZE;
        uint64_t acc = 0;
        unsigned i;
        for (i = 0; i < out->count; i++) {
            uint64_t remain = total_sectors - acc;
            if (out->extents[i].sector_count >= remain) {
                out->extents[i].sector_count = remain;
                out->count = i + 1u;
                break;
            }
            acc += out->extents[i].sector_count;
        }
    }
    return 0;
}

int hype_fat32_resolve(hype_fat_read_fn read, void *ctx, const char *path, hype_fat_file_t *out) {
    uint8_t bpb[HYPE_FAT_SECTOR_SIZE];
    fat32_vol_t v;
    uint32_t dir_cl;
    unsigned pos = 0;
    char comp[128];
    char next[128];
    unsigned clen;

    if (read(ctx, 0u, 1u, bpb) != 0) {
        return -1;
    }
    if (rd16(bpb + 0x0B) != HYPE_FAT_SECTOR_SIZE) {
        return -1; /* only 512-byte logical sectors supported */
    }
    /* FAT32 has a zero 16-bit FATSz (0x16) and a nonzero 32-bit FATSz (0x24). */
    if (rd16(bpb + 0x16) != 0u || rd32(bpb + 0x24) == 0u) {
        return -1;
    }
    v.read = read;
    v.ctx = ctx;
    v.spc = bpb[0x0D];
    v.reserved = rd16(bpb + 0x0E);
    v.fat_start = v.reserved;
    v.fat_size = rd32(bpb + 0x24);
    v.root_cluster = rd32(bpb + 0x2C);
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
 * exFAT layout (Microsoft exFAT spec): the boot sector (sector 0) holds
 * FatOffset(0x50, u32 sectors), ClusterHeapOffset(0x58, u32 sectors),
 * FirstClusterOfRootDirectory(0x60, u32), BytesPerSectorShift(0x6C, u8),
 * SectorsPerClusterShift(0x6D, u8). Directory entries are 32 bytes; the ones we
 * need: File (0x85), Stream Extension (0xC0, carries first cluster + data
 * length + the NoFatChain flag), and File Name (0xC1, 15 UTF-16 chars each).
 */
typedef struct {
    hype_fat_read_fn read;
    void *ctx;
    uint32_t fat_offset;      /* sectors */
    uint32_t cluster_heap;    /* sectors */
    uint32_t root_cluster;
    uint32_t sec_per_cluster; /* 1 << SectorsPerClusterShift */
} exfat_vol_t;

#define EXFAT_EOC 0xFFFFFFF8u
#define EXFAT_ENT_FILE 0x85u
#define EXFAT_ENT_STREAM 0xC0u
#define EXFAT_ENT_NAME 0xC1u
#define EXFAT_ATTR_DIR 0x10u

static uint64_t exfat_cluster_lba(const exfat_vol_t *v, uint32_t cl) {
    return (uint64_t)v->cluster_heap + (uint64_t)(cl - 2u) * v->sec_per_cluster;
}

static uint32_t exfat_next(const exfat_vol_t *v, uint32_t cl) {
    uint8_t sec[HYPE_FAT_SECTOR_SIZE];
    uint32_t byte = cl * 4u;
    uint32_t fat_sec = v->fat_offset + byte / HYPE_FAT_SECTOR_SIZE;
    uint32_t within = byte % HYPE_FAT_SECTOR_SIZE;
    if (v->read(v->ctx, fat_sec, 1u, sec) != 0) {
        return EXFAT_EOC;
    }
    return rd32(sec + within);
}

/* Match one File entry set (File + Stream + Name entries) against `comp`. The
 * caller positions us at the 0x85 File entry (`file_ent`); we then read the
 * following Stream + Name entries. Because a set can straddle sectors, this
 * pulls entries via absolute entry index through a small helper. Simplified:
 * we require the whole set within the directory's cluster chain and read entry
 * by entry. Returns 1 on match (fills outputs), 0 no-match, -1 read error. */

/* Read the 32-byte directory entry at global index `ei` within the directory
 * whose chain starts at dir_cl. Returns 0 ok (fills ent[32]), -1 error/end. */
static int exfat_read_entry(const exfat_vol_t *v, uint32_t dir_cl, uint32_t ei, uint8_t ent[32]) {
    uint32_t per_cluster = v->sec_per_cluster * (HYPE_FAT_SECTOR_SIZE / 32u);
    uint32_t cluster_index = ei / per_cluster;
    uint32_t in_cluster = ei % per_cluster;
    uint32_t sec_in_cluster = in_cluster / (HYPE_FAT_SECTOR_SIZE / 32u);
    uint32_t in_sec = in_cluster % (HYPE_FAT_SECTOR_SIZE / 32u);
    uint8_t sec[HYPE_FAT_SECTOR_SIZE];
    uint32_t cl = dir_cl;
    unsigned guard = 0;
    unsigned i;

    while (cluster_index-- > 0u) {
        cl = exfat_next(v, cl);
        if (cl < 2u || cl >= EXFAT_EOC || guard++ > (1u << 20)) {
            return -1;
        }
    }
    if (v->read(v->ctx, exfat_cluster_lba(v, cl) + sec_in_cluster, 1u, sec) != 0) {
        return -1;
    }
    for (i = 0; i < 32u; i++) {
        ent[i] = sec[in_sec * 32u + i];
    }
    return 0;
}

int hype_exfat_resolve(hype_fat_read_fn read, void *ctx, const char *path, hype_fat_file_t *out) {
    uint8_t boot[HYPE_FAT_SECTOR_SIZE];
    exfat_vol_t v;
    uint32_t dir_cl;
    unsigned pos = 0;
    char comp[128];

    if (read(ctx, 0u, 1u, boot) != 0) {
        return -1;
    }
    /* "EXFAT   " signature at offset 3. */
    if (boot[3] != 'E' || boot[4] != 'X' || boot[5] != 'F' || boot[6] != 'A' || boot[7] != 'T') {
        return -1;
    }
    if (boot[0x6C] != 9u) {
        return -1; /* BytesPerSectorShift must be 9 (512) */
    }
    if (boot[0x6D] > 25u) {
        return -1; /* implausible SectorsPerClusterShift (>32 MiB clusters); also
                    * keeps the 1u << shift below well-defined */
    }
    v.read = read;
    v.ctx = ctx;
    v.fat_offset = rd32(boot + 0x50);
    v.cluster_heap = rd32(boot + 0x58);
    v.root_cluster = rd32(boot + 0x60);
    v.sec_per_cluster = 1u << boot[0x6D];
    if (v.root_cluster < 2u) {
        return -1;
    }

    dir_cl = v.root_cluster;
    for (;;) {
        unsigned clen = next_component(path, &pos, comp, sizeof(comp));
        uint32_t ei = 0;
        int found = 0;
        uint32_t first_cl = 0;
        uint64_t data_len = 0;
        int no_fat_chain = 0;
        int is_dir = 0;
        unsigned guard = 0;
        int at_end;

        if (clen == 0u) {
            return -1;
        }
        /* Peek whether this is the last component. */
        {
            unsigned save = pos;
            char tmp[8];
            at_end = (next_component(path, &save, tmp, sizeof(tmp)) == 0u);
        }

        while (guard++ < (1u << 20)) {
            uint8_t ent[32];
            if (exfat_read_entry(&v, dir_cl, ei, ent) != 0) {
                break;
            }
            if (ent[0] == 0x00u) {
                break; /* end of directory */
            }
            if (ent[0] == EXFAT_ENT_FILE) {
                uint8_t secondary = ent[1];      /* count of following entries */
                uint16_t attr = rd16(ent + 4);
                uint8_t sent[32];
                char name[256];
                unsigned nlen = 0;
                unsigned k;
                if (exfat_read_entry(&v, dir_cl, ei + 1u, sent) != 0 || sent[0] != EXFAT_ENT_STREAM) {
                    ei += 1u;
                    continue;
                }
                /* Stream Extension: flags(1), first cluster (0x14,u32), data length (0x18,u64). */
                no_fat_chain = (sent[1] & 0x02u) != 0;
                first_cl = rd32(sent + 0x14);
                data_len = rd64(sent + 0x18);
                /* Name entries follow: secondary-1 of them, 15 chars each. */
                for (k = 2u; k <= secondary && nlen < sizeof(name) - 1u; k++) {
                    uint8_t nent[32];
                    unsigned c;
                    if (exfat_read_entry(&v, dir_cl, ei + k, nent) != 0 || nent[0] != EXFAT_ENT_NAME) {
                        break;
                    }
                    for (c = 0; c < 15u && nlen < sizeof(name) - 1u; c++) {
                        uint16_t u = rd16(nent + 2u + c * 2u);
                        if (u == 0u) {
                            break;
                        }
                        name[nlen++] = (char)(u & 0xFFu);
                    }
                }
                name[nlen] = '\0';
                if (nlen == clen && ci_eq(name, comp, clen)) {
                    is_dir = (attr & EXFAT_ATTR_DIR) != 0;
                    found = 1;
                    break;
                }
                ei += 1u + secondary; /* skip this whole set */
                continue;
            }
            ei += 1u;
        }

        if (!found) {
            return -1;
        }
        if (at_end) {
            if (is_dir) {
                return -1; /* names a directory, not a file */
            }
            /* Build extents. */
            out->count = 0;
            out->size_bytes = data_len;
            if (data_len == 0u) {
                return 0;
            }
            {
                uint64_t total_sectors =
                    (data_len + HYPE_FAT_SECTOR_SIZE - 1u) / HYPE_FAT_SECTOR_SIZE;
                if (no_fat_chain) {
                    /* Contiguous: one extent (the common case for a written ISO). */
                    out->extents[0].start_lba = exfat_cluster_lba(&v, first_cl);
                    out->extents[0].sector_count = total_sectors;
                    out->count = 1u;
                    return 0;
                } else {
                    uint32_t cl = first_cl;
                    uint64_t acc = 0;
                    unsigned g2 = 0;
                    while (cl >= 2u && cl < EXFAT_EOC && acc < total_sectors && g2++ < (1u << 20)) {
                        uint64_t lba = exfat_cluster_lba(&v, cl);
                        uint64_t remain = total_sectors - acc;
                        uint64_t this_sectors =
                            (remain < v.sec_per_cluster) ? remain : v.sec_per_cluster;
                        if (out->count > 0u) {
                            hype_fat_extent_t *last = &out->extents[out->count - 1u];
                            if (last->start_lba + last->sector_count == lba) {
                                last->sector_count += this_sectors;
                                acc += this_sectors;
                                cl = exfat_next(&v, cl);
                                continue;
                            }
                        }
                        if (out->count >= HYPE_FAT_MAX_EXTENTS) {
                            return -1;
                        }
                        out->extents[out->count].start_lba = lba;
                        out->extents[out->count].sector_count = this_sectors;
                        out->count++;
                        acc += this_sectors;
                        cl = exfat_next(&v, cl);
                    }
                    return 0;
                }
            }
        }
        if (!is_dir) {
            return -1;
        }
        dir_cl = first_cl;
    }
}
