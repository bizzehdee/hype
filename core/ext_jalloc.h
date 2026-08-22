#ifndef HYPE_CORE_EXT_JALLOC_H
#define HYPE_CORE_EXT_JALLOC_H

#include <stdint.h>

#include "blk_io.h"
#include "file_range.h"
#include "jbd2.h"

/*
 * #385: the JOURNALED allocating writer for ext3/ext4 -- persist guest
 * writes into sparse backing files on journaled ext volumes, without ever
 * bypassing jbd2 for a structural update. plan.md §10 decision 29.
 *
 * What #204 could safely skip the journal for -- an in-place data write to
 * an allocated block -- stays the metadata-free fast path here. Everything
 * else (block bitmaps, group descriptors, inode block maps, extent-tree
 * nodes, superblock counters) is collected in a bounded block-image
 * transaction, committed to the journal FIRST, and only then written to its
 * final location (core/jbd2.c). Data and zero-fill always reach the medium
 * before the mapping that exposes them is committed.
 *
 * Supported per file: ext3 classic block maps (the #384 allocation logic on
 * a journaled substrate) and ext4 extent trees -- entry insertion, merge
 * with a contiguous predecessor, leaf splits, in-inode root growth, and
 * unwritten-extent conversion at block granularity.
 *
 * Refused at open, per the ticket: a volume without a journal (that is the
 * ext2 writer's case), an EXTERNAL journal, a NON-EMPTY journal (a crashed
 * writer's transactions await replay: fsck/mount on a real OS recovers, hype
 * never guesses), unsupported journal metadata features (checksummed, async
 * or fast commit -- see core/jbd2.h for the journal's own 64-bit handling),
 * bigalloc, and META_BG. Refused per write: a transaction that would exceed
 * the fixed journal-credit bound (HYPE_JBD2_MAX_BLOCKS images).
 *
 * #495: GDT_CSUM (crc16 "uninit_bg") and METADATA_CSUM (crc32c, seeded from
 * the FS UUID) volumes are supported -- every structure this writer ever
 * mutates (superblock, group descriptor, block bitmap, inode, extent-tree
 * blocks) has its checksum recomputed inside the same transaction, before
 * the image is handed to jbd2. A METADATA_CSUM volume whose s_checksum_type
 * is not the one defined value (crc32c) is still refused: hype does not
 * guess at an algorithm the spec does not define.
 *
 * #496: 64BIT (64-bit block numbers, 64-byte-or-larger group descriptors) is
 * supported. `mkfs.ext4` turns it on by DEFAULT (verified on this machine:
 * /etc/mke2fs.conf's `ext4` fs_type lists it), so refusing it meant refusing
 * ordinary modern volumes, not exotic ones. The group descriptor's high
 * halves (block-bitmap, inode-table, and free-block-count pointers), the
 * superblock's s_blocks_count_hi, and the extent tree's ee_start_hi /
 * ei_leaf_hi are all carried as genuine uint64_t values end to end -- never
 * truncated. The group-descriptor checksum (crc16 GDT_CSUM or crc32c
 * METADATA_CSUM) covers the FULL descriptor width once desc_size > 32
 * (verified against the kernel's fs/ext4/super.c ext4_group_desc_csum() and
 * fs/ext4/bitmap.c's block-bitmap checksum setters, not from memory), so the
 * #495 checksum paths above extend their hash over the added high-half bytes
 * too. Classic (non-extent) block maps stay genuinely 32-bit-only pointers
 * -- that is the on-disk format, not a shortcut -- so an allocation for a
 * classic-mapped file that would land above 2^32 is refused rather than
 * truncated.
 */

#define HYPE_EXTJ_CACHE 24u /* == HYPE_JBD2_MAX_BLOCKS: one image per slot */

typedef struct {
    hype_blk_read_fn read;
    hype_blk_write_fn write;
    void *ctx;
    uint32_t block_size;
    uint32_t spb;
    uint64_t blocks_count;
    uint32_t blocks_per_group;
    uint32_t first_data_block;
    uint32_t groups;
    uint32_t inode_size;
    uint32_t inodes_per_group;
    uint32_t desc_size; /* #496: 32, or a power of two >= 64 under INCOMPAT_64BIT */
    uint32_t ino;
    uint64_t inode_byte;
    uint64_t size_bytes;
    int is_extents;       /* the FILE's mapping scheme */
    int dead;             /* a transaction failed after journal exposure: the
                           * on-media metadata is part-old/part-new until
                           * REPLAY runs, so this handle refuses all writes */
    uint32_t mtime;
    hype_jbd2_t journal;
    hype_file_rmap_t map;
    /* #495: metadata-checksum state */
    int has_gdt_csum;      /* RO_COMPAT_GDT_CSUM: crc16 group-desc checksum */
    int has_metadata_csum; /* RO_COMPAT_METADATA_CSUM: crc32c-seeded family */
    uint8_t uuid[16];      /* s_uuid -- the crc16 (GDT_CSUM) path rehashes it */
    uint32_t csum_seed;    /* crc32c(~0, uuid) -- the METADATA_CSUM fs seed */
    uint32_t i_csum_seed;  /* seed chained with THIS inode's number+generation:
                            * the inode's own checksum and its extent-tree
                            * tail checksums both key off this one value */
    /* the transaction block cache is a module-static single instance
     * (ext_jalloc.c): the writer is BSP-serialized, one transaction ever in
     * flight, and 96 KiB per handle would bloat every hype_fs_file_t */
} hype_extj_wfile_t;

int hype_extj_open_rw(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, hype_extj_wfile_t *out);

void hype_extj_set_time(hype_extj_wfile_t *f, uint32_t unix_seconds);

int hype_extj_read_at(hype_extj_wfile_t *f, uint64_t offset, void *out, unsigned int len);

/*
 * Writes `len` bytes at `offset` inside the file. DATA spans are written in
 * place with no transaction. Spans crossing HOLE ranges allocate through a
 * journaled transaction; spans crossing UNWRITTEN ranges zero the uncovered
 * bytes of the touched blocks and convert those blocks to written, likewise
 * journaled. Since #497 a span ending past EOF GROWS the file (tail block
 * extended in place with the stale gap zeroed, new blocks allocated, an
 * untouched gap left sparse, i_size published in the same journal commit).
 * Returns 0; -1 on error, a full volume (rolled back), or a span
 * whose metadata footprint exceeds the per-call journal credit bound.
 */
int hype_extj_write_at(hype_extj_wfile_t *f, uint64_t offset, const void *data,
                       unsigned int len);

#endif /* HYPE_CORE_EXT_JALLOC_H */
