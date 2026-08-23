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
    /* #417: $Bitmap (record 6), loaded lazily on first hype_ntfs_cluster_alloc/free()
     * call -- never at mount, so a read-only mount/resolve never depends on it. */
    hype_file_rmap_t bitmap;
    uint64_t total_clusters;
    int bitmap_loaded;
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

/*
 * #416: write-side record access, for core/ntfs_journal.c and later NTFS
 * writer slices (#417+). See core/ntfs.c for the full contract of each.
 */
int hype_ntfs_record_read(hype_ntfs_t *fs, uint64_t n, uint8_t *rec);
void hype_ntfs_fixup_stamp(uint8_t *rec, uint32_t rec_bytes, uint16_t usn);
int hype_ntfs_record_write(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t n, uint8_t *rec,
                           uint16_t usn);
int hype_ntfs_volume_dirty_get(hype_ntfs_t *fs);
int hype_ntfs_volume_dirty_set(hype_ntfs_t *fs, hype_blk_write_fn write, int dirty, uint16_t usn);

/*
 * #417: $Bitmap (record 6) cluster allocation and release. $Bitmap has one
 * bit per cluster, LSB-first within each byte (bit 0 of byte 0 == cluster
 * 0), 1 == allocated. Loaded lazily on first call and cached for the life
 * of the mount, so a read-only mount/resolve never needs a well-formed
 * $Bitmap (matches every other write-side #416+ primitive).
 *
 * hype_ntfs_cluster_alloc() first-fits: the first free run of `count`
 * contiguous clusters, scanning from cluster 0. NTFS does not mandate an
 * allocation policy (chkdsk validates the bitmap's CONTENTS, never how a
 * writer chose where to allocate), so first-fit is a complete, correct
 * choice, not a placeholder for something fancier.
 *
 * hype_ntfs_cluster_free() refuses (-1) unless every bit in [lcn, lcn+count)
 * is currently SET: freeing a run that is not fully allocated is a caller
 * bug or a sign the bitmap is already inconsistent, and clearing it anyway
 * would silently paper over either.
 *
 * Neither function is crash-safety-complete on its own: they only ever
 * touch $Bitmap's own bytes. A caller that allocates/frees clusters as part
 * of a larger mutation (linking a new run into a runlist, updating a
 * record's allocated size, ...) MUST wrap the whole operation in one
 * hype_ntfs_txn_open()/hype_ntfs_txn_close() bracket (core/ntfs_journal.h)
 * so an interrupted write leaves the volume marked dirty rather than
 * silently inconsistent (plan.md §10 decision 64).
 */
int hype_ntfs_cluster_alloc(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t count,
                            uint64_t *out_lcn);
int hype_ntfs_cluster_free(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t lcn, uint64_t count);

/*
 * #418: append ONE new run of `cluster_count` contiguous clusters starting
 * at `lcn` (already allocated by the caller, typically via
 * hype_ntfs_cluster_alloc()) to MFT record `rec_no`'s unnamed, non-resident
 * $DATA attribute's mapping pairs, and set the attribute's allocated/real/
 * initialized sizes to `new_alloc_size`/`new_real_size`/`new_init_size`
 * (bytes). Writes the record back via hype_ntfs_record_write() (fixups +
 * $MFTMirr, same as every other write-side primitive).
 *
 * Refused, permanently out of scope for this slice (a caller needing any of
 * these must fall back to #422/a future ticket, never silently degraded
 * here):
 *   - resident $DATA (that is #422's resident-to-non-resident conversion);
 *   - an $ATTRIBUTE_LIST already present in the record (a $DATA stream
 *     split across multiple MFT records) -- growing that needs the list
 *     itself maintained, which this function does not do;
 *   - more than one unnamed $DATA piece already inside this one record;
 *   - the growth would not fit in the record's allocated size (no
 *     $ATTRIBUTE_LIST is created to spill into an extension record).
 *
 * Crash safety is the caller's job, same as hype_ntfs_cluster_alloc(): wrap
 * the allocate-then-append pair (and any $Bitmap release on a failure path)
 * in one hype_ntfs_txn_open()/close() bracket.
 */
int hype_ntfs_data_append(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no, uint64_t lcn,
                          uint64_t cluster_count, uint64_t new_alloc_size, uint64_t new_real_size,
                          uint64_t new_init_size, uint16_t usn);

/*
 * #419: materialize part or all of a sparse (HOLE) run inside an unnamed,
 * non-resident $DATA attribute. [fill_start_vcn, fill_start_vcn+cluster_count)
 * must lie ENTIRELY within one existing HOLE run (never spanning two runs,
 * never touching an already-allocated run) -- the caller already allocated
 * `cluster_count` contiguous clusters at `lcn` (typically via
 * hype_ntfs_cluster_alloc()). Splits the hole at the fill boundaries as
 * needed (0, 1, or 2 remaining HOLE pieces), zero-fills the new clusters on
 * the medium BEFORE committing the runlist change (a crash before the
 * commit leaves the old, still-valid HOLE state; one after leaves the new,
 * fully-committed state -- never a readable stale byte), advances
 * AllocatedSize by the newly-backed bytes, and clears the attribute's
 * SPARSE flag if this was the last HOLE run. DataSize/InitializedSize are
 * untouched: filling a hole does not change the file's logical length.
 *
 * Same refusals as hype_ntfs_data_append() (resident, $ATTRIBUTE_LIST
 * present, a second unnamed $DATA piece, compressed/encrypted), plus:
 * the target range not fully inside one HOLE run, and more runs following
 * the split than this function's internal cap can re-encode (reported the
 * same way as HYPE_FILE_MAX_RANGES: a real refusal, not silent truncation).
 */
int hype_ntfs_hole_fill(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no,
                        uint64_t fill_start_vcn, uint64_t cluster_count, uint64_t lcn,
                        uint16_t usn);

#endif /* HYPE_CORE_NTFS_H */
