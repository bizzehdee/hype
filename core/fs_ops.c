#include "fs_ops.h"

#include "fat.h" /* hype_fat32_resolve / hype_exfat_resolve */
#include "ext_jalloc.h"
#include "lebytes.h"

/*
 * #293: the four host-FS drivers behind one vtable. Every function here is a
 * thin adapter; the on-disk logic stays in the drivers. See fs_ops.h for the
 * capability story.
 */

#define TAG_NONE 0u
#define TAG_RMAP 1u   /* u.rmap: generic read-only handle (#381) */
#define TAG_NATIVE 2u /* the driver's own writable handle arm */
#define TAG_EXT2 3u   /* u.ext2: the #384 allocating ext2 writer */
#define TAG_EXTJ 4u   /* u.extj: the #385 journaled ext3/4 writer */

/* ---------------- ISO9660 (read-only, whole-image) ---------------- */

/* The Primary Volume Descriptor: 2048 bytes at byte offset 32768. */
#define ISO_PVD_LBA 64u /* in 512-byte sectors */
#define ISO_PVD_SECTORS 4u

static int iso_read_pvd(hype_blk_read_fn read, void *ctx, uint64_t *out_size) {
    uint8_t pvd[ISO_PVD_SECTORS * HYPE_BLK_SECTOR_SIZE];
    uint64_t space, bs;

    if (read(ctx, ISO_PVD_LBA, ISO_PVD_SECTORS, pvd) != 0) {
        return -1;
    }
    if (pvd[0] != 0x01u || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' ||
        pvd[5] != '1') {
        return -1;
    }
    space = hype_rd32(pvd + 80); /* VolumeSpaceSize, little-endian half */
    bs = hype_rd16(pvd + 128);   /* LogicalBlockSize */
    if (space == 0 || bs == 0 || space > (~0ull) / bs) {
        return -1;
    }
    if (out_size) {
        *out_size = space * bs;
    }
    return 0;
}

static int iso_probe(hype_blk_read_fn read, void *ctx) {
    return iso_read_pvd(read, ctx, 0);
}

static int iso_mount(hype_fs_t *fs, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx) {
    (void)write; /* read-only by nature; a write callback is simply unused */
    fs->read = read;
    fs->write = 0;
    fs->ctx = ctx;
    return iso_read_pvd(read, ctx, &fs->u.iso.size_bytes);
}

/* The only path ISO9660 resolves is the image itself: hype's ISO consumers
 * stream the whole disc (iso_stream), there is no directory walk to expose.
 * Anything that is not the root/empty path is honestly refused. */
static int iso_is_root(const char *path) {
    if (path == 0) {
        return 1;
    }
    if (path[0] == 0) {
        return 1;
    }
    if ((path[0] == '/' || path[0] == '\\') && path[1] == 0) {
        return 1;
    }
    return 0;
}

static int iso_map_root(hype_fs_t *fs, hype_file_rmap_t *out) {
    uint64_t sectors =
        (fs->u.iso.size_bytes + HYPE_BLK_SECTOR_SIZE - 1) / HYPE_BLK_SECTOR_SIZE;
    hype_file_rmap_init(out, fs->u.iso.size_bytes);
    if (sectors == 0) {
        return -1;
    }
    return hype_file_rmap_append(out, HYPE_RANGE_DATA, 0, sectors);
}

static int iso_map_ranges(hype_fs_t *fs, const char *path, hype_file_rmap_t *out) {
    if (!iso_is_root(path)) {
        return -1;
    }
    return iso_map_root(fs, out);
}

static int iso_lookup(hype_fs_t *fs, const char *path, hype_fs_file_t *out) {
    if (!iso_is_root(path)) {
        return -1;
    }
    out->fs = fs;
    out->tag = TAG_RMAP;
    out->size = fs->u.iso.size_bytes;
    return iso_map_root(fs, &out->u.rmap);
}

static int rmap_read_at(hype_fs_file_t *f, uint64_t offset, void *dst, unsigned int len) {
    if (f->tag != TAG_RMAP) {
        return -1;
    }
    return hype_file_rmap_read_at(&f->u.rmap, f->fs->read, f->fs->ctx, offset, dst, len);
}

static const hype_fs_ops_t iso_ops = {
    "iso9660",
    HYPE_FS_CAP_READ,
    iso_probe,
    iso_mount,
    iso_lookup,
    iso_map_ranges,
    rmap_read_at,
    0, /* write_at: read-only filesystem, exactly as the ticket requires */
    0, /* append */
    0, /* create */
    0, /* unlink */
    0, /* mkdir */
    0, /* rmdir */
    0, /* rename */
    0, /* sync */
    0, /* set_time */
    0, /* set_barrier */
};

/* ---------------- FAT32 ---------------- */

static int fat32_probe(hype_blk_read_fn read, void *ctx) {
    /* The strictest recognition hype has is the mount parse itself (BPB +
     * FSInfo validation); run it against a throwaway descriptor. */
    hype_fat32_fs_t scratch;
    return hype_fat32_fs_mount(read, 0, ctx, &scratch);
}

static int fat32_mount(hype_fs_t *fs, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx) {
    fs->read = read;
    fs->write = write;
    fs->ctx = ctx;
    return hype_fat32_fs_mount(read, write, ctx, &fs->u.fat32);
}

/* The three map_ranges adapters keep their 4 KiB+ extent map static, not on
 * the stack: a frame that large needs the __chkstk probe the freestanding
 * build has no definition for (the core/ext.c dir_search precedent, #366).
 * Resolution is BSP setup-time and single-threaded, and each call fully
 * rewrites the buffer. */
static hype_file_map_t g_resolve_map;

static int fat32_map_ranges(hype_fs_t *fs, const char *path, hype_file_rmap_t *out) {
    if (hype_fat32_resolve(fs->read, fs->ctx, path, &g_resolve_map) != 0) {
        return -1;
    }
    return hype_file_rmap_from_extents(&g_resolve_map, out);
}

static int fat32_lookup(hype_fs_t *fs, const char *path, hype_fs_file_t *out) {
    out->fs = fs;
    if (fs->write != 0) {
        /* #382: a writable mount opens the native random-I/O handle, chain
         * validated against DIR_FileSize. */
        out->tag = TAG_NATIVE;
        if (hype_fat32_open(&fs->u.fat32, path, &out->u.fat32) != 0) {
            return -1;
        }
        out->size = out->u.fat32.size;
        return 0;
    }
    out->tag = TAG_RMAP;
    if (fat32_map_ranges(fs, path, &out->u.rmap) != 0) {
        return -1;
    }
    out->size = out->u.rmap.size_bytes;
    return 0;
}

static int fat32_read_at(hype_fs_file_t *f, uint64_t offset, void *dst, unsigned int len) {
    if (f->tag == TAG_NATIVE) {
        return hype_fat32_read_at(&f->u.fat32, offset, dst, len);
    }
    return rmap_read_at(f, offset, dst, len);
}

static int fat32_write_at(hype_fs_file_t *f, uint64_t offset, const void *src, unsigned int len) {
    if (f->tag != TAG_NATIVE) {
        return -1;
    }
    if (hype_fat32_write_at(&f->u.fat32, offset, src, len) != 0) {
        return -1;
    }
    if (f->u.fat32.size > f->size) f->size = f->u.fat32.size;
    return 0;
}

static int fat32_append(hype_fs_file_t *f, const void *src, unsigned int len) {
    if (f->tag != TAG_NATIVE) {
        return -1; /* only a create() handle is append-shaped today (#382) */
    }
    if (hype_fat32_append(&f->u.fat32, src, len) != 0) {
        return -1;
    }
    f->size = f->u.fat32.size;
    return 0;
}

static int fat32_create(hype_fs_t *fs, const char *path, hype_fs_file_t *out) {
    out->fs = fs;
    out->tag = TAG_NATIVE;
    out->size = 0;
    return hype_fat32_create(&fs->u.fat32, path, &out->u.fat32);
}

static int fat32_unlink(hype_fs_t *fs, const char *path) {
    return hype_fat32_unlink(&fs->u.fat32, path);
}
static int fat32_mkdir(hype_fs_t *fs, const char *path) {
    return hype_fat32_mkdir(&fs->u.fat32, path);
}
static int fat32_rmdir(hype_fs_t *fs, const char *path) {
    return hype_fat32_rmdir(&fs->u.fat32, path);
}
static int fat32_rename(hype_fs_t *fs, const char *from, const char *to) {
    return hype_fat32_rename(&fs->u.fat32, from, to);
}
static void fat32_set_time(hype_fs_t *fs, const hype_rtc_time_t *now) {
    hype_fat32_fs_set_time(&fs->u.fat32, now);
}
static void fat32_set_barrier(hype_fs_t *fs, hype_blk_sync_fn sync) {
    hype_fat32_fs_set_sync(&fs->u.fat32, sync);
}

static const hype_fs_ops_t fat32_ops = {
    "fat32",
    HYPE_FS_CAP_READ | HYPE_FS_CAP_WRITE_INPLACE | HYPE_FS_CAP_WRITE_GROW | HYPE_FS_CAP_APPEND |
        HYPE_FS_CAP_NAMESPACE,
    fat32_probe,
    fat32_mount,
    fat32_lookup,
    fat32_map_ranges,
    fat32_read_at,
    fat32_write_at, /* #382: in-place + growth with gap zero-fill */
    fat32_append,
    fat32_create,
    fat32_unlink,
    fat32_mkdir,
    fat32_rmdir,
    fat32_rename,
    0, /* sync: the FAT writer's barrier is injected per-volume (set_barrier) */
    fat32_set_time,
    fat32_set_barrier,
};

/* ---------------- exFAT ---------------- */

static int exfat_probe(hype_blk_read_fn read, void *ctx) {
    uint8_t bs[HYPE_BLK_SECTOR_SIZE];
    unsigned i;
    static const char sig[8] = {'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};

    if (read(ctx, 0, 1, bs) != 0) {
        return -1;
    }
    for (i = 0; i < 8; i++) {
        if (bs[3 + i] != (uint8_t)sig[i]) {
            return -1;
        }
    }
    if (bs[510] != 0x55u || bs[511] != 0xAAu) {
        return -1;
    }
    return 0;
}

static int exfat_mount(hype_fs_t *fs, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx) {
    fs->read = read;
    fs->write = write;
    fs->ctx = ctx;
    return hype_exfat_fs_mount(read, write, ctx, &fs->u.exfat);
}

static int exfat_lookup(hype_fs_t *fs, const char *path, hype_fs_file_t *out) {
    out->fs = fs;
    out->tag = TAG_NATIVE;
    if (hype_exfat_lookup(&fs->u.exfat, path, 0, &out->u.exfat) != 0) {
        return -1;
    }
    out->size = out->u.exfat.size;
    return 0;
}

static int exfat_map_ranges(hype_fs_t *fs, const char *path, hype_file_rmap_t *out) {
    if (hype_exfat_resolve(fs->read, fs->ctx, path, &g_resolve_map) != 0) {
        return -1;
    }
    return hype_file_rmap_from_extents(&g_resolve_map, out);
}

static int exfat_read_at(hype_fs_file_t *f, uint64_t offset, void *dst, unsigned int len) {
    if (f->tag != TAG_NATIVE) {
        return -1;
    }
    return hype_exfat_read_at(&f->u.exfat, offset, dst, len);
}

static int exfat_write_at(hype_fs_file_t *f, uint64_t offset, const void *src, unsigned int len) {
    if (f->tag != TAG_NATIVE) {
        return -1;
    }
    if (hype_exfat_write_at(&f->u.exfat, offset, src, len) != 0) {
        return -1;
    }
    if (f->u.exfat.size > f->size) f->size = f->u.exfat.size; /* #383 growth */
    return 0;
}

static int exfat_append(hype_fs_file_t *f, const void *src, unsigned int len) {
    if (f->tag != TAG_NATIVE) {
        return -1;
    }
    if (hype_exfat_append(&f->u.exfat, src, len) != 0) {
        return -1;
    }
    f->size = f->u.exfat.size;
    return 0;
}

static int exfat_create(hype_fs_t *fs, const char *path, hype_fs_file_t *out) {
    out->fs = fs;
    out->tag = TAG_NATIVE;
    out->size = 0;
    return hype_exfat_create(&fs->u.exfat, path, &out->u.exfat);
}

static int exfat_unlink(hype_fs_t *fs, const char *path) {
    return hype_exfat_unlink(&fs->u.exfat, path);
}
static int exfat_mkdir(hype_fs_t *fs, const char *path) {
    return hype_exfat_mkdir(&fs->u.exfat, path);
}
static int exfat_rmdir(hype_fs_t *fs, const char *path) {
    return hype_exfat_rmdir(&fs->u.exfat, path);
}
static int exfat_rename(hype_fs_t *fs, const char *from, const char *to) {
    return hype_exfat_rename(&fs->u.exfat, from, to);
}
static int exfat_sync(hype_fs_t *fs) {
    return hype_exfat_fs_sync(&fs->u.exfat);
}
static void exfat_set_time(hype_fs_t *fs, const hype_rtc_time_t *now) {
    hype_exfat_fs_set_time(&fs->u.exfat, now);
}

static const hype_fs_ops_t exfat_ops = {
    "exfat",
    HYPE_FS_CAP_READ | HYPE_FS_CAP_WRITE_INPLACE | HYPE_FS_CAP_WRITE_GROW | HYPE_FS_CAP_APPEND |
        HYPE_FS_CAP_NAMESPACE, /* #383: random writes grow with VDL semantics */
    exfat_probe,
    exfat_mount,
    exfat_lookup,
    exfat_map_ranges,
    exfat_read_at,
    exfat_write_at,
    exfat_append,
    exfat_create,
    exfat_unlink,
    exfat_mkdir,
    exfat_rmdir,
    exfat_rename,
    exfat_sync,
    exfat_set_time,
    0, /* set_barrier: the exFAT writer orders its own commits */
};

/* ---------------- ext2/3/4 ---------------- */

static int ext_probe(hype_blk_read_fn read, void *ctx) {
    return hype_ext_probe(read, ctx);
}

static int ext_mount(hype_fs_t *fs, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx) {
    fs->read = read;
    fs->write = write;
    fs->ctx = ctx;
    /* No mount state: the resolver revalidates the superblock (and the
     * clean-unmount gate, when writing) on every call. */
    return hype_ext_probe(read, ctx);
}

static int ext_map_ranges(hype_fs_t *fs, const char *path, hype_file_rmap_t *out) {
    if (hype_ext_resolve(fs->read, fs->ctx, path, &g_resolve_map) != 0) {
        return -1;
    }
    return hype_file_rmap_from_extents(&g_resolve_map, out);
}

static int ext_lookup(hype_fs_t *fs, const char *path, hype_fs_file_t *out) {
    out->fs = fs;
    if (fs->write != 0) {
        /* #384/#385: the allocating writers open first, so a sparse backing
         * file gets hole-filling writes -- the journaled one on ext3/4, the
         * direct one on ext2. Whatever both refuse (unsupported features,
         * checksummed metadata, a non-empty journal) falls back to the
         * legacy in-place-only handle. All require a clean volume. */
        out->tag = TAG_EXTJ;
        if (hype_extj_open_rw(fs->read, fs->write, fs->ctx, path, &out->u.extj) == 0) {
            out->size = out->u.extj.size_bytes;
            return 0;
        }
        out->tag = TAG_EXT2;
        if (hype_ext2_open_rw(fs->read, fs->write, fs->ctx, path, &out->u.ext2) == 0) {
            out->size = out->u.ext2.size_bytes;
            return 0;
        }
        out->tag = TAG_NATIVE;
        if (hype_ext_open_rw(fs->read, fs->write, fs->ctx, path, &out->u.ext) != 0) {
            return -1;
        }
        out->size = out->u.ext.map.size_bytes;
        return 0;
    }
    out->tag = TAG_RMAP;
    if (ext_map_ranges(fs, path, &out->u.rmap) != 0) {
        return -1;
    }
    out->size = out->u.rmap.size_bytes;
    return 0;
}

static int ext_read_at(hype_fs_file_t *f, uint64_t offset, void *dst, unsigned int len) {
    if (f->tag == TAG_RMAP) {
        return rmap_read_at(f, offset, dst, len);
    }
    if (f->tag == TAG_EXT2) {
        return hype_ext2_read_at(&f->u.ext2, offset, dst, len);
    }
    if (f->tag == TAG_EXTJ) {
        return hype_extj_read_at(&f->u.extj, offset, dst, len);
    }
    if (f->tag == TAG_NATIVE) {
        return hype_ext_read_at(&f->u.ext, offset, dst, len);
    }
    return -1;
}

static int ext_write_chunked(hype_fs_file_t *f, uint64_t offset, const void *src,
                             unsigned int len);

static int ext_write_at(hype_fs_file_t *f, uint64_t offset, const void *src, unsigned int len) {
    if (f->tag == TAG_EXT2 || f->tag == TAG_EXTJ) {
        /* allocates across holes (#384/#385) and, since #497, GROWS past EOF -- chunked so a
         * large span never trips the writers' per-transaction bounds */
        return ext_write_chunked(f, offset, src, len);
    }
    if (f->tag != TAG_NATIVE) {
        return -1;
    }
    return hype_ext_write_at(&f->u.ext, offset, src, len);
}

/*
 * #497: both allocating writers carry a per-call transaction bound (EXT2_ALLOC_MAX undo slots;
 * HYPE_JBD2_MAX_BLOCKS journal credits), so a large write/append is CHUNKED here rather than
 * refused -- each chunk is its own all-or-nothing transaction. 256 KiB keeps a chunk's metadata
 * footprint (blocks + indirection/extent nodes) comfortably inside both bounds at every block
 * size the writers accept -- the tightest is ext2's EXT2_ALLOC_MAX/2 blocks per call, which at a
 * 1 KiB block size is 128 KiB -- and a failure mid-sequence leaves a shorter, CONSISTENT file:
 * every committed chunk already published its own i_size.
 */
#define EXT_GROW_CHUNK (64u * 1024u)

static int ext_write_chunked(hype_fs_file_t *f, uint64_t offset, const void *src,
                             unsigned int len) {
    const uint8_t *p = (const uint8_t *)src;
    while (len > 0u) {
        unsigned int n = (len > EXT_GROW_CHUNK) ? EXT_GROW_CHUNK : len;
        int rc = (f->tag == TAG_EXT2) ? hype_ext2_write_at(&f->u.ext2, offset, p, n)
                                      : hype_extj_write_at(&f->u.extj, offset, p, n);
        if (rc != 0) {
            return -1;
        }
        offset += n;
        p += n;
        len -= n;
    }
    f->size = (f->tag == TAG_EXT2) ? f->u.ext2.size_bytes : f->u.extj.size_bytes;
    return 0;
}

static int ext_append(hype_fs_file_t *f, const void *src, unsigned int len) {
    if (f->tag != TAG_EXT2 && f->tag != TAG_EXTJ) {
        return -1; /* the legacy in-place handle cannot change a file's size */
    }
    return ext_write_chunked(f, f->size, src, len);
}

static const hype_fs_ops_t ext_ops = {
    "ext",
    HYPE_FS_CAP_READ | HYPE_FS_CAP_WRITE_INPLACE | HYPE_FS_CAP_SPARSE |
        HYPE_FS_CAP_WRITE_GROW | HYPE_FS_CAP_APPEND, /* #497 */
    ext_probe,
    ext_mount,
    ext_lookup,
    ext_map_ranges,
    ext_read_at,
    ext_write_at,
    ext_append, /* #497 */
    0, /* create */
    0, /* unlink */
    0, /* mkdir */
    0, /* rmdir */
    0, /* rename */
    0, /* sync */
    0, /* set_time */
    0, /* set_barrier */
};

/* ---------------- NTFS (#337: read + in-place write, sparse-aware) -------- */

static int ntfs_probe(hype_blk_read_fn read, void *ctx) {
    return hype_ntfs_probe(read, ctx);
}

static int ntfs_mount(hype_fs_t *fs, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx) {
    fs->read = read;
    fs->write = write;
    fs->ctx = ctx;
    return hype_ntfs_mount(read, ctx, &fs->u.ntfs);
}

static int ntfs_map_ranges(hype_fs_t *fs, const char *path, hype_file_rmap_t *out) {
    return hype_ntfs_resolve(&fs->u.ntfs, path, out);
}

static int ntfs_lookup(hype_fs_t *fs, const char *path, hype_fs_file_t *out) {
    out->fs = fs;
    out->tag = TAG_RMAP;
    if (ntfs_map_ranges(fs, path, &out->u.rmap) != 0) {
        return -1;
    }
    out->size = out->u.rmap.size_bytes;
    return 0;
}

/* In-place only, DATA ranges only: hype_file_rmap_write_at refuses a span
 * touching a HOLE (needs allocation) or UNWRITTEN (needs an initialized-size
 * advance) BEFORE writing anything -- decision 30's no-silent-short-write. */
static int ntfs_write_at(hype_fs_file_t *f, uint64_t offset, const void *src, unsigned int len) {
    if (f->tag != TAG_RMAP) {
        return -1;
    }
    return hype_file_rmap_write_at(&f->u.rmap, f->fs->read, f->fs->write, f->fs->ctx, offset,
                                   src, len);
}

static const hype_fs_ops_t ntfs_ops = {
    "ntfs",
    HYPE_FS_CAP_READ | HYPE_FS_CAP_WRITE_INPLACE | HYPE_FS_CAP_SPARSE,
    ntfs_probe,
    ntfs_mount,
    ntfs_lookup,
    ntfs_map_ranges,
    rmap_read_at,
    ntfs_write_at,
    0, /* append: growth is out of scope, permanently (decision 30) */
    0, /* create */
    0, /* unlink */
    0, /* mkdir */
    0, /* rmdir */
    0, /* rename */
    0, /* sync */
    0, /* set_time */
    0, /* set_barrier */
};

/* ---------------- registry + wrappers ---------------- */

static const hype_fs_ops_t *const g_registry[] = {
    &iso_ops,
    &exfat_ops,
    &fat32_ops,
    &ntfs_ops,
    &ext_ops,
};

const hype_fs_ops_t *const *hype_fs_registry(unsigned *count) {
    if (count) {
        *count = sizeof(g_registry) / sizeof(g_registry[0]);
    }
    return g_registry;
}

int hype_fs_mount_auto(hype_fs_t *fs, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx) {
    unsigned i, n;
    const hype_fs_ops_t *const *reg = hype_fs_registry(&n);

    fs->ops = 0;
    if (read == 0) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (reg[i]->probe(read, ctx) == 0) {
            if (reg[i]->mount(fs, read, write, ctx) != 0) {
                return -1; /* claimed but unmountable: a real refusal, not "try the next" */
            }
            fs->ops = reg[i];
            return 0;
        }
    }
    return -1;
}

unsigned hype_fs_caps(const hype_fs_t *fs) {
    unsigned caps;
    if (fs == 0 || fs->ops == 0) {
        return 0;
    }
    caps = fs->ops->caps;
    if (fs->write == 0) {
        caps &= HYPE_FS_CAP_READ | HYPE_FS_CAP_SPARSE; /* read-only mount */
    }
    return caps;
}

static int fs_ready(const hype_fs_t *fs) {
    return fs != 0 && fs->ops != 0;
}

static int file_ready(const hype_fs_file_t *f) {
    return f != 0 && f->fs != 0 && f->fs->ops != 0;
}

int hype_fs_lookup(hype_fs_t *fs, const char *path, hype_fs_file_t *out) {
    if (!fs_ready(fs) || fs->ops->lookup == 0) {
        return -1;
    }
    return fs->ops->lookup(fs, path, out);
}

int hype_fs_map_ranges(hype_fs_t *fs, const char *path, hype_file_rmap_t *out) {
    if (!fs_ready(fs) || fs->ops->map_ranges == 0) {
        return -1;
    }
    return fs->ops->map_ranges(fs, path, out);
}

int hype_fs_read_at(hype_fs_file_t *f, uint64_t offset, void *dst, unsigned int len) {
    if (!file_ready(f) || f->fs->ops->read_at == 0) {
        return -1;
    }
    return f->fs->ops->read_at(f, offset, dst, len);
}

int hype_fs_write_at(hype_fs_file_t *f, uint64_t offset, const void *src, unsigned int len) {
    if (!file_ready(f) || f->fs->ops->write_at == 0 || f->fs->write == 0) {
        return -1;
    }
    return f->fs->ops->write_at(f, offset, src, len);
}

int hype_fs_append(hype_fs_file_t *f, const void *src, unsigned int len) {
    if (!file_ready(f) || f->fs->ops->append == 0 || f->fs->write == 0) {
        return -1;
    }
    return f->fs->ops->append(f, src, len);
}

int hype_fs_create(hype_fs_t *fs, const char *path, hype_fs_file_t *out) {
    if (!fs_ready(fs) || fs->ops->create == 0 || fs->write == 0) {
        return -1;
    }
    return fs->ops->create(fs, path, out);
}

int hype_fs_unlink(hype_fs_t *fs, const char *path) {
    if (!fs_ready(fs) || fs->ops->unlink == 0 || fs->write == 0) {
        return -1;
    }
    return fs->ops->unlink(fs, path);
}

int hype_fs_mkdir(hype_fs_t *fs, const char *path) {
    if (!fs_ready(fs) || fs->ops->mkdir == 0 || fs->write == 0) {
        return -1;
    }
    return fs->ops->mkdir(fs, path);
}

int hype_fs_rmdir(hype_fs_t *fs, const char *path) {
    if (!fs_ready(fs) || fs->ops->rmdir == 0 || fs->write == 0) {
        return -1;
    }
    return fs->ops->rmdir(fs, path);
}

int hype_fs_rename(hype_fs_t *fs, const char *from, const char *to) {
    if (!fs_ready(fs) || fs->ops->rename == 0 || fs->write == 0) {
        return -1;
    }
    return fs->ops->rename(fs, from, to);
}

int hype_fs_sync(hype_fs_t *fs) {
    if (!fs_ready(fs)) {
        return -1;
    }
    if (fs->ops->sync == 0) {
        return 0; /* nothing to flush is success, not a missing capability */
    }
    return fs->ops->sync(fs);
}

void hype_fs_set_time(hype_fs_t *fs, const hype_rtc_time_t *now) {
    if (fs_ready(fs) && fs->ops->set_time != 0) {
        fs->ops->set_time(fs, now);
    }
}

void hype_fs_set_barrier(hype_fs_t *fs, hype_blk_sync_fn sync) {
    if (fs_ready(fs) && fs->ops->set_barrier != 0) {
        fs->ops->set_barrier(fs, sync);
    }
}

int hype_fs_file_identity_error(const hype_fs_file_t *f) {
    if (f == 0 || f->fs == 0 || f->fs->ops != &fat32_ops || f->tag != TAG_NATIVE) {
        return 0;
    }
    return f->u.fat32.last_error == HYPE_FAT32_WFILE_ERR_IDENTITY;
}
