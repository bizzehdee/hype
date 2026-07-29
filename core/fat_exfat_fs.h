#ifndef HYPE_CORE_FAT_EXFAT_FS_H
#define HYPE_CORE_FAT_EXFAT_FS_H

#include <stdint.h>
#include "fat.h"          /* hype_fat_read_fn, HYPE_FAT_SECTOR_SIZE */
#include "fat_exfat.h"    /* hype_exfat_upcase_t and the pure primitives */
#include "fat_write_fs.h" /* hype_fat_write_fn (shared with the FAT32 writer) */

/*
 * #198 (STORAGE: writable FAT32/exFAT) -- block-backed exFAT read/write
 * orchestration, the exFAT counterpart of core/fat_write_fs.c. Driven by the
 * same injected read+write sector-callback pair as the read-only reader in
 * core/fat.c, so it is exercised end-to-end against synthetic and real
 * (mkfs.exfat-produced) volumes in the host unit tests with no hardware.
 *
 * What it does:
 *   - mounts a volume: boot sector, active FAT selection, the root directory's
 *     Allocation Bitmap and Up-case Table entries, and the up-case table itself
 *     (checksum-verified against the entry's TableChecksum);
 *   - resolves an existing path (subdirectories included) to a writable handle;
 *   - creates-or-truncates a file in the root directory;
 *   - overwrites bytes in place inside an existing file -- the ticket's primary
 *     case, a pre-allocated raw disk image whose clusters never move;
 *   - appends bytes, growing the chain through the allocation bitmap + FAT and
 *     rewriting the directory entry set (with a fresh set checksum) as it goes;
 *   - flushes the volume: clears VolumeDirty and refreshes PercentInUse.
 *
 * #246 added the directory-manipulation half #198 scoped out: create a
 * directory, remove an (empty) directory, delete a file, and rename/move an
 * entry -- and generalised create to an arbitrary parent directory, growing
 * that parent (and keeping its DataLength in its own parent's entry set
 * honest) when the new entry set does not fit.
 *
 * Growth always produces a FAT-chained (NoFatChain == 0) allocation, and a file
 * (or directory) that was contiguous gets its FAT chain materialised the first
 * time it grows, so a fragmented volume is handled correctly rather than
 * corrupted.
 *
 * Two mount-time restrictions are worth knowing about, both `plan.md` §10
 * decision #24: a volume whose allocation bitmap is not physically contiguous is
 * refused (no formatter produces one, and the bitmap is indexed by plain sector
 * arithmetic), and so is one whose up-case table fails its own TableChecksum
 * (hype must fold names exactly the way other exFAT implementations do, or not
 * at all).
 *
 * All LBAs are VOLUME-RELATIVE (sector 0 == the main boot sector); a partitioned
 * medium wraps the callbacks so they add the partition's first LBA.
 *
 * NOTE (freestanding, no libc): hype_exfat_fs_t contains arrays, so it must
 * never be assigned or passed by value -- that emits a memcpy call which does
 * not exist at EFI link time. Pass a pointer.
 */

/* Bit 1 of VolumeFlags. Set while the volume has un-flushed changes; both it and
 * PercentInUse sit at boot-sector offsets the boot-region checksum deliberately
 * excludes, so they can be rewritten without recomputing that checksum. */
#define HYPE_EXFAT_VOLUME_DIRTY 0x0002u

/* Above this many bitmap sectors hype does not scan the bitmap at mount, so the
 * used-cluster total stays unknown and PercentInUse is left alone on flush.
 * 4096 sectors == 2 MiB of bitmap == 16M clusters, far beyond any medium hype
 * writes to today; the cap exists so mount cost stays bounded. */
#define HYPE_EXFAT_MAX_BITMAP_SCAN 4096u

typedef struct {
    hype_fat_read_fn read;
    hype_fat_write_fn write;
    void *ctx;
    uint64_t volume_length; /* total sectors, from the boot sector */
    uint32_t fat_lba;       /* first sector of the ACTIVE FAT */
    uint32_t fat_length;    /* sectors per FAT */
    uint32_t heap_lba;      /* first sector of the cluster heap (cluster 2) */
    uint32_t cluster_count; /* clusters in the heap; valid clusters are 2..count+1 */
    uint32_t root_cluster;
    uint32_t spc;             /* sectors per cluster */
    uint64_t bitmap_lba;      /* first sector of the allocation bitmap */
    uint64_t bitmap_bytes;    /* the bitmap's DataLength */
    uint32_t upcase_cluster;  /* first cluster of the up-case table */
    uint64_t upcase_bytes;    /* the up-case table's DataLength */
    uint32_t next_free;       /* allocation search hint */
    uint32_t used_clusters;   /* allocated clusters, or HYPE_EXFAT_USED_UNKNOWN */
    uint8_t dirty;            /* 1 == VolumeDirty has been set on the medium */
    hype_exfat_upcase_t upcase;
    /* Wall-clock snapshot for directory entries; zeroed (invalid) by mount so
     * the 1980 epoch is used until hype_exfat_fs_set_time() supplies one. */
    hype_rtc_time_t now;
} hype_exfat_fs_t;

#define HYPE_EXFAT_USED_UNKNOWN 0xFFFFFFFFu

typedef struct {
    hype_exfat_fs_t *fs;
    uint32_t dir_cluster;   /* first cluster of the directory holding the entry set */
    uint8_t dir_contiguous; /* 1 == that directory is NoFatChain */
    uint32_t set_index;     /* directory-entry index of the File (0x85) entry */
    uint8_t secondary;      /* the File entry's SecondaryCount */
    uint32_t first_cluster; /* first cluster of the data chain (0 == no allocation) */
    uint32_t tail_cluster;  /* last cluster of the chain, 0 == not resolved yet */
    uint64_t size;          /* DataLength in bytes */
    uint8_t contiguous;     /* 1 == the data stream is NoFatChain */
    uint8_t is_dir;         /* 1 == the entry names a directory */
    /* Seek cache: `seek_cluster` is the cluster at chain index `seek_index`, so
     * sequential access does not re-walk the chain from the start each call. */
    uint32_t seek_index;
    uint32_t seek_cluster;
} hype_exfat_wfile_t;

/*
 * Parses the boot sector and the root directory's Allocation Bitmap / Up-case
 * Table entries into *out, loading and checksum-verifying the up-case table.
 * `write` may be NULL for a read-only mount (every mutating call then fails).
 * Returns 0 on success, -1 if the volume is not a supported 512-byte-sector
 * exFAT volume, its critical structures are missing or inconsistent, or a read
 * fails.
 */
int hype_exfat_fs_mount(hype_fat_read_fn read, hype_fat_write_fn write, void *ctx,
                        hype_exfat_fs_t *out);

/*
 * Resolves `path` ('\\' or '/' separated, case-insensitive via the volume's
 * up-case table) to a writable handle. Returns 0 on success, -1 if the path does
 * not resolve, names a directory when `want_dir` is 0 (or a file when it is 1),
 * or an entry set fails its checksum.
 */
int hype_exfat_lookup(hype_exfat_fs_t *fs, const char *path, int want_dir,
                      hype_exfat_wfile_t *out);

/*
 * Creates the file named by `path` ('\\' or '/' separated; every directory on
 * the way must already exist), or truncates it to empty if it already exists
 * (freeing its old chain). No data cluster is allocated until the first append.
 * Returns 0 on success, -1 on I/O error, an invalid/unsupported name, a
 * missing parent directory, or a full volume/directory.
 */
int hype_exfat_create(hype_exfat_fs_t *fs, const char *path, hype_exfat_wfile_t *out);

/*
 * Deletes the file named by `path`: frees its allocation (FAT chain and bitmap
 * bits) and retires its whole entry set. Refuses a directory. Returns 0 on
 * success, -1 on I/O error or if the path does not name a file.
 */
int hype_exfat_unlink(hype_exfat_fs_t *fs, const char *path);

/*
 * Creates the directory named by `path` (every directory on the way must
 * already exist; no mkdir -p). The new directory gets one zeroed cluster --
 * exFAT directories have no '.'/'..' entries -- and a DataLength of exactly
 * that cluster. Refuses an existing name of either kind. Returns 0 on success,
 * -1 on error.
 */
int hype_exfat_mkdir(hype_exfat_fs_t *fs, const char *path);

/*
 * Removes the directory named by `path`. The directory must be empty (no
 * in-use entry of any type); the root directory cannot be removed. Frees its
 * allocation and retires its entry set. Returns 0 on success, -1 on error or a
 * non-empty directory.
 */
int hype_exfat_rmdir(hype_exfat_fs_t *fs, const char *path);

/*
 * Renames (and/or moves) `from` to `to`. `to`'s parent directory must exist,
 * `to` itself must not (rename never replaces), and a directory cannot be
 * moved into itself or a descendant of itself. Because lookup is
 * case-insensitive, a rename that only changes a name's case is refused as
 * "target exists". The entry keeps its attributes, timestamps and allocation;
 * only its name (and, when moving, its directory) change. The new entry set is
 * written before the old one is retired, so an interruption leaves the entry
 * findable under at least one name. Returns 0 on success, -1 on error.
 */
int hype_exfat_rename(hype_exfat_fs_t *fs, const char *from, const char *to);

/*
 * Overwrites `len` bytes at byte `offset` of the file. The range must lie wholly
 * within the file's current size -- this is the pre-allocated-backing-file path
 * and never grows or moves an allocation. Returns 0 on success, -1 on I/O error
 * or an out-of-range range.
 */
int hype_exfat_write_at(hype_exfat_wfile_t *f, uint64_t offset, const void *data,
                        unsigned int len);

/* Reads `len` bytes from byte `offset` of the file. Bounds-checked as above. */
int hype_exfat_read_at(hype_exfat_wfile_t *f, uint64_t offset, void *out, unsigned int len);

/*
 * Appends `len` bytes to the file, growing the allocation as needed and
 * rewriting the directory entry set's DataLength/ValidDataLength/first cluster
 * plus its set checksum. Returns 0 on success, -1 on I/O error or a full volume.
 */
int hype_exfat_append(hype_exfat_wfile_t *f, const void *data, unsigned int len);

/*
 * Clears VolumeDirty and refreshes PercentInUse in the main (and, when present,
 * the backup) boot sector. Call once writing is finished. Returns 0 on success,
 * -1 on I/O error.
 */
int hype_exfat_fs_sync(hype_exfat_fs_t *fs);

/* Sets the timestamp stamped into subsequently written entry sets. */
void hype_exfat_fs_set_time(hype_exfat_fs_t *fs, const hype_rtc_time_t *now);

#endif /* HYPE_CORE_FAT_EXFAT_FS_H */