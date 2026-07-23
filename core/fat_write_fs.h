#ifndef HYPE_CORE_FAT_WRITE_FS_H
#define HYPE_CORE_FAT_WRITE_FS_H

#include <stdint.h>
#include "fat.h" /* hype_fat_read_fn, HYPE_FAT_SECTOR_SIZE */

/*
 * #198 pt2 (STORAGE: writable FAT32) -- block-backed write orchestration on top
 * of the pt1 pure primitives (core/fat_write.c). Mounts a FAT32 volume through
 * an injected read+write sector-callback pair (the same dependency-injection
 * pattern as the read-only reader in core/fat.c), then supports creating (or
 * truncating) a file in the root directory and appending bytes to it -- growing
 * the cluster chain, updating every FAT copy, the directory entry's size/first
 * cluster, and the FSInfo free-cluster accounting as it goes.
 *
 * The intended consumer is #230: streaming hype's post-EBS debug log to a file
 * on the USB stick's FAT32 ESP. Root-directory, 8.3-short-name files only (no
 * LFN generation, no subdirectory creation) -- sufficient for a fixed log path.
 * 512-byte logical sectors; little-endian on disk.
 *
 * All LBAs are VOLUME-RELATIVE (sector 0 = the boot sector); a partitioned
 * medium wraps read/write so the callback adds the partition's first LBA.
 */

/* Writes `count` 512-byte sectors from `src` at volume-relative `lba`. Returns
 * 0 on success, non-zero on error. Mirror of hype_fat_read_fn. */
typedef int (*hype_fat_write_fn)(void *ctx, uint64_t lba, uint32_t count, const void *src);

typedef struct {
    hype_fat_read_fn read;
    hype_fat_write_fn write;
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
} hype_fat32_fs_t;

typedef struct {
    hype_fat32_fs_t *fs;
    uint8_t name11[11];       /* 8.3 name, for dirent rewrites on flush */
    uint32_t first_cluster;   /* first cluster of the file's data chain */
    uint32_t tail_cluster;    /* last cluster of the chain (append cursor) */
    uint64_t size;            /* current file size in bytes */
    uint64_t dirent_lba;      /* volume-relative LBA of the sector holding the dirent */
    unsigned int dirent_off;  /* byte offset of the 32-byte dirent within that sector */
} hype_fat32_wfile_t;

/*
 * Parses the BPB (+ FSInfo) via `read` and fills *out. Returns 0 on success; -1
 * if the volume is not a 512-byte-sector FAT32 volume.
 */
int hype_fat32_fs_mount(hype_fat_read_fn read, hype_fat_write_fn write, void *ctx,
                        hype_fat32_fs_t *out);

/*
 * Creates `name` (8.3) in the root directory, truncating it to empty if it
 * already exists (its old cluster chain is freed). Allocates one initial data
 * cluster and writes the directory entry. Fills *out ready for append. Returns
 * 0 on success, -1 on any I/O error or if the volume/root directory is full.
 */
int hype_fat32_create(hype_fat32_fs_t *fs, const char *name, hype_fat32_wfile_t *out);

/*
 * Appends `len` bytes of `data` to the file, growing the chain as needed, then
 * flushes the updated size to the directory entry and the free-cluster counts
 * to FSInfo. Returns 0 on success, -1 on I/O error or when the volume is full.
 */
int hype_fat32_append(hype_fat32_wfile_t *f, const void *data, unsigned int len);

#endif /* HYPE_CORE_FAT_WRITE_FS_H */
