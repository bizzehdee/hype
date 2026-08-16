#ifndef HYPE_CORE_FAT_WRITE_FS_H
#define HYPE_CORE_FAT_WRITE_FS_H

#include <stdint.h>

#include "rtc.h"
#include "blk_io.h" /* the shared block I/O callbacks + sector size (#292) */

/*
 * #198 pt2 (STORAGE: writable FAT32) -- block-backed write orchestration on top
 * of the pt1 pure primitives (core/fat_write.c). Mounts a FAT32 volume through
 * an injected read+write sector-callback pair (the same dependency-injection
 * pattern as the read-only reader in core/fat.c), then supports creating (or
 * truncating) a file in the root directory and appending bytes to it -- growing
 * the cluster chain, updating every FAT copy, the directory entry's size/first
 * cluster, and the FSInfo free-cluster accounting as it goes.
 *
 * The original consumer is #230: streaming hype's post-EBS debug log to a file
 * on the USB stick's FAT32 ESP. #247 added the rest of the mutating surface:
 * file delete, directory create/remove, rename/move, arbitrary-parent paths,
 * and Long File Name handling -- names that do not fit a strict 8.3 short name
 * get a spec-shaped LFN run over a collision-avoiding "LONGFI~1.TXT" short
 * name, and deleting/renaming retires an entry's whole validated LFN run with
 * it. 512-byte logical sectors; little-endian on disk.
 *
 * All LBAs are VOLUME-RELATIVE (sector 0 = the boot sector); a partitioned
 * medium wraps read/write so the callback adds the partition's first LBA.
 */

#define HYPE_FAT32_WFILE_ERR_NONE 0
#define HYPE_FAT32_WFILE_ERR_IDENTITY 1

typedef struct {
    hype_blk_read_fn read;
    hype_blk_write_fn write;
    hype_blk_sync_fn sync; /* optional persistence barrier */
    void *ctx;
    uint32_t reserved;      /* reserved sector count (== FAT copy 0 start LBA) */
    uint32_t num_fats;      /* number of FAT copies to keep in sync */
    uint32_t fat_size;      /* sectors per FAT */
    uint32_t spc;           /* sectors per cluster */
    uint32_t root_cluster;  /* first cluster of the root directory */
    uint32_t data_start;    /* first data sector (cluster 2) */
    uint32_t max_cluster;   /* highest allocatable cluster number */
    uint32_t fsinfo_sector; /* FSInfo sector LBA (0 == none) */
    uint32_t free_count;    /* running free-cluster count (unknown == 0xFFFFFFFF) */
    uint32_t next_free;     /* next-free-cluster search hint */
    int fsinfo_dirty;       /* this instance changed allocation hints */
    /*
     * Authoritative write-through view of the most recently used FAT sector.
     * Every file writer on one mounted volume must share this fs object. This
     * prevents a medium with stale read-after-write data from making a cluster
     * allocated earlier in this mount appear free again.
     */
    uint32_t fat_cache_off;
    int fat_cache_valid;
    uint8_t fat_cache[HYPE_BLK_SECTOR_SIZE];
    /*
     * Wall-clock snapshot stamped into directory entries. Zeroed (i.e. invalid)
     * by mount, which reproduces the old all-zero-timestamp behaviour; call
     * hype_fat32_fs_set_time() to supply a real one. Held rather than read
     * on demand so this layer stays free of hardware access -- same reason its
     * block I/O is injected callbacks.
     */
    hype_rtc_time_t now;
} hype_fat32_fs_t;

typedef struct {
    hype_fat32_fs_t *fs;
    uint8_t name11[11];       /* 8.3 name, for dirent rewrites on flush */
    uint32_t first_cluster;   /* first cluster of the file's data chain */
    /* #382 seek cache: `seek_cluster` is the cluster at chain index
     * `seek_index`, so sequential read_at/write_at does not re-walk the chain
     * from the start on every call. seek_cluster == 0 means empty. */
    uint32_t seek_index;
    uint32_t seek_cluster;
    /*
     * Detect an unexpected change to the immutable chain root before a
     * directory update can publish another file's cluster. This is a guard,
     * not another source of truth; the existing on-disk directory entry is
     * checked independently once the root has been published.
     */
    uint32_t first_cluster_guard;
    uint32_t tail_cluster;    /* last cluster of the chain (append cursor) */
    uint64_t size;            /* current file size in bytes */
    uint64_t dirent_lba;      /* volume-relative LBA of the sector holding the dirent */
    unsigned int dirent_off;  /* byte offset of the 32-byte dirent within that sector */
    int last_error;           /* HYPE_FAT32_WFILE_ERR_* diagnostic for the last append */
} hype_fat32_wfile_t;

/*
 * Parses the BPB (+ FSInfo) via `read` and fills *out. Returns 0 on success; -1
 * if the volume is not a 512-byte-sector FAT32 volume.
 */
int hype_fat32_fs_mount(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                        hype_fat32_fs_t *out);

/* Install an optional persistence barrier. The FAT writer invokes it around a
 * directory-size commit only when an append extended the cluster chain. */
void hype_fat32_fs_set_sync(hype_fat32_fs_t *fs, hype_blk_sync_fn sync);

/* #464: how many growth rollbacks failed to complete. Non-zero means a volume was left dirty
 * and should be fsck'd before its contents are trusted. */
unsigned long long hype_fat_write_rollback_failures(void);
void hype_fat_write_note_rollback_failure(void);

/*
 * Creates the file named by `path` ('\\' or '/' separated; every directory on
 * the way must already exist), truncating it to empty if it already exists
 * (its old cluster chain is freed and its directory entry -- LFN run included
 * -- is kept in place). A name that is not a strict 8.3 short name gets an LFN
 * run over a generated "~N" short name. An empty file has first cluster zero;
 * the first append allocates its data chain. Fills *out ready for append. Returns 0 on
 * success, -1 on any I/O error, an invalid name, a missing parent, or a full
 * volume/directory.
 */
int hype_fat32_create(hype_fat32_fs_t *fs, const char *path, hype_fat32_wfile_t *out);

/*
 * Deletes the file named by `path`: frees its cluster chain, updates FSInfo,
 * and marks its directory entry AND its whole (validated) LFN run 0xE5.
 * Refuses a directory. Returns 0 on success, -1 otherwise.
 */
int hype_fat32_unlink(hype_fat32_fs_t *fs, const char *path);

/*
 * Creates the directory named by `path` (parents must exist; no mkdir -p).
 * The new directory gets one zeroed cluster whose first two entries are '.'
 * and '..' -- with '..' holding the parent's first cluster, or 0 when the
 * parent is the root, per the FAT spec. Returns 0 on success, -1 on error or
 * an existing name of either kind.
 */
int hype_fat32_mkdir(hype_fat32_fs_t *fs, const char *path);

/*
 * Removes the directory named by `path`. It must contain nothing but its own
 * '.' and '..' entries (deleted slots excepted); the root cannot be removed.
 * Returns 0 on success, -1 on error or a non-empty directory.
 */
int hype_fat32_rmdir(hype_fat32_fs_t *fs, const char *path);

/*
 * Renames (and/or moves) `from` to `to`. `to`'s parent must exist, `to` itself
 * must not (rename never replaces), and a directory cannot be moved into
 * itself or a descendant. The entry keeps its attributes, timestamps, size and
 * cluster chain; a moved directory gets its '..' entry re-pointed at the new
 * parent. Because lookup is case-insensitive, a rename that only changes case
 * is refused as "target exists". The new entry is written before the old one
 * is deleted. Returns 0 on success, -1 otherwise.
 */
int hype_fat32_rename(hype_fat32_fs_t *fs, const char *from, const char *to);

/*
 * Appends `len` bytes of `data` to the file, growing the chain as needed, then
 * flushes the updated size to the directory entry and the free-cluster counts
 * to FSInfo. Returns 0 on success, -1 on I/O error or when the volume is full.
 */
int hype_fat32_append(hype_fat32_wfile_t *f, const void *data, unsigned int len);

/*
 * #382: opens the EXISTING file named by `path` for random-position I/O,
 * validating its complete cluster chain against DIR_FileSize before handing
 * out a handle. Refused (-1): a missing path, a directory, an out-of-range or
 * free cluster in the chain, a loop, a chain shorter than the recorded size,
 * or a chain longer than the size justifies (FAT32 has no representation for
 * an internal hole -- every cluster through DIR_FileSize must belong, so a
 * short chain is corruption, never sparseness, and slack whole clusters are
 * what fsck reports as allocation-size mismatch). All writers that mutate one
 * mounted volume must share the same `fs`, exactly as with create().
 */
int hype_fat32_open(hype_fat32_fs_t *fs, const char *path, hype_fat32_wfile_t *out);

/* Reads `len` bytes at byte `offset`. The range must lie wholly inside the
 * file (offset+len <= size, overflow-guarded) -- refused, not clamped. */
int hype_fat32_read_at(hype_fat32_wfile_t *f, uint64_t offset, void *out, unsigned int len);

/*
 * #382: writes `len` bytes at byte `offset`, allocating on demand.
 *
 * Inside the current size this is a pure in-place data write (no metadata).
 * Past it, the file GROWS: every intervening cluster is allocated (FAT32
 * cannot represent a hole), the logical gap [old_size, offset) -- including
 * stale slack in the last already-allocated cluster -- is zeroed, and fresh
 * clusters are zeroed before they are linked into the chain, all BEFORE the
 * larger size is published to the directory entry. Commit order: volume
 * dirty flag, FAT copies (link-by-link), zeroed+written data, barrier,
 * directory size, FSInfo, barrier, dirty flag cleared. On allocation or I/O
 * failure mid-growth the new clusters are freed and the chain terminator
 * restored, so the file is unchanged. Returns 0, -1 on error or full volume.
 */
int hype_fat32_write_at(hype_fat32_wfile_t *f, uint64_t offset, const void *data,
                        unsigned int len);

/*
 * Sets the timestamp stamped into subsequently written directory entries. Pass
 * the result of hype_rtc_read(). Safe to call repeatedly -- a long-lived log
 * should refresh it so its modification time advances.
 */
void hype_fat32_fs_set_time(hype_fat32_fs_t *fs, const hype_rtc_time_t *now);

#endif /* HYPE_CORE_FAT_WRITE_FS_H */
