#ifndef HYPE_CORE_EXT_NAMESPACE_H
#define HYPE_CORE_EXT_NAMESPACE_H

#include <stdint.h>

#include "blk_io.h"

/*
 * #498 (STORAGE: ext namespace mutation): create/unlink/mkdir/rmdir/rename on
 * ext2, ext3 and ext4, journaled where the volume has one. Builds on top of
 * #384/#385/#495/#496/#497's allocation and checksum machinery -- the SAME
 * discipline applies here: every structural update (inode bitmap, block
 * bitmap, directory entries, group descriptor and superblock counters) goes
 * through the journal on ext3/4 (never a direct write, never a second pass
 * after the fact) and through the ordered direct-write discipline plan.md
 * §10 decision 29 already established for ext2.
 *
 * Two independent implementations, split the same way #384 (core/ext2_alloc.c)
 * and #385 (core/ext_jalloc.c) already are:
 *   - core/ext2_namespace.c: ext2 (no journal), direct ordered writes.
 *   - core/extj_namespace.c: ext3/4 (COMPAT_HAS_JOURNAL), one bounded jbd2
 *     transaction per call, checksummed exactly where #495/#496 require it.
 * Each function below reads the superblock ONCE to decide which applies, then
 * dispatches -- ext has no persistent mount state (core/fs_ops.c's ext_mount
 * comment), so every call revalidates from scratch, same as the rest of the
 * ext writer family.
 *
 * HTREE (dir_index) DIRECTORIES: this slice implements only LINEAR directory
 * insertion. `create`, `mkdir`, and the destination side of `rename` refuse
 * (-1) outright when the PARENT directory being inserted into carries
 * EXT4_INDEX_FL (0x1000) -- inserting a new entry with a linear-scan
 * algorithm shuffles bytes an htree's interior index blocks (disguised as
 * inode-0 entries at the SAME logical positions a linear insert would use)
 * depend on staying put; guessing is not an option, per the ticket. Removal
 * (`unlink`, `rmdir`, and the source side of `rename`) is safe on an htree
 * directory and is NOT refused: the kernel's own ext4_delete_entry() treats
 * htree and linear directories identically -- it only ever marks an existing
 * entry's inode field 0 / merges its rec_len into its predecessor, touching
 * no interior index data -- so hype's linear removal (which does exactly
 * that) matches real ext4 semantics rather than guessing at them. See
 * research/linux-ext4-directory-2026-08-22.html.
 *
 * Refused at open, same feature gates as the #384/#385/#497 writers this sits
 * on top of: an unreplayed journal, a volume not cleanly unmounted, any
 * INCOMPAT/RO_COMPAT feature outside what #495/#496 maintain (checksums
 * excepted -- those ARE maintained), a non-empty journal, bigalloc, META_BG.
 *
 * `rename` never silently replaces an existing destination -- matches FAT32
 * (#247) and exFAT (#246): a dirent already at `to` refuses the whole
 * operation before anything is touched.
 */

/*
 * Creates a new, empty (zero-length) regular file at `path`. Refused if
 * anything already exists there, if the parent does not exist or is not a
 * directory, if the parent is htree-indexed, or if the volume/parent
 * component is otherwise unsupported. `mtime` (Unix epoch seconds, 0 = leave
 * the "unset" convention already in place) stamps the new inode's
 * atime/ctime/mtime. Returns 0 or -1.
 */
int hype_ext_ns_create(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime);

/*
 * Removes the directory entry and, when the target inode's link count drops
 * to zero, the inode and its data blocks. Refused if `path` does not resolve
 * to a REGULAR file (directories are rmdir's job), if it does not exist, if
 * the volume/feature gates refuse, or -- when the link count is about to
 * reach zero -- if the file's blocks are mapped more deeply than a classic
 * direct+single-indirect map or a depth-0 extent root (a shape this slice's
 * own writes never produce, but the #384/#385/#497 write path can on a
 * real, heavily used volume; freeing it would mean either walking structure
 * this slice does not implement or misreading it, so it refuses instead of
 * guessing, same as the htree gate). Returns 0 or -1.
 */
int hype_ext_ns_unlink(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime);

/*
 * Creates a new, empty directory at `path`: a fresh inode (link count 2) plus
 * one data block holding "." and "..", the parent's link count and
 * used-directory counters incremented for the new ".." backlink. Refused if
 * anything already exists at `path`, the parent is htree-indexed, or the
 * usual volume/feature gates refuse. Returns 0 or -1.
 */
int hype_ext_ns_mkdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, uint32_t mtime);

/*
 * Removes an EMPTY directory (only "." and ".." present) at `path`: frees its
 * inode and data block(s), decrements the parent's link count and
 * used-directory counter. Refused if `path` is not a directory, is not
 * empty, is the volume root, carries EXT4_INDEX_FL itself (an htree
 * directory reduced to "empty" without ever being compacted back to a plain
 * one-block directory is outside this slice -- see ext_namespace.c), or its
 * own blocks are mapped too deeply to free safely (same refusal and
 * reasoning as unlink's). Returns 0 or -1.
 */
int hype_ext_ns_rmdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, uint32_t mtime);

/*
 * Renames `from` to `to`, across directories or within one. NEVER replaces
 * an existing entry at `to` -- refused (-1) up front, matching FAT32/exFAT.
 * Also refused: `to`'s parent is htree-indexed; `from` names a directory and
 * `to` lies inside `from` itself (walked via `to`'s parent chain up to root,
 * refusing a cycle); the usual volume/feature gates. Moving a directory
 * across parents updates its ".." entry and both parents' link counts /
 * used-directory counters; a same-directory rename is a plain entry
 * replace. Returns 0 or -1.
 */
int hype_ext_ns_rename(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *from, const char *to, uint32_t mtime);

#endif /* HYPE_CORE_EXT_NAMESPACE_H */
