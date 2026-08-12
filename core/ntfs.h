#ifndef HYPE_CORE_NTFS_H
#define HYPE_CORE_NTFS_H

#include <stdint.h>

#include "blk_io.h"
#include "file_range.h"

/*
 * #337 (STORAGE: host NTFS): resolve a path on an NTFS volume to a #381
 * logical range map, so ISOs and file-backed guest disk images can live on a
 * Windows-formatted drive. Read, plus IN-PLACE write through the resolved
 * map (core/file_range.c's hype_file_rmap_write_at). plan.md §10 decision 30.
 *
 * What maps where: a non-resident $DATA attribute is a runlist of
 * (LCN, length) runs. An allocated run becomes DATA; a sparse run (no LCN
 * stored) becomes HOLE and reads as zeroes; allocated bytes past the
 * stream's initialized size become UNWRITTEN -- reading the media there
 * would leak whatever was in those clusters before.
 *
 * Deliberately refused, per decision 30 -- each is detect-and-refuse, never
 * a degraded read:
 *   - a DIRTY volume ($VOLUME_INFORMATION flag 0x0001): Windows fast startup
 *     and hibernation leave this set routinely, and resolving paths through
 *     metadata a journal replay would change is the ext INCOMPAT_RECOVER
 *     mistake with a different name;
 *   - BitLocker (not NTFS at the volume level -- the "-FVE-FS-" OEM id is
 *     recognised and refused, never probed further);
 *   - compressed (LZNT1) or encrypted $DATA;
 *   - resident $DATA (the bytes live inside the MFT record, not at any media
 *     LBA the range contract can express; no disk image or ISO is resident);
 *   - a fixup (update sequence) mismatch on any MFT record or INDX block --
 *     a torn write, and the classic silent-corruption trap if ignored;
 *   - a name whose case folding needs a code point past the cached $UpCase
 *     prefix (the decision-24 rule: fold exactly as other implementations
 *     do, or not at all);
 *   - any structural inconsistency: bad magics, out-of-heap LCNs, runlist
 *     overflow, attribute-list recursion, oversized records.
 *
 * 512-byte logical sectors only, like the rest of hype's block world. All
 * LBAs are VOLUME-RELATIVE (sector 0 = the boot sector).
 */

#define HYPE_NTFS_UPCASE_CACHE 256u

typedef struct {
    hype_blk_read_fn read;
    void *ctx;
    uint32_t spc;             /* sectors per cluster */
    uint32_t mft_record_size; /* bytes; <= HYPE_NTFS_MAX_RECORD */
    uint64_t total_sectors;
    uint64_t mft_lcn;      /* $MFT's first cluster, from the boot sector */
    hype_file_rmap_t mft;  /* $MFT's own $DATA map: every record is read through this */
    uint16_t upcase[HYPE_NTFS_UPCASE_CACHE];
    int upcase_loaded;
} hype_ntfs_t;

/* Largest MFT record and INDX block hype accepts (the on-disk norm is 1024
 * and 4096 respectively; 4096-byte records exist on 4Kn media, which hype's
 * 512-byte world already refuses at the boot sector). */
#define HYPE_NTFS_MAX_RECORD 4096u

/*
 * Recognition without resolution: NTFS boot-sector shape (OEM id, 512-byte
 * sectors, sane geometry, 0xAA55). Returns 0 (claimed) or -1. A BitLocker
 * "-FVE-FS-" volume is -1: out of scope, permanently. Read-only.
 */
int hype_ntfs_probe(hype_blk_read_fn read, void *ctx);

/*
 * Mounts: boot sector, $MFT record 0 (whose own runlist bootstraps every
 * other record), the $Volume dirty flag (refused when set), and the first
 * HYPE_NTFS_UPCASE_CACHE code points of $UpCase (sanity-checked: ASCII
 * letters must fold correctly, identity below 'a'). Returns 0 or -1.
 */
int hype_ntfs_mount(hype_blk_read_fn read, void *ctx, hype_ntfs_t *out);

/*
 * Resolves `path` ('\\' or '/' separated, case-insensitive via $UpCase) to
 * the file's unnamed $DATA stream as a #381 range map: DATA for allocated
 * runs, HOLE for sparse runs, UNWRITTEN for the allocated tail past the
 * initialized size. Returns 0 on success, -1 on any refusal above, a path
 * that does not resolve, or a directory.
 */
int hype_ntfs_resolve(hype_ntfs_t *fs, const char *path, hype_file_rmap_t *out);

#endif /* HYPE_CORE_NTFS_H */
