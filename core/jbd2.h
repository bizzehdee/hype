#ifndef HYPE_CORE_JBD2_H
#define HYPE_CORE_JBD2_H

#include <stdint.h>

#include "blk_io.h"
#include "file_range.h"

/*
 * #385: a minimal, bounded jbd2 WRITER -- just enough journal to make ext3/4
 * metadata allocation crash-safe, and nothing more. plan.md §10 decision 29's
 * ext3/4 arm: "allocation requires a valid jbd2 metadata transaction and
 * commit before the new mapping is exposed."
 *
 * The flow per transaction (hype is the volume's only writer, so there is
 * exactly one transaction in flight, ever):
 *   1. open: validate the journal superblock (V2, internal, no feature hype
 *      does not understand) and require it EMPTY (s_start == 0). A non-empty
 *      journal means a crashed writer's transactions await replay: hype
 *      REFUSES rather than replays -- an fsck/mount cycle on a real OS is the
 *      recovery path, exactly the ext INCOMPAT_RECOVER discipline.
 *   2. commit: write descriptor block(s) + the metadata block images + a
 *      commit block at s_first, then point the journal superblock's s_start
 *      at the transaction. From this moment a crash replays hype's metadata.
 *   3. the caller writes the same images to their final locations;
 *   4. checkpoint: s_start back to 0, s_sequence advanced. The journal is
 *      empty again; the transaction is fully on disk.
 * A crash before 2 completes leaves the old metadata untouched (the commit
 * block is what makes a transaction real). A crash between 2 and 4 is
 * repaired by replay. Escaped blocks (images that begin with the jbd2 magic)
 * are handled per the format.
 *
 * Refused: external journals, checksummed journals (CSUM_V2/V3 track
 * metadata_csum, which the ext writer refuses too), async-commit,
 * fast-commit, a non-empty journal, and any transaction that does not fit
 * the bounded tag/block budget below.
 *
 * #496: a journal already carrying JBD2_FEATURE_INCOMPAT_64BIT is accepted
 * (its block tags gain a t_blocknr_high word -- see journal_tag_bytes() in
 * the kernel's fs/jbd2/journal.c). A fresh `mkfs.ext4 -O 64bit` journal does
 * NOT set this bit itself (verified empirically: e2fsprogs' own journal
 * creation never does); the kernel sets it lazily on the FIRST real mount of
 * a 64BIT-featured filesystem (fs/ext4/super.c, ext4_load_journal()). hype
 * mirrors that: hype_jbd2_upgrade_64bit() sets the bit once, unconditionally,
 * whenever the ext4 volume itself has INCOMPAT_64BIT -- so a metadata block
 * anywhere on the volume, however far out, can always be journaled.
 */

#define HYPE_JBD2_MAX_BLOCKS 24u /* metadata block images per transaction */

typedef struct {
    hype_blk_read_fn read;
    hype_blk_write_fn write;
    void *ctx;
    uint32_t block_size;      /* == the filesystem block size */
    uint32_t spb;
    hype_file_rmap_t map;     /* the journal file's blocks (inode 8) */
    uint32_t maxlen;          /* s_maxlen: journal length in blocks */
    uint32_t first;           /* s_first: first log block */
    uint32_t sequence;        /* s_sequence: next transaction id */
    uint8_t uuid[16];
    int has_64bit;            /* #496: JBD2_FEATURE_INCOMPAT_64BIT is active --
                               * every tag carries a t_blocknr_high word */
} hype_jbd2_t;

typedef struct {
    uint64_t blocknr;                /* final on-disk block number */
    const uint8_t *data;             /* block_size bytes */
} hype_jbd2_block_t;

/*
 * Opens the journal whose blocks are `map` (the resolved journal inode).
 * Validates the superblock and requires the journal empty. Returns 0, or -1
 * on any refusal above.
 */
int hype_jbd2_open(hype_jbd2_t *j, hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                   uint32_t block_size, const hype_file_rmap_t *map);

/*
 * #496: idempotently sets JBD2_FEATURE_INCOMPAT_64BIT on the journal
 * superblock -- a no-op if already set. Only valid on an empty journal (the
 * only state hype_jbd2_open ever accepts), so this is a direct write, not a
 * transaction: a crash right after it leaves an empty journal that simply
 * declares wide tags, exactly the state a real mount would leave it in.
 * Returns 0 or -1 on I/O failure.
 */
int hype_jbd2_upgrade_64bit(hype_jbd2_t *j);

/*
 * Commits one bounded transaction: descriptor(s) + images + commit block,
 * then exposes it via the journal superblock. After 0 is returned, a crash
 * at ANY later point replays these images. Returns -1 on I/O failure or a
 * transaction over HYPE_JBD2_MAX_BLOCKS.
 */
int hype_jbd2_commit(hype_jbd2_t *j, const hype_jbd2_block_t *blocks, unsigned count);

/*
 * Declares the committed transaction fully written to its final locations:
 * the journal superblock returns to empty and the sequence advances. Only
 * valid after a successful hype_jbd2_commit. Returns 0 or -1.
 */
int hype_jbd2_checkpoint(hype_jbd2_t *j);

#endif /* HYPE_CORE_JBD2_H */
