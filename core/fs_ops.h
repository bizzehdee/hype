#ifndef HYPE_CORE_FS_OPS_H
#define HYPE_CORE_FS_OPS_H

#include <stdint.h>

#include "blk_io.h"
#include "file_range.h"
#include "fat_write_fs.h"  /* hype_fat32_fs_t / hype_fat32_wfile_t */
#include "fat_exfat_fs.h"  /* hype_exfat_fs_t / hype_exfat_wfile_t */
#include "ext.h"           /* hype_ext_wfile_t */
#include "ntfs.h"          /* hype_ntfs_t (#337) */
#include "rtc.h"

/*
 * #293 (STORAGE: one common filesystem interface): every host-FS driver --
 * FAT32, exFAT, ext, ISO9660 -- behind ONE vtable, the same pattern
 * hype_blk_backend_t established and for the same reason: the caller should
 * not know which driver it has. `probe` is what makes drivers genuinely
 * interchangeable -- hand the volume to each registered driver and let the one
 * that recognises it claim it, instead of the caller hardcoding the family.
 *
 * Capability honesty (the ticket's hard rule): the four drivers are NOT
 * equally capable, and this interface exposes that rather than papering over
 * it. A driver that cannot do an operation has a NULL slot and a missing caps
 * bit -- never a stub returning fake success. The wrappers below turn a NULL
 * slot into a clean -1 so no caller ever dereferences one.
 *
 * Capabilities as of #293:
 *   ISO9660  read-only, streaming; lookup resolves only the whole image.
 *   FAT32    read + create/append + namespace + random write_at (#382):
 *            in-place inside the size, allocate-and-zero-fill growth past it.
 *   exFAT    read + full mutation + IN-PLACE write_at (never grows/moves).
 *   ext      sparse-aware read (#384: holes/unwritten read as zeros);
 *            write_at in place, plus HOLE-FILLING allocation on ext2
 *            volumes (journaled allocation is #385); no namespace mutation.
 *   NTFS     read (sparse runs read as zeroes) + IN-PLACE write_at into
 *            DATA ranges only; a write into a HOLE or UNWRITTEN range is
 *            refused (#337, plan.md §10 decision 30). No mutation beyond
 *            that, permanently.
 *
 * File mapping and reads speak the #381 logical range contract
 * (hype_file_rmap_t): DATA / HOLE / UNWRITTEN, zeros synthesized by the
 * common layer. FAT32 and exFAT can only ever emit DATA ranges -- a chain
 * that cannot cover the file size is a hard error there, never a hole.
 *
 * Freestanding note: hype_fs_t and hype_fs_file_t contain arrays; never
 * assign or pass them by value (that emits a memcpy that does not exist at
 * EFI link time). Pass pointers.
 */

/* Driver capability bits (hype_fs_ops_t.caps). What a MOUNTED WRITABLE volume
 * could do; a read-only mount (NULL write callback) refuses mutation at call
 * time regardless of these. */
#define HYPE_FS_CAP_READ 0x01u          /* lookup/map_ranges/read_at */
#define HYPE_FS_CAP_WRITE_INPLACE 0x02u /* write_at inside the current size */
#define HYPE_FS_CAP_APPEND 0x04u        /* append, growing the allocation */
#define HYPE_FS_CAP_NAMESPACE 0x08u     /* create/unlink/mkdir/rmdir/rename */
#define HYPE_FS_CAP_WRITE_GROW 0x10u    /* write_at past EOF allocates (FAT32 via #382) */
#define HYPE_FS_CAP_SPARSE 0x20u        /* may emit HOLE/UNWRITTEN ranges (NTFS #337, ext #384) */

typedef struct hype_fs hype_fs_t;
typedef struct hype_fs_file hype_fs_file_t;

typedef struct hype_fs_ops {
    const char *name; /* "fat32", "exfat", "ext", "iso9660" */
    unsigned caps;    /* HYPE_FS_CAP_* the driver implements */

    /* Cheap recognition: does this volume belong to this driver? Must not
     * mutate anything. 0 == claimed, -1 == not mine. */
    int (*probe)(hype_blk_read_fn read, void *ctx);

    /* `write` NULL == read-only mount; every mutating call then fails. */
    int (*mount)(hype_fs_t *fs, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx);

    /* Resolve an existing file to a handle for read_at/write_at/append. */
    int (*lookup)(hype_fs_t *fs, const char *path, hype_fs_file_t *out);

    /* Resolve an existing file to its #381 logical range map. */
    int (*map_ranges)(hype_fs_t *fs, const char *path, hype_file_rmap_t *out);

    int (*read_at)(hype_fs_file_t *f, uint64_t offset, void *dst, unsigned int len);

    /* In-place only until a driver sets HYPE_FS_CAP_WRITE_GROW: the range must
     * lie inside the file's current size; refused, never grown, otherwise. */
    int (*write_at)(hype_fs_file_t *f, uint64_t offset, const void *src, unsigned int len);

    /* Append to the file, growing its allocation. */
    int (*append)(hype_fs_file_t *f, const void *src, unsigned int len);

    /* Namespace mutation. NULL on drivers without HYPE_FS_CAP_NAMESPACE. */
    int (*create)(hype_fs_t *fs, const char *path, hype_fs_file_t *out);
    int (*unlink)(hype_fs_t *fs, const char *path);
    int (*mkdir)(hype_fs_t *fs, const char *path);
    int (*rmdir)(hype_fs_t *fs, const char *path);
    int (*rename)(hype_fs_t *fs, const char *from, const char *to);

    /* Optional: persistence barrier / dirty-flag retirement; timestamp source
     * for new directory entries. */
    int (*sync)(hype_fs_t *fs);
    void (*set_time)(hype_fs_t *fs, const hype_rtc_time_t *now);

    /* Optional: install a per-transfer durability barrier the driver invokes
     * around metadata commits (FAT32's set_sync). NULL on drivers whose
     * writers order their own commits. */
    void (*set_barrier)(hype_fs_t *fs, hype_blk_sync_fn sync);
} hype_fs_ops_t;

/* A mounted volume: the claiming driver plus its per-mount state. */
struct hype_fs {
    const hype_fs_ops_t *ops;
    hype_blk_read_fn read;
    hype_blk_write_fn write; /* NULL == read-only mount */
    void *ctx;
    union {
        hype_fat32_fs_t fat32;
        hype_exfat_fs_t exfat;
        hype_ntfs_t ntfs;
        struct {
            uint64_t size_bytes; /* whole image, from the PVD */
        } iso;
        /* ext keeps no mount state: its resolver revalidates the superblock
         * per call, which also re-checks the clean-unmount gate. */
    } u;
};

/* An open file. Which union arm is live is the driver's business; callers
 * only ever pass the handle back to the same mounted fs. */
struct hype_fs_file {
    hype_fs_t *fs;
    uint64_t size; /* file size in bytes at open time */
    unsigned tag;  /* driver-private discriminator for the union arm */
    union {
        hype_fat32_wfile_t fat32; /* FAT32 create/append handle */
        hype_exfat_wfile_t exfat;
        hype_ext_wfile_t ext;     /* ext in-place read/write handle */
        hype_ext2_wfile_t ext2;   /* #384 allocating ext2 writer handle */
        hype_file_rmap_t rmap;    /* generic read-only handle (#381 contract) */
    } u;
};

/*
 * The registry, in probe order. exFAT before FAT32 (an exFAT boot sector
 * carries a FAT-shaped BPB region of zeros, so FAT32's stricter parse cannot
 * claim it -- but specific-first ordering keeps that a non-issue), ISO9660
 * first (its anchor lives at a fixed offset no FAT/ext structure occupies).
 */
const hype_fs_ops_t *const *hype_fs_registry(unsigned *count);

/*
 * Probe every registered driver against the volume and mount with the one
 * that claims it. `write` NULL == read-only. Returns 0 and fills *fs, or -1
 * if no driver claims the volume (or the claimer then fails to mount).
 */
int hype_fs_mount_auto(hype_fs_t *fs, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx);

/*
 * NULL-safe wrappers. Each returns -1 when the mounted driver lacks the
 * operation (NULL slot), the fs/file is not mounted/open, or -- for mutating
 * calls -- the mount is read-only. Otherwise they dispatch to the driver.
 */
int hype_fs_lookup(hype_fs_t *fs, const char *path, hype_fs_file_t *out);
int hype_fs_map_ranges(hype_fs_t *fs, const char *path, hype_file_rmap_t *out);
int hype_fs_read_at(hype_fs_file_t *f, uint64_t offset, void *dst, unsigned int len);
int hype_fs_write_at(hype_fs_file_t *f, uint64_t offset, const void *src, unsigned int len);
int hype_fs_append(hype_fs_file_t *f, const void *src, unsigned int len);
int hype_fs_create(hype_fs_t *fs, const char *path, hype_fs_file_t *out);
int hype_fs_unlink(hype_fs_t *fs, const char *path);
int hype_fs_mkdir(hype_fs_t *fs, const char *path);
int hype_fs_rmdir(hype_fs_t *fs, const char *path);
int hype_fs_rename(hype_fs_t *fs, const char *from, const char *to);
int hype_fs_sync(hype_fs_t *fs);
void hype_fs_set_time(hype_fs_t *fs, const hype_rtc_time_t *now);

/* Install a durability barrier where the driver supports one; a driver whose
 * writer already orders its own commits silently has none to install. */
void hype_fs_set_barrier(hype_fs_t *fs, hype_blk_sync_fn sync);

/* 1 when a FAT32 append was refused by the #377 chain-identity guard (the
 * writer detected another file's cluster about to be published); 0 for every
 * other driver, handle shape, or error. Diagnostic, not control flow. */
int hype_fs_file_identity_error(const hype_fs_file_t *f);

/* The mounted driver's capability bits (0 if fs is not mounted). A read-only
 * mount masks off every mutating capability, so callers can gate on what THIS
 * mount can actually do rather than on what the driver could do. */
unsigned hype_fs_caps(const hype_fs_t *fs);

#endif /* HYPE_CORE_FS_OPS_H */
