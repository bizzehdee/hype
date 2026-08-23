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

/*
 * #420: $MFT record allocation and release, over $MFT's OWN $BITMAP
 * attribute (record 0, unnamed $BITMAP -- one bit per MFT record, tracking
 * which records are in use; distinct from #417's $Bitmap file, which
 * tracks CLUSTERS). $MFTMirr consistency and fixups are already handled by
 * every hype_ntfs_record_write() call (#416) -- this slice adds only the
 * record-slot bookkeeping create/unlink/mkdir/rmdir (#423/#425) need.
 *
 * hype_ntfs_mft_record_alloc() scans strictly within the bitmap's
 * CURRENTLY-INITIALIZED region (its real/init size) for a clear bit,
 * cross-checks the corresponding $MFT record is genuinely not in use
 * on-disk (a bitmap/record disagreement is refused, not trusted either
 * way), initializes a fresh record (FILE magic, fixups sized for
 * fs->mft_record_size, an empty attribute list, sequence number bumped
 * from whatever was last stored there so stale references become
 * detectable), marks the bit used, and writes it out. Returns the new
 * record number and its sequence number.
 *
 * Deliberately out of scope for this slice, refused rather than silently
 * degraded: growing $MFT itself (or its $BITMAP) when the initialized
 * region is fully packed -- that needs the cluster allocator plus
 * hype_ntfs_data_append() chained through $MFT's own $DATA and $BITMAP,
 * a real but rarer path than the common case of allocating into an
 * already-initialized, partially-used $MFT.
 *
 * hype_ntfs_mft_record_free() clears MFT_IN_USE, bumps the sequence
 * number again (so a reference minted before the free is stale even if
 * the slot is reused before anyone notices), and clears the bit.
 */
int hype_ntfs_mft_record_alloc(hype_ntfs_t *fs, hype_blk_write_fn write, int is_dir,
                               uint64_t *out_rec_no, uint16_t *out_seq, uint16_t usn);
int hype_ntfs_mft_record_free(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no,
                              uint16_t usn);

/*
 * #421: $I30 directory index insert/delete -- RESIDENT $INDEX_ROOT only.
 * hype's own read-side lookup (dir_lookup(), core/ntfs.c) already scans
 * $INDEX_ROOT plus every $INDEX_ALLOCATION block LINEARLY rather than
 * descending a B+tree -- the same simplification this write-side slice
 * makes: a directory small enough to keep its whole index resident in
 * $INDEX_ROOT (no $INDEX_ALLOCATION) is maintained as one sorted array,
 * with no node split/merge machinery at all. This covers the common case
 * every create/mkdir into a modest directory needs.
 *
 * Deliberately out of scope for this slice, refused rather than silently
 * degraded: a directory that already has (or would need to grow into) an
 * $INDEX_ALLOCATION B+tree -- real node split/merge, INDX block
 * allocation, and index-bitmap maintenance are a materially bigger, later
 * ticket, not a partial implementation of this one.
 *
 * Both maintain sorted order via $UpCase collation (case-insensitive,
 * shorter-is-less on a common prefix, raw-byte tiebreak so two names that
 * only differ by case still get a deterministic total order) -- required
 * for chkdsk's own index-order validation, not just for correctness of
 * hype's own lookup.
 *
 * hype_ntfs_index_insert() refuses a duplicate name (case-insensitively).
 * `name`/`name_len` is ASCII, the same convention hype_ntfs_resolve() and
 * every other name-taking function in this module already use (decision
 * 24: fold exactly through the verified $UpCase prefix, or not at all --
 * a byte string is always inside that prefix by construction). Stored in
 * the WIN32 namespace, matching how dir_lookup() already interprets it.
 * `is_dir` sets FILE_ATTR_I30_INDEX (0x10000000) so this slice's own
 * dir_lookup treats the new entry as a directory when it should.
 *
 * hype_ntfs_index_delete() removes the first entry (any namespace) whose
 * name matches, refusing if none does.
 */
int hype_ntfs_index_insert(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t dir_rec,
                           uint64_t mft_ref, const char *name, uint32_t name_len, int is_dir,
                           uint16_t usn);
int hype_ntfs_index_delete(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t dir_rec,
                           const char *name, uint32_t name_len, uint16_t usn);

/*
 * #422: converts an unnamed, resident $DATA attribute to non-resident, for
 * a stream growing past what fits inside its MFT record. Allocates
 * `ceil(new_size / cluster_bytes)` clusters via the #417 allocator, writes
 * the existing resident bytes (zero-padded to new_size, then zero-padded
 * again through the rest of the allocation) to the medium BEFORE replacing
 * the attribute -- a crash before the replace leaves the old, still-valid
 * resident attribute; one after leaves the new, fully-committed
 * non-resident one; never a half-converted record. `new_size` must be >=
 * the current resident length (this is a GROWTH path, not a truncation --
 * #422 does not shrink).
 *
 * Same refusals as #418/#419/#421 (non-resident already, named-only,
 * duplicate unnamed $DATA, $ATTRIBUTE_LIST present, compressed/encrypted),
 * plus: new_size shorter than the current resident length, and no room in
 * the record for the new non-resident attribute header + single-run
 * mapping pairs (vanishingly unlikely -- that header is far smaller than
 * the resident bytes it replaces -- but checked, not assumed).
 */
int hype_ntfs_data_to_nonresident(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t rec_no,
                                  uint64_t new_size, uint16_t usn);

/*
 * #423: create and unlink a regular file, composed entirely from #417-#421's
 * primitives (allocate an MFT record, build its base attributes, link it
 * into the parent's index; reverse on unlink).
 *
 * hype_ntfs_create(): allocates an MFT record (#420), appends
 * $STANDARD_INFORMATION and $FILE_NAME (both resident, real caller-supplied
 * FILETIME timestamps -- never a fixed or zero epoch, per #253's fix this
 * mirrors) and an empty resident $DATA, then inserts the $FILE_NAME key
 * into the parent directory's index (#421). Only ever creates ONE
 * $FILE_NAME (WIN32 namespace) -- no separate 8.3 DOS name, so there is
 * nothing for unlink to reconcile across multiple names for a file this
 * function itself created. Refuses (rolling back the MFT record it just
 * allocated) if the index insert fails, e.g. a duplicate name.
 *
 * hype_ntfs_unlink(): finds the file via the parent's index, removes that
 * index entry, decrements the target's hard-link count, and -- only once
 * it reaches zero -- releases every DATA range's clusters (resident $DATA
 * owns none) and frees the MFT record. A file with more than one
 * $FILE_NAME entry (e.g. an alias inserted directly via
 * hype_ntfs_index_insert(), not through this function) is therefore left
 * fully intact by unlinking any one name, exactly like a real hard link.
 */
int hype_ntfs_create(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t parent_dir_rec,
                     const char *name, uint32_t name_len, uint64_t timestamp_filetime,
                     uint64_t *out_rec_no, uint16_t usn);
int hype_ntfs_unlink(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t parent_dir_rec,
                     const char *name, uint32_t name_len, uint16_t usn);

/*
 * #425: create and remove directories, mirroring #423's create()/unlink()
 * shape. hype_ntfs_mkdir() allocates a directory MFT record (#420),
 * appends $STANDARD_INFORMATION + $FILE_NAME, appends an empty, correctly
 * NAMED ($I30) $INDEX_ROOT (real directories' $INDEX_ROOT carries that
 * name -- see research/README.md's #421 entry on why this matters), and
 * links the name into the parent (#421). Rolls back the allocated record
 * on any later failure, same as create().
 *
 * hype_ntfs_rmdir() refuses a non-empty directory (any entry beyond the
 * index terminator) or one that already has an $INDEX_ALLOCATION (out of
 * scope for #421, so out of scope here too -- such a directory was never
 * empty by this slice's own definition of empty), then removes the parent
 * index entry and frees the MFT record (a directory's $INDEX_ROOT is
 * always resident, so there are never clusters to release).
 */
int hype_ntfs_mkdir(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t parent_dir_rec,
                    const char *name, uint32_t name_len, uint64_t timestamp_filetime,
                    uint64_t *out_rec_no, uint16_t usn);
int hype_ntfs_rmdir(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t parent_dir_rec,
                    const char *name, uint32_t name_len, uint16_t usn);

/*
 * #424: rename (and/or move) a file or directory. Removes the old $I30
 * entry from `src_parent`, inserts a new one into `dst_parent` (which may
 * equal src_parent -- a same-directory rename), and rewrites the target
 * record's own $FILE_NAME attribute (new name, new parent reference) to
 * match. If the insert into `dst_parent` fails (e.g. a name collision),
 * the removed entry is RE-INSERTED into `src_parent` before returning
 * -1 -- the entry is never left in neither directory, matching the
 * "never lose it from both, never leave it in both" ordering the ticket
 * requires; the reverse (present in both at once) cannot happen because
 * the insert is attempted before anything about the target record is
 * touched.
 *
 * Only ever updates the file's WIN32-namespace $FILE_NAME (the one
 * #423/#425 create) -- a file with more than one $FILE_NAME (an alias
 * inserted directly via hype_ntfs_index_insert()) has the others left
 * exactly as they were, same as a real hard link surviving a rename of
 * one name.
 *
 * Known, deliberate simplification: does not adjust either parent's own
 * link/subdirectory-count bookkeeping when moving a directory between
 * parents -- a cosmetic Explorer/`stat` nicety, not something a
 * conformant reader needs to open, list, or otherwise use the moved
 * directory correctly.
 */
int hype_ntfs_rename(hype_ntfs_t *fs, hype_blk_write_fn write, uint64_t src_parent,
                     const char *src_name, uint32_t src_name_len, uint64_t dst_parent,
                     const char *dst_name, uint32_t dst_name_len, uint16_t usn);

/*
 * #692: path-based wrappers over #423/#424/#425's (dir_rec, name) writer
 * primitives, splitting `path` into "every component but the last, walked
 * as directories via the same dir_lookup() hype_ntfs_resolve() itself
 * uses" and "the final component" -- so NTFS can be driven by the same
 * ASCII-path calling convention as every other host-FS driver
 * (core/fs_ops.h's hype_fs_ops_t vtable), instead of every caller needing
 * to know MFT record numbers.
 */
int hype_ntfs_create_path(hype_ntfs_t *fs, hype_blk_write_fn write, const char *path,
                          uint64_t timestamp_filetime, uint64_t *out_rec_no, uint16_t usn);
int hype_ntfs_unlink_path(hype_ntfs_t *fs, hype_blk_write_fn write, const char *path,
                          uint16_t usn);
int hype_ntfs_mkdir_path(hype_ntfs_t *fs, hype_blk_write_fn write, const char *path,
                         uint64_t timestamp_filetime, uint64_t *out_rec_no, uint16_t usn);
int hype_ntfs_rmdir_path(hype_ntfs_t *fs, hype_blk_write_fn write, const char *path,
                         uint16_t usn);
int hype_ntfs_rename_path(hype_ntfs_t *fs, hype_blk_write_fn write, const char *from,
                          const char *to, uint16_t usn);

#endif /* HYPE_CORE_NTFS_H */
