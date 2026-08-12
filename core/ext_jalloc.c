#include "ext_jalloc.h"
#include "ext.h"
#include "lebytes.h"

/* See ext_jalloc.h. The classic-map allocation logic deliberately parallels
 * core/ext2_alloc.c -- the difference is the substrate: every metadata
 * mutation here goes through the transaction cache and reaches the medium
 * via the journal, never directly. */

#define SECSZ HYPE_BLK_SECTOR_SIZE

/* The transaction block cache: one instance, module-static (the writer is
 * BSP-serialized and exactly one transaction is in flight, ever; per-handle
 * copies would put 96 KiB inside every hype_fs_file_t). Reset per write. */
typedef struct {
    uint64_t blocknr;
    int used;
    int dirty;
    uint8_t data[4096];
} extj_slot_t;
static extj_slot_t g_cache[HYPE_EXTJ_CACHE];

/* superblock */
#define SB_BLOCKS_COUNT 0x04u
#define SB_FREE_BLOCKS 0x0Cu
#define SB_FIRST_DATA_BLOCK 0x14u
#define SB_LOG_BLOCK_SIZE 0x18u
#define SB_BLOCKS_PER_GROUP 0x20u
#define SB_INODES_PER_GROUP 0x28u
#define SB_MAGIC 0x38u
#define SB_STATE 0x3Au
#define SB_REV_LEVEL 0x4Cu
#define SB_INODE_SIZE 0x58u
#define SB_FEATURE_COMPAT 0x5Cu
#define SB_FEATURE_INCOMPAT 0x60u
#define SB_FEATURE_RO_COMPAT 0x64u
#define SB_JOURNAL_INUM 0xE0u
#define SB_JOURNAL_DEV 0xE4u

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
#define INCOMPAT_OK (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_FLEX_BG)
#define RO_OK 0x006Bu /* SPARSE_SUPER|LARGE_FILE|HUGE_FILE|DIR_NLINK|EXTRA_ISIZE */

/* group descriptor (32-byte, non-64bit) */
#define GD_BLOCK_BITMAP 0x00u
#define GD_INODE_TABLE 0x08u
#define GD_FREE_BLOCKS 0x0Cu

/* inode */
#define IN_MODE 0x00u
#define IN_CTIME 0x0Cu
#define IN_MTIME 0x10u
#define IN_BLOCKS 0x1Cu
#define IN_FLAGS 0x20u
#define IN_BLOCK 0x28u
#define FL_EXTENTS 0x00080000u
#define MODE_FMT 0xF000u
#define MODE_REG 0x8000u

/* extent tree */
#define EH_MAGIC 0xF30Au
#define EE_UNWRIT 32768u

#define JOURNAL_INO 8u

static void bzero8(uint8_t *p, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) p[i] = 0;
}
static void bcopy8(uint8_t *dst, const uint8_t *src, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}
/* backwards-safe move for in-block entry shifting */
static void bmove8(uint8_t *dst, const uint8_t *src, unsigned n) {
    unsigned i;
    if (dst < src) {
        for (i = 0; i < n; i++) dst[i] = src[i];
    } else {
        for (i = n; i > 0; i--) dst[i - 1u] = src[i - 1u];
    }
}

/* ---- the transaction block cache ---- */

static extj_slot_t *cache_get(hype_extj_wfile_t *f, uint64_t blocknr) {
    unsigned i, free_slot = HYPE_EXTJ_CACHE;
    if (blocknr >= f->blocks_count) return 0;
    for (i = 0; i < HYPE_EXTJ_CACHE; i++) {
        if (g_cache[i].used && g_cache[i].blocknr == blocknr) return &g_cache[i];
        if (!g_cache[i].used && free_slot == HYPE_EXTJ_CACHE) free_slot = i;
    }
    if (free_slot == HYPE_EXTJ_CACHE) return 0; /* over the credit bound */
    {
        extj_slot_t *s = &g_cache[free_slot];
        uint32_t sec;
        for (sec = 0; sec < f->spb; sec++) {
            if (f->read(f->ctx, blocknr * f->spb + sec, 1u, s->data + sec * SECSZ) != 0) {
                return 0;
            }
        }
        s->blocknr = blocknr;
        s->used = 1;
        s->dirty = 0;
        return s;
    }
}

static void cache_reset(hype_extj_wfile_t *f) {
    unsigned i;
    (void)f;
    for (i = 0; i < HYPE_EXTJ_CACHE; i++) {
        g_cache[i].used = 0;
        g_cache[i].dirty = 0;
    }
}

/*
 * Commit the transaction: journal every dirty image, expose it, write the
 * images to their final locations, checkpoint. After 0 is returned the
 * transaction is fully durable. A failure AFTER the journal commit leaves
 * the journal non-empty -- the honest state: replay (fsck/kernel) finishes
 * the transaction, and hype itself will refuse the volume until then.
 */
static int txn_commit(hype_extj_wfile_t *f) {
    hype_jbd2_block_t imgs[HYPE_JBD2_MAX_BLOCKS];
    unsigned i, n = 0;

    for (i = 0; i < HYPE_EXTJ_CACHE; i++) {
        if (g_cache[i].used && g_cache[i].dirty) {
            imgs[n].blocknr = g_cache[i].blocknr;
            imgs[n].data = g_cache[i].data;
            n++;
        }
    }
    if (n == 0u) return 0; /* nothing structural changed */
    if (hype_jbd2_commit(&f->journal, imgs, n) != 0) return -1; /* unexposed: safe */
    /* From here the transaction is EXPOSED: a failure leaves the media
     * part-old/part-new until replay applies the journal. The handle is
     * poisoned so no later write can build on that mixed state -- the
     * recovery path is fsck/mount replay, never hype continuing. */
    for (i = 0; i < HYPE_EXTJ_CACHE; i++) {
        if (g_cache[i].used && g_cache[i].dirty) {
            uint32_t sec;
            for (sec = 0; sec < f->spb; sec++) {
                if (f->write(f->ctx, g_cache[i].blocknr * f->spb + sec, 1u,
                             g_cache[i].data + sec * SECSZ) != 0) {
                    f->dead = 1;
                    return -1;
                }
            }
        }
    }
    if (hype_jbd2_checkpoint(&f->journal) != 0) {
        f->dead = 1;
        return -1;
    }
    return 0;
}

/* ---- cached metadata accessors ---- */

static uint64_t gd_blocknr(const hype_extj_wfile_t *f, uint32_t group, uint32_t *out_off) {
    uint64_t byte = (uint64_t)(f->first_data_block + 1u) * f->block_size + (uint64_t)group * 32u;
    *out_off = (uint32_t)(byte % f->block_size);
    return byte / f->block_size;
}

/* Find + claim one free block; all bookkeeping through the cache. */
static int claim_block(hype_extj_wfile_t *f, uint64_t near, uint64_t *out) {
    uint32_t start_group =
        (near >= f->first_data_block)
            ? (uint32_t)((near - f->first_data_block) / f->blocks_per_group) % f->groups
            : 0u;
    uint32_t gi;

    for (gi = 0; gi < f->groups; gi++) {
        uint32_t group = (start_group + gi) % f->groups;
        uint32_t gd_off;
        uint64_t gdb = gd_blocknr(f, group, &gd_off);
        extj_slot_t *gd = cache_get(f, gdb);
        uint64_t bmb;
        extj_slot_t *bm;
        uint64_t base = (uint64_t)group * f->blocks_per_group + f->first_data_block;
        uint32_t in_group = f->blocks_per_group;
        uint32_t b;

        if (gd == 0) return -1;
        if (base >= f->blocks_count) break;
        if (in_group > f->blocks_count - base) in_group = (uint32_t)(f->blocks_count - base);
        if (hype_rd16(gd->data + gd_off + GD_FREE_BLOCKS) == 0u) continue;
        bmb = hype_rd32(gd->data + gd_off + GD_BLOCK_BITMAP);
        if (bmb == 0u || bmb >= f->blocks_count) return -1;
        bm = cache_get(f, bmb);
        if (bm == 0) return -1;
        for (b = 0; b < in_group; b++) {
            if (!(bm->data[b / 8u] & (1u << (b % 8u)))) {
                uint16_t freeb = hype_rd16(gd->data + gd_off + GD_FREE_BLOCKS);
                bm->data[b / 8u] |= (uint8_t)(1u << (b % 8u));
                bm->dirty = 1;
                hype_wr16(gd->data + gd_off + GD_FREE_BLOCKS, (uint16_t)(freeb - 1u));
                gd->dirty = 1;
                /* superblock free count */
                {
                    uint64_t sbb = 1024u / f->block_size;
                    uint32_t sbo = (f->block_size == 1024u) ? 0u : 1024u;
                    extj_slot_t *sb = cache_get(f, sbb);
                    uint32_t v;
                    if (sb == 0) return -1;
                    v = hype_rd32(sb->data + sbo + SB_FREE_BLOCKS);
                    hype_wr32(sb->data + sbo + SB_FREE_BLOCKS, v - 1u);
                    sb->dirty = 1;
                }
                *out = base + b;
                return 0;
            }
        }
    }
    return -1; /* full */
}

/* The inode-table block slot + the inode's offset inside it. */
static extj_slot_t *inode_slot(hype_extj_wfile_t *f, uint32_t *out_off) {
    extj_slot_t *s = cache_get(f, f->inode_byte / f->block_size);
    if (s != 0) *out_off = (uint32_t)(f->inode_byte % f->block_size);
    return s;
}

/* The inode-table block is ALWAYS resident by the time blocks are added:
 * every allocation path begins by reading the inode through the cache, and
 * slots are never evicted within a transaction. Hence no failure path. */
static void inode_add_blocks(hype_extj_wfile_t *f, uint32_t nblocks) {
    uint32_t off;
    extj_slot_t *s = inode_slot(f, &off);
    hype_wr32(s->data + off + IN_BLOCKS,
              hype_rd32(s->data + off + IN_BLOCKS) + nblocks * f->spb);
    if (f->mtime != 0u) {
        hype_wr32(s->data + off + IN_MTIME, f->mtime);
        hype_wr32(s->data + off + IN_CTIME, f->mtime);
    }
    s->dirty = 1;
}

/* Zero a freshly claimed block on the MEDIUM (data-before-metadata). */
static int media_zero_block(hype_extj_wfile_t *f, uint64_t blk) {
    static const uint8_t z[SECSZ];
    uint32_t s;
    for (s = 0; s < f->spb; s++) {
        if (f->write(f->ctx, blk * f->spb + s, 1u, z) != 0) return -1;
    }
    return 0;
}

/* Write one whole file block's content: zeros overlaid with the write's
 * slice. Direct to the medium -- data never goes through the journal. */
static int media_block_content(hype_extj_wfile_t *f, uint64_t blk, uint64_t lb, uint64_t offset,
                               uint64_t end, const uint8_t *data) {
    uint8_t sec[SECSZ];
    uint32_t s;
    uint64_t blk_off = lb * f->block_size;
    for (s = 0; s < f->spb; s++) {
        uint64_t ss = blk_off + (uint64_t)s * SECSZ, se = ss + SECSZ;
        bzero8(sec, SECSZ);
        if (se > offset && ss < end) {
            uint64_t from = (offset > ss) ? offset - ss : 0u;
            uint64_t to = (end < se) ? end - ss : SECSZ;
            bcopy8(sec + from, data + (ss + from - offset), (unsigned)(to - from));
        }
        if (f->write(f->ctx, blk * f->spb + s, 1u, sec) != 0) return -1;
    }
    return 0;
}

/* ---- classic (ext3) block maps: allocation through the cache ---- */

static int classic_map_block(hype_extj_wfile_t *f, uint64_t lb, uint64_t *out_blk,
                             int *out_fresh) {
    uint32_t ppb = f->block_size / 4u;
    uint32_t path[3];
    uint32_t levels, root_slot, li;
    uint64_t node;
    uint32_t ioff;
    extj_slot_t *is = inode_slot(f, &ioff);

    if (is == 0) return -1;
    if (lb < 12u) {
        root_slot = (uint32_t)lb;
        levels = 0;
    } else {
        uint64_t r = lb - 12u;
        if (r < ppb) {
            root_slot = 12u; levels = 1; path[0] = (uint32_t)r;
        } else if ((r -= ppb) < (uint64_t)ppb * ppb) {
            root_slot = 13u; levels = 2;
            path[0] = (uint32_t)(r / ppb); path[1] = (uint32_t)(r % ppb);
        } else if ((r -= (uint64_t)ppb * ppb) < (uint64_t)ppb * ppb * ppb) {
            root_slot = 14u; levels = 3;
            path[0] = (uint32_t)(r / ((uint64_t)ppb * ppb));
            path[1] = (uint32_t)((r / ppb) % ppb);
            path[2] = (uint32_t)(r % ppb);
        } else {
            return -1;
        }
    }

    node = hype_rd32(is->data + ioff + IN_BLOCK + root_slot * 4u);
    if (levels == 0u) {
        if (node != 0u) { *out_blk = node; *out_fresh = 0; return 0; }
        if (claim_block(f, f->inode_byte / SECSZ / f->spb, out_blk) != 0) return -1;
        hype_wr32(is->data + ioff + IN_BLOCK + root_slot * 4u, (uint32_t)*out_blk);
        is->dirty = 1;
        inode_add_blocks(f, 1u);
        *out_fresh = 1;
        return 0;
    }
    if (node == 0u) {
        uint64_t nb;
        extj_slot_t *ps;
        if (claim_block(f, f->inode_byte / SECSZ / f->spb, &nb) != 0) return -1;
        if (media_zero_block(f, nb) != 0) return -1; /* pre-image for replay */
        ps = cache_get(f, nb);
        if (ps == 0) return -1;
        bzero8(ps->data, f->block_size);
        ps->dirty = 1; /* journaled as an all-zero pointer block */
        hype_wr32(is->data + ioff + IN_BLOCK + root_slot * 4u, (uint32_t)nb);
        is->dirty = 1;
        inode_add_blocks(f, 1u);
        node = nb;
    }
    for (li = 0; li + 1u < levels; li++) {
        extj_slot_t *ps = cache_get(f, node);
        uint64_t child;
        if (ps == 0) return -1;
        child = hype_rd32(ps->data + path[li] * 4u);
        if (child == 0u) {
            uint64_t nb;
            extj_slot_t *cs;
            if (claim_block(f, node, &nb) != 0) return -1;
            if (media_zero_block(f, nb) != 0) return -1;
            cs = cache_get(f, nb);
            if (cs == 0) return -1;
            bzero8(cs->data, f->block_size);
            cs->dirty = 1;
            hype_wr32(ps->data + path[li] * 4u, (uint32_t)nb);
            ps->dirty = 1;
            inode_add_blocks(f, 1u);
            child = nb;
        } else if (child >= f->blocks_count) {
            return -1;
        }
        node = child;
    }
    {
        extj_slot_t *ps = cache_get(f, node);
        uint64_t blk;
        if (ps == 0) return -1;
        blk = hype_rd32(ps->data + path[levels - 1u] * 4u);
        if (blk != 0u) { *out_blk = blk; *out_fresh = 0; return 0; }
        if (claim_block(f, node, &blk) != 0) return -1;
        hype_wr32(ps->data + path[levels - 1u] * 4u, (uint32_t)blk);
        ps->dirty = 1;
        inode_add_blocks(f, 1u);
        *out_blk = blk;
        *out_fresh = 1;
        return 0;
    }
}

/* ---- ext4 extent trees: mutation through the cache ---- */

/* One node on the root-to-leaf path. root: slot==inode-table slot, base ==
 * offset of the extent header inside it, capacity from eh_max. */
typedef struct {
    extj_slot_t *slot;
    uint32_t base; /* byte offset of the node's extent header in slot->data */
} epath_t;

#define EXT_DEPTH_MAX 5u

static uint16_t node_entries(const epath_t *n) { return hype_rd16(n->slot->data + n->base + 2u); }
static uint16_t node_max(const epath_t *n) { return hype_rd16(n->slot->data + n->base + 4u); }
static uint16_t node_depth(const epath_t *n) { return hype_rd16(n->slot->data + n->base + 6u); }
static uint8_t *node_entry(const epath_t *n, uint32_t i) {
    return n->slot->data + n->base + 12u + i * 12u;
}
static void node_set_entries(epath_t *n, uint16_t v) {
    hype_wr16(n->slot->data + n->base + 2u, v);
    n->slot->dirty = 1;
}

/*
 * Walks the tree to the leaf covering (or that WOULD cover) logical block
 * `lb`. Fills path[0..*out_depth]; path[*out_depth] is the leaf. Also
 * returns the entry index in each interior node that was followed.
 */
static int epath_find(hype_extj_wfile_t *f, uint64_t lb, epath_t *path, uint32_t *idx,
                      uint32_t *out_depth) {
    uint32_t ioff;
    extj_slot_t *is = inode_slot(f, &ioff);
    uint32_t d, depth;

    if (is == 0) return -1;
    path[0].slot = is;
    path[0].base = ioff + IN_BLOCK;
    if (hype_rd16(path[0].slot->data + path[0].base) != EH_MAGIC) return -1;
    depth = node_depth(&path[0]);
    if (depth > EXT_DEPTH_MAX) return -1;
    for (d = 0; d < depth; d++) {
        uint16_t n = node_entries(&path[d]);
        uint32_t i, follow = 0;
        uint64_t child;
        if (n == 0u) return -1; /* an interior node with no children */
        for (i = 0; i < n; i++) {
            uint32_t first = hype_rd32(node_entry(&path[d], i));
            if ((uint64_t)first <= lb) follow = i;
        }
        idx[d] = follow;
        child = (uint64_t)hype_rd32(node_entry(&path[d], follow) + 4u) |
                ((uint64_t)hype_rd16(node_entry(&path[d], follow) + 8u) << 32);
        if (child == 0u || child >= f->blocks_count) return -1;
        path[d + 1u].slot = cache_get(f, child);
        if (path[d + 1u].slot == 0) return -1;
        path[d + 1u].base = 0;
        if (hype_rd16(path[d + 1u].slot->data) != EH_MAGIC) return -1;
        if (node_depth(&path[d + 1u]) != depth - d - 1u) return -1;
    }
    *out_depth = depth;
    return 0;
}

/* Insert a 12-byte entry image at index `at` of node `n` (must have room). */
static void node_insert(epath_t *n, uint32_t at, const uint8_t ent[12]) {
    uint16_t cnt = node_entries(n);
    bmove8(node_entry(n, at + 1u), node_entry(n, at), (cnt - at) * 12u);
    bcopy8(node_entry(n, at), ent, 12u);
    node_set_entries(n, (uint16_t)(cnt + 1u));
}

/*
 * Ensure the leaf at path[depth] has room for one more entry, splitting the
 * leaf (and, when the ROOT is the full node, growing the tree) as needed.
 * Re-walks the path afterwards. Bounded: one leaf split OR one root growth
 * per call; a full interior node above a full leaf is refused (a shape a
 * bounded transaction cannot safely mutate).
 */
static int leaf_make_room(hype_extj_wfile_t *f, uint64_t lb, epath_t *path, uint32_t *idx,
                          uint32_t *depth, unsigned min_free) {
    epath_t *leaf = &path[*depth];
    if ((uint32_t)node_entries(leaf) + min_free <= node_max(leaf)) return 0;

    if (*depth == 0u) {
        /* the in-inode root is full: grow the tree by one level */
        uint64_t nb;
        extj_slot_t *ns;
        uint8_t ent[12];
        if (claim_block(f, f->inode_byte / SECSZ / f->spb, &nb) != 0) return -1;
        if (media_zero_block(f, nb) != 0) return -1;
        ns = cache_get(f, nb);
        if (ns == 0) return -1;
        bzero8(ns->data, f->block_size);
        /* the new node inherits every root entry, same depth as the root had */
        hype_wr16(ns->data + 0, EH_MAGIC);
        hype_wr16(ns->data + 2, node_entries(&path[0]));
        hype_wr16(ns->data + 4, (uint16_t)((f->block_size - 12u) / 12u));
        hype_wr16(ns->data + 6, node_depth(&path[0]));
        bcopy8(ns->data + 12u, node_entry(&path[0], 0), node_entries(&path[0]) * 12u);
        ns->dirty = 1;
        /* the root becomes a one-entry index a level up */
        hype_wr16(path[0].slot->data + path[0].base + 6u,
                  (uint16_t)(node_depth(&path[0]) + 1u));
        node_set_entries(&path[0], 0u);
        hype_wr32(ent + 0, hype_rd32(ns->data + 12u)); /* first key below */
        hype_wr32(ent + 4, (uint32_t)nb);
        hype_wr16(ent + 8, 0u);
        hype_wr16(ent + 10, 0u);
        node_insert(&path[0], 0u, ent);
        inode_add_blocks(f, 1u);
        if (epath_find(f, lb, path, idx, depth) != 0) return -1;
        /* the new on-disk leaf may still be too full for min_free */
        return leaf_make_room(f, lb, path, idx, depth, min_free);
    }

    /* a full on-disk leaf: split the upper half into a fresh block */
    {
        epath_t *parent = &path[*depth - 1u];
        uint16_t cnt = node_entries(leaf);
        uint16_t keep = (uint16_t)(cnt / 2u);
        uint64_t nb;
        extj_slot_t *ns;
        uint8_t ent[12];
        if (node_entries(parent) >= node_max(parent)) {
            return -1; /* cascading splits exceed the bounded transaction */
        }
        if (claim_block(f, leaf->slot->blocknr, &nb) != 0) return -1;
        if (media_zero_block(f, nb) != 0) return -1;
        ns = cache_get(f, nb);
        if (ns == 0) return -1;
        bzero8(ns->data, f->block_size);
        hype_wr16(ns->data + 0, EH_MAGIC);
        hype_wr16(ns->data + 2, (uint16_t)(cnt - keep));
        hype_wr16(ns->data + 4, (uint16_t)((f->block_size - 12u) / 12u));
        hype_wr16(ns->data + 6, 0u);
        bcopy8(ns->data + 12u, node_entry(leaf, keep), (uint32_t)(cnt - keep) * 12u);
        ns->dirty = 1;
        node_set_entries(leaf, keep);
        hype_wr32(ent + 0, hype_rd32(ns->data + 12u));
        hype_wr32(ent + 4, (uint32_t)nb);
        hype_wr16(ent + 8, 0u);
        hype_wr16(ent + 10, 0u);
        node_insert(parent, idx[*depth - 1u] + 1u, ent);
        inode_add_blocks(f, 1u);
        if (epath_find(f, lb, path, idx, depth) != 0) return -1;
        /* after one split each half has room for any bounded min_free */
        return ((uint32_t)node_entries(&path[*depth]) + min_free <= node_max(&path[*depth]))
                   ? 0
                   : -1;
    }
}

/* Insert (lb -> phys, 1 block, written) into the tree, merging with a
 * directly contiguous predecessor when possible. */
static int extent_insert_block(hype_extj_wfile_t *f, uint64_t lb, uint64_t phys) {
    epath_t path[EXT_DEPTH_MAX + 1u];
    uint32_t idx[EXT_DEPTH_MAX + 1u];
    uint32_t depth;
    epath_t *leaf;
    uint16_t cnt;
    uint32_t at = 0, i;

    if (epath_find(f, lb, path, idx, &depth) != 0) return -1;
    leaf = &path[depth];
    cnt = node_entries(leaf);
    for (i = 0; i < cnt; i++) {
        if ((uint64_t)hype_rd32(node_entry(leaf, i)) <= lb) at = i + 1u;
    }
    /* merge with the predecessor when logically AND physically adjacent */
    if (at > 0u) {
        uint8_t *pe = node_entry(leaf, at - 1u);
        uint32_t plb = hype_rd32(pe);
        uint16_t plen = hype_rd16(pe + 4);
        uint64_t pph = (uint64_t)hype_rd32(pe + 8) | ((uint64_t)hype_rd16(pe + 6) << 32);
        if (plen != 0u && plen < EE_UNWRIT && (uint64_t)plb + plen == lb &&
            pph + plen == phys && plen + 1u < EE_UNWRIT) {
            hype_wr16(pe + 4, (uint16_t)(plen + 1u));
            leaf->slot->dirty = 1;
            return 0;
        }
    }
    if (node_entries(leaf) >= node_max(leaf)) {
        if (leaf_make_room(f, lb, path, idx, &depth, 1u) != 0) return -1;
        leaf = &path[depth];
        cnt = node_entries(leaf);
        at = 0;
        for (i = 0; i < cnt; i++) {
            if ((uint64_t)hype_rd32(node_entry(leaf, i)) <= lb) at = i + 1u;
        }
    }
    {
        uint8_t ent[12];
        hype_wr32(ent + 0, (uint32_t)lb);
        hype_wr16(ent + 4, 1u);
        hype_wr16(ent + 6, (uint16_t)(phys >> 32));
        hype_wr32(ent + 8, (uint32_t)phys);
        node_insert(leaf, at, ent);
    }
    /* a new FIRST key must be reflected in the parent chain */
    if (at == 0u && depth > 0u) {
        uint32_t d;
        for (d = depth; d > 0u; d--) {
            if (idx[d - 1u] == 0u || 1) {
                uint8_t *pe = node_entry(&path[d - 1u], idx[d - 1u]);
                if ((uint64_t)hype_rd32(pe) > lb) {
                    hype_wr32(pe, (uint32_t)lb);
                    path[d - 1u].slot->dirty = 1;
                }
            }
        }
    }
    return 0;
}

/*
 * Convert the single block `lb` inside an UNWRITTEN extent to written:
 * split the extent into up to three (before / this block, written / after).
 * The leaf needs room for up to two extra entries -- checked up front.
 */
static int extent_convert_block(hype_extj_wfile_t *f, uint64_t lb) {
    epath_t path[EXT_DEPTH_MAX + 1u];
    uint32_t idx[EXT_DEPTH_MAX + 1u];
    uint32_t depth;
    epath_t *leaf;
    uint16_t cnt;
    uint32_t i;

    if (epath_find(f, lb, path, idx, &depth) != 0) return -1;
    leaf = &path[depth];
    cnt = node_entries(leaf);
    for (i = 0; i < cnt; i++) {
        uint8_t *e = node_entry(leaf, i);
        uint32_t elb = hype_rd32(e);
        uint16_t raw = hype_rd16(e + 4);
        uint16_t elen = (uint16_t)(raw > EE_UNWRIT ? raw - EE_UNWRIT : raw);
        uint64_t eph = (uint64_t)hype_rd32(e + 8) | ((uint64_t)hype_rd16(e + 6) << 32);
        if (raw <= EE_UNWRIT) continue; /* already written */
        if (lb < elb || lb >= (uint64_t)elb + elen) continue;
        {
            uint32_t before = (uint32_t)(lb - elb);
            uint32_t after = (uint32_t)(elb + elen - lb - 1u);
            unsigned need = (before ? 1u : 0u) + (after ? 1u : 0u);
            if (need != 0u && (uint32_t)node_entries(leaf) + need > node_max(leaf)) {
                /* make room for the split pieces, then redo the scan against
                 * the (re-walked) leaf -- ONE bounded retry, not recursion */
                if (leaf_make_room(f, lb, path, idx, &depth, need) != 0) return -1;
                leaf = &path[depth];
                cnt = node_entries(leaf);
                i = (uint32_t)-1; /* restart the entry scan */
                continue;
            }
            /* rewrite this entry as the WRITTEN single block */
            hype_wr32(e + 0, (uint32_t)lb);
            hype_wr16(e + 4, 1u);
            hype_wr16(e + 6, (uint16_t)((eph + before) >> 32));
            hype_wr32(e + 8, (uint32_t)(eph + before));
            leaf->slot->dirty = 1;
            if (after) {
                uint8_t ent[12];
                hype_wr32(ent + 0, (uint32_t)(lb + 1u));
                hype_wr16(ent + 4, (uint16_t)(EE_UNWRIT + after));
                hype_wr16(ent + 6, (uint16_t)((eph + before + 1u) >> 32));
                hype_wr32(ent + 8, (uint32_t)(eph + before + 1u));
                node_insert(leaf, i + 1u, ent);
            }
            if (before) {
                uint8_t ent[12];
                hype_wr32(ent + 0, elb);
                hype_wr16(ent + 4, (uint16_t)(EE_UNWRIT + before));
                hype_wr16(ent + 6, (uint16_t)(eph >> 32));
                hype_wr32(ent + 8, (uint32_t)eph);
                node_insert(leaf, i, ent);
            }
            return 0;
        }
    }
    return -1; /* lb was not inside an unwritten extent */
}

/* ---- open / read / write ---- */

int hype_extj_open_rw(hype_blk_read_fn read, hype_blk_write_fn write, void *ctx,
                      const char *path, hype_extj_wfile_t *out) {
    uint8_t sb[1024];
    uint32_t incompat, compat, rocompat, log_bs, rev;
    uint16_t state;

    if (read == 0 || write == 0) return -1;
    if (read(ctx, 2u, 2u, sb) != 0) return -1;
    if (hype_rd16(sb + SB_MAGIC) != EXT_MAGIC) return -1;
    compat = hype_rd32(sb + SB_FEATURE_COMPAT);
    incompat = hype_rd32(sb + SB_FEATURE_INCOMPAT);
    rocompat = hype_rd32(sb + SB_FEATURE_RO_COMPAT);
    if (!(compat & COMPAT_HAS_JOURNAL)) return -1; /* ext2: the OTHER writer */
    if (incompat & INCOMPAT_RECOVER) return -1;    /* unreplayed journal */
    if (incompat & INCOMPAT_JOURNAL_DEV) return -1;
    if (incompat & ~INCOMPAT_OK) return -1;        /* 64BIT, META_BG, bigalloc-adjacent, ... */
    if (rocompat & ~RO_OK) return -1;              /* incl. GDT_CSUM/METADATA_CSUM/BIGALLOC */
    state = hype_rd16(sb + SB_STATE);
    if ((state & STATE_VALID) == 0u || (state & STATE_ERROR) != 0u) return -1;
    log_bs = hype_rd32(sb + SB_LOG_BLOCK_SIZE);
    if (log_bs > 2u) return -1;
    if (hype_rd32(sb + SB_JOURNAL_INUM) != JOURNAL_INO) return -1; /* incl. external (0) */

    out->read = read;
    out->write = write;
    out->ctx = ctx;
    out->block_size = 1024u << log_bs;
    out->spb = out->block_size / SECSZ;
    out->blocks_count = hype_rd32(sb + SB_BLOCKS_COUNT);
    out->blocks_per_group = hype_rd32(sb + SB_BLOCKS_PER_GROUP);
    out->first_data_block = hype_rd32(sb + SB_FIRST_DATA_BLOCK);
    out->inodes_per_group = hype_rd32(sb + SB_INODES_PER_GROUP);
    rev = hype_rd32(sb + SB_REV_LEVEL);
    out->inode_size = (rev == 0u) ? 128u : (uint32_t)hype_rd16(sb + SB_INODE_SIZE);
    out->mtime = 0;
    out->dead = 0;
    if (out->blocks_per_group == 0u || out->inodes_per_group == 0u || out->blocks_count < 2u ||
        out->inode_size < 128u) {
        return -1;
    }
    out->groups = (uint32_t)((out->blocks_count - out->first_data_block +
                              out->blocks_per_group - 1u) /
                             out->blocks_per_group);

    /* the journal itself, via inode 8's map: validated + must be empty */
    {
        static hype_file_rmap_t jmap;
        if (hype_ext_map_ino_rmap(read, ctx, JOURNAL_INO, &jmap) != 0) return -1;
        if (hype_jbd2_open(&out->journal, read, write, ctx, out->block_size, &jmap) != 0) {
            return -1;
        }
    }

    if (hype_ext_resolve_rmap(read, ctx, path, &out->map) != 0) return -1;
    out->size_bytes = out->map.size_bytes;
    if (hype_ext_resolve_ino(read, ctx, path, &out->ino) != 0) return -1;
    {
        uint8_t sec[SECSZ];
        uint32_t group = (out->ino - 1u) / out->inodes_per_group;
        uint32_t index = (out->ino - 1u) % out->inodes_per_group;
        uint64_t gdb = (uint64_t)(out->first_data_block + 1u) * out->block_size +
                       (uint64_t)group * 32u + GD_INODE_TABLE;
        uint64_t table;
        if (read(ctx, gdb / SECSZ, 1u, sec) != 0) return -1;
        table = hype_rd32(sec + gdb % SECSZ);
        if (table == 0u || table >= out->blocks_count) return -1;
        out->inode_byte = table * out->block_size + (uint64_t)index * out->inode_size;
        if (read(ctx, out->inode_byte / SECSZ, 1u, sec) != 0) return -1;
        out->is_extents = (hype_rd32(sec + out->inode_byte % SECSZ + IN_FLAGS) & FL_EXTENTS) ? 1 : 0;
        if ((hype_rd16(sec + out->inode_byte % SECSZ + IN_MODE) & MODE_FMT) != MODE_REG) return -1;
    }
    cache_reset(out);
    return 0;
}

void hype_extj_set_time(hype_extj_wfile_t *f, uint32_t unix_seconds) {
    f->mtime = unix_seconds;
}

int hype_extj_read_at(hype_extj_wfile_t *f, uint64_t offset, void *out, unsigned int len) {
    return hype_file_rmap_read_at(&f->map, f->read, f->ctx, offset, out, len);
}

int hype_extj_write_at(hype_extj_wfile_t *f, uint64_t offset, const void *data,
                       unsigned int len) {
    const uint8_t *src = (const uint8_t *)data;
    uint64_t end = offset + len;
    uint64_t bs;

    if (f->dead) return -1; /* an exposed transaction awaits replay */
    if (len == 0u) return 0;
    if (end < offset || end > f->size_bytes) return -1;
    bs = f->block_size;

    /* the metadata-free fast path stays: a span wholly inside DATA */
    {
        uint64_t probe = offset;
        int all_data = 1;
        while (probe < end) {
            hype_range_kind_t kind;
            uint64_t lba, run;
            uint32_t head;
            if (hype_file_rmap_locate(&f->map, probe, &kind, &lba, &head, &run) != 0) return -1;
            if (kind != HYPE_RANGE_DATA) { all_data = 0; break; }
            probe += run;
        }
        if (all_data) {
            return hype_file_rmap_write_at(&f->map, f->read, f->write, f->ctx, offset, src, len);
        }
    }

    /* Structural work: one bounded journaled transaction. The bound is on
     * METADATA images (the transaction cache's slots), not on data blocks --
     * data is never journaled. A span whose metadata footprint exceeds the
     * cache is refused by cache_get() mid-flight and rolled back below. */
    cache_reset(f);

    {
        uint64_t pos = offset;
        while (pos < end) {
            uint64_t lb = pos / bs;
            uint64_t n = bs - pos % bs;
            hype_range_kind_t kind;
            uint64_t lba, run;
            uint32_t head;
            if (n > end - pos) n = end - pos;
            if (hype_file_rmap_locate(&f->map, pos, &kind, &lba, &head, &run) != 0) return -1;

            if (kind == HYPE_RANGE_DATA) {
                uint64_t m = (run < n) ? run : n;
                if (hype_file_rmap_write_at(&f->map, f->read, f->write, f->ctx, pos, src,
                                            (unsigned int)m) != 0) {
                    return -1;
                }
                pos += m;
                src += m;
                continue;
            }

            if (kind == HYPE_RANGE_UNWRITTEN) {
                /* the block is allocated at `lba`'s block: write zeros+data
                 * to the medium, then convert it to written in the tree */
                uint64_t blk = lba / f->spb;
                if (!f->is_extents) return -1; /* classic maps have no unwritten state */
                if (media_block_content(f, blk, lb, offset, end, (const uint8_t *)data) != 0) {
                    return -1;
                }
                if (extent_convert_block(f, lb) != 0) return -1;
            } else {
                /* a hole: claim a block, content first, then map it */
                uint64_t blk = 0;
                int fresh = 0;
                if (f->is_extents) {
                    if (claim_block(f, f->inode_byte / SECSZ / f->spb, &blk) != 0) return -1;
                    if (media_block_content(f, blk, lb, offset, end, (const uint8_t *)data) != 0) {
                        return -1;
                    }
                    if (extent_insert_block(f, lb, blk) != 0) return -1;
                    inode_add_blocks(f, 1u);
                } else {
                    if (classic_map_block(f, lb, &blk, &fresh) != 0) return -1;
                    if (media_block_content(f, blk, lb, offset, end, (const uint8_t *)data) != 0) {
                        return -1;
                    }
                }
            }
            pos = (lb + 1u) * bs;
            if (pos > end) pos = end;
            src = (const uint8_t *)data + (pos - offset);
        }
    }

    if (txn_commit(f) != 0) return -1;
    cache_reset(f);
    return hype_ext_map_ino_rmap(f->read, f->ctx, f->ino, &f->map);
}
