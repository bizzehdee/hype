#include "ext.h"
#include "ext_namespace_impl.h"
#include "ext_dirent.h"
#include "ext_csum.h"
#include "jbd2.h"
#include "lebytes.h"

/*
 * #498: ext3/4 (COMPAT_HAS_JOURNAL) namespace mutation -- create/unlink/
 * mkdir/rmdir/rename. One bounded jbd2 transaction per call, exactly the
 * discipline core/ext_jalloc.c established for allocation: every structural
 * mutation (inode bitmap, block bitmap, directory entries, group descriptor
 * and superblock counters, the inode table) is collected in the SAME
 * module-static block-image cache, checksummed (#495/#496's crc32c/crc16,
 * desc_size-aware) where the volume requires it, journaled via
 * core/jbd2.c's hype_jbd2_commit/checkpoint, and only THEN written to its
 * final location -- never a direct write, never a second checksum pass.
 *
 * See core/ext_namespace.h for the shared contract (htree refusal, rename
 * no-clobber) and core/ext_dirent.h for the directory-block format this
 * shares with core/ext2_namespace.c.
 */

#define SECSZ HYPE_BLK_SECTOR_SIZE
#define NSJ_MAX_NAME 255u
#define NSJ_MAX_PATH 512u
#define NSJ_CACHE 24u /* == HYPE_JBD2_MAX_BLOCKS, same bound as ext_jalloc.c */

/* superblock */
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
#define SB_UUID 0x68u
#define SB_CHECKSUM_TYPE 0x175u
#define SB_CHECKSUM 0x3FCu
#define SB_DESC_SIZE 0xFEu
#define SB_BLOCKS_COUNT_HI 0x150u
#define SB_JOURNAL_INUM 0xE0u

#define EXT_MAGIC 0xEF53u
#define STATE_VALID 0x0001u
#define STATE_ERROR 0x0002u
#define COMPAT_HAS_JOURNAL 0x0004u
#define INCOMPAT_FILETYPE 0x0002u
#define INCOMPAT_RECOVER 0x0004u
#define INCOMPAT_JOURNAL_DEV 0x0008u
#define INCOMPAT_EXTENTS 0x0040u
#define INCOMPAT_64BIT 0x0080u
#define INCOMPAT_FLEX_BG 0x0200u
#define INCOMPAT_OK (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_FLEX_BG | INCOMPAT_64BIT)
#define RO_GDT_CSUM 0x0010u
#define RO_METADATA_CSUM 0x0400u
#define RO_OK (0x006Bu | RO_GDT_CSUM | RO_METADATA_CSUM)

/* group descriptor */
#define GD_BLOCK_BITMAP 0x00u
#define GD_INODE_BITMAP 0x04u
#define GD_INODE_TABLE 0x08u
#define GD_FREE_BLOCKS 0x0Cu
#define GD_FREE_INODES 0x0Eu
#define GD_USED_DIRS 0x10u
#define GD_FLAGS 0x12u
#define GD_BBITMAP_CSUM_LO 0x18u
#define GD_IBITMAP_CSUM_LO 0x1Au
#define GD_ITABLE_UNUSED 0x1Cu
#define GD_CHECKSUM 0x1Eu
#define GD_BLOCK_BITMAP_HI 0x20u
#define GD_INODE_BITMAP_HI 0x24u
#define GD_INODE_TABLE_HI 0x28u
#define GD_FREE_BLOCKS_HI 0x2Cu
#define GD_FREE_INODES_HI 0x2Eu
#define GD_USED_DIRS_HI 0x30u
#define GD_ITABLE_UNUSED_HI 0x32u
#define GD_BBITMAP_CSUM_HI 0x38u
#define GD_IBITMAP_CSUM_HI 0x3Au

#define BG_INODE_UNINIT 0x0001u

/* inode */
#define IN_MODE 0x00u
#define IN_SIZE_LO 0x04u
#define IN_ATIME 0x08u
#define IN_CTIME 0x0Cu
#define IN_MTIME 0x10u
#define IN_DTIME 0x14u
#define IN_LINKS_COUNT 0x1Au
#define IN_BLOCKS 0x1Cu
#define IN_FLAGS 0x20u
#define IN_GENERATION 0x64u
#define IN_BLOCK 0x28u
#define IN_SIZE_HIGH 0x6Cu
#define IN_CHECKSUM_LO 0x7Cu
#define IN_EXTRA_ISIZE 0x80u
#define IN_CHECKSUM_HI 0x82u
#define FL_EXTENTS 0x00080000u
#define FL_INDEX 0x00001000u /* EXT4_INDEX_FL: htree */
#define MODE_FMT 0xF000u
#define MODE_DIR 0x4000u
#define MODE_REG 0x8000u
#define MODE_DIR_DEFAULT 0x41EDu
#define MODE_REG_DEFAULT 0x81A4u

/* extent tree */
#define EH_MAGIC 0xF30Au
#define EXT_TAIL_CHECKSUM 4u

#define JOURNAL_INO 8u

typedef struct {
    uint64_t blocknr;
    int used;
    int dirty;
    int is_extent;
    uint8_t data[4096];
} slot_t;
static slot_t g_cache[NSJ_CACHE];

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
    uint32_t desc_size;
    uint32_t first_ino;
    int use_extents; /* the FS supports INCOMPAT_EXTENTS: new inodes are extent-mapped */
    int has_gdt_csum;
    int has_metadata_csum;
    uint8_t uuid[16];
    uint32_t csum_seed;
    hype_jbd2_t journal;
} v_t;

static void bzero8(uint8_t *p, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) p[i] = 0;
}
static void bcopy8(uint8_t *d, const uint8_t *s, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) d[i] = s[i];
}
/* ---- transaction block cache (mirrors core/ext_jalloc.c's) ---- */

static slot_t *cache_get(v_t *v, uint64_t blocknr) {
    unsigned i, free_slot = NSJ_CACHE;
    if (blocknr >= v->blocks_count) return 0;
    for (i = 0; i < NSJ_CACHE; i++) {
        if (g_cache[i].used && g_cache[i].blocknr == blocknr) return &g_cache[i];
        if (!g_cache[i].used && free_slot == NSJ_CACHE) free_slot = i;
    }
    if (free_slot == NSJ_CACHE) return 0;
    {
        slot_t *s = &g_cache[free_slot];
        uint32_t sec;
        for (sec = 0; sec < v->spb; sec++) {
            if (v->read(v->ctx, blocknr * v->spb + sec, 1u, s->data + (uint64_t)sec * SECSZ) != 0) {
                return 0;
            }
        }
        s->blocknr = blocknr;
        s->used = 1;
        s->dirty = 0;
        s->is_extent = 0;
        return s;
    }
}
static void cache_reset(void) {
    unsigned i;
    for (i = 0; i < NSJ_CACHE; i++) {
        g_cache[i].used = 0;
        g_cache[i].dirty = 0;
    }
}

/* ---- checksums (mirrors core/ext_jalloc.c's #495/#496 helpers) ---- */

static void sb_csum_finalize(v_t *v, slot_t *sb, uint32_t sbo) {
    uint32_t crc;
    if (!v->has_metadata_csum) return;
    crc = hype_ext_crc32c(0xFFFFFFFFu, sb->data + sbo, SB_CHECKSUM);
    hype_wr32(sb->data + sbo + SB_CHECKSUM, crc);
}

static void gd_csum_finalize(v_t *v, slot_t *gd, uint32_t gd_off, uint32_t group) {
    uint8_t grp_le[4];
    uint32_t tail_off = GD_CHECKSUM + 2u;
    hype_wr32(grp_le, group);
    if (v->has_metadata_csum) {
        uint32_t crc;
        hype_wr16(gd->data + gd_off + GD_CHECKSUM, 0u);
        crc = hype_ext_crc32c(v->csum_seed, grp_le, 4u);
        crc = hype_ext_crc32c(crc, gd->data + gd_off, 32u);
        if (v->desc_size > tail_off) {
            crc = hype_ext_crc32c(crc, gd->data + gd_off + tail_off, v->desc_size - tail_off);
        }
        hype_wr16(gd->data + gd_off + GD_CHECKSUM, (uint16_t)crc);
    } else if (v->has_gdt_csum) {
        uint16_t crc = hype_ext_crc16(0xFFFFu, v->uuid, 16u);
        crc = hype_ext_crc16(crc, grp_le, 4u);
        crc = hype_ext_crc16(crc, gd->data + gd_off, GD_CHECKSUM);
        if (v->desc_size > tail_off) {
            crc = hype_ext_crc16(crc, gd->data + gd_off + tail_off, v->desc_size - tail_off);
        }
        hype_wr16(gd->data + gd_off + GD_CHECKSUM, crc);
    }
}

static void block_bitmap_csum_finalize(v_t *v, const slot_t *bm, slot_t *gd, uint32_t gd_off) {
    uint32_t crc;
    if (!v->has_metadata_csum) return;
    /* The kernel hashes blocks_per_group/8 bytes (fs/ext4/bitmap.c's
     * ext4_block_bitmap_csum_set(): `sz = EXT4_CLUSTERS_PER_GROUP(sb)/8`),
     * not the whole block -- but BIGALLOC is refused at open (RO_OK excludes
     * it), and without it blocks_per_group is always exactly block_size*8
     * (one bitmap block's worth of bits, the universal mkfs convention), so
     * hashing the whole block IS hashing blocks_per_group/8 bytes here.
     * inodes_per_group has no equivalent identity with block_size -- see
     * inode_bitmap_csum_finalize below, which cannot take this shortcut. */
    crc = hype_ext_crc32c(v->csum_seed, bm->data, v->block_size);
    hype_wr16(gd->data + gd_off + GD_BBITMAP_CSUM_LO, (uint16_t)crc);
    if (v->desc_size >= 64u) hype_wr16(gd->data + gd_off + GD_BBITMAP_CSUM_HI, (uint16_t)(crc >> 16));
}

static void inode_bitmap_csum_finalize(v_t *v, const slot_t *bm, slot_t *gd, uint32_t gd_off) {
    uint32_t crc;
    if (!v->has_metadata_csum) return;
    /* UNLIKE the block bitmap above, this hashes only inodes_per_group/8
     * bytes -- NOT the whole block. inodes_per_group is typically far
     * smaller than block_size*8 (e.g. 16384 inodes in a 4096-byte, 32768-bit
     * block), so hashing the whole block would fold in meaningless padding
     * bytes and produce the wrong checksum; the block bitmap's own
     * blocks_per_group happens to equal block_size*8 for any volume this
     * writer's fixed-desc_size math actually supports, which is why that
     * path stays exact despite hashing the whole block. Verified against
     * fs/ext4/bitmap.c's ext4_inode_bitmap_csum_set() (`sz =
     * EXT4_INODES_PER_GROUP(sb) >> 3`) and ext4_block_bitmap_csum_set()
     * (`sz = EXT4_CLUSTERS_PER_GROUP(sb) / 8`) -- both PER_GROUP/8, never
     * the block size -- not from memory. */
    crc = hype_ext_crc32c(v->csum_seed, bm->data, (v->inodes_per_group + 7u) / 8u);
    hype_wr16(gd->data + gd_off + GD_IBITMAP_CSUM_LO, (uint16_t)crc);
    if (v->desc_size >= 64u) hype_wr16(gd->data + gd_off + GD_IBITMAP_CSUM_HI, (uint16_t)(crc >> 16));
}

static uint32_t inode_csum_seed(v_t *v, uint32_t ino_no, const uint8_t *inode_image) {
    uint8_t inum_le[4];
    uint32_t seed;
    hype_wr32(inum_le, ino_no);
    seed = hype_ext_crc32c(v->csum_seed, inum_le, 4u);
    seed = hype_ext_crc32c(seed, inode_image + IN_GENERATION, 4u);
    return seed;
}

/* Finalizes inode `ino_no`'s own checksum in its (already fully mutated)
 * cached table-block image. Must be the LAST touch before commit. */
static void inode_csum_finalize(v_t *v, uint32_t ino_no, slot_t *s, uint32_t off) {
    uint32_t seed, crc;
    int has_hi;
    if (!v->has_metadata_csum) return;
    seed = inode_csum_seed(v, ino_no, s->data + off);
    has_hi = v->inode_size > 128u && hype_rd16(s->data + off + IN_EXTRA_ISIZE) >= 4u;
    hype_wr16(s->data + off + IN_CHECKSUM_LO, 0u);
    if (has_hi) hype_wr16(s->data + off + IN_CHECKSUM_HI, 0u);
    crc = hype_ext_crc32c(seed, s->data + off, v->inode_size);
    hype_wr16(s->data + off + IN_CHECKSUM_LO, (uint16_t)crc);
    if (has_hi) hype_wr16(s->data + off + IN_CHECKSUM_HI, (uint16_t)(crc >> 16));
}

/* Commits the transaction exactly like core/ext_jalloc.c's txn_commit. */
static int txn_commit(v_t *v) {
    hype_jbd2_block_t imgs[NSJ_CACHE];
    unsigned i, n = 0;
    for (i = 0; i < NSJ_CACHE; i++) {
        if (g_cache[i].used && g_cache[i].dirty) {
            imgs[n].blocknr = g_cache[i].blocknr;
            imgs[n].data = g_cache[i].data;
            n++;
        }
    }
    if (n == 0u) return 0;
    if (hype_jbd2_commit(&v->journal, imgs, n) != 0) return -1;
    for (i = 0; i < NSJ_CACHE; i++) {
        if (g_cache[i].used && g_cache[i].dirty) {
            uint32_t sec;
            for (sec = 0; sec < v->spb; sec++) {
                if (v->write(v->ctx, g_cache[i].blocknr * v->spb + sec, 1u,
                            g_cache[i].data + (uint64_t)sec * SECSZ) != 0) {
                    return -1; /* exposed but part-written: honestly unrecoverable here */
                }
            }
        }
    }
    return hype_jbd2_checkpoint(&v->journal);
}

/* ---- open ---- */

static int v_open(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx, v_t *v) {
    uint8_t sb[1024];
    uint32_t incompat, compat, rocompat, log_bs, rev;
    uint16_t state;

    if (read == 0 || write == 0) return -1;
    if (read(ctx, 2u, 2u, sb) != 0) return -1;
    if (hype_rd16(sb + SB_MAGIC) != EXT_MAGIC) return -1;
    compat = hype_rd32(sb + SB_FEATURE_COMPAT);
    incompat = hype_rd32(sb + SB_FEATURE_INCOMPAT);
    rocompat = hype_rd32(sb + SB_FEATURE_RO_COMPAT);
    if (!(compat & COMPAT_HAS_JOURNAL)) return -1;
    if (incompat & INCOMPAT_RECOVER) return -1;
    if (incompat & INCOMPAT_JOURNAL_DEV) return -1;
    if (!(incompat & INCOMPAT_FILETYPE) || (incompat & ~INCOMPAT_OK)) return -1;
    if (rocompat & ~RO_OK) return -1;
    v->has_gdt_csum = (rocompat & RO_GDT_CSUM) ? 1 : 0;
    v->has_metadata_csum = (rocompat & RO_METADATA_CSUM) ? 1 : 0;
    if (v->has_metadata_csum && sb[SB_CHECKSUM_TYPE] != 1u) return -1;
    bcopy8(v->uuid, sb + SB_UUID, 16u);
    v->csum_seed = v->has_metadata_csum ? hype_ext_crc32c(0xFFFFFFFFu, v->uuid, 16u) : 0u;
    state = hype_rd16(sb + SB_STATE);
    if ((state & STATE_VALID) == 0u || (state & STATE_ERROR) != 0u) return -1;
    log_bs = hype_rd32(sb + SB_LOG_BLOCK_SIZE);
    if (log_bs > 2u) return -1;
    if (hype_rd32(sb + SB_JOURNAL_INUM) != JOURNAL_INO) return -1; /* incl. external (0) */

    v->read = read;
    v->write = write;
    v->ctx = ctx;
    v->block_size = 1024u << log_bs;
    v->spb = v->block_size / SECSZ;
    v->blocks_count = hype_rd32(sb + SB_BLOCKS_COUNT);
    if (incompat & INCOMPAT_64BIT) {
        v->blocks_count |= (uint64_t)hype_rd32(sb + SB_BLOCKS_COUNT_HI) << 32;
    }
    v->blocks_per_group = hype_rd32(sb + SB_BLOCKS_PER_GROUP);
    v->first_data_block = hype_rd32(sb + SB_FIRST_DATA_BLOCK);
    v->inodes_count = hype_rd32(sb + SB_INODES_COUNT);
    v->inodes_per_group = hype_rd32(sb + SB_INODES_PER_GROUP);
    rev = hype_rd32(sb + SB_REV_LEVEL);
    v->inode_size = (rev == 0u) ? 128u : (uint32_t)hype_rd16(sb + SB_INODE_SIZE);
    v->first_ino = (rev == 0u) ? 11u : hype_rd32(sb + SB_FIRST_INO);
    v->desc_size = (incompat & INCOMPAT_64BIT) ? (uint32_t)hype_rd16(sb + SB_DESC_SIZE) : 32u;
    v->use_extents = (incompat & INCOMPAT_EXTENTS) ? 1 : 0;
    if (v->blocks_per_group == 0u || v->inodes_per_group == 0u || v->blocks_count < 2u ||
        v->inode_size < 128u || v->inodes_count == 0u) {
        return -1;
    }
    if (v->first_ino < 1u || v->first_ino > v->inodes_count) return -1;
    if (v->desc_size != 32u &&
        (v->desc_size < 64u || v->desc_size > 512u || (v->desc_size & (v->desc_size - 1u)) != 0u)) return -1;
    v->groups = (uint32_t)((v->blocks_count - v->first_data_block + v->blocks_per_group - 1u) /
                           v->blocks_per_group);

    {
        static hype_file_rmap_t jmap;
        if (hype_ext_map_ino_rmap(read, ctx, JOURNAL_INO, &jmap) != 0) return -1;
        if (hype_jbd2_open(&v->journal, read, write, ctx, v->block_size, &jmap) != 0) return -1;
        if ((incompat & INCOMPAT_64BIT) && hype_jbd2_upgrade_64bit(&v->journal) != 0) return -1;
    }
    cache_reset();
    return 0;
}

/* ---- group descriptor + bitmap access (mirrors core/ext_jalloc.c's #496
 * lo/hi convention throughout, for full spec fidelity even though a
 * per-GROUP inode/dir count structurally never needs the hi half in
 * practice -- ext_jalloc.c made the same choice for free_blocks_count). ---- */

static uint64_t gd_blocknr(const v_t *v, uint32_t group, uint32_t *out_off) {
    uint64_t byte = (uint64_t)(v->first_data_block + 1u) * v->block_size + (uint64_t)group * v->desc_size;
    *out_off = (uint32_t)(byte % v->block_size);
    return byte / v->block_size;
}

static uint32_t gd_get16(const v_t *v, const slot_t *gd, uint32_t gd_off, uint32_t lo,
                         uint32_t hi) {
    uint32_t val = hype_rd16(gd->data + gd_off + lo);
    if (v->desc_size >= 64u) val |= (uint32_t)hype_rd16(gd->data + gd_off + hi) << 16;
    return val;
}
static void gd_set16(const v_t *v, slot_t *gd, uint32_t gd_off, uint32_t lo, uint32_t hi,
                     uint32_t val) {
    hype_wr16(gd->data + gd_off + lo, (uint16_t)val);
    if (v->desc_size >= 64u) hype_wr16(gd->data + gd_off + hi, (uint16_t)(val >> 16));
}

static uint64_t gd_get32pair(const v_t *v, const slot_t *gd, uint32_t gd_off, uint32_t lo,
                             uint32_t hi) {
    uint64_t val = hype_rd32(gd->data + gd_off + lo);
    if (v->desc_size >= 64u) val |= (uint64_t)hype_rd32(gd->data + gd_off + hi) << 32;
    return val;
}

/* Find + claim one free BLOCK; all bookkeeping through the cache. Verbatim
 * port of core/ext_jalloc.c's claim_block (including its acceptance of
 * BG_BLOCK_UNINIT as inert -- see that file's header comment). */
static int claim_block(v_t *v, uint64_t near, uint64_t *out) {
    uint32_t start_group =
        (near >= v->first_data_block)
            ? (uint32_t)((near - v->first_data_block) / v->blocks_per_group) % v->groups
            : 0u;
    uint32_t gi;
    for (gi = 0; gi < v->groups; gi++) {
        uint32_t group = (start_group + gi) % v->groups;
        uint32_t gd_off;
        uint64_t gdb = gd_blocknr(v, group, &gd_off);
        slot_t *gd = cache_get(v, gdb);
        uint64_t bmb, base = (uint64_t)group * v->blocks_per_group + v->first_data_block;
        slot_t *bm;
        uint32_t in_group = v->blocks_per_group, b;
        if (gd == 0) return -1;
        if (base >= v->blocks_count) break;
        if (in_group > v->blocks_count - base) in_group = (uint32_t)(v->blocks_count - base);
        if (gd_get16(v, gd, gd_off, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI) == 0u) continue;
        bmb = gd_get32pair(v, gd, gd_off, GD_BLOCK_BITMAP, GD_BLOCK_BITMAP_HI);
        if (bmb == 0u || bmb >= v->blocks_count) return -1;
        bm = cache_get(v, bmb);
        if (bm == 0) return -1;
        for (b = 0; b < in_group; b++) {
            if (!(bm->data[b / 8u] & (1u << (b % 8u)))) {
                uint32_t freeb = gd_get16(v, gd, gd_off, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI);
                bm->data[b / 8u] |= (uint8_t)(1u << (b % 8u));
                bm->dirty = 1;
                block_bitmap_csum_finalize(v, bm, gd, gd_off);
                gd_set16(v, gd, gd_off, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI, freeb - 1u);
                gd->dirty = 1;
                {
                    uint64_t sbb = 1024u / v->block_size;
                    uint32_t sbo = (v->block_size == 1024u) ? 0u : 1024u;
                    slot_t *sb = cache_get(v, sbb);
                    if (sb == 0) return -1;
                    hype_wr32(sb->data + sbo + SB_FREE_BLOCKS,
                             hype_rd32(sb->data + sbo + SB_FREE_BLOCKS) - 1u);
                    sb->dirty = 1;
                    sb_csum_finalize(v, sb, sbo);
                }
                gd_csum_finalize(v, gd, gd_off, group);
                *out = base + b;
                return 0;
            }
        }
    }
    return -1;
}

static int free_block(v_t *v, uint64_t blk) {
    uint32_t group = (uint32_t)((blk - v->first_data_block) / v->blocks_per_group);
    uint32_t bit = (uint32_t)((blk - v->first_data_block) % v->blocks_per_group);
    uint32_t gd_off;
    uint64_t gdb = gd_blocknr(v, group, &gd_off);
    slot_t *gd = cache_get(v, gdb);
    uint64_t bmb;
    slot_t *bm;
    uint32_t freeb;
    if (gd == 0) return -1;
    bmb = gd_get32pair(v, gd, gd_off, GD_BLOCK_BITMAP, GD_BLOCK_BITMAP_HI);
    if (bmb == 0u || bmb >= v->blocks_count) return -1;
    bm = cache_get(v, bmb);
    if (bm == 0) return -1;
    bm->data[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
    bm->dirty = 1;
    block_bitmap_csum_finalize(v, bm, gd, gd_off);
    freeb = gd_get16(v, gd, gd_off, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI);
    gd_set16(v, gd, gd_off, GD_FREE_BLOCKS, GD_FREE_BLOCKS_HI, freeb + 1u);
    gd->dirty = 1;
    {
        uint64_t sbb = 1024u / v->block_size;
        uint32_t sbo = (v->block_size == 1024u) ? 0u : 1024u;
        slot_t *sb = cache_get(v, sbb);
        if (sb == 0) return -1;
        hype_wr32(sb->data + sbo + SB_FREE_BLOCKS, hype_rd32(sb->data + sbo + SB_FREE_BLOCKS) + 1u);
        sb->dirty = 1;
        sb_csum_finalize(v, sb, sbo);
    }
    gd_csum_finalize(v, gd, gd_off, group);
    return 0;
}

/* Find + claim one free INODE, maintaining GD_FREE_INODES, the superblock's
 * free-inode count, BG_INODE_UNINIT (cleared the first time a group's
 * bitmap is actually used), and GD_ITABLE_UNUSED (shrunk only when the
 * claimed index falls inside the "never touched" tail it already
 * describes -- verified against fs/ext4/ialloc.c's own
 * ext4_itable_unused_count() bookkeeping). */
static int claim_inode(v_t *v, uint32_t *out_ino) {
    uint32_t group;
    for (group = 0; group < v->groups; group++) {
        uint32_t gd_off;
        uint64_t gdb = gd_blocknr(v, group, &gd_off);
        slot_t *gd = cache_get(v, gdb);
        uint64_t ibmb;
        slot_t *bm;
        uint32_t floor = (group == 0u) ? (v->first_ino - 1u) : 0u;
        uint32_t b;
        if (gd == 0) return -1;
        if (gd_get16(v, gd, gd_off, GD_FREE_INODES, GD_FREE_INODES_HI) == 0u) continue;
        ibmb = gd_get32pair(v, gd, gd_off, GD_INODE_BITMAP, GD_INODE_BITMAP_HI);
        if (ibmb == 0u || ibmb >= v->blocks_count) return -1;
        bm = cache_get(v, ibmb);
        if (bm == 0) return -1;
        for (b = floor; b < v->inodes_per_group; b++) {
            uint32_t ino = group * v->inodes_per_group + b + 1u;
            if (ino > v->inodes_count) break;
            if (bm->data[b / 8u] & (1u << (b % 8u))) continue;
            {
                uint32_t freei = gd_get16(v, gd, gd_off, GD_FREE_INODES, GD_FREE_INODES_HI);
                uint32_t flags, unused;
                bm->data[b / 8u] |= (uint8_t)(1u << (b % 8u));
                bm->dirty = 1;
                inode_bitmap_csum_finalize(v, bm, gd, gd_off);
                gd_set16(v, gd, gd_off, GD_FREE_INODES, GD_FREE_INODES_HI, freei - 1u);
                flags = hype_rd16(gd->data + gd_off + GD_FLAGS);
                if (flags & BG_INODE_UNINIT) {
                    hype_wr16(gd->data + gd_off + GD_FLAGS, (uint16_t)(flags & ~BG_INODE_UNINIT));
                }
                unused = gd_get16(v, gd, gd_off, GD_ITABLE_UNUSED, GD_ITABLE_UNUSED_HI);
                if (unused != 0u && b >= v->inodes_per_group - unused) {
                    gd_set16(v, gd, gd_off, GD_ITABLE_UNUSED, GD_ITABLE_UNUSED_HI,
                            v->inodes_per_group - b - 1u);
                }
                gd->dirty = 1;
                {
                    uint64_t sbb = 1024u / v->block_size;
                    uint32_t sbo = (v->block_size == 1024u) ? 0u : 1024u;
                    slot_t *sb = cache_get(v, sbb);
                    if (sb == 0) return -1;
                    hype_wr32(sb->data + sbo + SB_FREE_INODES,
                             hype_rd32(sb->data + sbo + SB_FREE_INODES) - 1u);
                    sb->dirty = 1;
                    sb_csum_finalize(v, sb, sbo);
                }
                gd_csum_finalize(v, gd, gd_off, group);
                *out_ino = ino;
                return 0;
            }
        }
    }
    return -1;
}

static int free_inode(v_t *v, uint32_t ino, int is_dir) {
    uint32_t group = (ino - 1u) / v->inodes_per_group;
    uint32_t bit = (ino - 1u) % v->inodes_per_group;
    uint32_t gd_off;
    uint64_t gdb = gd_blocknr(v, group, &gd_off);
    slot_t *gd = cache_get(v, gdb);
    uint64_t ibmb;
    slot_t *bm;
    uint32_t freei, dirs;
    if (gd == 0) return -1;
    ibmb = gd_get32pair(v, gd, gd_off, GD_INODE_BITMAP, GD_INODE_BITMAP_HI);
    if (ibmb == 0u || ibmb >= v->blocks_count) return -1;
    bm = cache_get(v, ibmb);
    if (bm == 0) return -1;
    bm->data[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
    bm->dirty = 1;
    inode_bitmap_csum_finalize(v, bm, gd, gd_off);
    freei = gd_get16(v, gd, gd_off, GD_FREE_INODES, GD_FREE_INODES_HI);
    gd_set16(v, gd, gd_off, GD_FREE_INODES, GD_FREE_INODES_HI, freei + 1u);
    if (is_dir) {
        dirs = gd_get16(v, gd, gd_off, GD_USED_DIRS, GD_USED_DIRS_HI);
        if (dirs > 0u) gd_set16(v, gd, gd_off, GD_USED_DIRS, GD_USED_DIRS_HI, dirs - 1u);
    }
    gd->dirty = 1;
    {
        uint64_t sbb = 1024u / v->block_size;
        uint32_t sbo = (v->block_size == 1024u) ? 0u : 1024u;
        slot_t *sb = cache_get(v, sbb);
        if (sb == 0) return -1;
        hype_wr32(sb->data + sbo + SB_FREE_INODES, hype_rd32(sb->data + sbo + SB_FREE_INODES) + 1u);
        sb->dirty = 1;
        sb_csum_finalize(v, sb, sbo);
    }
    gd_csum_finalize(v, gd, gd_off, group);
    return 0;
}

static int bump_used_dirs(v_t *v, uint32_t ino, int delta) {
    uint32_t group = (ino - 1u) / v->inodes_per_group;
    uint32_t gd_off;
    uint64_t gdb = gd_blocknr(v, group, &gd_off);
    slot_t *gd = cache_get(v, gdb);
    uint32_t dirs;
    if (gd == 0) return -1;
    dirs = gd_get16(v, gd, gd_off, GD_USED_DIRS, GD_USED_DIRS_HI);
    dirs = (uint32_t)((int)dirs + delta);
    gd_set16(v, gd, gd_off, GD_USED_DIRS, GD_USED_DIRS_HI, dirs);
    gd->dirty = 1;
    gd_csum_finalize(v, gd, gd_off, group);
    return 0;
}

/* ---- inode table access through the cache ---- */

static slot_t *inode_slot(v_t *v, uint32_t ino, uint32_t *out_off) {
    uint32_t group = (ino - 1u) / v->inodes_per_group;
    uint32_t index = (ino - 1u) % v->inodes_per_group;
    uint32_t gd_off;
    uint64_t gdb = gd_blocknr(v, group, &gd_off);
    slot_t *gd = cache_get(v, gdb);
    uint64_t table;
    uint64_t inode_byte;
    slot_t *s;
    if (gd == 0) return 0;
    table = gd_get32pair(v, gd, gd_off, GD_INODE_TABLE, GD_INODE_TABLE_HI);
    if (table == 0u || table >= v->blocks_count) return 0;
    inode_byte = table * v->block_size + (uint64_t)index * v->inode_size;
    s = cache_get(v, inode_byte / v->block_size);
    if (s != 0) *out_off = (uint32_t)(inode_byte % v->block_size);
    return s;
}

static void stamp_times(uint8_t *in, uint32_t off, uint32_t mtime, int touch_atime) {
    if (mtime == 0u) return;
    hype_wr32(in + off + IN_CTIME, mtime);
    hype_wr32(in + off + IN_MTIME, mtime);
    if (touch_atime) hype_wr32(in + off + IN_ATIME, mtime);
}

static int is_htree(const uint8_t *in, uint32_t off) {
    return (hype_rd32(in + off + IN_FLAGS) & FL_INDEX) != 0;
}

/* ---- directory block enumeration + content ops, through the cache ---- */

#define NSJ_MAX_DIR_BLOCKS 512u

static int dir_blocks(v_t *v, uint32_t ino, uint64_t *out, unsigned *out_n) {
    hype_file_rmap_t map;
    unsigned r, n = 0;
    if (hype_ext_map_dir_ino_rmap(v->read, v->ctx, ino, &map) != 0) return -1;
    for (r = 0; r < map.count; r++) {
        uint64_t s;
        if (map.ranges[r].kind != HYPE_RANGE_DATA) return -1;
        for (s = 0; s < map.ranges[r].sector_count; s += v->spb) {
            if (n >= NSJ_MAX_DIR_BLOCKS) return -1;
            out[n++] = (map.ranges[r].start_lba + s) / v->spb;
        }
    }
    *out_n = n;
    return 0;
}

static int has_csum_tail(const v_t *v) { return v->has_metadata_csum; }

static int dir_find(v_t *v, uint32_t dir_ino, const char *name, unsigned nlen,
                    uint32_t *out_ino) {
    uint64_t blocks[NSJ_MAX_DIR_BLOCKS];
    unsigned n, i;
    *out_ino = 0; /* deterministic "not found" for every caller that tests *out_ino directly */
    if (dir_blocks(v, dir_ino, blocks, &n) != 0) return -1;
    for (i = 0; i < n; i++) {
        slot_t *s = cache_get(v, blocks[i]);
        uint32_t off;
        if (s == 0) return -1;
        if (hype_extd_validate(s->data, v->block_size, has_csum_tail(v)) != 0) return -1;
        if (hype_extd_find(s->data, v->block_size, has_csum_tail(v), name, nlen, &off, out_ino) ==
            1) {
            return 1;
        }
    }
    return 0;
}

/* the owning directory's i_csum_seed -- needed to finalize a dirent block's
 * metadata_csum tail after mutating it */
static uint32_t dir_csum_seed(v_t *v, uint32_t dir_ino) {
    uint32_t off;
    slot_t *s = inode_slot(v, dir_ino, &off);
    if (s == 0) return 0u;
    return inode_csum_seed(v, dir_ino, s->data + off);
}

/* Appends one freshly claimed+zeroed block as the directory's next logical
 * block (direct/single-indirect classic map, or an extent-tree insert for an
 * extent-mapped directory), publishing the pointer through the SAME cached
 * inode-table slot. Direct/single-indirect and a single in-inode extent
 * entry cover every directory this writer itself ever creates or grows in
 * the ticket's bar; deeper growth is refused (see core/ext2_namespace.c's
 * twin for the same documented scope limit). Returns the new block number,
 * or ~0ull. */
static uint64_t dir_grow_one(v_t *v, uint32_t dir_ino, uint64_t cur_blocks) {
    uint32_t off;
    slot_t *is = inode_slot(v, dir_ino, &off);
    uint64_t nb;
    int is_ext;
    static const uint8_t z[4096];

    if (is == 0) return ~0ull;
    is_ext = (hype_rd32(is->data + off + IN_FLAGS) & FL_EXTENTS) != 0;
    if (claim_block(v, is->blocknr, &nb) != 0) return ~0ull;
    {
        slot_t *ns = cache_get(v, nb);
        if (ns == 0) return ~0ull;
        bcopy8(ns->data, z, v->block_size);
        ns->dirty = 1;
    }
    /* re-fetch: cache_get(nb) above may have evicted nothing (inode slot was
     * already resident), but re-derive `is` defensively before mutating it */
    is = inode_slot(v, dir_ino, &off);
    if (is == 0) return ~0ull;

    if (is_ext) {
        uint32_t entries = hype_rd16(is->data + off + IN_BLOCK + 2u);
        uint32_t max = hype_rd16(is->data + off + IN_BLOCK + 4u);
        if (entries >= max) return ~0ull; /* root growth: out of this slice's scope */
        if (entries > 0u) {
            uint8_t *pe = is->data + off + IN_BLOCK + 12u + (entries - 1u) * 12u;
            uint32_t plb = hype_rd32(pe);
            uint16_t plen = hype_rd16(pe + 4);
            uint64_t pph = (uint64_t)hype_rd32(pe + 8) | ((uint64_t)hype_rd16(pe + 6) << 32);
            if (plen != 0u && plen < 32768u && (uint64_t)plb + plen == cur_blocks &&
                pph + plen == nb && plen + 1u < 32768u) {
                hype_wr16(pe + 4, (uint16_t)(plen + 1u));
                is->dirty = 1;
                hype_wr32(is->data + off + IN_BLOCKS, hype_rd32(is->data + off + IN_BLOCKS) + v->spb);
                hype_wr32(is->data + off + IN_SIZE_LO, (uint32_t)((cur_blocks + 1u) * v->block_size));
                return nb;
            }
        }
        {
            uint8_t *ent = is->data + off + IN_BLOCK + 12u + entries * 12u;
            hype_wr32(ent + 0, (uint32_t)cur_blocks);
            hype_wr16(ent + 4, 1u);
            hype_wr16(ent + 6, (uint16_t)(nb >> 32));
            hype_wr32(ent + 8, (uint32_t)nb);
            hype_wr16(is->data + off + IN_BLOCK + 2u, (uint16_t)(entries + 1u));
            is->dirty = 1;
        }
    } else {
        uint32_t ppb = v->block_size / 4u;
        if (cur_blocks < 12u) {
            hype_wr32(is->data + off + IN_BLOCK + (uint32_t)cur_blocks * 4u, (uint32_t)nb);
        } else if (cur_blocks - 12u < ppb) {
            uint32_t root = hype_rd32(is->data + off + IN_BLOCK + 12u * 4u);
            if (root == 0u) {
                uint64_t rb;
                slot_t *rs;
                if (claim_block(v, is->blocknr, &rb) != 0) return ~0ull;
                rs = cache_get(v, rb);
                if (rs == 0) return ~0ull;
                bcopy8(rs->data, z, v->block_size);
                rs->dirty = 1;
                is = inode_slot(v, dir_ino, &off);
                if (is == 0) return ~0ull;
                hype_wr32(is->data + off + IN_BLOCK + 12u * 4u, (uint32_t)rb);
                hype_wr32(is->data + off + IN_BLOCKS,
                         hype_rd32(is->data + off + IN_BLOCKS) + v->spb);
                is->dirty = 1;
                root = (uint32_t)rb;
            }
            {
                slot_t *ps = cache_get(v, root);
                if (ps == 0) return ~0ull;
                hype_wr32(ps->data + (cur_blocks - 12u) * 4u, (uint32_t)nb);
                ps->dirty = 1;
            }
        } else {
            return ~0ull; /* double/triple indirect: out of this slice's scope */
        }
    }
    hype_wr32(is->data + off + IN_BLOCKS, hype_rd32(is->data + off + IN_BLOCKS) + v->spb);
    hype_wr32(is->data + off + IN_SIZE_LO, (uint32_t)((cur_blocks + 1u) * v->block_size));
    is->dirty = 1;
    return nb;
}

static int dir_insert(v_t *v, uint32_t dir_ino, const char *name, unsigned nlen, uint32_t ino,
                      uint8_t ftype) {
    uint64_t blocks[NSJ_MAX_DIR_BLOCKS];
    unsigned n, i;
    uint32_t seed = dir_csum_seed(v, dir_ino);
    int tail = has_csum_tail(v);

    if (dir_blocks(v, dir_ino, blocks, &n) != 0) return -1;
    for (i = 0; i < n; i++) {
        slot_t *s = cache_get(v, blocks[i]);
        if (s == 0) return -1;
        if (hype_extd_validate(s->data, v->block_size, tail) != 0) return -1;
        if (hype_extd_insert(s->data, v->block_size, tail, ino, name, nlen, ftype) == 0) {
            hype_extd_csum_finalize(s->data, v->block_size, tail, seed);
            s->dirty = 1;
            return 0;
        }
    }
    {
        uint64_t nb = dir_grow_one(v, dir_ino, n);
        slot_t *s;
        if (nb == ~0ull) return -1;
        s = cache_get(v, nb);
        if (s == 0) return -1;
        hype_extd_block_init(s->data, v->block_size, tail);
        if (hype_extd_insert(s->data, v->block_size, tail, ino, name, nlen, ftype) != 0) return -1;
        hype_extd_csum_finalize(s->data, v->block_size, tail, seed);
        s->dirty = 1;
        return 0;
    }
}

static int dir_remove(v_t *v, uint32_t dir_ino, const char *name, unsigned nlen) {
    uint64_t blocks[NSJ_MAX_DIR_BLOCKS];
    unsigned n, i;
    uint32_t seed = dir_csum_seed(v, dir_ino);
    int tail = has_csum_tail(v);
    if (dir_blocks(v, dir_ino, blocks, &n) != 0) return -1;
    for (i = 0; i < n; i++) {
        slot_t *s = cache_get(v, blocks[i]);
        if (s == 0) return -1;
        if (hype_extd_validate(s->data, v->block_size, tail) != 0) return -1;
        if (hype_extd_remove(s->data, v->block_size, tail, name, nlen) == 1) {
            hype_extd_csum_finalize(s->data, v->block_size, tail, seed);
            s->dirty = 1;
            return 0;
        }
    }
    return -1;
}

static int dir_is_empty(v_t *v, uint32_t dir_ino) {
    uint64_t blocks[NSJ_MAX_DIR_BLOCKS];
    unsigned n, i;
    int tail = has_csum_tail(v);
    if (dir_blocks(v, dir_ino, blocks, &n) != 0) return -1;
    for (i = 0; i < n; i++) {
        slot_t *s = cache_get(v, blocks[i]);
        if (s == 0) return -1;
        if (hype_extd_validate(s->data, v->block_size, tail) != 0) return -1;
        if (!hype_extd_only_dots(s->data, v->block_size, tail)) return 0;
    }
    return 1;
}

/*
 * True if `is`'s (already cached) inode image maps its data in a shape
 * free_all_blocks below can enumerate SAFELY: for a classic map, direct
 * pointers + single indirect only (never double/triple); for an
 * extent-mapped inode, a DEPTH-0 (leaf-only) in-inode root -- an interior
 * extent index entry (ext4_extent_idx) is the exact same 12 bytes as a leaf
 * entry (ext4_extent) with different field meanings, so misreading one as
 * the other would free garbage block numbers instead of the real ones. A
 * file/directory this writer itself ever creates or grows always qualifies
 * (mkdir/create never produce more than a depth-0 root or a single indirect
 * block), but unlink/rmdir must accept ANY existing regular file or
 * directory by that name, including one #384/#385/#497's write path grew
 * past this on a real, heavily used or fragmented volume -- refusing a
 * deletion this writer cannot safely enumerate is the same "refuse rather
 * than guess" rule the htree gate already applies.
 */
static int inode_blocks_are_shallow(const uint8_t *data, uint32_t off) {
    if (hype_rd32(data + off + IN_FLAGS) & FL_EXTENTS) {
        return hype_rd16(data + off + IN_BLOCK + 6u) == 0u; /* eh_depth */
    }
    return hype_rd32(data + off + IN_BLOCK + 13u * 4u) == 0u &&
           hype_rd32(data + off + IN_BLOCK + 14u * 4u) == 0u;
}

/* Frees every block currently mapping the inode's data (classic direct +
 * single-indirect, or every extent leaf in the in-inode root -- the only
 * shapes dir_grow_one itself ever produces; a pre-existing larger directory
 * this writer never grew is out of scope for the same reason
 * core/ext2_namespace.c's twin already documents). Caller must have already
 * confirmed inode_blocks_are_shallow(). */
static int free_all_blocks(v_t *v, uint32_t ino) {
    uint32_t off;
    slot_t *is = inode_slot(v, ino, &off);
    int is_ext;
    if (is == 0) return -1;
    is_ext = (hype_rd32(is->data + off + IN_FLAGS) & FL_EXTENTS) != 0;
    if (is_ext) {
        uint32_t entries = hype_rd16(is->data + off + IN_BLOCK + 2u);
        uint32_t i;
        for (i = 0; i < entries; i++) {
            const uint8_t *e = is->data + off + IN_BLOCK + 12u + i * 12u;
            uint16_t raw = hype_rd16(e + 4);
            uint16_t len = (uint16_t)(raw > 32768u ? raw - 32768u : raw);
            uint64_t phys = (uint64_t)hype_rd32(e + 8) | ((uint64_t)hype_rd16(e + 6) << 32);
            uint32_t j;
            for (j = 0; j < len; j++) {
                if (free_block(v, phys + j) != 0) return -1;
            }
        }
    } else {
        uint32_t ppb = v->block_size / 4u, i;
        uint32_t root;
        for (i = 0; i < 12u; i++) {
            uint32_t b = hype_rd32(is->data + off + IN_BLOCK + i * 4u);
            if (b != 0u && free_block(v, b) != 0) return -1;
        }
        root = hype_rd32(is->data + off + IN_BLOCK + 12u * 4u);
        if (root != 0u) {
            slot_t *ps = cache_get(v, root);
            if (ps == 0) return -1;
            for (i = 0; i < ppb; i++) {
                uint32_t b = hype_rd32(ps->data + i * 4u);
                if (b != 0u && free_block(v, b) != 0) return -1;
            }
            if (free_block(v, root) != 0) return -1;
        }
    }
    return 0;
}

/* ---- path helpers (identical shape to core/ext2_namespace.c's) ---- */

static int split_path(const char *path, char *parent, char *leaf, unsigned *leaf_len) {
    unsigned len = 0, last_sep = 0, has_sep = 0, i;
    while (path[len] != '\0') {
        if (len >= NSJ_MAX_PATH - 1u) return -1;
        len++;
    }
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
        unsigned base = has_sep ? last_sep + 1u : 0u;
        unsigned j;
        if (nlen == 0u || nlen > NSJ_MAX_NAME) return -1;
        if (nlen == 1u && path[base] == '.') return -1;
        if (nlen == 2u && path[base] == '.' && path[base + 1u] == '.') return -1;
        for (j = 0; j < nlen; j++) leaf[j] = path[base + j];
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

/* ---- public ops ---- */

int hype_extj_ns_create(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime) {
    v_t v;
    char parent[NSJ_MAX_PATH];
    char leaf[NSJ_MAX_NAME + 1u];
    unsigned nlen;
    uint32_t parent_ino, existing, new_ino, poff;
    slot_t *ps;

    if (v_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(path, parent, leaf, &nlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, parent, &parent_ino) != 0) return -1;
    ps = inode_slot(&v, parent_ino, &poff);
    if (ps == 0) return -1;
    if (is_htree(ps->data, poff)) return -1;
    if (dir_find(&v, parent_ino, leaf, nlen, &existing) != 0) return -1;
    if (existing) return -1;

    if (claim_inode(&v, &new_ino) != 0) return -1;
    {
        uint32_t noff;
        slot_t *ns = inode_slot(&v, new_ino, &noff);
        if (ns == 0) return -1;
        bzero8(ns->data + noff, v.inode_size); /* inode entries never straddle a block */
        hype_wr16(ns->data + noff + IN_MODE, (uint16_t)MODE_REG_DEFAULT);
        hype_wr16(ns->data + noff + IN_LINKS_COUNT, 1u);
        if (v.use_extents) {
            hype_wr32(ns->data + noff + IN_FLAGS, FL_EXTENTS);
            hype_wr16(ns->data + noff + IN_BLOCK + 0, (uint16_t)EH_MAGIC);
            hype_wr16(ns->data + noff + IN_BLOCK + 4, 4u); /* eh_max: 60-byte root */
        }
        if (v.inode_size > 128u) hype_wr16(ns->data + noff + IN_EXTRA_ISIZE, 32u);
        stamp_times(ns->data, noff, mtime ? mtime : 1u, 1);
        if (mtime == 0u) {
            bzero8(ns->data + noff + IN_ATIME, 4u);
            bzero8(ns->data + noff + IN_CTIME, 4u);
            bzero8(ns->data + noff + IN_MTIME, 4u);
        }
        ns->dirty = 1;
        inode_csum_finalize(&v, new_ino, ns, noff);
    }
    if (dir_insert(&v, parent_ino, leaf, nlen, new_ino, HYPE_EXTD_FT_REG) != 0) return -1;
    ps = inode_slot(&v, parent_ino, &poff);
    if (ps == 0) return -1;
    stamp_times(ps->data, poff, mtime, 0);
    ps->dirty = 1;
    inode_csum_finalize(&v, parent_ino, ps, poff);
    if (txn_commit(&v) != 0) return -1;
    cache_reset();
    return 0;
}

int hype_extj_ns_unlink(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *path, uint32_t mtime) {
    v_t v;
    char parent[NSJ_MAX_PATH];
    char leaf[NSJ_MAX_NAME + 1u];
    unsigned nlen;
    uint32_t parent_ino, target_ino, poff, toff;
    slot_t *ps, *ts;
    uint16_t links;

    if (v_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(path, parent, leaf, &nlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, parent, &parent_ino) != 0) return -1;
    if (dir_find(&v, parent_ino, leaf, nlen, &target_ino) != 1) return -1;
    ts = inode_slot(&v, target_ino, &toff);
    if (ts == 0) return -1;
    if ((hype_rd16(ts->data + toff + IN_MODE) & MODE_FMT) != MODE_REG) return -1;
    /* refuse UP FRONT, before any mutation, if this unlink will drop the
     * last link and the file's blocks are too deep to enumerate safely. */
    if (hype_rd16(ts->data + toff + IN_LINKS_COUNT) <= 1u &&
        !inode_blocks_are_shallow(ts->data, toff)) {
        return -1;
    }

    if (dir_remove(&v, parent_ino, leaf, nlen) != 0) return -1;
    ps = inode_slot(&v, parent_ino, &poff);
    if (ps == 0) return -1;
    stamp_times(ps->data, poff, mtime, 0);
    ps->dirty = 1;
    inode_csum_finalize(&v, parent_ino, ps, poff);

    ts = inode_slot(&v, target_ino, &toff);
    if (ts == 0) return -1;
    links = hype_rd16(ts->data + toff + IN_LINKS_COUNT);
    if (links > 0u) links--;
    hype_wr16(ts->data + toff + IN_LINKS_COUNT, links);
    ts->dirty = 1;
    if (links == 0u) {
        /* free_all_blocks reads this SAME cached inode image's block
         * pointers (the cache never re-reads from media once resident), so
         * it must run before the image below is zeroed -- otherwise it
         * frees nothing and leaks every block. */
        if (free_all_blocks(&v, target_ino) != 0) return -1;
        ts = inode_slot(&v, target_ino, &toff);
        if (ts == 0) return -1;
        /* A fully freed inode must look FULLY freed -- a leftover
         * mode/size/block-pointer combination alongside a nonzero dtime is
         * exactly the shape a real crashed-mid-unlink orphan leaves, and
         * e2fsck reports it as a corrupted orphan-list entry even though
         * this project never threads (or needs) the real orphan list. */
        bzero8(ts->data + toff, v.inode_size);
        if (mtime != 0u) hype_wr32(ts->data + toff + IN_DTIME, mtime); /* 0 (already zeroed) looks like a never-used slot; a REAL but tiny dtime looks like a corrupted orphan-list "next" pointer to e2fsck -- never write one */
        ts->dirty = 1;
        inode_csum_finalize(&v, target_ino, ts, toff);
        if (free_inode(&v, target_ino, 0) != 0) return -1;
    } else {
        stamp_times(ts->data, toff, mtime, 0);
        inode_csum_finalize(&v, target_ino, ts, toff);
    }
    if (txn_commit(&v) != 0) return -1;
    cache_reset();
    return 0;
}

int hype_extj_ns_mkdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, uint32_t mtime) {
    v_t v;
    char parent[NSJ_MAX_PATH];
    char leaf[NSJ_MAX_NAME + 1u];
    unsigned nlen;
    uint32_t parent_ino, existing, new_ino, poff;
    slot_t *ps;
    uint64_t db;
    uint32_t seed;

    if (v_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(path, parent, leaf, &nlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, parent, &parent_ino) != 0) return -1;
    ps = inode_slot(&v, parent_ino, &poff);
    if (ps == 0) return -1;
    if (is_htree(ps->data, poff)) return -1;
    if (dir_find(&v, parent_ino, leaf, nlen, &existing) != 0) return -1;
    if (existing) return -1;

    if (claim_inode(&v, &new_ino) != 0) return -1;
    if (claim_block(&v, ps->blocknr, &db) != 0) return -1;

    {
        uint32_t noff;
        slot_t *ns = inode_slot(&v, new_ino, &noff);
        if (ns == 0) return -1;
        bzero8(ns->data + noff, v.inode_size); /* inode entries never straddle a block */
        hype_wr16(ns->data + noff + IN_MODE, (uint16_t)MODE_DIR_DEFAULT);
        hype_wr16(ns->data + noff + IN_LINKS_COUNT, 2u);
        hype_wr32(ns->data + noff + IN_SIZE_LO, v.block_size);
        hype_wr32(ns->data + noff + IN_BLOCKS, v.spb);
        if (v.use_extents) {
            hype_wr32(ns->data + noff + IN_FLAGS, FL_EXTENTS);
            hype_wr16(ns->data + noff + IN_BLOCK + 0, (uint16_t)EH_MAGIC);
            hype_wr16(ns->data + noff + IN_BLOCK + 2, 1u); /* eh_entries */
            hype_wr16(ns->data + noff + IN_BLOCK + 4, 4u); /* eh_max */
            hype_wr32(ns->data + noff + IN_BLOCK + 12u + 0, 0u);
            hype_wr16(ns->data + noff + IN_BLOCK + 12u + 4, 1u);
            hype_wr16(ns->data + noff + IN_BLOCK + 12u + 6, (uint16_t)(db >> 32));
            hype_wr32(ns->data + noff + IN_BLOCK + 12u + 8, (uint32_t)db);
        } else {
            hype_wr32(ns->data + noff + IN_BLOCK, (uint32_t)db);
        }
        if (v.inode_size > 128u) hype_wr16(ns->data + noff + IN_EXTRA_ISIZE, 32u);
        stamp_times(ns->data, noff, mtime ? mtime : 1u, 1);
        if (mtime == 0u) {
            bzero8(ns->data + noff + IN_ATIME, 4u);
            bzero8(ns->data + noff + IN_CTIME, 4u);
            bzero8(ns->data + noff + IN_MTIME, 4u);
        }
        ns->dirty = 1;
        seed = inode_csum_seed(&v, new_ino, ns->data + noff);
        inode_csum_finalize(&v, new_ino, ns, noff);
    }
    if (bump_used_dirs(&v, new_ino, 1) != 0) return -1;

    {
        slot_t *dblk = cache_get(&v, db);
        int tail = has_csum_tail(&v);
        if (dblk == 0) return -1;
        hype_extd_block_init(dblk->data, v.block_size, tail);
        if (hype_extd_insert(dblk->data, v.block_size, tail, new_ino, ".", 1u, HYPE_EXTD_FT_DIR) !=
            0) {
            return -1;
        }
        if (hype_extd_insert(dblk->data, v.block_size, tail, parent_ino, "..", 2u,
                             HYPE_EXTD_FT_DIR) != 0) {
            return -1;
        }
        hype_extd_csum_finalize(dblk->data, v.block_size, tail, seed);
        dblk->dirty = 1;
    }

    if (dir_insert(&v, parent_ino, leaf, nlen, new_ino, HYPE_EXTD_FT_DIR) != 0) return -1;
    ps = inode_slot(&v, parent_ino, &poff);
    if (ps == 0) return -1;
    hype_wr16(ps->data + poff + IN_LINKS_COUNT,
             (uint16_t)(hype_rd16(ps->data + poff + IN_LINKS_COUNT) + 1u));
    stamp_times(ps->data, poff, mtime, 0);
    ps->dirty = 1;
    inode_csum_finalize(&v, parent_ino, ps, poff);
    if (txn_commit(&v) != 0) return -1;
    cache_reset();
    return 0;
}

int hype_extj_ns_rmdir(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, uint32_t mtime) {
    v_t v;
    char parent[NSJ_MAX_PATH];
    char leaf[NSJ_MAX_NAME + 1u];
    unsigned nlen;
    uint32_t parent_ino, target_ino, poff, toff;
    slot_t *ps, *ts;
    int empty;

    if (v_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(path, parent, leaf, &nlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, parent, &parent_ino) != 0) return -1;
    if (dir_find(&v, parent_ino, leaf, nlen, &target_ino) != 1) return -1;
    if (target_ino == 2u) return -1;
    ts = inode_slot(&v, target_ino, &toff);
    if (ts == 0) return -1;
    if ((hype_rd16(ts->data + toff + IN_MODE) & MODE_FMT) != MODE_DIR) return -1;
    if (is_htree(ts->data, toff)) return -1;
    /* refuse up front, before any mutation, if this directory's own blocks
     * (e.g. a real mkfs lost+found pre-allocated with many blocks) are too
     * deep to enumerate safely -- see inode_blocks_are_shallow's comment. */
    if (!inode_blocks_are_shallow(ts->data, toff)) return -1;
    empty = dir_is_empty(&v, target_ino);
    if (empty != 1) return -1;

    if (dir_remove(&v, parent_ino, leaf, nlen) != 0) return -1;
    ps = inode_slot(&v, parent_ino, &poff);
    if (ps == 0) return -1;
    {
        uint16_t plinks = hype_rd16(ps->data + poff + IN_LINKS_COUNT);
        if (plinks > 0u) plinks--;
        hype_wr16(ps->data + poff + IN_LINKS_COUNT, plinks);
    }
    stamp_times(ps->data, poff, mtime, 0);
    ps->dirty = 1;
    inode_csum_finalize(&v, parent_ino, ps, poff);

    /* free_all_blocks reads the target's cached inode image, so it must run
     * before that image is zeroed below -- see unlink's twin. */
    if (free_all_blocks(&v, target_ino) != 0) return -1;
    ts = inode_slot(&v, target_ino, &toff);
    if (ts == 0) return -1;
    bzero8(ts->data + toff, v.inode_size); /* a completed rmdir must not look like a crashed one */
    if (mtime != 0u) hype_wr32(ts->data + toff + IN_DTIME, mtime); /* 0 (already zeroed) looks like a never-used slot; a REAL but tiny dtime looks like a corrupted orphan-list "next" pointer to e2fsck -- never write one */
    ts->dirty = 1;
    inode_csum_finalize(&v, target_ino, ts, toff);
    if (free_inode(&v, target_ino, 1) != 0) return -1;
    if (txn_commit(&v) != 0) return -1;
    cache_reset();
    return 0;
}

/* See core/ext2_namespace.c's twin for the rationale. */
static int is_ancestor_or_self(v_t *v, uint32_t ancestor_ino, uint32_t child_ino) {
    uint32_t cur = child_ino;
    uint32_t steps = 0;
    for (;;) {
        uint32_t up, uoff;
        slot_t *us;
        if (cur == ancestor_ino) return 1;
        if (cur == 2u) return 0;
        if (++steps > v->inodes_count) return 1;
        if (dir_find(v, cur, "..", 2u, &up) != 1) return 1;
        us = inode_slot(v, up, &uoff);
        if (us == 0) return 1;
        if ((hype_rd16(us->data + uoff + IN_MODE) & MODE_FMT) != MODE_DIR) return 1;
        cur = up;
    }
}

int hype_extj_ns_rename(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                       const char *from, const char *to, uint32_t mtime) {
    v_t v;
    char sparent[NSJ_MAX_PATH], dparent[NSJ_MAX_PATH];
    char sleaf[NSJ_MAX_NAME + 1u], dleaf[NSJ_MAX_NAME + 1u];
    unsigned snlen, dnlen;
    uint32_t sparent_ino, dparent_ino, src_ino, existing;
    uint32_t soff, doff, xoff;
    slot_t *ss, *ds, *xs;
    int is_dir, cross_parent;

    if (v_open(read, write, ctx, &v) != 0) return -1;
    if (split_path(from, sparent, sleaf, &snlen) != 0) return -1;
    if (split_path(to, dparent, dleaf, &dnlen) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, sparent, &sparent_ino) != 0) return -1;
    if (hype_ext_resolve_dir_ino(read, ctx, dparent, &dparent_ino) != 0) return -1;
    if (dir_find(&v, sparent_ino, sleaf, snlen, &src_ino) != 1) return -1;
    if (dir_find(&v, dparent_ino, dleaf, dnlen, &existing) != 0) return -1;
    if (existing) return -1;
    ds = inode_slot(&v, dparent_ino, &doff);
    if (ds == 0) return -1;
    if (is_htree(ds->data, doff)) return -1;
    xs = inode_slot(&v, src_ino, &xoff);
    if (xs == 0) return -1;
    is_dir = (hype_rd16(xs->data + xoff + IN_MODE) & MODE_FMT) == MODE_DIR;
    if (is_dir && is_ancestor_or_self(&v, src_ino, dparent_ino)) return -1;

    cross_parent = (sparent_ino != dparent_ino);

    if (dir_remove(&v, sparent_ino, sleaf, snlen) != 0) return -1;
    if (dir_insert(&v, dparent_ino, dleaf, dnlen, src_ino,
                   is_dir ? HYPE_EXTD_FT_DIR : HYPE_EXTD_FT_REG) != 0) {
        return -1;
    }
    if (is_dir && cross_parent) {
        if (dir_remove(&v, src_ino, "..", 2u) != 0) return -1;
        if (dir_insert(&v, src_ino, "..", 2u, dparent_ino, HYPE_EXTD_FT_DIR) != 0) return -1;
        ss = inode_slot(&v, sparent_ino, &soff);
        ds = inode_slot(&v, dparent_ino, &doff);
        if (ss == 0 || ds == 0) return -1;
        {
            uint16_t sl = hype_rd16(ss->data + soff + IN_LINKS_COUNT);
            if (sl > 0u) sl--;
            hype_wr16(ss->data + soff + IN_LINKS_COUNT, sl);
            hype_wr16(ds->data + doff + IN_LINKS_COUNT,
                     (uint16_t)(hype_rd16(ds->data + doff + IN_LINKS_COUNT) + 1u));
        }
        ss->dirty = 1;
        ds->dirty = 1;
    }

    xs = inode_slot(&v, src_ino, &xoff);
    if (xs == 0) return -1;
    stamp_times(xs->data, xoff, mtime, 0);
    xs->dirty = 1;
    inode_csum_finalize(&v, src_ino, xs, xoff);

    ss = inode_slot(&v, sparent_ino, &soff);
    if (ss == 0) return -1;
    stamp_times(ss->data, soff, mtime, 0);
    ss->dirty = 1;
    inode_csum_finalize(&v, sparent_ino, ss, soff);

    if (cross_parent) {
        ds = inode_slot(&v, dparent_ino, &doff);
        if (ds == 0) return -1;
        stamp_times(ds->data, doff, mtime, 0);
        ds->dirty = 1;
        inode_csum_finalize(&v, dparent_ino, ds, doff);
    }
    if (txn_commit(&v) != 0) return -1;
    cache_reset();
    return 0;
}
