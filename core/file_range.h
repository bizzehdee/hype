#ifndef HYPE_CORE_FILE_RANGE_H
#define HYPE_CORE_FILE_RANGE_H

#include <stdint.h>

#include "blk_io.h" /* hype_blk_read_fn, hype_file_map_t, HYPE_BLK_SECTOR_SIZE */

/*
 * #381 (STORAGE: sparse-aware logical file-range contract): the successor to
 * the physical-only extent map for describing WHERE a file's bytes live.
 *
 * hype_file_map_t can say only "these sectors, in this order" -- it has no way
 * to express an ext hole ("no sectors exist; reads are zeros") or an
 * unwritten/preallocated extent ("sectors exist but were never initialised;
 * reads are zeros, the media contents are stale"). Both are normal on ext4 and
 * NTFS, so resolvers for those formats had to REFUSE sparse files outright
 * (core/ext.c's documented refusal). This map represents them explicitly, and
 * the shared read path synthesizes the zeros, so no driver invents its own
 * hole semantics.
 *
 * Representation: an ORDERED list of ranges, each covering the next
 * `sector_count` logical sectors of the file. The logical offset of a range is
 * implicit -- the sum of the sector counts before it -- so an unsorted or
 * overlapping map is UNREPRESENTABLE by construction, which is stronger than
 * validating for it. What still needs validation (hype_file_rmap_validate) is
 * everything the representation cannot rule out: kinds, zero-length ranges,
 * media bounds on DATA ranges, coverage vs size_bytes, and overflow.
 *
 * Kinds:
 *   DATA      -- sectors exist and hold the file's bytes; start_lba says where.
 *   HOLE      -- no sectors are allocated. Reads are zeros. A write here needs
 *                allocation (a filesystem-writer capability, not this layer's).
 *   UNWRITTEN -- sectors are allocated (start_lba is meaningful) but their
 *                contents were never initialised. Reads are zeros -- reading
 *                the media here would leak stale bytes, exactly what
 *                ValidDataLength (exFAT/NTFS) and ext4's unwritten extents
 *                exist to prevent.
 *
 * FAT32 and exFAT (DIR_FileSize rule / ValidDataLength aside) cannot represent
 * an internal hole: every cluster through the file size must be chained. Their
 * resolvers therefore keep HARD-FAILING on a short or malformed chain; lifting
 * a physical map into this contract (hype_file_rmap_from_extents) never
 * invents a HOLE.
 *
 * LBA space: as with hype_file_map_t, start_lba is in whatever space the
 * producer's read callback speaks (volume-relative for the host-FS resolvers).
 */

typedef enum {
    HYPE_RANGE_DATA = 0,
    HYPE_RANGE_HOLE = 1,
    HYPE_RANGE_UNWRITTEN = 2,
} hype_range_kind_t;

typedef struct {
    uint32_t kind;         /* hype_range_kind_t; fixed-width on disk of the struct */
    uint64_t start_lba;    /* first LBA of the run; meaningful for DATA and UNWRITTEN */
    uint64_t sector_count; /* logical length of the run, in 512-byte sectors */
} hype_file_range_t;

/*
 * Same ceiling philosophy as HYPE_FILE_MAX_EXTENTS (#366): finite, reported
 * when exceeded, and NOT a physical-fragmentation cap smuggled into the
 * logical contract -- a HOLE costs one range but zero physical extents, so a
 * sparse file's range count is decoupled from its allocation's shape.
 */
#define HYPE_FILE_MAX_RANGES 256u

typedef struct {
    hype_file_range_t ranges[HYPE_FILE_MAX_RANGES];
    unsigned count;      /* number of ranges used */
    uint64_t size_bytes; /* exact file length in bytes */
    /* Set when the producer stopped because the file needs more than
     * HYPE_FILE_MAX_RANGES ranges -- same diagnostic contract as
     * hype_file_map_t::too_fragmented (#366). */
    int too_fragmented;
} hype_file_rmap_t;

/* Reset `m` to an empty map of `size_bytes`. */
void hype_file_rmap_init(hype_file_rmap_t *m, uint64_t size_bytes);

/*
 * Appends a run of `sector_count` sectors of `kind` (DATA/UNWRITTEN carry
 * `start_lba`; HOLE ignores it and stores 0). Coalesces with the previous
 * range when the kind matches and, for DATA/UNWRITTEN, the LBAs are
 * contiguous -- so producers can emit per-cluster and still get a compact map.
 * Returns 0; -1 with m->too_fragmented=1 when the map is full, on a zero
 * `sector_count`, an invalid kind, or LBA/sector-count overflow.
 */
int hype_file_rmap_append(hype_file_rmap_t *m, hype_range_kind_t kind, uint64_t start_lba,
                          uint64_t sector_count);

/*
 * Validates everything the representation cannot exclude:
 *   - count within HYPE_FILE_MAX_RANGES; every kind valid; no zero-length range;
 *   - total logical coverage exactly spans size_bytes (the final range may
 *     extend into the last partial sector, never a full sector beyond);
 *   - every DATA/UNWRITTEN range lies inside [0, media_sectors) with an
 *     overflow guard on start_lba + sector_count;
 *   - the logical sector total does not overflow.
 * A zero-byte file must have zero ranges. Returns 0 if valid, -1 otherwise.
 */
int hype_file_rmap_validate(const hype_file_rmap_t *m, uint64_t media_sectors);

/*
 * Lifts a physical-only extent map (what the FAT32/exFAT/ext resolvers produce
 * today) into an all-DATA range map. Never invents a HOLE: a map whose extents
 * cover fewer sectors than size_bytes needs is refused (-1), preserving the
 * FAT-family rule that a short allocation chain is corruption, not sparseness.
 * Carries too_fragmented through. Returns 0 on success.
 */
int hype_file_rmap_from_extents(const hype_file_map_t *src, hype_file_rmap_t *out);

/*
 * Maps logical byte offset `off` to what covers it. Returns 0 and fills:
 *   *out_kind -- the covering range's kind;
 *   *out_lba  -- the LBA of the sector holding `off` (DATA/UNWRITTEN; 0 for HOLE);
 *   *out_head -- byte offset within that sector;
 *   *out_run  -- how many bytes from `off` stay inside this range (capped at
 *                size_bytes), i.e. how far one transfer or one memset may go
 *                before the next range must be consulted.
 * Returns -1 if `off` >= size_bytes or the map is malformed. Pure.
 */
int hype_file_rmap_locate(const hype_file_rmap_t *m, uint64_t off, hype_range_kind_t *out_kind,
                          uint64_t *out_lba, uint32_t *out_head, uint64_t *out_run);

/*
 * Reads `len` bytes at byte `offset` into `dst`: DATA ranges through `read`,
 * HOLE and UNWRITTEN ranges synthesized as zeros WITHOUT touching the medium.
 * The range must lie wholly inside the file (offset+len <= size_bytes, with an
 * overflow guard) -- refused, not clamped, per the AGENTS.md bounds rule.
 * Ragged sector edges bounce through a stack sector. len==0 is a no-op.
 * Returns 0 on success; -1 on bounds, a malformed map, or a failed read.
 */
int hype_file_rmap_read_at(const hype_file_rmap_t *m, hype_blk_read_fn read, void *ctx,
                           uint64_t offset, void *dst, unsigned int len);

#endif /* HYPE_CORE_FILE_RANGE_H */
