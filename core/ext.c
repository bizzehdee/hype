#include "ext.h"
#include "lebytes.h"
#include "file_range.h"

/*
 * #203: read-only ext2/3/4 path-to-extents resolver. Layout facts are from the
 * kernel's Documentation/filesystems/ext4/ on-disk format description and
 * fs/ext4/ext4.h, cross-checked byte-for-byte against real mkfs.ext2/ext4
 * (e2fsprogs) images -- see the ticket notes.
 *
 * Everything is bounds-checked against the volume's own block count before a
 * single derived LBA is read: this code parses attacker-shapeable bytes (a
 * plugged-in disk), and a crafted block pointer must fail the resolve, never
 * steer a read outside the volume or wedge a walk (AGENTS.md input-validation
 * rule; same posture as core/fat.c).
 *
 * All I/O is done in one- or two-sector pieces (block_bytes / pcache_t), never
 * whole-filesystem-block stack buffers: hype's freestanding build keeps every
 * stack frame under a page, and the extent-tree walk below is iterative with
 * an explicit bounded stack for the same reason.
 */

#define SECSZ HYPE_BLK_SECTOR_SIZE

/* Superblock byte offsets (little-endian). */
#define SB_INODES_COUNT 0x00u
#define SB_BLOCKS_COUNT_LO 0x04u
#define SB_FIRST_DATA_BLOCK 0x14u
#define SB_LOG_BLOCK_SIZE 0x18u
#define SB_INODES_PER_GROUP 0x28u
#define SB_MAGIC 0x38u
#define SB_REV_LEVEL 0x4Cu
#define SB_INODE_SIZE 0x58u
#define SB_FEATURE_INCOMPAT 0x60u
#define SB_DESC_SIZE 0xFEu
#define SB_BLOCKS_COUNT_HI 0x150u

#define EXT_MAGIC 0xEF53u

/* Incompatible features. Anything set outside SUPPORTED refuses the mount. */
#define INCOMPAT_FILETYPE 0x0002u
#define INCOMPAT_RECOVER 0x0004u /* journal needs replay: metadata is stale */
#define INCOMPAT_EXTENTS 0x0040u
#define INCOMPAT_64BIT 0x0080u
#define INCOMPAT_FLEX_BG 0x0200u
#define INCOMPAT_CSUM_SEED 0x2000u /* seeds metadata checksums, which this
                                    * reader never verifies: safe to ignore
                                    * (mkfs.ext4 sets it by default now) */
#define INCOMPAT_SUPPORTED \
    (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG | INCOMPAT_CSUM_SEED)

/* Group descriptor offsets. */
#define GD_INODE_TABLE_LO 0x08u
#define GD_INODE_TABLE_HI 0x28u /* present when desc_size >= 64 (64BIT) */

/* Inode offsets (the first 128 bytes carry everything a reader needs). */
#define IN_MODE 0x00u
#define IN_SIZE_LO 0x04u
#define IN_FLAGS 0x20u
#define IN_BLOCK 0x28u /* i_block[15]: 60 bytes of map or extent-tree root */
#define IN_SIZE_HIGH 0x6Cu
#define IN_CORE 128u /* bytes of the inode this reader uses */

#define MODE_FMT 0xF000u
#define MODE_DIR 0x4000u
#define MODE_REG 0x8000u

#define FL_EXTENTS 0x00080000u /* i_block holds an extent tree */
#define FL_INLINE 0x10000000u  /* data lives in the inode: refused */

/* Extent tree on-disk shapes. */
#define EH_MAGIC 0xF30Au
#define EH_ENTRIES 2u
#define EH_MAX 4u
#define EH_DEPTH 6u
#define EH_SIZE 12u
#define EE_SIZE 12u
#define EXT_MAX_DEPTH 5u        /* the kernel's own cap */
#define EE_LEN_UNWRITTEN 32768u /* lengths ABOVE this mark unwritten extents */

#define EXT_MAX_NAME 255u


typedef struct {
    hype_blk_read_fn read;
    void *ctx;
    uint32_t block_size;
    uint32_t spb; /* 512-byte sectors per block */
    uint64_t blocks_count;
    uint32_t inodes_count;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t desc_size;
} ext_vol_t;

/*
 * Reads `len` (1..512) bytes at byte offset `off` inside filesystem block
 * `block`, through one- or two-sector reads. Block 0 is never a valid target
 * (the superblock is read directly at mount), so it doubles as the hole mark.
 */
static int block_bytes(ext_vol_t *v, uint64_t block, uint32_t off, uint32_t len, uint8_t *out) {
    uint8_t buf[2u * SECSZ];
    uint64_t sector;
    uint32_t within, nsec, i;

    if (len == 0u || len > SECSZ || off >= v->block_size || len > v->block_size - off) {
        return -1;
    }
    if (block == 0u || block >= v->blocks_count) {
        return -1;
    }
    sector = block * v->spb + off / SECSZ;
    within = off % SECSZ;
    nsec = (within + len + SECSZ - 1u) / SECSZ;
    if (v->read(v->ctx, sector, nsec, buf) != 0) {
        return -1;
    }
    for (i = 0; i < len; i++) {
        out[i] = buf[within + i];
    }
    return 0;
}

static int vol_open(hype_blk_read_fn read, void *ctx, ext_vol_t *v) {
    uint8_t sb[1024];
    uint32_t log_bs, incompat, rev;

    /* The superblock lives at byte 1024, whatever the block size. */
    if (read(ctx, 2u, 2u, sb) != 0) {
        return -1;
    }
    if (hype_rd16(sb + SB_MAGIC) != EXT_MAGIC) {
        return -1;
    }
    incompat = hype_rd32(sb + SB_FEATURE_INCOMPAT);
    if (incompat & INCOMPAT_RECOVER) {
        return -1; /* an unreplayed journal: nothing on disk can be trusted yet */
    }
    if (incompat & ~INCOMPAT_SUPPORTED) {
        return -1; /* a shape this reader does not understand: refuse, don't guess */
    }
    log_bs = hype_rd32(sb + SB_LOG_BLOCK_SIZE);
    if (log_bs > 2u) {
        return -1; /* > 4096-byte blocks */
    }
    v->read = read;
    v->ctx = ctx;
    v->block_size = 1024u << log_bs;
    v->spb = v->block_size / SECSZ;
    v->blocks_count = hype_rd32(sb + SB_BLOCKS_COUNT_LO);
    if (incompat & INCOMPAT_64BIT) {
        v->blocks_count |= (uint64_t)hype_rd32(sb + SB_BLOCKS_COUNT_HI) << 32;
    }
    v->inodes_count = hype_rd32(sb + SB_INODES_COUNT);
    v->inodes_per_group = hype_rd32(sb + SB_INODES_PER_GROUP);
    v->first_data_block = hype_rd32(sb + SB_FIRST_DATA_BLOCK);
    rev = hype_rd32(sb + SB_REV_LEVEL);
    v->inode_size = (rev == 0u) ? 128u : (uint32_t)hype_rd16(sb + SB_INODE_SIZE);
    v->desc_size = (incompat & INCOMPAT_64BIT) ? (uint32_t)hype_rd16(sb + SB_DESC_SIZE) : 32u;

    if (v->blocks_count < 2u || v->inodes_count == 0u || v->inodes_per_group == 0u) {
        return -1;
    }
    if (v->first_data_block != ((v->block_size == 1024u) ? 1u : 0u)) {
        return -1;
    }
    /* Power-of-two inode size in [128, 1024]; group descriptors are 32 bytes
     * or a declared power-of-two of at least 64 -- all divide the sector, so
     * neither a <= 512-byte inode nor a descriptor ever straddles one. */
    if (v->inode_size < 128u || v->inode_size > 1024u ||
        (v->inode_size & (v->inode_size - 1u)) != 0u) {
        return -1;
    }
    if (v->desc_size != 32u && (v->desc_size < 64u || v->desc_size > 512u ||
                                (v->desc_size & (v->desc_size - 1u)) != 0u)) {
        return -1;
    }
    return 0;
}

/* Reads the first IN_CORE bytes of inode `ino` into `out`. */
static int inode_read(ext_vol_t *v, uint32_t ino, uint8_t out[IN_CORE]) {
    uint8_t sec[SECSZ];
    uint64_t gdt_byte, table_block, inode_byte;
    uint32_t group, index, i;

    if (ino == 0u || ino > v->inodes_count) {
        return -1;
    }
    group = (ino - 1u) / v->inodes_per_group;
    index = (ino - 1u) % v->inodes_per_group;

    /* The group descriptor table starts in the block after the superblock. */
    gdt_byte = (uint64_t)(v->first_data_block + 1u) * v->block_size + (uint64_t)group * v->desc_size;
    if (v->read(v->ctx, gdt_byte / SECSZ, 1u, sec) != 0) {
        return -1;
    }
    {
        const uint8_t *gd = sec + (gdt_byte % SECSZ);
        table_block = hype_rd32(gd + GD_INODE_TABLE_LO);
        if (v->desc_size >= 64u) {
            table_block |= (uint64_t)hype_rd32(gd + GD_INODE_TABLE_HI) << 32;
        }
    }
    if (table_block == 0u || table_block >= v->blocks_count) {
        return -1;
    }
    inode_byte = table_block * v->block_size + (uint64_t)index * v->inode_size;
    /* An inode table must stay inside the volume. */
    if (inode_byte / SECSZ >= v->blocks_count * v->spb) {
        return -1;
    }
    if (v->read(v->ctx, inode_byte / SECSZ, 1u, sec) != 0) {
        return -1;
    }
    /* IN_CORE == 128 divides the sector, so the copy never straddles it. */
    for (i = 0; i < IN_CORE; i++) {
        out[i] = sec[(inode_byte % SECSZ) + i];
    }
    return 0;
}

/* ---- extent emission (shared by both mapping schemes) ---- */

typedef struct {
    hype_file_map_t *out;   /* legacy physical-only target (refuses holes) */
    hype_file_rmap_t *rout; /* #384 sparse target: gaps become HOLE ranges */
    ext_vol_t *v;
    uint64_t next_logical; /* the next file block expected: gaps are holes */
    uint64_t total_blocks; /* ceil(size / block_size) */
} emit_t;

/*
 * Emits `count` file blocks that live physically at `phys`, logically at
 * `logical`. Refuses holes (logical skips) and out-of-order maps, clamps a
 * final extent that runs past EOF (preallocated tails), bounds every physical
 * block against the volume, and coalesces adjacent runs.
 */
static int emit_run(emit_t *e, uint64_t logical, uint64_t phys, uint64_t count) {
    hype_file_map_t *f = e->out;
    uint64_t start_lba, sectors;

    if (count == 0u || logical < e->next_logical) {
        return -1; /* zero-length or out-of-order */
    }
    if (logical != e->next_logical) {
        /* a logical gap: an explicit HOLE under the #384 contract, corruption
         * under the legacy physical-only one */
        if (e->rout == 0) {
            return -1;
        }
        if (logical > e->total_blocks) {
            return -1;
        }
        if (hype_file_rmap_append(e->rout, HYPE_RANGE_HOLE, 0,
                                  (logical - e->next_logical) * e->v->spb) != 0) {
            return -1;
        }
        e->next_logical = logical;
    }
    if (logical >= e->total_blocks) {
        return -1;
    }
    if (count > e->total_blocks - logical) {
        count = e->total_blocks - logical; /* preallocation past EOF: ignore it */
    }
    if (phys == 0u || phys >= e->v->blocks_count || count > e->v->blocks_count - phys) {
        return -1; /* the run leaves the volume */
    }
    e->next_logical = logical + count;

    start_lba = phys * e->v->spb;
    sectors = count * e->v->spb;
    if (e->rout != 0) {
        if (hype_file_rmap_append(e->rout, HYPE_RANGE_DATA, start_lba, sectors) != 0) {
            return -1; /* over the range cap: too_fragmented is already set */
        }
        return 0;
    }
    if (f->count > 0u && f->extents[f->count - 1u].start_lba +
                                 f->extents[f->count - 1u].sector_count == start_lba) {
        f->extents[f->count - 1u].sector_count += sectors;
        return 0;
    }
    if (f->count >= HYPE_FILE_MAX_EXTENTS) {
        /* #366: say WHY, like the FAT resolvers do. ext matters most for this: core/ext.h notes
         * large indirect-mapped files are STRUCTURALLY fragmented, so it is the filesystem most
         * likely to hit the cap -- and it was the one that never reported hitting it. */
        f->too_fragmented = 1;
        return -1;
    }
    f->extents[f->count].start_lba = start_lba;
    f->extents[f->count].sector_count = sectors;
    f->count++;
    return 0;
}

/* ---- ext4 extent trees ---- */

/* One in-flight tree level: an on-disk node being iterated. */
typedef struct {
    uint64_t block;   /* the node's block */
    uint32_t idx;     /* next entry to visit */
    uint32_t entries; /* eh_entries */
    uint32_t depth;   /* eh_depth: 0 == leaf entries */
} ext_frame_t;

/* Reads and validates an on-disk extent node's header. */
static int node_header(ext_vol_t *v, uint64_t block, uint32_t want_depth, ext_frame_t *f) {
    uint8_t hdr[EH_SIZE];
    uint16_t entries, max;
    if (block_bytes(v, block, 0u, EH_SIZE, hdr) != 0) {
        return -1;
    }
    if (hype_rd16(hdr + 0) != EH_MAGIC) {
        return -1;
    }
    entries = hype_rd16(hdr + EH_ENTRIES);
    max = hype_rd16(hdr + EH_MAX);
    if (entries > max || EH_SIZE + (uint32_t)max * EE_SIZE > v->block_size) {
        return -1;
    }
    if ((uint32_t)hype_rd16(hdr + EH_DEPTH) != want_depth) {
        return -1; /* every child must sit exactly one level below its parent */
    }
    f->block = block;
    f->idx = 0;
    f->entries = entries;
    f->depth = want_depth;
    return 0;
}

/* Emits one leaf extent entry. */
static int leaf_entry(emit_t *e, const uint8_t ee[EE_SIZE]) {
    uint32_t lblk = hype_rd32(ee + 0);
    uint32_t len = hype_rd16(ee + 4);
    uint64_t phys = (uint64_t)hype_rd32(ee + 8) | ((uint64_t)hype_rd16(ee + 6) << 32);
    if (len == 0u) {
        return -1;
    }
    if (len > EE_LEN_UNWRITTEN) {
        /* len > 32768 marks an UNWRITTEN extent (real length len - 32768): it
         * reads as zeros. Under the #384 contract that is an UNWRITTEN range;
         * under the legacy physical-only contract it stays a refusal. */
        uint64_t real_len = len - EE_LEN_UNWRITTEN;
        if (e->rout == 0) {
            return -1;
        }
        if (real_len == 0u || lblk < e->next_logical) {
            return -1;
        }
        if ((uint64_t)lblk != e->next_logical) {
            if ((uint64_t)lblk > e->total_blocks ||
                hype_file_rmap_append(e->rout, HYPE_RANGE_HOLE, 0,
                                      ((uint64_t)lblk - e->next_logical) * e->v->spb) != 0) {
                return -1;
            }
            e->next_logical = lblk;
        }
        if (lblk >= e->total_blocks) {
            return -1;
        }
        if (real_len > e->total_blocks - lblk) {
            real_len = e->total_blocks - lblk;
        }
        if (phys == 0u || phys >= e->v->blocks_count || real_len > e->v->blocks_count - phys) {
            return -1;
        }
        if (hype_file_rmap_append(e->rout, HYPE_RANGE_UNWRITTEN, phys * e->v->spb,
                                  real_len * e->v->spb) != 0) {
            return -1;
        }
        e->next_logical = lblk + real_len;
        return 0;
    }
    return emit_run(e, lblk, phys, len);
}

/*
 * Walks the extent tree rooted in the inode's 60-byte i_block area, emitting
 * leaf extents in file order. Iterative: the explicit frame stack bounds the
 * depth the way the kernel does, and keeps every stack frame under a page --
 * a self-referential tree fails instead of recursing forever.
 */
static int walk_extents(ext_vol_t *v, const uint8_t *root, uint32_t root_bytes, emit_t *e) {
    ext_frame_t st[EXT_MAX_DEPTH + 1u];
    uint32_t sp;
    uint16_t entries, max;
    uint32_t depth;

    if (hype_rd16(root + 0) != EH_MAGIC) {
        return -1;
    }
    entries = hype_rd16(root + EH_ENTRIES);
    max = hype_rd16(root + EH_MAX);
    depth = hype_rd16(root + EH_DEPTH);
    if (entries > max || EH_SIZE + (uint32_t)max * EE_SIZE > root_bytes || depth > EXT_MAX_DEPTH) {
        return -1;
    }
    st[0].block = 0u; /* 0 == the in-inode root, read from `root`, not disk */
    st[0].idx = 0;
    st[0].entries = entries;
    st[0].depth = depth;
    sp = 1;

    while (sp > 0u) {
        ext_frame_t *f = &st[sp - 1u];
        uint8_t ent[EE_SIZE];
        uint32_t i;
        if (f->idx >= f->entries) {
            sp--;
            continue;
        }
        if (f->block == 0u) {
            for (i = 0; i < EE_SIZE; i++) {
                ent[i] = root[EH_SIZE + f->idx * EE_SIZE + i];
            }
        } else if (block_bytes(v, f->block, EH_SIZE + f->idx * EE_SIZE, EE_SIZE, ent) != 0) {
            return -1;
        }
        f->idx++;
        if (f->depth == 0u) {
            if (leaf_entry(e, ent) != 0) {
                return -1;
            }
        } else {
            uint64_t child = (uint64_t)hype_rd32(ent + 4) | ((uint64_t)hype_rd16(ent + 8) << 32);
            if (sp > EXT_MAX_DEPTH) {
                return -1;
            }
            if (node_header(v, child, f->depth - 1u, &st[sp]) != 0) {
                return -1;
            }
            sp++;
        }
    }
    return 0;
}

/* ---- classic ext2/3 block maps ---- */

/* A one-sector pointer cache (128 block pointers), one per indirection level,
 * so a sequential walk re-reads each pointer sector once, not per block. Lives
 * on the walker's stack -- never file-global, which two concurrent resolves
 * would silently share (the classic multi-VM singleton leak). */
typedef struct {
    uint64_t sector; /* ~0 == empty */
    uint8_t data[SECSZ];
} pcache_t;

typedef struct {
    pcache_t l1, l2, l3;
} ind_scratch_t;

/* Reads 32-bit pointer `index` of the pointer block `block` through a cache. */
static int pread32(ext_vol_t *v, pcache_t *c, uint64_t block, uint32_t index, uint32_t *out) {
    uint64_t sector;
    if (block == 0u || block >= v->blocks_count) {
        return -1; /* a hole where an indirection block should be */
    }
    sector = block * v->spb + (index * 4u) / SECSZ;
    if (c->sector != sector) {
        if (v->read(v->ctx, sector, 1u, c->data) != 0) {
            return -1;
        }
        c->sector = sector;
    }
    *out = hype_rd32(c->data + (index * 4u) % SECSZ);
    return 0;
}

static int walk_indirect(ext_vol_t *v, const uint8_t *iblk, ind_scratch_t *s, emit_t *e) {
    uint32_t ppb = v->block_size / 4u; /* pointers per block */
    uint64_t lb;

    s->l1.sector = (uint64_t)-1;
    s->l2.sector = (uint64_t)-1;
    s->l3.sector = (uint64_t)-1;
    for (lb = 0; lb < e->total_blocks; lb++) {
        uint32_t ptr = 0;
        if (lb < 12u) {
            ptr = hype_rd32(iblk + lb * 4u);
        } else {
            uint64_t r = lb - 12u;
            if (r < ppb) { /* single indirect */
                uint32_t root = hype_rd32(iblk + 12u * 4u);
                if (root == 0u && e->rout != 0) {
                    ptr = 0u; /* the whole level is a hole */
                } else if (pread32(v, &s->l1, root, (uint32_t)r, &ptr) != 0) {
                    return -1;
                }
            } else if ((r -= ppb) < (uint64_t)ppb * ppb) { /* double */
                uint32_t mid;
                uint32_t root = hype_rd32(iblk + 13u * 4u);
                if (root == 0u && e->rout != 0) {
                    ptr = 0u;
                } else if (pread32(v, &s->l2, root, (uint32_t)(r / ppb), &mid) != 0) {
                    return -1;
                } else if (mid == 0u && e->rout != 0) {
                    ptr = 0u;
                } else if (pread32(v, &s->l1, mid, (uint32_t)(r % ppb), &ptr) != 0) {
                    return -1;
                }
            } else if ((r -= (uint64_t)ppb * ppb) < (uint64_t)ppb * ppb * ppb) { /* triple */
                uint32_t hi, mid;
                uint32_t root = hype_rd32(iblk + 14u * 4u);
                if (root == 0u && e->rout != 0) {
                    ptr = 0u;
                } else if (pread32(v, &s->l3, root,
                                   (uint32_t)(r / ((uint64_t)ppb * ppb)), &hi) != 0) {
                    return -1;
                } else if (hi == 0u && e->rout != 0) {
                    ptr = 0u;
                } else if (pread32(v, &s->l2, hi, (uint32_t)((r / ppb) % ppb), &mid) != 0) {
                    return -1;
                } else if (mid == 0u && e->rout != 0) {
                    ptr = 0u;
                } else if (pread32(v, &s->l1, mid, (uint32_t)(r % ppb), &ptr) != 0) {
                    return -1;
                }
            } else {
                return -1; /* beyond what a triple-indirect map can address */
            }
        }
        if (ptr == 0u) {
            if (e->rout == 0) {
                return -1; /* a hole: the legacy contract cannot say "zeros" */
            }
            /* skip it -- emit_run() emits the accumulated gap as one HOLE
             * when the next mapped block appears, and the post-walk pad
             * covers a file that ENDS in holes */
            continue;
        }
        if (emit_run(e, lb, ptr, 1u) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- file mapping ---- */

static uint64_t inode_file_size(const uint8_t ino[IN_CORE], int is_dir) {
    uint64_t size = hype_rd32(ino + IN_SIZE_LO);
    if (!is_dir) {
        /* i_size_high doubles as i_dir_acl on directories: files only. */
        size |= (uint64_t)hype_rd32(ino + IN_SIZE_HIGH) << 32;
    }
    return size;
}

/* Resolves an inode's data to extents in *out. Sets out->size_bytes. */
static int map_inode(ext_vol_t *v, const uint8_t ino[IN_CORE], int is_dir, hype_file_map_t *out) {
    uint32_t flags = hype_rd32(ino + IN_FLAGS);
    emit_t e;

    out->count = 0;
    out->size_bytes = inode_file_size(ino, is_dir);
    if (flags & FL_INLINE) {
        return -1; /* data inside the inode: not addressable as extents */
    }
    if (out->size_bytes == 0u) {
        return 0;
    }
    e.out = out;
    e.rout = 0; /* legacy physical-only contract: holes refuse */
    e.v = v;
    e.next_logical = 0;
    e.total_blocks = (out->size_bytes + v->block_size - 1u) / v->block_size;
    if (flags & FL_EXTENTS) {
        if (walk_extents(v, ino + IN_BLOCK, 60u, &e) != 0) {
            return -1;
        }
    } else {
        ind_scratch_t scratch;
        if (walk_indirect(v, ino + IN_BLOCK, &scratch, &e) != 0) {
            return -1;
        }
    }
    /* Every file block must have been mapped: a short map is a hole at EOF. */
    return (e.next_logical == e.total_blocks) ? 0 : -1;
}

/*
 * #384: as map_inode, but into the sparse-aware #381 contract: classic-map
 * and extent holes become HOLE ranges, unwritten extents become UNWRITTEN,
 * and a file that simply ENDS in a hole gets a trailing HOLE pad. The final
 * partial block's slack sectors are trimmed so the map covers exactly
 * ceil(size / 512) sectors.
 */
static int map_inode_rmap(ext_vol_t *v, const uint8_t ino[IN_CORE], hype_file_rmap_t *out) {
    uint32_t flags = hype_rd32(ino + IN_FLAGS);
    emit_t e;

    hype_file_rmap_init(out, inode_file_size(ino, 0));
    if (flags & FL_INLINE) {
        return -1; /* data inside the inode: not addressable on media */
    }
    if (out->size_bytes == 0u) {
        return 0;
    }
    e.out = 0;
    e.rout = out;
    e.v = v;
    e.next_logical = 0;
    e.total_blocks = (out->size_bytes + v->block_size - 1u) / v->block_size;
    if (flags & FL_EXTENTS) {
        if (walk_extents(v, ino + IN_BLOCK, 60u, &e) != 0) {
            return -1;
        }
    } else {
        ind_scratch_t scratch;
        if (walk_indirect(v, ino + IN_BLOCK, &scratch, &e) != 0) {
            return -1;
        }
    }
    /* a file ending in a hole: pad the tail */
    if (e.next_logical < e.total_blocks) {
        if (hype_file_rmap_append(out, HYPE_RANGE_HOLE, 0,
                                  (e.total_blocks - e.next_logical) * v->spb) != 0) {
            return -1;
        }
    }
    /* trim block-tail slack down to exactly ceil(size/512) sectors */
    {
        uint64_t need = (out->size_bytes + SECSZ - 1u) / SECSZ;
        uint64_t covered = 0;
        unsigned r;
        for (r = 0; r < out->count; r++) {
            if (covered >= need) {
                out->count = r;
                break;
            }
            if (covered + out->ranges[r].sector_count > need) {
                out->ranges[r].sector_count = need - covered;
            }
            covered += out->ranges[r].sector_count;
        }
    }
    return hype_file_rmap_validate(out, v->blocks_count * v->spb);
}

/* ---- directories ---- */

/*
 * Finds `name` in the directory described by `dino`, filling *out_ino.
 * A plain linear scan: htree-indexed directories deliberately disguise their
 * index blocks as empty (inode 0) entries so exactly this walk still works.
 * Returns 1 found, 0 not found, -1 on a broken directory.
 */
static int dir_search(ext_vol_t *v, const uint8_t dino[IN_CORE], const char *name,
                      unsigned int nlen, uint32_t *out_ino) {
    /* #366: static for the same reason as boot/main.c's resolve buffers -- at
     * HYPE_FILE_MAX_EXTENTS = 256 this struct is over 4 KiB, and a frame that large needs a
     * __chkstk probe the freestanding build has no definition for. dir_search is called in a
     * LOOP by hype_ext_resolve, never nested, and resolution is setup-time and single-threaded,
     * so there is no live second copy to collide with. map_inode() below fully rewrites it. */
    static hype_file_map_t map;
    unsigned int x;

    if (map_inode(v, dino, 1, &map) != 0) {
        return -1;
    }
    uint32_t scanned = 0;
    for (x = 0; x < map.count; x++) {
        uint64_t s;
        for (s = 0; s < map.extents[x].sector_count; s += v->spb) {
            uint64_t block = (map.extents[x].start_lba + s) / v->spb;
            uint32_t off = 0;
            /* #346: sector_count comes from DISK. A corrupt or absurd directory extent must not
             * turn one lookup into millions of individually-valid block reads -- on the real-HW
             * media scan that presents as a boot wedged for hours with every read returning
             * rc=0. 64K blocks (256MB at 4K) is far beyond any sane directory. */
            if (++scanned > (1u << 16)) {
                return -1;
            }
            while (off + 8u <= v->block_size) {
                uint8_t hdr[8];
                uint32_t ino, rec, nl;
                if (block_bytes(v, block, off, 8u, hdr) != 0) {
                    return -1;
                }
                ino = hype_rd32(hdr + 0);
                rec = hype_rd16(hdr + 4);
                nl = hdr[6];
                if (rec < 8u || (rec & 3u) != 0u || off + rec > v->block_size) {
                    return -1; /* corrupt record: stop rather than misparse */
                }
                if (ino != 0u && nl == nlen && off + 8u + nl <= v->block_size) {
                    uint8_t nbuf[EXT_MAX_NAME];
                    unsigned int i;
                    if (block_bytes(v, block, off + 8u, nl, nbuf) != 0) {
                        return -1;
                    }
                    for (i = 0; i < nlen; i++) {
                        if (nbuf[i] != (uint8_t)name[i]) {
                            break;
                        }
                    }
                    if (i == nlen) {
                        *out_ino = ino;
                        return 1;
                    }
                }
                off += rec;
            }
        }
    }
    return 0;
}

/* ---- path resolution ---- */

/*
 * Walks `path` to its final inode, requiring MODE_DIR (want_dir) or MODE_REG
 * (!want_dir) there. Fills ino[] and, when out_ino is non-NULL, the inode
 * number. want_dir also accepts "" or an all-separator path as the ROOT
 * itself (inode 2) -- #498's namespace ops need to resolve a bare parent
 * directory ("/" for a top-level create/mkdir), which no earlier caller did.
 * Shared by every resolver flavour.
 */
static int resolve_inode_ex(ext_vol_t *v, const char *path, uint8_t ino[IN_CORE],
                            uint32_t *out_ino, int want_dir) {
    unsigned int pos = 0;

    if (inode_read(v, 2u, ino) != 0) { /* the root directory is always inode 2 */
        return -1;
    }
    if ((hype_rd16(ino + IN_MODE) & MODE_FMT) != MODE_DIR) {
        return -1;
    }
    if (want_dir) {
        unsigned int p2 = 0;
        while (path[p2] == '/' || path[p2] == '\\') {
            p2++;
        }
        if (path[p2] == '\0') {
            if (out_ino != 0) {
                *out_ino = 2u;
            }
            return 0;
        }
    }
    for (;;) {
        char comp[EXT_MAX_NAME + 1u];
        unsigned int n = 0;
        uint32_t next = 0;
        int last;

        while (path[pos] == '/' || path[pos] == '\\') {
            pos++;
        }
        if (path[pos] == '\0') {
            return -1; /* no final component: the path names no file */
        }
        while (path[pos] != '\0' && path[pos] != '/' && path[pos] != '\\') {
            if (n >= EXT_MAX_NAME) {
                return -1;
            }
            comp[n++] = path[pos++];
        }
        {
            unsigned int peek = pos;
            while (path[peek] == '/' || path[peek] == '\\') {
                peek++;
            }
            last = (path[peek] == '\0') ? 1 : 0;
        }
        if (dir_search(v, ino, comp, n, &next) != 1) {
            return -1;
        }
        if (inode_read(v, next, ino) != 0) {
            return -1;
        }
        if (last) {
            if ((hype_rd16(ino + IN_MODE) & MODE_FMT) != (want_dir ? MODE_DIR : MODE_REG)) {
                /* !want_dir: directories and symlinks are not stream targets.
                 * want_dir: #498 callers need a real directory, not a file. */
                return -1;
            }
            if (out_ino != 0) {
                *out_ino = next;
            }
            return 0;
        }
        if ((hype_rd16(ino + IN_MODE) & MODE_FMT) != MODE_DIR) {
            return -1; /* a non-final component must be a directory */
        }
    }
}

/* Back-compat name for the (far more common) regular-file resolve. */
static int resolve_inode(ext_vol_t *v, const char *path, uint8_t ino[IN_CORE],
                         uint32_t *out_ino) {
    return resolve_inode_ex(v, path, ino, out_ino, 0);
}

int hype_ext_resolve(hype_blk_read_fn read, void *ctx, const char *path, hype_file_map_t *out) {
    /* #366: cleared at ENTRY, before any early return, so a failure that never reaches the extent
     * walk cannot inherit the previous call's reason. Same rule as hype_fat32_resolve. */
    if (out != 0) {
        out->too_fragmented = 0;
    }
    ext_vol_t v;
    uint8_t ino[IN_CORE];

    if (vol_open(read, ctx, &v) != 0) {
        return -1;
    }
    if (resolve_inode(&v, path, ino, 0) != 0) {
        return -1;
    }
    return map_inode(&v, ino, 0, out);
}

int hype_ext_resolve_rmap(hype_blk_read_fn read, void *ctx, const char *path,
                          hype_file_rmap_t *out) {
    ext_vol_t v;
    uint8_t ino[IN_CORE];

    if (out != 0) {
        out->too_fragmented = 0;
    }
    if (vol_open(read, ctx, &v) != 0) {
        return -1;
    }
    if (resolve_inode(&v, path, ino, 0) != 0) {
        return -1;
    }
    return map_inode_rmap(&v, ino, out);
}

/* #384: the inode NUMBER behind a path, for the ext2 writer's metadata
 * updates. Same walk, same refusals. */
int hype_ext_resolve_ino(hype_blk_read_fn read, void *ctx, const char *path, uint32_t *out_ino) {
    ext_vol_t v;
    uint8_t ino[IN_CORE];
    if (vol_open(read, ctx, &v) != 0) {
        return -1;
    }
    return resolve_inode(&v, path, ino, out_ino);
}

/* #498: as hype_ext_resolve_ino, but resolves a DIRECTORY -- the namespace
 * writers need their target's PARENT directory's inode number, and "" / "/"
 * must resolve to the root (inode 2) itself, which no earlier caller needed. */
int hype_ext_resolve_dir_ino(hype_blk_read_fn read, void *ctx, const char *path,
                             uint32_t *out_ino) {
    ext_vol_t v;
    uint8_t ino[IN_CORE];
    if (vol_open(read, ctx, &v) != 0) {
        return -1;
    }
    return resolve_inode_ex(&v, path, ino, out_ino, 1);
}

/* #498: as hype_ext_map_ino_rmap, but for a DIRECTORY inode -- the namespace
 * writers enumerate a directory's own data blocks (to scan/insert/remove
 * entries) the same way #384's writer re-derives a FILE's map, but
 * hype_ext_map_ino_rmap deliberately refuses anything that is not
 * MODE_REG (a file writer must never be pointed at a directory's blocks by
 * mistake), so a sibling with the opposite requirement is needed rather than
 * loosening that guarantee. */
int hype_ext_map_dir_ino_rmap(hype_blk_read_fn read, void *ctx, uint32_t ino_no,
                              hype_file_rmap_t *out) {
    ext_vol_t v;
    uint8_t ino[IN_CORE];
    if (vol_open(read, ctx, &v) != 0) {
        return -1;
    }
    if (inode_read(&v, ino_no, ino) != 0) {
        return -1;
    }
    if ((hype_rd16(ino + IN_MODE) & MODE_FMT) != MODE_DIR) {
        return -1;
    }
    return map_inode_rmap(&v, ino, out);
}

/* #384: re-derive an inode's sparse map straight from its (just-committed)
 * on-disk state, so the allocating writer can refresh its handle without a
 * path re-walk. */
int hype_ext_map_ino_rmap(hype_blk_read_fn read, void *ctx, uint32_t ino_no,
                          hype_file_rmap_t *out) {
    ext_vol_t v;
    uint8_t ino[IN_CORE];
    if (vol_open(read, ctx, &v) != 0) {
        return -1;
    }
    if (inode_read(&v, ino_no, ino) != 0) {
        return -1;
    }
    if ((hype_rd16(ino + IN_MODE) & MODE_FMT) != MODE_REG) {
        return -1;
    }
    return map_inode_rmap(&v, ino, out);
}

/*
 * #293: recognition without resolution, for the common-interface probe. The
 * same superblock validation vol_open applies per resolve -- magic, supported
 * feature set, clean (non-RECOVER) journal -- against a throwaway volume
 * descriptor. Read-only.
 */
int hype_ext_probe(hype_blk_read_fn read, void *ctx) {
    ext_vol_t v;
    return vol_open(read, ctx, &v);
}
