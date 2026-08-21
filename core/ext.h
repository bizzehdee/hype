#ifndef HYPE_CORE_EXT_H
#define HYPE_CORE_EXT_H

#include <stdint.h>

#include "blk_io.h"     /* the shared block I/O + file-map contract (#292) */
#include "file_range.h" /* the sparse-aware logical range contract (#381) */

/*
 * #203 (STORAGE: ext2/3/4 host filesystem READ) -- the ext counterpart of the
 * FAT32/exFAT reader in core/fat.c. Resolves an absolute path on an
 * ext2/3/4-formatted volume to the file's on-disk extents, in the SAME
 * (volume-relative LBA, sector count) contract core/iso_stream.c and the block
 * stack already consume -- so a raw disk image or installer ISO living as a
 * file on an ext-formatted host disk (the common Linux case) can back a
 * virtual disk exactly the way a FAT-hosted one does.
 *
 * Pure logic over the injected volume-relative sector-read callback
 * (core/gpt.c / core/fat.c pattern); read-only, never writes; unit-tested
 * against synthetic volumes and validated against real mkfs.ext2/3/4 images.
 *
 * Supported on-disk shapes:
 *   - block sizes 1024, 2048 and 4096 (mkfs defaults; 512-byte logical
 *     sectors underneath, like the rest of hype's block world);
 *   - ext4 EXTENT trees of any depth, and classic ext2/3 direct +
 *     single/double/triple-indirect block maps;
 *   - the 64BIT feature (64-bit block numbers, larger group descriptors);
 *   - directory traversal by linear scan, which also covers htree-indexed
 *     directories: their interior index blocks are disguised as empty
 *     (inode 0) entries precisely so linear readers skip them.
 *
 * Deliberately refused, with -1:
 *   - a volume whose journal needs recovery (INCOMPAT_RECOVER): its metadata
 *     is not consistent until the journal is replayed, and hype must never
 *     resolve a path through stale structures;
 *   - any other incompatible feature outside the supported set (META_BG,
 *     MMP, INLINE_DATA, ENCRYPT, CASEFOLD, LARGEDIR, ...);
 *   - SPARSE or unwritten-extent files: the extent contract has no way to
 *     say "this range reads as zeros", and a disk image with silent zero
 *     holes is exactly the kind of quiet corruption hype refuses; the image
 *     prep tooling writes images fully;
 *   - files needing more than HYPE_FILE_MAX_EXTENTS runs (same cap as FAT).
 *     NOTE this makes very large ext2/3 INDIRECT-mapped files structurally
 *     unresolvable: the classic allocator interleaves an indirection block
 *     into the data every 256 blocks (at 1 KiB), so a 64 MiB+ file needs
 *     hundreds of runs however contiguously it was written. Real usage --
 *     ext4 extent trees -- coalesces to a handful of runs (a validated 8 MiB
 *     file was 1-2). Host big images on ext4, not ext2.
 *
 * Unlike the FAT resolvers, matching is CASE-SENSITIVE -- ext filesystems
 * are. Path separators may be '/' or '\\'.
 */
int hype_ext_resolve(hype_blk_read_fn read, void *ctx, const char *path, hype_file_map_t *out);

/*
 * #293: does this volume carry a supported, cleanly-unmounted ext2/3/4
 * filesystem? The same superblock gate hype_ext_resolve applies, without
 * resolving anything. Returns 0 (claimed) or -1. Read-only.
 */
int hype_ext_probe(hype_blk_read_fn read, void *ctx);

/*
 * #204 (STORAGE: ext2/3/4 host filesystem WRITE) -- IN-PLACE writes to an
 * existing, fully-allocated backing file, the ext counterpart of
 * hype_exfat_write_at(): persist guest disk writes back into a raw image
 * file that lives on an ext-formatted host volume. Implemented in
 * core/ext_write.c on top of the resolver above: the file's extents never
 * move, so writes are plain sector I/O through the injected write callback
 * (the same hype_blk_write_fn the FAT32/exFAT writers use, carried by
 * blk_phys over AHCI/NVMe/USB).
 *
 * THE JOURNAL CONSTRAINT (the ticket's hard part, resolved by scoping):
 * ext3/4 journal METADATA. In-place data writes to an already-allocated
 * file touch no metadata at all -- no bitmaps, no size, no extent tree, no
 * journal -- so they cannot race or corrupt a jbd2 journal. What is NOT
 * safe is trusting the metadata of a volume that crashed while mounted, so
 * hype_ext_open_rw() refuses any volume that is not CLEANLY UNMOUNTED:
 * s_state must say VALID and not ERROR, and the resolver already refuses an
 * unreplayed journal (INCOMPAT_RECOVER). Growing/allocating files stays a
 * follow-on; hype must also be the volume's only writer while it holds it
 * (true by construction post-ExitBootServices).
 */
typedef struct {
    hype_file_map_t map; /* the file's resolved extents (they never move) */
    hype_blk_read_fn read;
    hype_blk_write_fn write;
    void *ctx;
} hype_ext_wfile_t;

/*
 * Resolves `path` for in-place read/write. Returns 0 on success; -1 on any
 * resolver failure, a NULL write callback, or a volume that is not cleanly
 * unmounted (mounted-dirty or marked with errors).
 */
int hype_ext_open_rw(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                     const char *path, hype_ext_wfile_t *out);

/*
 * Overwrites `len` bytes at byte `offset` of the file. The range must lie
 * wholly inside the file -- this path NEVER grows or moves an allocation
 * (offset+len validation per AGENTS.md; a guest-steered range outside the
 * file is refused, not clamped). Whole aligned sectors are written in bulk
 * runs per extent; ragged edges read-modify-write. Returns 0 or -1.
 */
int hype_ext_write_at(hype_ext_wfile_t *f, uint64_t offset, const void *data, unsigned int len);

/* Reads `len` bytes at byte `offset`. Bounds-checked exactly as above. */
int hype_ext_read_at(hype_ext_wfile_t *f, uint64_t offset, void *out, unsigned int len);

/*
 * #384: as hype_ext_resolve, but into the sparse-aware #381 contract.
 * Classic-map holes (at every indirection level, including whole missing
 * indirect trees and a file that ends in a hole) become HOLE ranges; ext4
 * unwritten extents become UNWRITTEN ranges. Both read as zeros through
 * hype_file_rmap_read_at. The refusal list otherwise matches the legacy
 * resolver (unsupported features, INCOMPAT_RECOVER, inline data, ...).
 */
int hype_ext_resolve_rmap(hype_blk_read_fn read, void *ctx, const char *path,
                          hype_file_rmap_t *out);

/* #384 support helpers for the ext2 writer: the inode number behind a path,
 * and an inode's sparse map re-derived from its current on-disk state. */
int hype_ext_resolve_ino(hype_blk_read_fn read, void *ctx, const char *path, uint32_t *out_ino);
int hype_ext_map_ino_rmap(hype_blk_read_fn read, void *ctx, uint32_t ino_no,
                          hype_file_rmap_t *out);

/*
 * #384: the ext2 allocating writer (core/ext2_alloc.c) -- persist guest
 * writes into a sparse backing file on an EXT2 volume, allocating blocks
 * when a write reaches a hole. plan.md §10 decision 29's ext2 arm: ordered
 * direct metadata updates while the volume is marked dirty; ext3/4
 * allocation needs jbd2 and is #385.
 *
 * Gates at open: ext2 ONLY (any journal -> refuse; that is #385's work),
 * cleanly unmounted (s_state VALID and not ERROR), no RO_COMPAT feature
 * beyond SPARSE_SUPER/LARGE_FILE (a checksummed volume must not be written
 * by code that does not maintain its checksums), classic block map (an
 * extent-mapped file is refused), 32-bit block numbers.
 *
 * Ordering per write (decision 29): superblock marked dirty -> block claimed
 * in the bitmap + group/superblock free counts -> content (zeros + data)
 * written to the block -> the POINTER exposing it published last (a fresh
 * pointer block is zeroed on media before its parent references it) -> inode
 * (roots, i_blocks, optional times) -> superblock restored clean. A failure
 * mid-way rolls the claims and hooks back; a failure DURING rollback leaves
 * the volume marked dirty, honestly.
 */
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
    uint32_t ino;
    uint64_t inode_byte;  /* media byte offset of the inode structure */
    uint64_t size_bytes;
    uint32_t mtime;       /* stamped into i_mtime/i_ctime on commit; 0 = leave */
    hype_file_rmap_t map; /* the file's CURRENT ranges (refreshed after allocation) */
} hype_ext2_wfile_t;

int hype_ext2_open_rw(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, hype_ext2_wfile_t *out);

/* Unix-epoch timestamp for subsequent commits' i_mtime/i_ctime. */
void hype_ext2_set_time(hype_ext2_wfile_t *f, uint32_t unix_seconds);

/* Reads `len` bytes at `offset`: holes read as zeros. Bounds-checked. */
int hype_ext2_read_at(hype_ext2_wfile_t *f, uint64_t offset, void *out, unsigned int len);

/*
 * Writes `len` bytes at `offset`, allocating blocks (and any missing
 * indirection blocks) where the span crosses holes -- and, since #497, GROWING
 * the file when the span ends past EOF: the final partially-used block is
 * extended in place (with the stale bytes between the old size and the write
 * zeroed before the new i_size can expose them), wholly-new blocks are
 * allocated and filled, an untouched gap stays a sparse hole, and i_size is
 * published with the same commit -- so a failure rolls back to a file whose
 * size never moved. Newly allocated blocks are zero-filled around the written
 * bytes before the pointer publishing them lands. Returns 0, -1 on error, a
 * full volume (rolled back), or a span needing more than the per-call
 * allocation bound (the fs_ops layer chunks large writes for this).
 */
int hype_ext2_write_at(hype_ext2_wfile_t *f, uint64_t offset, const void *data,
                       unsigned int len);

#endif /* HYPE_CORE_EXT_H */
