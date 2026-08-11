#ifndef HYPE_CORE_FAT_H
#define HYPE_CORE_FAT_H

#include <stdint.h>

#include "blk_io.h" /* hype_blk_read_fn + hype_file_map_t: the shared contract (#292) */

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


/*
 * Resolves `path` (absolute, '\\' or '/' separated, case-insensitive) on a
 * FAT32 volume to *out. Matches long (LFN) names, falling back to 8.3 short
 * names. Returns 0 on success; -1 if the volume is not a supported FAT32
 * volume, the path does not resolve to a regular file, the file needs more than
 * HYPE_FILE_MAX_EXTENTS runs, or a sector read fails. Read-only.
 *
 * #366: on the too-many-extents failure specifically, out->too_fragmented is set to 1 and
 * out->count holds HYPE_FILE_MAX_EXTENTS. The return value is still -1, so existing callers are
 * unaffected -- but one that wants to tell the operator WHY now can.
 */
int hype_fat32_resolve(hype_blk_read_fn read, void *ctx, const char *path, hype_file_map_t *out);

/*
 * As hype_fat32_resolve but for an exFAT volume. Handles both contiguous
 * (NoFatChain) files -- the common case -- and FAT-chained files. Read-only.
 */
int hype_exfat_resolve(hype_blk_read_fn read, void *ctx, const char *path, hype_file_map_t *out);

#endif /* HYPE_CORE_FAT_H */
