#ifndef HYPE_CORE_EXT_DIRENT_H
#define HYPE_CORE_EXT_DIRENT_H

#include <stdint.h>

/*
 * #498: pure, in-memory ext2/3/4 directory-BLOCK-content operations, shared
 * by core/ext2_namespace.c (ext2, direct writes) and core/extj_namespace.c
 * (ext3/4, journaled) -- the one piece of #498's logic that is genuinely
 * substrate-independent (it never touches media itself: the caller reads a
 * whole filesystem block into `block`, calls one of these, and writes the
 * (possibly mutated) block back through whatever I/O discipline its
 * substrate requires -- the journal transaction cache on ext3/4, a direct
 * read-modify-write on ext2).
 *
 * On-disk shape (struct ext4_dir_entry_2, INCOMPAT_FILETYPE format -- the
 * only one this writer ever emits, and the only one core/ext.c's reader ever
 * assumes): inode(4) rec_len(2) name_len(1) file_type(1) name[name_len],
 * rec_len always a multiple of 4 and never 0. A record whose inode is 0 is
 * free space, not a real entry -- exactly the "(ab)used to fool the old
 * linear-scan algorithm" disguise an htree interior/root block also uses
 * (research/linux-ext4-directory-2026-08-22.html), which is why every
 * function below only ever inspects and rewrites REAL entries by name/rec_len
 * bookkeeping and never assumes a particular COUNT of entries -- the same
 * property that lets plain removal stay safe on an htree directory (see
 * ext_namespace.h's htree note).
 *
 * RO_COMPAT_METADATA_CSUM directories reserve the last 12 bytes of every leaf
 * block for a phony `struct ext4_dir_entry_tail` (det_reserved_zero1=0,
 * det_rec_len=12, det_reserved_zero2=0, det_reserved_ft=0xDE, det_checksum) --
 * `has_tail` selects this and every function below treats
 * [block_size-12, block_size) as untouchable except through
 * hype_extd_csum_finalize, called once after any mutation. The checksum is
 * crc32c(i_csum_seed, block, block_size-12) -- the OWNING directory inode's
 * seed (core/ext_jalloc.c's i_csum_seed: the filesystem seed chained with
 * that inode's own number and generation), never the file being looked up.
 */

#define HYPE_EXTD_FT_UNKNOWN 0u
#define HYPE_EXTD_FT_REG 1u
#define HYPE_EXTD_FT_DIR 2u

/* Rounds a name length up to the on-disk record size (8-byte header + name,
 * rounded to 4): the space a TIGHTLY PACKED entry for this name needs. */
uint32_t hype_extd_reclen(unsigned int name_len);

/* Initializes a freshly allocated, all-zero block as one giant free slot
 * spanning its usable region ([0, block_size) or, under has_tail,
 * [0, block_size-12)) -- and, when has_tail, writes a valid (checksummed by
 * the caller afterwards) tail at the end. `block` must already be zeroed. */
void hype_extd_block_init(uint8_t *block, uint32_t block_size, int has_tail);

/*
 * Validates a block's structure: every record's rec_len is a non-zero
 * multiple of 4, in-bounds, and the chain sums to exactly the usable region
 * -- and, under has_tail, that the final 12 bytes really are a well-formed
 * tail marker (ino 0, rec_len 12, name_len 0, file_type 0xDE). Returns 0 if
 * well-formed, -1 otherwise (a shape this writer refuses to touch rather
 * than guess about). Every other function in this header assumes its caller
 * validated first.
 */
int hype_extd_validate(const uint8_t *block, uint32_t block_size, int has_tail);

/*
 * Scans `block` for a REAL (inode != 0) entry named `name` (`name_len`
 * bytes, case-sensitive). Returns 1 and fills *out_off (the record's byte
 * offset) and *out_ino (its inode number) if found, 0 if not present in this
 * block. Assumes hype_extd_validate already passed.
 */
int hype_extd_find(const uint8_t *block, uint32_t block_size, int has_tail, const char *name,
                   unsigned int name_len, uint32_t *out_off, uint32_t *out_ino);

/*
 * Attempts to insert (ino, name, file_type) into `block`: first-fit over
 * every free record (inode == 0, whole slot reused) or splittable record
 * (a real entry whose rec_len exceeds what its own name needs by enough for
 * the new one, shrunk to its tight size with the new entry placed in the
 * remainder). Returns 0 and mutates `block` on success (caller must then
 * call hype_extd_csum_finalize when has_tail); -1 if no slot in this block
 * fits (the caller must try another block or allocate a fresh one -- never a
 * partial/torn mutation on failure).
 */
int hype_extd_insert(uint8_t *block, uint32_t block_size, int has_tail, uint32_t ino,
                     const char *name, unsigned int name_len, uint8_t file_type);

/*
 * Removes the REAL entry named `name`. Matches the kernel's own
 * ext4_delete_entry(): the entry BEFORE it in this same block (if any)
 * absorbs its rec_len; if it is the first entry in the block, it is left in
 * place with inode forced to 0 (rec_len/name untouched, now free space) --
 * never shifted, so record OFFSETS in this block never move (an htree
 * interior node's fixed positions, when this block IS one, survive
 * untouched). Returns 1 and mutates `block` on success (caller must then
 * call hype_extd_csum_finalize when has_tail); 0 if `name` is not in this
 * block.
 */
int hype_extd_remove(uint8_t *block, uint32_t block_size, int has_tail, const char *name,
                     unsigned int name_len);

/*
 * True (1) if `block` holds no real entry other than "." and ".."; 0 if it
 * holds any other real entry. Used per-block; the caller ANDs this over
 * every block of a directory to decide rmdir's "must be empty" rule.
 */
int hype_extd_only_dots(const uint8_t *block, uint32_t block_size, int has_tail);

/*
 * Recomputes and writes the block's metadata_csum tail. `i_csum_seed` is the
 * OWNING directory inode's per-inode seed (see core/ext_jalloc.c). A no-op
 * when has_tail is false. Must be the LAST thing done to a block before it
 * is committed -- every field the hash covers must already be final.
 */
void hype_extd_csum_finalize(uint8_t *block, uint32_t block_size, int has_tail,
                             uint32_t i_csum_seed);

#endif /* HYPE_CORE_EXT_DIRENT_H */
