#ifndef HYPE_CORE_EXT_H
#define HYPE_CORE_EXT_H

#include <stdint.h>

#include "fat.h"          /* hype_fat_read_fn, hype_fat_file_t: the shared extent contract */
#include "fat_write_fs.h" /* hype_fat_write_fn (shared with the FAT32/exFAT writers) */

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
 *   - files needing more than HYPE_FAT_MAX_EXTENTS runs (same cap as FAT).
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
int hype_ext_resolve(hype_fat_read_fn read, void *ctx, const char *path, hype_fat_file_t *out);

/*
 * #204 (STORAGE: ext2/3/4 host filesystem WRITE) -- IN-PLACE writes to an
 * existing, fully-allocated backing file, the ext counterpart of
 * hype_exfat_write_at(): persist guest disk writes back into a raw image
 * file that lives on an ext-formatted host volume. Implemented in
 * core/ext_write.c on top of the resolver above: the file's extents never
 * move, so writes are plain sector I/O through the injected write callback
 * (the same hype_fat_write_fn the FAT32/exFAT writers use, carried by
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
    hype_fat_file_t map; /* the file's resolved extents (they never move) */
    hype_fat_read_fn read;
    hype_fat_write_fn write;
    void *ctx;
} hype_ext_wfile_t;

/*
 * Resolves `path` for in-place read/write. Returns 0 on success; -1 on any
 * resolver failure, a NULL write callback, or a volume that is not cleanly
 * unmounted (mounted-dirty or marked with errors).
 */
int hype_ext_open_rw(hype_fat_read_fn read, hype_fat_write_fn write, void *ctx,
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

#endif /* HYPE_CORE_EXT_H */
