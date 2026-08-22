#include "ext.h"
#include "ext_namespace_impl.h"
#include "ext_dirent.h"
#include "lebytes.h"
#include "file_range.h"

/*
 * #498: ext2 (no journal) namespace mutation -- create/unlink/mkdir/rmdir/
 * rename. Direct ordered writes, no checksums (RO_COMPAT_METADATA_CSUM is
 * refused at open, same as core/ext2_alloc.c): plan.md §10 decision 29's
 * discipline -- superblock marked dirty before any structural change, every
 * claim/free goes through the bitmap + group + superblock counters together,
 * content before the pointer that exposes it, superblock restored clean
 * last. See core/ext_namespace.h for the shared contract (htree refusal,
 * rename no-clobber) and core/ext_dirent.h for the directory-block format
 * both this file and core/extj_namespace.c share.
 */

#define SECSZ HYPE_BLK_SECTOR_SIZE
#define NS2_MAX_NAME 255u
#define NS2_MAX_PATH 512u

/* superblock (ext2: always 32-byte group descriptors -- 64BIT/METADATA_CSUM
 * require extents, which ext2 never has) */
#define SB_INODES_COUNT 0x00u
#define SB_BLOCKS_COUNT 0x04u
#define SB_FREE_BLOCKS 0x0Cu
#define SB_FREE_INODES 0x10u
#define SB_FIRST_DATA_BLOCK 0x14u
#define SB_LOG_BLOCK_SIZE 0x18u
#define SB_BLOCKS_PER_GROUP 0x20u
#define SB_INODES_PER_GROUP 0x28u
#define SB_MAGIC 0x38u
#define SB_STATE 0x3Au
#define SB_REV_LEVEL 0x4Cu
#define SB_FIRST_INO 0x54u
#define SB_INODE_SIZE 0x58u
#define SB_FEATURE_COMPAT 0x5Cu
#define SB_FEATURE_INCOMPAT 0x60u
#define SB_FEATURE_RO_COMPAT 0x64u

#define EXT_MAGIC 0xEF53u
#define STATE_VALID 0x0001u
#define STATE_ERROR 0x0002u
#define COMPAT_HAS_JOURNAL 0x0004u
#define INCOMPAT_FILETYPE 0x0002u
#define RO_SPARSE_SUPER 0x0001u
#define RO_LARGE_FILE 0x0002u

#define GD_BLOCK_BITMAP 0x00u
#define GD_INODE_BITMAP 0x04u
#define GD_INODE_TABLE 0x08u
#define GD_FREE_BLOCKS 0x0Cu
#define GD_FREE_INODES 0x0Eu
#define GD_USED_DIRS 0x10u

#define IN_MODE 0x00u
#define IN_SIZE_LO 0x04u
#define IN_ATIME 0x08u
#define IN_CTIME 0x0Cu
#define IN_MTIME 0x10u
#define IN_DTIME 0x14u
#define IN_LINKS_COUNT 0x1Au
#define IN_BLOCKS 0x1Cu
#define IN_FLAGS 0x20u
#define IN_BLOCK 0x28u
#define IN_SIZE_HIGH 0x6Cu
#define IN_CORE 128u

#define MODE_FMT 0xF000u
#define MODE_DIR 0x4000u
#define MODE_REG 0x8000u
#define MODE_DIR_DEFAULT 0x41EDu /* 040755 */
#define MODE_REG_DEFAULT 0x81A4u /* 0100644 */

#define FL_INDEX 0x00001000u /* EXT4_INDEX_FL: htree. dir_index is a COMPAT
                              * (not RO_COMPAT) feature, on by default even
                              * for ext2 on this project's reference e2fsprogs
                              * config -- so an ext2 directory CAN be
                              * htree-indexed too, and the refusal below is
                              * unconditional, not ext4-only. */

typedef struct {
    hype_blk_read_fn read;
    hype_blk_write_fn write;
    void *ctx;
    uint32_t block_size, spb;
    uint64_t blocks_count;
    uint32_t blocks_per_group;
    uint32_t first_data_block;
    uint32_t groups;
    uint32_t inode_size;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t first_ino;
} v2_t;

static void bzero8(uint8_t *p, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) p[i] = 0;
}
static void bcopy8(uint8_t *d, const uint8_t *s, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static int media_read(v2_t *v, uint64_t at, uint8_t *dst, unsigned len) {
    uint8_t sec[SECSZ];
    if (v->read(v->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    bcopy8(dst, sec + at % SECSZ, len);
    return 0;
}
static int media_rmw(v2_t *v, uint64_t at, const uint8_t *src, unsigned len) {
    uint8_t sec[SECSZ];
    if (v->read(v->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    bcopy8(sec + at % SECSZ, src, len);
    return v->write(v->ctx, at / SECSZ, 1u, sec);
}
static int block_read(v2_t *v, uint64_t blk, uint8_t *dst) {
    uint32_t s;
    for (s = 0; s < v->spb; s++) {
        if (v->read(v->ctx, blk * v->spb + s, 1u, dst + (uint64_t)s * SECSZ) != 0) return -1;
    }
    return 0;
}
static int block_write(v2_t *v, uint64_t blk, const uint8_t *src) {
    uint32_t s;
    for (s = 0; s < v->spb; s++) {
        if (v->write(v->ctx, blk * v->spb + s, 1u, src + (uint64_t)s * SECSZ) != 0) return -1;
    }
    return 0;
}
static int block_zero(v2_t *v, uint64_t blk) {
    static const uint8_t z[SECSZ];
    uint32_t s;
    for (s = 0; s < v->spb; s++) {
        if (v->write(v->ctx, blk * v->spb + s, 1u, z) != 0) return -1;
    }
    return 0;
}

static int v2_open(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx, v2_t *v) {
    uint8_t sb[1024];
    uint32_t incompat, compat, rocompat, log_bs, rev;
    uint16_t state;

    if (read == 0 || write == 0) return -1;
    if (read(ctx, 2u, 2u, sb) != 0) return -1;
    if (hype_rd16(sb + SB_MAGIC) != EXT_MAGIC) return -1;
    compat = hype_rd32(sb + SB_FEATURE_COMPAT);
    incompat = hype_rd32(sb + SB_FEATURE_INCOMPAT);
    rocompat = hype_rd32(sb + SB_FEATURE_RO_COMPAT);
    if (compat & COMPAT_HAS_JOURNAL) return -1; /* ext3/4: core/extj_namespace.c's job */
    if (!(incompat & INCOMPAT_FILETYPE) || (incompat & ~INCOMPAT_FILETYPE)) {
        return -1; /* this writer only ever emits filetype-shaped dirents */
    }
    if (rocompat & ~(RO_SPARSE_SUPER | RO_LARGE_FILE)) return -1; /* incl. any checksum feature */
    state = hype_rd16(sb + SB_STATE);
    if ((state & STATE_VALID) == 0u || (state & STATE_ERROR) != 0u) return -1;
    log_bs = hype_rd32(sb + SB_LOG_BLOCK_SIZE);
    if (log_bs > 2u) return -1;

    v->read = read;
    v->write = write;
    v->ctx = ctx;
    v->block_size = 1024u << log_bs;
    v->spb = v->block_size / SECSZ;
    v->blocks_count = hype_rd32(sb + SB_BLOCKS_COUNT);
    v->blocks_per_group = hype_rd32(sb + SB_BLOCKS_PER_GROUP);
    v->first_data_block = hype_rd32(sb + SB_FIRST_DATA_BLOCK);
    v->inodes_count = hype_rd32(sb + SB_INODES_COUNT);
    v->inodes_per_group = hype_rd32(sb + SB_INODES_PER_GROUP);
    rev = hype_rd32(sb + SB_REV_LEVEL);
    v->inode_size = (rev == 0u) ? 128u : (uint32_t)hype_rd16(sb + SB_INODE_SIZE);
    v->first_ino = (rev == 0u) ? 11u : hype_rd32(sb + SB_FIRST_INO);
    if (v->blocks_per_group == 0u || v->inodes_per_group == 0u || v->blocks_count < 2u ||
        v->inode_size < 128u || v->inodes_count == 0u) {
        return -1;
    }
    if (v->first_ino < 1u || v->first_ino > v->inodes_count) return -1;
    v->groups = (uint32_t)((v->blocks_count - v->first_data_block + v->blocks_per_group - 1u) /
                           v->blocks_per_group);
    return 0;
}

/* ---- superblock / group-descriptor counters ---- */

static uint64_t gd_byte(const v2_t *v, uint32_t group) {
    return (uint64_t)(v->first_data_block + 1u) * v->block_size + (uint64_t)group * 32u;
}

static int sb_set_dirty(v2_t *v, int dirty) {
    uint8_t sb[1024];
    uint16_t state;
    if (v->read(v->ctx, 2u, 2u, sb) != 0) return -1;
    state = hype_rd16(sb + SB_STATE);
    state = dirty ? (uint16_t)(state & ~STATE_VALID) : (uint16_t)(state | STATE_VALID);
    hype_wr16(sb + SB_STATE, state);
    return v->write(v->ctx, 2u, 2u, sb);
}

static int sb_adjust32(v2_t *v, uint32_t field_off, int64_t delta) {
    uint8_t sb[1024];
    uint32_t val;
    if (v->read(v->ctx, 2u, 2u, sb) != 0) return -1;
    val = hype_rd32(sb + field_off);
    hype_wr32(sb + field_off, (uint32_t)((int64_t)val + delta));
    return v->write(v->ctx, 2u, 2u, sb);
}

static int gd_adjust16(v2_t *v, uint32_t group, uint32_t field_off, int delta) {
    uint8_t sec[SECSZ];
    uint64_t at = gd_byte(v, group) + field_off;
    uint16_t val;
    if (v->read(v->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    val = hype_rd16(sec + at % SECSZ);
    if (delta < 0 && (int)val < -delta) return -1;
    hype_wr16(sec + at % SECSZ, (uint16_t)((int)val + delta));
    return v->write(v->ctx, at / SECSZ, 1u, sec);
}

static int gd_bitmap_block(v2_t *v, uint32_t group, uint32_t field_off, uint64_t *out) {
    uint8_t sec[SECSZ];
    uint64_t at = gd_byte(v, group) + field_off;
    if (v->read(v->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    *out = hype_rd32(sec + at % SECSZ);
    if (*out == 0u || *out >= v->blocks_count) return -1;
    return 0;
}

static int bitmap_flip(v2_t *v, uint64_t bmb, uint32_t bit, int val) {
    uint64_t at = bmb * v->block_size + bit / 8u;
    uint8_t sec[SECSZ];
    uint8_t mask = (uint8_t)(1u << (bit % 8u));
    if (v->read(v->ctx, at / SECSZ, 1u, sec) != 0) return -1;
    if (val) {
        if (sec[at % SECSZ] & mask) return -1; /* multiply-referenced: refuse */
        sec[at % SECSZ] |= mask;
    } else {
        sec[at % SECSZ] &= (uint8_t)~mask;
    }
    return v->write(v->ctx, at / SECSZ, 1u, sec);
}

/* ---- block + inode allocation (no undo log: a failure past a successful
 * claim leaves it allocated-but-unreferenced, which e2fsck reports and
 * repairs -- an accepted simplification for this slice; the happy-path bar
 * this ticket validates never exercises a mid-operation failure). ---- */

static int claim_block(v2_t *v, uint64_t near, uint64_t *out) {
    uint32_t start_group =
        (near >= v->first_data_block)
            ? (uint32_t)((near - v->first_data_block) / v->blocks_per_group) % v->groups
            : 0u;
    uint32_t gi;
    for (gi = 0; gi < v->groups; gi++) {
        uint32_t group = (start_group + gi) % v->groups;
        uint64_t bmb, base = (uint64_t)group * v->blocks_per_group + v->first_data_block;
        uint32_t in_group = v->blocks_per_group, b;
        uint8_t sec[SECSZ];
        uint64_t cached = ~0ull;
        if (base >= v->blocks_count) break;
        if (in_group > v->blocks_count - base) in_group = (uint32_t)(v->blocks_count - base);
        if (gd_bitmap_block(v, group, GD_BLOCK_BITMAP, &bmb) != 0) return -1;
        for (b = 0; b < in_group; b++) {
            uint64_t at = bmb * v->block_size + b / 8u;
            if (at / SECSZ != cached) {
                if (v->read(v->ctx, at / SECSZ, 1u, sec) != 0) return -1;
                cached = at / SECSZ;
            }
            if (!(sec[at % SECSZ] & (1u << (b % 8u)))) {
                if (bitmap_flip(v, bmb, b, 1) != 0) return -1;
                if (gd_adjust16(v, group, GD_FREE_BLOCKS, -1) != 0) {
                    (void)bitmap_flip(v, bmb, b, 0);
                    return -1;
                }
                if (sb_adjust32(v, SB_FREE_BLOCKS, -1) != 0) return -1;
                *out = base + b;
                return 0;
            }
        }
    }
    return -1; /* full */
}

static int claim_inode(v2_t *v, uint32_t *out_ino) {
    uint32_t group;
    for (group = 0; group < v->groups; group++) {
        uint64_t bmb;
        uint32_t b, floor = (group == 0u) ? (v->first_ino - 1u) : 0u;
        uint8_t sec[SECSZ];
        uint64_t cached = ~0ull;
        if (gd_bitmap_block(v, group, GD_INODE_BITMAP, &bmb) != 0) return -1;
        for (b = floor; b < v->inodes_per_group; b++) {
            uint32_t ino = group * v->inodes_per_group + b + 1u;
            uint64_t at;
            if (ino > v->inodes_count) break;
            at = bmb * v->block_size + b / 8u;
            if (at / SECSZ != cached) {
                if (v->read(v->ctx, at / SECSZ, 1u, sec) != 0) return -1;
                cached = at / SECSZ;
            }
            if (!(sec[at % SECSZ] & (1u << (b % 8u)))) {
                if (bitmap_flip(v, bmb, b, 1) != 0) return -1;
                if (gd_adjust16(v, group, GD_FREE_INODES, -1) != 0) {
                    (void)bitmap_flip(v, bmb, b, 0);
                    return -1;
                }
                if (sb_adjust32(v, SB_FREE_INODES, -1) != 0) return -1;
                *out_ino = ino;
                return 0;
            }
        }
    }
    return -1; /* full */
}

static int free_block(v2_t *v, uint64_t blk) {
    uint32_t group = (uint32_t)((blk - v->first_data_block) / v->blocks_per_group);
    uint32_t bit = (uint32_t)((blk - v->first_data_block) % v->blocks_per_group);
    uint64_t bmb;
    if (gd_bitmap_block(v, group, GD_BLOCK_BITMAP, &bmb) != 0) return -1;
    if (bitmap_flip(v, bmb, bit, 0) != 0) return -1;
    if (gd_adjust16(v, group, GD_FREE_BLOCKS, 1) != 0) return -1;
    return sb_adjust32(v, SB_FREE_BLOCKS, 1);
}

static int free_inode(v2_t *v, uint32_t ino, int is_dir) {
    uint32_t group = (ino - 1u) / v->inodes_per_group;
    uint32_t bit = (ino - 1u) % v->inodes_per_group;
    uint64_t bmb;
    if (gd_bitmap_block(v, group, GD_INODE_BITMAP, &bmb) != 0) return -1;
    if (bitmap_flip(v, bmb, bit, 0) != 0) return -1;
    if (gd_adjust16(v, group, GD_FREE_INODES, 1) != 0) return -1;
    if (is_dir && gd_adjust16(v, group, GD_USED_DIRS, -1) != 0) return -1;
    return sb_adjust32(v, SB_FREE_INODES, 1);
}

/* ---- inode table access (first IN_CORE bytes only -- ext2 has no
 * checksums and, on this project's reference config, no RO_COMPAT_EXTRA_ISIZE
 * either, so the tail of a >128-byte inode is inert and never inspected;
 * core/ext2_alloc.c already established leaving it untouched is fsck-clean) ---- */

static uint64_t inode_byte(v2_t *v, uint32_t ino, uint64_t *out_table) {
    uint32_t group = (ino - 1u) / v->inodes_per_group;
    uint32_t index = (ino - 1u) % v->inodes_per_group;
    uint8_t sec[SECSZ];
    uint64_t gdb = gd_byte(v, group) + GD_INODE_TABLE;
    uint64_t table;
    if (v->read(v->ctx, gdb / SECSZ, 1u, sec) != 0) return 0;
    table = hype_rd32(sec + gdb % SECSZ);
    if (table == 0u || table >= v->blocks_count) return 0;
    if (out_table != 0) *out_table = table;
    return table * v->block_size + (uint64_t)index * v->inode_size;
}

static int inode_read128(v2_t *v, uint32_t ino, uint8_t out[IN_CORE]) {
    uint64_t ib = inode_byte(v, ino, 0);
    if (ib == 0u) return -1;
    return media_read(v, ib, out, IN_CORE) == 0 ? 0 : -1;
}
static int inode_write128(v2_t *v, uint32_t ino, const uint8_t in[IN_CORE]) {
    uint64_t ib = inode_byte(v, ino, 0);
    if (ib == 0u) return -1;
    return media_rmw(v, ib, in, IN_CORE);
}

/* ---- directory content: enumerate a directory's blocks via the #384 sparse
 * map (never sparse in practice for a directory; a HOLE/UNWRITTEN range is
 * refused as a shape this writer does not understand) ---- */

#define NS2_MAX_DIR_BLOCKS 512u

static int dir_blocks(v2_t *v, uint32_t ino, uint64_t *out, unsigned *out_n, uint64_t *out_size) {
    hype_file_rmap_t map;
    unsigned r, n = 0;
    if (hype_ext_map_dir_ino_rmap(v->read, v->ctx, ino, &map) != 0) return -1;
    if (out_size != 0) *out_size = map.size_bytes;
    for (r = 0; r < map.count; r++) {
        uint64_t s;
        if (map.ranges[r].kind != HYPE_RANGE_DATA) return -1; /* sparse dir: unsupported shape */
        for (s = 0; s < map.ranges[r].sector_count; s += v->spb) {
            if (n >= NS2_MAX_DIR_BLOCKS) return -1;
            out[n++] = (map.ranges[r].start_lba + s) / v->spb;
        }
    }
    *out_n = n;
    return 0;
}

/* Appends exactly one freshly claimed+zeroed block as the directory's next
 * logical block, publishing it into the CLASSIC block map (direct or
 * single-indirect only -- a directory needing double/triple indirect to grow
 * is refused, an honest scope limit no real test in this ticket's bar
 * reaches). Updates the inode's size and block count. Returns the new
 * block's number, or ~0ull on error/refusal. */
static uint64_t dir_grow_one(v2_t *v, uint32_t dir_ino, uint64_t cur_blocks) {
    uint8_t in[IN_CORE];
    uint32_t ppb = v->block_size / 4u;
    uint64_t nb;
    uint64_t near;

    if (inode_read128(v, dir_ino, in) != 0) return ~0ull;
    near = inode_byte(v, dir_ino, 0) / SECSZ / v->spb;
    if (claim_block(v, near, &nb) != 0) return ~0ull;
    if (block_zero(v, nb) != 0) return ~0ull;

    if (cur_blocks < 12u) {
        hype_wr32(in + IN_BLOCK + (uint32_t)cur_blocks * 4u, (uint32_t)nb);
    } else if (cur_blocks - 12u < ppb) {
        uint32_t root = hype_rd32(in + IN_BLOCK + 12u * 4u);
        uint8_t p4[4];
        if (root == 0u) {
            uint64_t rb;
            if (claim_block(v, near, &rb) != 0) return ~0ull;
            if (block_zero(v, rb) != 0) return ~0ull;
            hype_wr32(in + IN_BLOCK + 12u * 4u, (uint32_t)rb);
            hype_wr32(in + IN_BLOCKS, hype_rd32(in + IN_BLOCKS) + v->spb);
            root = (uint32_t)rb;
        }
        hype_wr32(p4, (uint32_t)nb);
        if (media_rmw(v, (uint64_t)root * v->block_size + (cur_blocks - 12u) * 4u, p4, 4u) != 0) {
            return ~0ull;
        }
    } else {
        return ~0ull; /* double/triple indirect growth: out of this slice's scope */
    }
    hype_wr32(in + IN_BLOCKS, hype_rd32(in + IN_BLOCKS) + v->spb);
    hype_wr32(in + IN_SIZE_LO, (uint32_t)((cur_blocks + 1u) * v->block_size));
    if (inode_write128(v, dir_ino, in) != 0) return ~0ull;
    return nb;
}

/* ---- directory entry insert/remove/scan over a directory's real blocks ---- */

static int dir_find(v2_t *v, uint32_t dir_ino, const char *name, unsigned nlen,
                    uint32_t *out_ino) {
    uint64_t blocks[NS2_MAX_DIR_BLOCKS];
    unsigned n, i;
    uint8_t buf[4096];
    *out_ino = 0; /* deterministic "not found" for every caller that tests *out_ino directly */
    if (dir_blocks(v, dir_ino, blocks, &n, 0) != 0) return -1;
    for (i = 0; i < n; i++) {
        uint32_t off;
        if (block_read(v, blocks[i], buf) != 0) return -1;
        if (hype_extd_validate(buf, v->block_size, 0) != 0) return -1;
        if (hype_extd_find(buf, v->block_size, 0, name, nlen, &off, out_ino) == 1) return 1;
    }
    return 0;
}

static int dir_insert(v2_t *v, uint32_t dir_ino, const char *name, unsigned nlen, uint32_t ino,
                      uint8_t ftype) {
    uint64_t blocks[NS2_MAX_DIR_BLOCKS];
    unsigned n, i;
    uint8_t buf[4096];
    uint64_t nb, cur_size;

    if (dir_blocks(v, dir_ino, blocks, &n, &cur_size) != 0) return -1;
    for (i = 0; i < n; i++) {
        if (block_read(v, blocks[i], buf) != 0) return -1;
        if (hype_extd_validate(buf, v->block_size, 0) != 0) return -1;
        if (hype_extd_insert(buf, v->block_size, 0, ino, name, nlen, ftype) == 0) {
            return block_write(v, blocks[i], buf);
        }
    }
    nb = dir_grow_one(v, dir_ino, n);
    if (nb == ~0ull) return -1;
    bzero8(buf, v->block_size);
    hype_extd_block_init(buf, v->block_size, 0);
    if (hype_extd_insert(buf, v->block_size, 0, ino, name, nlen, ftype) != 0) return -1;
    return block_write(v, nb, buf);
}

static int dir_remove(v2_t *v, uint32_t dir_ino, const char *name, unsigned nlen) {
    uint64_t blocks[NS2_MAX_DIR_BLOCKS];
    unsigned n, i;
    uint8_t buf[4096];
    if (dir_blocks(v, dir_ino, blocks, &n, 0) != 0) return -1;
    for (i = 0; i < n; i++) {
        if (block_read(v, blocks[i], buf) != 0) return -1;
        if (hype_extd_validate(buf, v->block_size, 0) != 0) return -1;
        if (hype_extd_remove(buf, v->block_size, 0, name, nlen) == 1) {
            return block_write(v, blocks[i], buf);
        }
    }
    return -1;
}

static int dir_is_empty(v2_t *v, uint32_t dir_ino) {
    uint64_t blocks[NS2_MAX_DIR_BLOCKS];
    unsigned n, i;
    uint8_t buf[4096];
    if (dir_blocks(v, dir_ino, blocks, &n, 0) != 0) return -1;
    for (i = 0; i < n; i++) {
        if (block_read(v, blocks[i], buf) != 0) return -1;
        if (hype_extd_validate(buf, v->block_size, 0) != 0) return -1;
        if (!hype_extd_only_dots(buf, v->block_size, 0)) return 0;
    }
    return 1;
}

/*
 * True if `in`'s classic block map is shallow enough for free_all_blocks
 * below to enumerate SAFELY: direct pointers + single indirect only, never
 * double or triple. A file this writer itself ever creates always is (it
 * never allocates past a single directory-growth block, and create() never
 * allocates any block at all) -- but unlink/rmdir must accept ANY existing
 * regular file or directory by that name, including one the #384/#385/#497
 * write path grew past this depth on a real, heavily used volume. Refusing
 * a deletion this writer cannot safely enumerate is the same "refuse rather
 * than guess" rule the htree gate already applies -- misreading a
 * double-indirect pointer as one more block to free would corrupt the
 * volume instead of merely leaking space.
 */
static int classic_map_is_shallow(const uint8_t *in) {
    return hype_rd32(in + IN_BLOCK + 13u * 4u) == 0u && hype_rd32(in + IN_BLOCK + 14u * 4u) == 0u;
}

/* Frees every block currently mapping the file/directory's data (classic map
 * only -- the ext2 backend never sees extents). Direct + single indirect,
 * matching dir_grow_one's own scope limit; the caller must have already
 * confirmed classic_map_is_shallow(). */
static int free_all_blocks(v2_t *v, uint32_t ino) {
    uint8_t in[IN_CORE];
    uint32_t ppb = v->block_size / 4u, i;
    uint32_t root;

    if (inode_read128(v, ino, in) != 0) return -1;
    for (i = 0; i < 12u; i++) {
        uint32_t b = hype_rd32(in + IN_BLOCK + i * 4u);
        if (b != 0u && free_block(v, b) != 0) return -1;
    }
    root = hype_rd32(in + IN_BLOCK + 12u * 4u);
    if (root != 0u) {
        uint8_t pblk[4096];
        uint32_t j;
        if (block_read(v, root, pblk) != 0) return -1;
        for (j = 0; j < ppb; j++) {
            uint32_t b = hype_rd32(pblk + j * 4u);
            if (b != 0u && free_block(v, b) != 0) return -1;
        }
        if (free_block(v, root) != 0) return -1;
    }
    /* double/triple indirect: not populated by anything this writer creates
     * (dir_grow_one refuses past single-indirect), so nothing further to
     * walk for a directory this writer made; a pre-existing larger
     * classic-mapped file being unlinked is #384/#497's write path, which
     * this op does not touch here since only its BLOCK COUNT (i_blocks) is
     * used below, not a re-walk. */
    return 0;
}

/* ---- path helpers ---- */

static int split_path(const char *path, char *parent, char *leaf, unsigned *leaf_len) {
    unsigned len = 0, last_sep = 0, has_sep = 0, i;
    while (path[len] != '\0') {
        if (len >= NS2_MAX_PATH - 1u) return -1;
        len++;
    }
    /* trim trailing separators (but keep at least one leading '/') */
    while (len > 1u && (path[len - 1u] == '/' || path[len - 1u] == '\\')) len--;
    if (len == 0u) return -1;
    for (i = len; i > 0u; i--) {
        if (path[i - 1u] == '/' || path[i - 1u] == '\\') {
            last_sep = i - 1u;
            has_sep = 1;
            break;
        }
    }
    {
        unsigned nlen = has_sep ? (len - last_sep - 1u) : len;
        unsigned plen = has_sep ? last_sep : 0u;
        unsigned j;
        if (nlen == 0u || nlen > NS2_MAX_NAME) return -1;
        if (nlen == 1u && path[has_sep ? last_sep + 1u : 0u] == '.') return -1;
        if (nlen == 2u && path[(has_sep ? last_sep + 1u : 0u)] == '.' &&
            path[(has_sep ? last_sep + 1u : 0u) + 1u] == '.') {
            return -1; /* "." / ".." are never a mutation target */
        }
        for (j = 0; j < nlen; j++) leaf[j] = path[(has_sep ? last_sep + 1u : 0u) + j];
        *leaf_len = nlen;
        if (plen == 0u) {
            parent[0] = '/';
            parent[1] = '\0';
        } else {
            for (j = 0; j < plen; j++) parent[j] = path[j];
            parent[plen] = '\0';
        }
    }
    return 0;
}

/* ---- htree / mode / link-count helpers on an already-read inode image ---- */

static int is_htree(const uint8_t in[IN_CORE]) { return (hype_rd32(in + IN_FLAGS) & FL_INDEX) != 0; }

static void stamp_times(uint8_t in[IN_CORE], uint32_t mtime, int touch_atime) {
    if (mtime == 0u) return;
    hype_wr32(in + IN_CTIME, mtime);
    hype_wr32(in + IN_MTIME, mtime);
    if (touch_atime) hype_wr32(in + IN_ATIME, mtime);
}

/* ---- public ops ---- */

int hype_ext2_ns_create(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime) {
    v2_t v;
    char parent[NS2_MAX_PATH];
    char leaf[NS2_MAX_NAME + 1u];
    unsigned nlen;
    uint32_t parent_ino, existing, new_ino;
    uint8_t pin[IN_CORE], nin[IN_CORE];

    if (v2_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(path, parent, leaf, &nlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, parent, &parent_ino) != 0) return -1;
    if (inode_read128(&v, parent_ino, pin) != 0) return -1;
    if (dir_find(&v, parent_ino, leaf, nlen, &existing) != 0) return -1;
    if (existing) return -1; /* the caller's job to unlink first */
    if (is_htree(pin)) return -1;

    if (sb_set_dirty(&v, 1) != 0) return -1;
    if (claim_inode(&v, &new_ino) != 0) return -1;
    bzero8(nin, IN_CORE);
    hype_wr16(nin + IN_MODE, (uint16_t)MODE_REG_DEFAULT);
    hype_wr16(nin + IN_LINKS_COUNT, 1u);
    stamp_times(nin, mtime ? mtime : 1u, 1);
    if (mtime == 0u) {
        bzero8(nin + IN_ATIME, 4u);
        bzero8(nin + IN_CTIME, 4u);
        bzero8(nin + IN_MTIME, 4u);
    }
    if (inode_write128(&v, new_ino, nin) != 0) return -1;
    if (dir_insert(&v, parent_ino, leaf, nlen, new_ino, HYPE_EXTD_FT_REG) != 0) return -1;
    /* dir_insert may have just grown the parent (new size/block pointer) --
     * re-read before stamping, or that growth would be overwritten by the
     * copy read at the top of this function. */
    if (inode_read128(&v, parent_ino, pin) != 0) return -1;
    stamp_times(pin, mtime, 0);
    if (inode_write128(&v, parent_ino, pin) != 0) return -1;
    return sb_set_dirty(&v, 0);
}

int hype_ext2_ns_unlink(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime) {
    v2_t v;
    char parent[NS2_MAX_PATH];
    char leaf[NS2_MAX_NAME + 1u];
    unsigned nlen;
    uint32_t parent_ino, target_ino;
    uint8_t pin[IN_CORE], tin[IN_CORE];
    uint16_t links;

    if (v2_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(path, parent, leaf, &nlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, parent, &parent_ino) != 0) return -1;
    if (dir_find(&v, parent_ino, leaf, nlen, &target_ino) != 1) return -1;
    if (inode_read128(&v, target_ino, tin) != 0) return -1;
    if ((hype_rd16(tin + IN_MODE) & MODE_FMT) != MODE_REG) return -1; /* rmdir's job */
    /* refuse UP FRONT, before any mutation, if this unlink will drop the
     * last link and the file's blocks are too deep for free_all_blocks to
     * enumerate safely -- see classic_map_is_shallow's own comment. */
    if (hype_rd16(tin + IN_LINKS_COUNT) <= 1u && !classic_map_is_shallow(tin)) return -1;
    if (inode_read128(&v, parent_ino, pin) != 0) return -1;

    if (sb_set_dirty(&v, 1) != 0) return -1;
    if (dir_remove(&v, parent_ino, leaf, nlen) != 0) return -1;
    stamp_times(pin, mtime, 0);
    if (inode_write128(&v, parent_ino, pin) != 0) return -1;

    links = hype_rd16(tin + IN_LINKS_COUNT);
    if (links > 0u) links--;
    hype_wr16(tin + IN_LINKS_COUNT, links);
    if (links == 0u) {
        /* free_all_blocks re-reads the inode's block pointers FROM MEDIA, so
         * it must run before this handle's own image is zeroed below --
         * otherwise it would free nothing and leak every block. */
        if (free_all_blocks(&v, target_ino) != 0) return -1;
        /* A fully freed inode must look FULLY freed, not merely "link count
         * zero, everything else still whatever it used to be": a leftover
         * mode/size/block-pointer combination alongside a nonzero dtime is
         * exactly the shape a REAL crashed-mid-unlink orphan leaves, and
         * e2fsck reports it as a corrupted orphan-list entry even though
         * this project never threads (or needs) the real orphan list --
         * a COMPLETED operation must not look like an interrupted one. */
        bzero8(tin, IN_CORE);
        if (mtime != 0u) hype_wr32(tin + IN_DTIME, mtime); /* 0 (already zeroed) looks like a never-used slot; a REAL but tiny dtime looks like a corrupted orphan-list "next" pointer to e2fsck -- never write one */
        if (inode_write128(&v, target_ino, tin) != 0) return -1;
        if (free_inode(&v, target_ino, 0) != 0) return -1;
    } else {
        stamp_times(tin, mtime, 0);
        if (inode_write128(&v, target_ino, tin) != 0) return -1;
    }
    return sb_set_dirty(&v, 0);
}

int hype_ext2_ns_mkdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, uint32_t mtime) {
    v2_t v;
    char parent[NS2_MAX_PATH];
    char leaf[NS2_MAX_NAME + 1u];
    unsigned nlen;
    uint32_t parent_ino, existing, new_ino;
    uint8_t pin[IN_CORE], nin[IN_CORE], dblk[4096];
    uint64_t db;

    if (v2_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(path, parent, leaf, &nlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, parent, &parent_ino) != 0) return -1;
    if (inode_read128(&v, parent_ino, pin) != 0) return -1;
    if (dir_find(&v, parent_ino, leaf, nlen, &existing) != 0) return -1;
    if (existing) return -1;
    if (is_htree(pin)) return -1;

    if (sb_set_dirty(&v, 1) != 0) return -1;
    if (claim_inode(&v, &new_ino) != 0) return -1;
    if (claim_block(&v, inode_byte(&v, parent_ino, 0) / SECSZ / v.spb, &db) != 0) return -1;

    bzero8(dblk, v.block_size);
    hype_extd_block_init(dblk, v.block_size, 0);
    if (hype_extd_insert(dblk, v.block_size, 0, new_ino, ".", 1u, HYPE_EXTD_FT_DIR) != 0) {
        return -1;
    }
    if (hype_extd_insert(dblk, v.block_size, 0, parent_ino, "..", 2u, HYPE_EXTD_FT_DIR) != 0) {
        return -1;
    }
    if (block_write(&v, db, dblk) != 0) return -1;

    bzero8(nin, IN_CORE);
    hype_wr16(nin + IN_MODE, (uint16_t)MODE_DIR_DEFAULT);
    hype_wr16(nin + IN_LINKS_COUNT, 2u);
    hype_wr32(nin + IN_SIZE_LO, v.block_size);
    hype_wr32(nin + IN_BLOCKS, v.spb);
    hype_wr32(nin + IN_BLOCK, (uint32_t)db);
    stamp_times(nin, mtime ? mtime : 1u, 1);
    if (mtime == 0u) {
        bzero8(nin + IN_ATIME, 4u);
        bzero8(nin + IN_CTIME, 4u);
        bzero8(nin + IN_MTIME, 4u);
    }
    if (inode_write128(&v, new_ino, nin) != 0) return -1;
    if (gd_adjust16(&v, (new_ino - 1u) / v.inodes_per_group, GD_USED_DIRS, 1) != 0) return -1;

    if (dir_insert(&v, parent_ino, leaf, nlen, new_ino, HYPE_EXTD_FT_DIR) != 0) return -1;
    /* dir_insert may have just grown the parent -- re-read before applying
     * the link-count bump, same reason create() re-reads. */
    if (inode_read128(&v, parent_ino, pin) != 0) return -1;
    {
        uint16_t plinks = hype_rd16(pin + IN_LINKS_COUNT);
        hype_wr16(pin + IN_LINKS_COUNT, (uint16_t)(plinks + 1u)); /* the new ".." */
    }
    stamp_times(pin, mtime, 0);
    if (inode_write128(&v, parent_ino, pin) != 0) return -1;
    return sb_set_dirty(&v, 0);
}

int hype_ext2_ns_rmdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, uint32_t mtime) {
    v2_t v;
    char parent[NS2_MAX_PATH];
    char leaf[NS2_MAX_NAME + 1u];
    unsigned nlen;
    uint32_t parent_ino, target_ino;
    uint8_t pin[IN_CORE], tin[IN_CORE];
    int empty;

    if (v2_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(path, parent, leaf, &nlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, parent, &parent_ino) != 0) return -1;
    if (dir_find(&v, parent_ino, leaf, nlen, &target_ino) != 1) return -1;
    if (target_ino == 2u) return -1; /* never rmdir the volume root */
    if (inode_read128(&v, target_ino, tin) != 0) return -1;
    if ((hype_rd16(tin + IN_MODE) & MODE_FMT) != MODE_DIR) return -1;
    if (is_htree(tin)) return -1; /* see ext_namespace.h */
    /* refuse up front, before any mutation, if this directory's own blocks
     * (e.g. a real mkfs lost+found pre-allocated with many blocks) are too
     * deep for free_all_blocks to enumerate safely -- see its own comment. */
    if (!classic_map_is_shallow(tin)) return -1;
    empty = dir_is_empty(&v, target_ino);
    if (empty != 1) return -1;
    if (inode_read128(&v, parent_ino, pin) != 0) return -1;

    if (sb_set_dirty(&v, 1) != 0) return -1;
    if (dir_remove(&v, parent_ino, leaf, nlen) != 0) return -1;
    {
        uint16_t plinks = hype_rd16(pin + IN_LINKS_COUNT);
        if (plinks > 0u) plinks--; /* the removed ".." */
        hype_wr16(pin + IN_LINKS_COUNT, plinks);
    }
    stamp_times(pin, mtime, 0);
    if (inode_write128(&v, parent_ino, pin) != 0) return -1;

    /* free_all_blocks re-reads the target's block pointers from media, so it
     * must run before the inode image is zeroed -- see unlink's twin. */
    if (free_all_blocks(&v, target_ino) != 0) return -1;
    bzero8(tin, IN_CORE); /* a completed rmdir must not look like a crashed one */
    if (mtime != 0u) hype_wr32(tin + IN_DTIME, mtime); /* 0 (already zeroed) looks like a never-used slot; a REAL but tiny dtime looks like a corrupted orphan-list "next" pointer to e2fsck -- never write one */
    if (inode_write128(&v, target_ino, tin) != 0) return -1;
    if (free_inode(&v, target_ino, 1) != 0) return -1;
    return sb_set_dirty(&v, 0);
}

/* Walks `child_ino`'s ".." chain up to the root, refusing if `ancestor_ino`
 * appears anywhere on the way (including as `child_ino` itself) -- rename's
 * cycle guard: a directory can never be moved into its own subtree. Bounded
 * by `groups`-derived inode count so a corrupt ".." loop cannot spin
 * forever. */
static int is_ancestor_or_self(v2_t *v, uint32_t ancestor_ino, uint32_t child_ino) {
    uint32_t cur = child_ino;
    uint32_t steps = 0;
    for (;;) {
        uint8_t in[IN_CORE];
        uint32_t up;
        if (cur == ancestor_ino) return 1;
        if (cur == 2u) return 0; /* reached the root without finding it */
        if (++steps > v->inodes_count) return 1; /* corrupt loop: refuse, don't spin */
        if (dir_find(v, cur, "..", 2u, &up) != 1) return 1; /* unreadable: refuse */
        if (inode_read128(v, up, in) != 0) return 1;
        if ((hype_rd16(in + IN_MODE) & MODE_FMT) != MODE_DIR) return 1;
        cur = up;
    }
}

int hype_ext2_ns_rename(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *from, const char *to, uint32_t mtime) {
    v2_t v;
    char sparent[NS2_MAX_PATH], dparent[NS2_MAX_PATH];
    char sleaf[NS2_MAX_NAME + 1u], dleaf[NS2_MAX_NAME + 1u];
    unsigned snlen, dnlen;
    uint32_t sparent_ino, dparent_ino, src_ino, existing;
    uint8_t spin[IN_CORE], dpin[IN_CORE], sin[IN_CORE];
    int is_dir, cross_parent;

    if (v2_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(from, sparent, sleaf, &snlen) != 0) return -1;
    if (split_path(to, dparent, dleaf, &dnlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, sparent, &sparent_ino) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, dparent, &dparent_ino) != 0) return -1;
    if (dir_find(&v, sparent_ino, sleaf, snlen, &src_ino) != 1) return -1;
    if (dir_find(&v, dparent_ino, dleaf, dnlen, &existing) != 0) return -1;
    if (existing) return -1; /* never silently replace an existing target */
    if (inode_read128(&v, src_ino, sin) != 0) return -1;
    is_dir = (hype_rd16(sin + IN_MODE) & MODE_FMT) == MODE_DIR;
    if (inode_read128(&v, dparent_ino, dpin) != 0) return -1;
    if (is_htree(dpin)) return -1;
    if (is_dir && is_ancestor_or_self(&v, src_ino, dparent_ino)) return -1;

    cross_parent = (sparent_ino != dparent_ino);
    if (inode_read128(&v, sparent_ino, spin) != 0) return -1;

    if (sb_set_dirty(&v, 1) != 0) return -1;
    if (dir_remove(&v, sparent_ino, sleaf, snlen) != 0) return -1;
    if (dir_insert(&v, dparent_ino, dleaf, dnlen, src_ino,
                   is_dir ? HYPE_EXTD_FT_DIR : HYPE_EXTD_FT_REG) != 0) {
        return -1;
    }
    {
        int src_link_delta = 0, dst_link_delta = 0;
        if (is_dir && cross_parent) {
            if (dir_remove(&v, src_ino, "..", 2u) != 0) return -1; /* re-add: only rec_len differs */
            if (dir_insert(&v, src_ino, "..", 2u, dparent_ino, HYPE_EXTD_FT_DIR) != 0) return -1;
            src_link_delta = -1;
            dst_link_delta = 1;
        }
        /* Every dir_insert/dir_remove above may have just grown or shrunk
         * ONE of these three inodes (a new size + block pointer on disk) --
         * re-read each fresh right before its final stamp+write, or a stale
         * in-memory copy would silently undo that structural change. */
        if (inode_read128(&v, src_ino, sin) != 0) return -1;
        stamp_times(sin, mtime, 0);
        if (inode_write128(&v, src_ino, sin) != 0) return -1;

        if (inode_read128(&v, sparent_ino, spin) != 0) return -1;
        if (src_link_delta != 0) {
            uint16_t sl = hype_rd16(spin + IN_LINKS_COUNT);
            if (sl > 0u) sl = (uint16_t)(sl + src_link_delta);
            hype_wr16(spin + IN_LINKS_COUNT, sl);
        }
        stamp_times(spin, mtime, 0);
        if (inode_write128(&v, sparent_ino, spin) != 0) return -1;

        if (cross_parent) {
            if (inode_read128(&v, dparent_ino, dpin) != 0) return -1;
            if (dst_link_delta != 0) {
                uint16_t dl = hype_rd16(dpin + IN_LINKS_COUNT);
                hype_wr16(dpin + IN_LINKS_COUNT, (uint16_t)(dl + dst_link_delta));
            }
            stamp_times(dpin, mtime, 0);
            if (inode_write128(&v, dparent_ino, dpin) != 0) return -1;
        }
    }
    return sb_set_dirty(&v, 0);
}
