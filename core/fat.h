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
    /*
     * #366: set when resolution stopped because the file needs MORE than HYPE_FAT_MAX_EXTENTS
     * runs, as opposed to any other failure.
     *
     * Every failure used to collapse into -1, so "this ISO is too fragmented for hype to map" was
     * indistinguishable from "no such file" and "this is not a FAT32 volume". boot/main.c even
     * had a diagnostic written for the fragmentation case -- and it was unreachable, because it
     * sat behind `else if (have_file)` and have_file only gets set when resolve SUCCEEDS.
     *
     * The operator's complaint is the point: whether an ISO streams should not depend on how
     * their stick happens to be laid out. It does today, and until this flag existed it did so
     * without saying why.
     */
    int too_fragmented;
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
 *
 * #366: on the too-many-extents failure specifically, out->too_fragmented is set to 1 and
 * out->count holds HYPE_FAT_MAX_EXTENTS. The return value is still -1, so existing callers are
 * unaffected -- but one that wants to tell the operator WHY now can.
 */
int hype_fat32_resolve(hype_fat_read_fn read, void *ctx, const char *path, hype_fat_file_t *out);

/*
 * As hype_fat32_resolve but for an exFAT volume. Handles both contiguous
 * (NoFatChain) files -- the common case -- and FAT-chained files. Read-only.
 */
int hype_exfat_resolve(hype_fat_read_fn read, void *ctx, const char *path, hype_fat_file_t *out);

#endif /* HYPE_CORE_FAT_H */
