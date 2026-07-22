#ifndef HYPE_CORE_FAT_H
#define HYPE_CORE_FAT_H

#include <stdint.h>

/*
 * #181 (STORAGE: host FAT32/exFAT reader): a minimal, read-only filesystem
 * reader that resolves an absolute path (e.g. "\iso\test.iso") to the file's
 * on-disk extents -- the (volume-relative LBA, sector count) runs its data
 * occupies. GLADDER-11 (#182) then streams an installer ISO that lives as a
 * FILE on the FAT32/exFAT ESP (the natural "copy hype.efi + an ISO onto a
 * stick" layout) by mapping a logical ISO offset to a disk LBA through these
 * extents, instead of requiring the ISO on its own raw partition.
 *
 * Pure logic driven by an injected volume-relative sector-read callback (the
 * core/gpt.c / core/iso_stream.c dependency-injection pattern), unit-tested
 * against synthetic volumes. Read-only; never writes. 512-byte logical sectors
 * only (matches the rest of hype's block world; a non-512 BytesPerSector volume
 * is rejected rather than mis-parsed).
 *
 * Extents are VOLUME-RELATIVE (sector 0 = the volume's boot sector); the caller
 * adds the partition's first LBA (from core/gpt.c) to get disk-absolute LBAs.
 */

#define HYPE_FAT_SECTOR_SIZE 512u

/* A single contiguous run of the file's data on the volume. A contiguous file
 * is one extent; fragmentation adds more, capped by HYPE_FAT_MAX_EXTENTS. */
typedef struct {
    uint64_t start_lba;    /* volume-relative first LBA of this run */
    uint64_t sector_count; /* length of the run, in 512-byte sectors */
} hype_fat_extent_t;

#define HYPE_FAT_MAX_EXTENTS 64u

typedef struct {
    hype_fat_extent_t extents[HYPE_FAT_MAX_EXTENTS];
    unsigned count;      /* number of extents used */
    uint64_t size_bytes; /* exact file length in bytes */
} hype_fat_file_t;

/*
 * Reads `count` 512-byte sectors starting at volume-relative `lba` into `dst`.
 * Returns 0 on success, non-zero on error. `ctx` carries whatever the backend
 * needs (e.g. ABAR+port+partition base for hype_ahci_host_read()).
 */
typedef int (*hype_fat_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *dst);

/*
 * Resolves `path` (absolute, '\\' or '/' separated, case-insensitive) on a
 * FAT32 volume to *out. Matches long (LFN) names, falling back to 8.3 short
 * names. Returns 0 on success; -1 if the volume is not a supported FAT32
 * volume, the path does not resolve to a regular file, the file needs more than
 * HYPE_FAT_MAX_EXTENTS runs, or a sector read fails. Read-only.
 */
int hype_fat32_resolve(hype_fat_read_fn read, void *ctx, const char *path, hype_fat_file_t *out);

/*
 * As hype_fat32_resolve but for an exFAT volume. Handles both contiguous
 * (NoFatChain) files -- the common case -- and FAT-chained files. Read-only.
 */
int hype_exfat_resolve(hype_fat_read_fn read, void *ctx, const char *path, hype_fat_file_t *out);

#endif /* HYPE_CORE_FAT_H */
