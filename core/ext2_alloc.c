#include "ext.h"
#include "lebytes.h"

/* #384: the ext2 allocating writer. See ext.h for the contract and plan.md
 * §10 decision 29 for the ordering rules this implements. */

#define SECSZ HYPE_BLK_SECTOR_SIZE

/* superblock offsets (byte 1024 on media) */
#define SB_INODES_COUNT 0x00u
#define SB_BLOCKS_COUNT 0x04u
#define SB_FREE_BLOCKS 0x0Cu
#define SB_FIRST_DATA_BLOCK 0x14u
#define SB_LOG_BLOCK_SIZE 0x18u
#define SB_BLOCKS_PER_GROUP 0x20u
#define SB_INODES_PER_GROUP 0x28u
#define SB_MAGIC 0x38u
#define SB_STATE 0x3Au
#define SB_REV_LEVEL 0x4Cu
#define SB_FEATURE_COMPAT 0x5Cu
#define SB_FEATURE_INCOMPAT 0x60u
#define SB_FEATURE_RO_COMPAT 0x64u
#define SB_INODE_SIZE 0x58u

#define EXT_MAGIC 0xEF53u
#define STATE_VALID 0x0001u
#define STATE_ERROR 0x0002u
#define COMPAT_HAS_JOURNAL 0x0004u
#define INCOMPAT_FILETYPE 0x0002u
#define RO_SPARSE_SUPER 0x0001u
#define RO_LARGE_FILE 0x0002u

/* group descriptor (32 bytes, ext2) */
#define GD_BLOCK_BITMAP 0x00u
#define GD_FREE_BLOCKS 0x0Cu
#define GD_INODE_TABLE 0x08u

/* inode offsets */
#define IN_MODE 0x00u
#define IN_CTIME 0x0Cu
#define IN_MTIME 0x10u
#define IN_BLOCKS 0x1Cu /* 512-byte sectors, indirection blocks included */
#define IN_FLAGS 0x20u
#define IN_BLOCK 0x28u
#define FL_EXTENTS 0x00080000u
#define MODE_FMT 0xF000u
#define MODE_REG 0x8000u

/*
 * Per-call transaction bound (§10 decision 29 / the #385 credit-bound rule
 * applied to ext2): one write_at may allocate at most this many blocks (data
 * + indirection). A larger span is refused up front; the caller loops.
 */
#define EXT2_ALLOC_MAX 256u

typedef struct {
    uint64_t block;        /* the allocated block */
    uint64_t parent_byte;  /* media byte of the 4-byte pointer that exposes it; 0 = inode-rooted (undone via inode non-write) */
} undo_t;

typedef struct {
    hype_ext2_wfile_t *f;
    undo_t undo[EXT2_ALLOC_MAX];
    unsigned undo_count;
    uint8_t inode[128];  /* in-RAM inode image, committed once at the end */
    int inode_dirty;
    uint64_t sb_free_delta; /* blocks claimed this transaction */
} txn_t;

static void bzero8(uint8_t *p, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) p[i] = 0;
}
static void bcopy8(uint8_t *dst, const uint8_t *src, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}

/* read-modify-write `len` (<= 512, non-straddling) bytes at media byte `at` */
static int media_rmw(hype_ext2_wfile_t *f, uint64_t at, const uint8_t *src, unsigned len) {
    uint8_t sec[SECSZ];
    if (f->read(f->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    bcopy8(sec + at % SECSZ, src, len);
    return f->write(f->ctx, at / SECSZ, 1u, sec);
}

static int media_read(hype_ext2_wfile_t *f, uint64_t at, uint8_t *dst, unsigned len) {
    uint8_t sec[SECSZ];
    if (f->read(f->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    bcopy8(dst, sec + at % SECSZ, len);
    return 0;
}

/* ---- superblock state + free count ---- */

static int sb_update(hype_ext2_wfile_t *f, int dirty, int64_t free_delta) {
    uint8_t sb[1024];
    uint32_t freeb;
    uint16_t state;
    if (f->read(f->ctx, 2u, 2u, sb) != 0) return -1;
    state = hype_rd16(sb + SB_STATE);
    state = dirty ? (uint16_t)(state & ~STATE_VALID) : (uint16_t)(state | STATE_VALID);
    hype_wr16(sb + SB_STATE, state);
    freeb = hype_rd32(sb + SB_FREE_BLOCKS);
    freeb = (uint32_t)((int64_t)freeb + free_delta);
    hype_wr32(sb + SB_FREE_BLOCKS, freeb);
    return f->write(f->ctx, 2u, 2u, sb);
}

/* ---- group descriptors + bitmaps ---- */

static uint64_t gd_byte(const hype_ext2_wfile_t *f, uint32_t group) {
    return (uint64_t)(f->first_data_block + 1u) * f->block_size + (uint64_t)group * 32u;
}

static int gd_free_adjust(hype_ext2_wfile_t *f, uint32_t group, int delta) {
    uint8_t sec[SECSZ];
    uint64_t at = gd_byte(f, group);
    uint16_t freeb;
    if (f->read(f->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    freeb = hype_rd16(sec + at % SECSZ + GD_FREE_BLOCKS);
    if (delta < 0 && freeb == 0u) return -1;
    hype_wr16(sec + at % SECSZ + GD_FREE_BLOCKS, (uint16_t)((int)freeb + delta));
    return f->write(f->ctx, at / SECSZ, 1u, sec);
}

static int gd_bitmap_block(hype_ext2_wfile_t *f, uint32_t group, uint64_t *out) {
    uint8_t sec[SECSZ];
    uint64_t at = gd_byte(f, group);
    if (f->read(f->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    *out = hype_rd32(sec + at % SECSZ + GD_BLOCK_BITMAP);
    if (*out == 0u || *out >= f->blocks_count) return -1;
    return 0;
}

/* Set (val=1) or clear the bitmap bit of block `blk`. When setting, the bit
 * must currently be clear -- a set bit means the "free" block is already
 * owned, i.e. the volume lies about its own allocation: refuse rather than
 * cross-link. */
static int bitmap_flip(hype_ext2_wfile_t *f, uint64_t blk, int val) {
    uint32_t group = (uint32_t)((blk - f->first_data_block) / f->blocks_per_group);
    uint32_t bit = (uint32_t)((blk - f->first_data_block) % f->blocks_per_group);
    uint64_t bmb, at;
    uint8_t sec[SECSZ];
    uint8_t mask = (uint8_t)(1u << (bit % 8u));

    if (gd_bitmap_block(f, group, &bmb) != 0) return -1;
    at = bmb * f->block_size + bit / 8u;
    if (f->read(f->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    if (val) {
        if (sec[at % SECSZ] & mask) return -1; /* multiply-referenced: refuse */
        sec[at % SECSZ] |= mask;
    } else {
        sec[at % SECSZ] &= (uint8_t)~mask;
    }
    return f->write(f->ctx, at / SECSZ, 1u, sec);
}

/* First-fit scan for a free block, starting at `near`'s group. Returns 0 and
 * the block in *out, -1 when the volume is full or unreadable. */
static int find_free(hype_ext2_wfile_t *f, uint64_t near, uint64_t *out) {
    uint32_t start_group =
        (near >= f->first_data_block)
            ? (uint32_t)((near - f->first_data_block) / f->blocks_per_group) % f->groups
            : 0u;
    uint32_t gi;

    for (gi = 0; gi < f->groups; gi++) {
        uint32_t group = (start_group + gi) % f->groups;
        uint64_t bmb;
        uint64_t base = (uint64_t)group * f->blocks_per_group + f->first_data_block;
        uint32_t in_group = f->blocks_per_group;
        uint32_t b;
        uint8_t sec[SECSZ];
        uint64_t cached = ~0ull;

        if (base >= f->blocks_count) break;
        if (in_group > f->blocks_count - base) in_group = (uint32_t)(f->blocks_count - base);
        if (gd_bitmap_block(f, group, &bmb) != 0) return -1;
        for (b = 0; b < in_group; b++) {
            uint64_t at = bmb * f->block_size + b / 8u;
            if (at / SECSZ != cached) {
                if (f->read(f->ctx, at / SECSZ, 1u, sec) != 0) return -1;
                cached = at / SECSZ;
            }
            if (!(sec[at % SECSZ] & (1u << (b % 8u)))) {
                *out = base + b;
                return 0;
            }
        }
    }
    return -1; /* full */
}

/* Claim one free block near `near`: bitmap bit + group and (deferred)
 * superblock counters. Records it in the undo log. */
static int claim_block(txn_t *t, uint64_t near, uint64_t *out) {
    hype_ext2_wfile_t *f = t->f;
    uint64_t blk;
    uint32_t group;
    if (t->undo_count >= EXT2_ALLOC_MAX) return -1; /* per-call bound */
    if (find_free(f, near, &blk) != 0) return -1;
    if (bitmap_flip(f, blk, 1) != 0) return -1;
    group = (uint32_t)((blk - f->first_data_block) / f->blocks_per_group);
    if (gd_free_adjust(f, group, -1) != 0) {
        (void)bitmap_flip(f, blk, 0);
        return -1;
    }
    t->undo[t->undo_count].block = blk;
    t->undo[t->undo_count].parent_byte = 0;
    t->undo_count++;
    t->sb_free_delta++;
    *out = blk;
    return 0;
}

/* Zero a whole block on media. */
static int block_zero(hype_ext2_wfile_t *f, uint64_t blk) {
    static const uint8_t z[SECSZ];
    uint32_t s;
    for (s = 0; s < f->spb; s++) {
        if (f->write(f->ctx, blk * f->spb + s, 1u, z) != 0) return -1;
    }
    return 0;
}

/* Roll the transaction back: unhook published pointers, free claimed blocks,
 * restore counters, and -- only if all of that landed -- mark clean again. */
static int txn_rollback(txn_t *t) {
    hype_ext2_wfile_t *f = t->f;
    int ok = 0;
    unsigned i;
    static const uint8_t zero4[4] = {0, 0, 0, 0};

    for (i = t->undo_count; i > 0u; i--) {
        const undo_t *u = &t->undo[i - 1u];
        if (u->parent_byte != 0u) {
            ok |= media_rmw(f, u->parent_byte, zero4, 4u);
        }
        ok |= bitmap_flip(f, u->block, 0);
        ok |= gd_free_adjust(
            f, (uint32_t)((u->block - f->first_data_block) / f->blocks_per_group), 1);
    }
    if (ok == 0) ok |= sb_update(f, 0, 0);
    return ok ? -1 : 0;
}

/*
 * Ensure logical block `lb` of the file is mapped, allocating the data block
 * and any missing indirection blocks. The block's CONTENT (zeros overlaid
 * with the caller's bytes for the part the write covers) is written before
 * the pointer exposing it. Direct and in-inode root pointers live in the
 * in-RAM inode image and are committed once, at the end of the write.
 */
static int map_block(txn_t *t, uint64_t lb, uint64_t *out_blk) {
    hype_ext2_wfile_t *f = t->f;
    uint32_t ppb = f->block_size / 4u;
    uint64_t near = 0;
    uint32_t path[3];  /* pointer indices per level, leaf-last */
    uint32_t levels = 0;
    uint32_t root_slot;
    uint64_t node;     /* current pointer block (0 = missing) */
    uint32_t li;
    uint8_t ptr4[4];

    /* decompose lb into the classic map shape */
    if (lb < 12u) {
        root_slot = (uint32_t)lb;
        levels = 0;
    } else {
        uint64_t r = lb - 12u;
        if (r < ppb) {
            root_slot = 12u;
            levels = 1;
            path[0] = (uint32_t)r;
        } else if ((r -= ppb) < (uint64_t)ppb * ppb) {
            root_slot = 13u;
            levels = 2;
            path[0] = (uint32_t)(r / ppb);
            path[1] = (uint32_t)(r % ppb);
        } else if ((r -= (uint64_t)ppb * ppb) < (uint64_t)ppb * ppb * ppb) {
            root_slot = 14u;
            levels = 3;
            path[0] = (uint32_t)(r / ((uint64_t)ppb * ppb));
            path[1] = (uint32_t)((r / ppb) % ppb);
            path[2] = (uint32_t)(r % ppb);
        } else {
            return -1;
        }
    }
    near = f->inode_byte / SECSZ / f->spb; /* start the search near the inode's group */

    /* the in-inode root pointer */
    node = hype_rd32(t->inode + IN_BLOCK + root_slot * 4u);
    if (levels == 0u) {
        if (node != 0u) {
            *out_blk = node; /* already mapped (raced with our own refresh) */
            return 0;
        }
        if (claim_block(t, near, out_blk) != 0) return -1;
        /* content is written by the caller BEFORE the inode commit exposes
         * this pointer */
        hype_wr32(t->inode + IN_BLOCK + root_slot * 4u, (uint32_t)*out_blk);
        hype_wr32(t->inode + IN_BLOCKS, hype_rd32(t->inode + IN_BLOCKS) + f->spb);
        t->inode_dirty = 1;
        return 0;
    }

    if (node == 0u) {
        /* missing root pointer block: claim + zero, expose via the inode */
        uint64_t nb;
        if (claim_block(t, near, &nb) != 0) return -1;
        if (block_zero(f, nb) != 0) return -1;
        hype_wr32(t->inode + IN_BLOCK + root_slot * 4u, (uint32_t)nb);
        hype_wr32(t->inode + IN_BLOCKS, hype_rd32(t->inode + IN_BLOCKS) + f->spb);
        t->inode_dirty = 1;
        node = nb;
    }

    /* interior pointer blocks */
    for (li = 0; li + 1u < levels; li++) {
        uint64_t entry_byte = node * f->block_size + (uint64_t)path[li] * 4u;
        uint64_t child;
        if (media_read(f, entry_byte, ptr4, 4u) != 0) return -1;
        child = hype_rd32(ptr4);
        if (child == 0u) {
            if (claim_block(t, node, &child) != 0) return -1;
            if (block_zero(f, child) != 0) return -1;
            /* expose the fully-zeroed child in its parent -- and remember
             * where, so rollback can unhook it */
            hype_wr32(ptr4, (uint32_t)child);
            if (media_rmw(f, entry_byte, ptr4, 4u) != 0) return -1;
            t->undo[t->undo_count - 1u].parent_byte = entry_byte;
            hype_wr32(t->inode + IN_BLOCKS, hype_rd32(t->inode + IN_BLOCKS) + f->spb);
            t->inode_dirty = 1;
        } else if (child >= f->blocks_count) {
            return -1;
        }
        node = child;
    }

    /* the leaf entry: the data block itself */
    {
        uint64_t entry_byte = node * f->block_size + (uint64_t)path[levels - 1u] * 4u;
        uint64_t blk;
        if (media_read(f, entry_byte, ptr4, 4u) != 0) return -1;
        blk = hype_rd32(ptr4);
        if (blk != 0u) {
            *out_blk = blk;
            return 0;
        }
        if (claim_block(t, node, &blk) != 0) return -1;
        *out_blk = blk;
        /* the caller writes the content NOW; the leaf pointer is published
         * only after it lands (see write_at) */
        t->undo[t->undo_count - 1u].parent_byte = entry_byte; /* for rollback bookkeeping */
        hype_wr32(t->inode + IN_BLOCKS, hype_rd32(t->inode + IN_BLOCKS) + f->spb);
        t->inode_dirty = 1;
        /* signal "publish me" to the caller via parent_byte */
        return 1; /* 1 == fresh: content + publish needed */
    }
}

/* ---- open / read / write ---- */

int hype_ext2_open_rw(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, hype_ext2_wfile_t *out) {
    uint8_t sb[1024];
    uint32_t incompat, compat, rocompat, log_bs, rev, inode_size, inodes_per_group;
    uint16_t state;

    if (read == 0 || write == 0) return -1;
    if (read(ctx, 2u, 2u, sb) != 0) return -1;
    if (hype_rd16(sb + SB_MAGIC) != EXT_MAGIC) return -1;

    compat = hype_rd32(sb + SB_FEATURE_COMPAT);
    incompat = hype_rd32(sb + SB_FEATURE_INCOMPAT);
    rocompat = hype_rd32(sb + SB_FEATURE_RO_COMPAT);
    if (compat & COMPAT_HAS_JOURNAL) return -1;       /* ext3/4: #385's work */
    if (incompat & ~INCOMPAT_FILETYPE) return -1;     /* ext2 shapes only */
    if (rocompat & ~(RO_SPARSE_SUPER | RO_LARGE_FILE)) return -1; /* incl. any checksums */
    state = hype_rd16(sb + SB_STATE);
    if ((state & STATE_VALID) == 0u || (state & STATE_ERROR) != 0u) return -1;
    log_bs = hype_rd32(sb + SB_LOG_BLOCK_SIZE);
    if (log_bs > 2u) return -1;

    out->read = read;
    out->write = write;
    out->ctx = ctx;
    out->block_size = 1024u << log_bs;
    out->spb = out->block_size / SECSZ;
    out->blocks_count = hype_rd32(sb + SB_BLOCKS_COUNT);
    out->blocks_per_group = hype_rd32(sb + SB_BLOCKS_PER_GROUP);
    out->first_data_block = hype_rd32(sb + SB_FIRST_DATA_BLOCK);
    out->mtime = 0;
    if (out->blocks_per_group == 0u || out->blocks_count < 2u) return -1;
    out->groups = (uint32_t)((out->blocks_count - out->first_data_block +
                              out->blocks_per_group - 1u) /
                             out->blocks_per_group);
    inodes_per_group = hype_rd32(sb + SB_INODES_PER_GROUP);
    rev = hype_rd32(sb + SB_REV_LEVEL);
    inode_size = (rev == 0u) ? 128u : (uint32_t)hype_rd16(sb + SB_INODE_SIZE);
    if (inodes_per_group == 0u || inode_size < 128u) return -1;

    /* resolve the path to its sparse map + locate the inode on media */
    if (hype_ext_resolve_rmap(read, ctx, path, &out->map) != 0) return -1;
    out->size_bytes = out->map.size_bytes;
    {
        /* re-walk to the inode number (resolve_rmap validates everything;
         * this only needs the number for the media location) */
        uint8_t sec[SECSZ];
        uint32_t ino = 0;
        uint64_t gdb, table;
        uint32_t group, index;
        if (hype_ext_resolve_ino(read, ctx, path, &ino) != 0) return -1;
        out->ino = ino;
        group = (ino - 1u) / inodes_per_group;
        index = (ino - 1u) % inodes_per_group;
        gdb = gd_byte(out, group) + GD_INODE_TABLE;
        if (read(ctx, gdb / SECSZ, 1u, sec) != 0) return -1;
        table = hype_rd32(sec + gdb % SECSZ);
        if (table == 0u || table >= out->blocks_count) return -1;
        out->inode_byte = table * out->block_size + (uint64_t)index * inode_size;
    }
    /* an extent-mapped file is #385's problem, not this allocator's */
    {
        uint8_t ib[128];
        if (media_read(out, out->inode_byte, ib, 64u) != 0 ||
            media_read(out, out->inode_byte + 64u, ib + 64u, 64u) != 0) {
            return -1;
        }
        if (hype_rd32(ib + IN_FLAGS) & FL_EXTENTS) return -1;
        if ((hype_rd16(ib + IN_MODE) & MODE_FMT) != MODE_REG) return -1;
    }
    return 0;
}

void hype_ext2_set_time(hype_ext2_wfile_t *f, uint32_t unix_seconds) {
    f->mtime = unix_seconds;
}

int hype_ext2_read_at(hype_ext2_wfile_t *f, uint64_t offset, void *out, unsigned int len) {
    return hype_file_rmap_read_at(&f->map, f->read, f->ctx, offset, out, len);
}

int hype_ext2_write_at(hype_ext2_wfile_t *f, uint64_t offset, const void *data,
                       unsigned int len) {
    static txn_t t; /* >4 KiB (undo log + inode image): static per the
                     * __chkstk rule; the writer is BSP-serialized */
    const uint8_t *src = (const uint8_t *)data;
    uint64_t end = offset + len;
    uint64_t bs;
    int allocated = 0;

    if (len == 0u) return 0;
    if (end < offset || end > f->size_bytes) return -1;
    bs = f->block_size;

    /* fast path: the whole span is DATA -- a pure in-place data write */
    {
        uint64_t probe = offset;
        int all_data = 1;
        while (probe < end) {
            hype_range_kind_t kind;
            uint64_t lba, run;
            uint32_t head;
            if (hype_file_rmap_locate(&f->map, probe, &kind, &lba, &head, &run) != 0) return -1;
            if (kind != HYPE_RANGE_DATA) {
                all_data = 0;
                break;
            }
            probe += run;
        }
        if (all_data) {
            return hype_file_rmap_write_at(&f->map, f->read, f->write, f->ctx, offset, src, len);
        }
    }

    /* allocation needed: bound the transaction up front */
    {
        uint64_t first_lb = offset / bs, last_lb = (end - 1u) / bs;
        if (last_lb - first_lb + 1u > EXT2_ALLOC_MAX / 2u) {
            return -1; /* over the per-call bound: the caller must split */
        }
    }

    t.f = f;
    t.undo_count = 0;
    t.inode_dirty = 0;
    t.sb_free_delta = 0;
    if (media_read(f, f->inode_byte, t.inode, 64u) != 0 ||
        media_read(f, f->inode_byte + 64u, t.inode + 64u, 64u) != 0) {
        return -1;
    }

    if (sb_update(f, 1, 0) != 0) return -1; /* volume marked dirty first */

    {
        uint64_t pos = offset;
        while (pos < end) {
            uint64_t lb = pos / bs;
            uint64_t in_blk = pos % bs;
            uint64_t n = bs - in_blk;
            hype_range_kind_t kind;
            uint64_t lba, run;
            uint32_t head;
            if (n > end - pos) n = end - pos;

            if (hype_file_rmap_locate(&f->map, pos, &kind, &lba, &head, &run) != 0) {
                goto fail;
            }
            if (kind == HYPE_RANGE_DATA) {
                uint64_t m = (run < n) ? run : n;
                if (hype_file_rmap_write_at(&f->map, f->read, f->write, f->ctx, pos, src, (unsigned int)m) != 0) {
                    goto fail;
                }
                pos += m;
                src += m;
                continue;
            }

            /* a hole: allocate this block, write zeros+data, then publish */
            {
                uint64_t blk = 0;
                uint64_t entry_byte = 0;
                int rc = map_block(&t, lb, &blk);
                if (rc < 0) goto fail;
                if (rc == 1) {
                    entry_byte = t.undo[t.undo_count - 1u].parent_byte;
                    /* leaf pointer publish deferred until content lands */
                    t.undo[t.undo_count - 1u].parent_byte = 0;
                }
                allocated = 1;
                /* content: zeros around the covered slice, one pass */
                {
                    uint8_t sec[SECSZ];
                    uint32_t s;
                    for (s = 0; s < f->spb; s++) {
                        uint64_t sec_start = (uint64_t)s * SECSZ;
                        uint64_t sec_end = sec_start + SECSZ;
                        uint64_t blk_off = lb * bs;
                        bzero8(sec, SECSZ);
                        /* overlay the written slice where it intersects */
                        if (blk_off + sec_end > offset && blk_off + sec_start < end) {
                            uint64_t from = (offset > blk_off + sec_start)
                                                ? offset - blk_off - sec_start
                                                : 0u;
                            uint64_t to = (end < blk_off + sec_end)
                                              ? end - blk_off - sec_start
                                              : SECSZ;
                            bcopy8(sec + from,
                                   (const uint8_t *)data + (blk_off + sec_start + from - offset),
                                   (unsigned)(to - from));
                        }
                        if (f->write(f->ctx, blk * f->spb + s, 1u, sec) != 0) goto fail;
                    }
                }
                if (rc == 1 && entry_byte != 0u) {
                    /* content is durable in order; publish the leaf pointer */
                    uint8_t p4[4];
                    hype_wr32(p4, (uint32_t)blk);
                    if (media_rmw(f, entry_byte, p4, 4u) != 0) goto fail;
                    t.undo[t.undo_count - 1u].parent_byte = entry_byte;
                }
                pos = (lb + 1u) * bs;
                if (pos > end) pos = end;
                src = (const uint8_t *)data + (pos - offset);
            }
        }
    }

    /* commit: inode (pointers, i_blocks, times), superblock counters + clean */
    if (t.inode_dirty) {
        if (f->mtime != 0u) {
            hype_wr32(t.inode + IN_MTIME, f->mtime);
            hype_wr32(t.inode + IN_CTIME, f->mtime);
        }
        if (media_rmw(f, f->inode_byte, t.inode, 64u) != 0 ||
            media_rmw(f, f->inode_byte + 64u, t.inode + 64u, 64u) != 0) {
            goto fail;
        }
    }
    if (sb_update(f, 0, -(int64_t)t.sb_free_delta) != 0) return -1;

    /* refresh the map so subsequent calls see the new DATA ranges */
    if (allocated) {
        if (hype_ext_map_ino_rmap(f->read, f->ctx, f->ino, &f->map) != 0) return -1;
    }
    return 0;

fail:
    (void)txn_rollback(&t);
    return -1;
}
