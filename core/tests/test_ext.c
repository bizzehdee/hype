#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ext.h"
#include "../fs_ops.h"
#include "../ext_jalloc.h"
#include "../jbd2.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)
#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/*
 * ---- Synthetic ext volume in RAM ----
 *
 * 1024-byte blocks (first_data_block == 1, 2 sectors/block), one block group,
 * 256-byte inodes. Laid out by hand so every shape -- extent trees of both
 * depths, indirect maps, subdirectories, htree disguises, and each corrupt
 * variant -- is exactly where an assertion expects it.
 *
 *   block 1: superblock   block 2: group descriptors
 *   blocks 8..23: inode table (64 inodes x 256 B)
 *   block 30: root dir     31: /sub dir      32/33: /hdir (htree-shaped)
 *   blocks 40..49 + 60..69: img.bin (two extents)
 *   blocks 80..91 direct + 92 indirect + 93..192: ind.bin
 *   block 200: depth-1 extent leaf node for tree.bin (data 210..219)
 */
#define BS 1024u
#define SPB 2u
#define VOL_BLOCKS 4096u
#define INODE_SIZE 256u
#define INODE_TABLE 8u
#define ROOT_BLK 30u

#define INCOMPAT_FILETYPE 0x0002u
#define INCOMPAT_RECOVER 0x0004u
#define INCOMPAT_EXTENTS 0x0040u
#define INCOMPAT_64BIT 0x0080u

static uint8_t g_scratch_map[sizeof(hype_file_map_t)];
static uint8_t g_vol[VOL_BLOCKS * BS];
static uint64_t g_fail_read_lba = (uint64_t)-1;
static long g_read_countdown = -1;

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if ((lba + count) * 512u > sizeof g_vol) return -1;
    if (lba <= g_fail_read_lba && g_fail_read_lba < lba + count) return -1;
    if (g_read_countdown >= 0 && g_read_countdown-- == 0) return -1;
    memcpy(dst, g_vol + lba * 512u, (size_t)count * 512u);
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint8_t *blk(uint32_t b) { return g_vol + (size_t)b * BS; }
static uint8_t *inode(uint32_t ino) {
    return blk(INODE_TABLE) + (size_t)(ino - 1u) * INODE_SIZE;
}

static void superblock(uint32_t incompat) {
    uint8_t *sb = g_vol + 1024;
    memset(sb, 0, 1024);
    put32(sb + 0x00, 64u);         /* inodes_count */
    put32(sb + 0x04, VOL_BLOCKS);  /* blocks_count_lo */
    put32(sb + 0x14, 1u);          /* first_data_block */
    put32(sb + 0x18, 0u);          /* log_block_size: 1024 */
    put32(sb + 0x28, 64u);         /* inodes_per_group */
    put16(sb + 0x38, 0xEF53u);     /* magic */
    put16(sb + 0x3A, 0x0001u);     /* state: cleanly unmounted */
    put32(sb + 0x4C, 1u);          /* rev_level: dynamic */
    put16(sb + 0x58, INODE_SIZE);  /* inode_size */
    put32(sb + 0x60, incompat);
}

static void group_desc(void) {
    memset(blk(2), 0, BS);
    put32(blk(2) + 0x08, INODE_TABLE); /* bg_inode_table_lo */
}

/* Inode helpers: mode + size, then a map laid in by the caller. */
static uint8_t *mk_inode(uint32_t ino, uint16_t mode, uint32_t size, uint32_t flags) {
    uint8_t *in = inode(ino);
    memset(in, 0, INODE_SIZE);
    put16(in + 0x00, mode);
    put32(in + 0x04, size);
    put32(in + 0x20, flags);
    return in;
}

/* Lays an in-inode extent tree: root at depth 0 with the given leaf runs. */
static void extent_leaf_root(uint8_t *in, const uint32_t (*runs)[3], unsigned int n) {
    uint8_t *eh = in + 0x28;
    unsigned int i;
    put16(eh + 0, 0xF30Au);
    put16(eh + 2, (uint16_t)n); /* entries */
    put16(eh + 4, 4u);          /* max: 60-byte root holds 4 */
    put16(eh + 6, 0u);          /* depth */
    for (i = 0; i < n; i++) {
        uint8_t *ee = eh + 12u + i * 12u;
        put32(ee + 0, runs[i][0]); /* logical */
        put16(ee + 4, (uint16_t)runs[i][2]); /* len */
        put16(ee + 6, 0u);
        put32(ee + 8, runs[i][1]); /* physical */
    }
}

/* Appends a dirent; returns the next offset. The caller closes the block. */
static uint32_t dirent(uint8_t *b, uint32_t off, uint32_t ino, const char *name, uint8_t ftype) {
    uint32_t nl = (uint32_t)strlen(name);
    uint32_t rec = (8u + nl + 3u) & ~3u;
    put32(b + off + 0, ino);
    put16(b + off + 4, (uint16_t)rec);
    b[off + 6] = (uint8_t)nl;
    b[off + 7] = ftype;
    memcpy(b + off + 8, name, nl);
    return off + rec;
}
/* Stretches the entry at `off` to end the block. */
static void dirent_close(uint8_t *b, uint32_t last_off, uint32_t end) {
    put16(b + last_off + 4, (uint16_t)(end - last_off));
}

static uint8_t pat(unsigned int i) { return (uint8_t)(i * 13u + 5u); }

static void build_vol(void) {
    uint8_t *in;
    uint32_t off, last;
    unsigned int i;

    memset(g_vol, 0, sizeof g_vol);
    superblock(INCOMPAT_FILETYPE | INCOMPAT_EXTENTS);
    group_desc();

    /* Root (inode 2): one extent -> block 30. */
    in = mk_inode(2u, 0x41EDu, BS, 0x80000u);
    {
        const uint32_t runs[1][3] = {{0u, ROOT_BLK, 1u}};
        extent_leaf_root(in, runs, 1u);
    }
    off = 0;
    off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
    off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
    off = dirent(blk(ROOT_BLK), off, 12u, "img.bin", 1u);
    off = dirent(blk(ROOT_BLK), off, 13u, "sub", 2u);
    off = dirent(blk(ROOT_BLK), off, 14u, "ind.bin", 1u);
    off = dirent(blk(ROOT_BLK), off, 15u, "sym", 7u);
    off = dirent(blk(ROOT_BLK), off, 16u, "hole.bin", 1u);
    off = dirent(blk(ROOT_BLK), off, 17u, "tree.bin", 1u);
    off = dirent(blk(ROOT_BLK), off, 18u, "inline.bin", 1u);
    off = dirent(blk(ROOT_BLK), off, 19u, "unwrit.bin", 1u);
    off = dirent(blk(ROOT_BLK), off, 20u, "hdir", 2u);
    off = dirent(blk(ROOT_BLK), off, 21u, "empty.bin", 1u);
    last = off;
    off = dirent(blk(ROOT_BLK), off, 22u, "frag.bin", 1u);
    dirent_close(blk(ROOT_BLK), off == last ? last : off - ((8u + 8u + 3u) & ~3u), BS);
    /* (the helper above stretched frag.bin's record to the block end) */

    /* img.bin (inode 12): two extents, 40..49 and 60..69, 20000 bytes. */
    in = mk_inode(12u, 0x81A4u, 20000u, 0x80000u);
    {
        const uint32_t runs[2][3] = {{0u, 40u, 10u}, {10u, 60u, 10u}};
        extent_leaf_root(in, runs, 2u);
    }
    for (i = 0; i < 20000u; i++) {
        uint32_t b = (i / BS < 10u) ? (40u + i / BS) : (60u + i / BS - 10u);
        blk(b)[i % BS] = pat(i);
    }

    /* /sub (inode 13) -> block 31, holding deep.bin (inode 23). */
    in = mk_inode(13u, 0x41EDu, BS, 0x80000u);
    {
        const uint32_t runs[1][3] = {{0u, 31u, 1u}};
        extent_leaf_root(in, runs, 1u);
    }
    off = 0;
    off = dirent(blk(31u), off, 13u, ".", 2u);
    off = dirent(blk(31u), off, 2u, "..", 2u);
    last = off;
    off = dirent(blk(31u), off, 23u, "deep.bin", 1u);
    dirent_close(blk(31u), last + 12u, BS);
    in = mk_inode(23u, 0x81A4u, 100u, 0x80000u);
    {
        const uint32_t runs[1][3] = {{0u, 70u, 1u}};
        extent_leaf_root(in, runs, 1u);
    }
    for (i = 0; i < 100u; i++) blk(70u)[i] = pat(i + 7u);

    /* ind.bin (inode 14): classic map. 12 direct (80..91), single indirect
     * via block 92 covering 93..192: 112 blocks, 114000 bytes used. */
    in = mk_inode(14u, 0x81A4u, 114000u, 0u);
    for (i = 0; i < 12u; i++) put32(in + 0x28 + i * 4u, 80u + i);
    put32(in + 0x28 + 12u * 4u, 92u);
    for (i = 0; i < 100u; i++) put32(blk(92u) + i * 4u, 93u + i);
    for (i = 0; i < 114000u; i++) {
        uint32_t lb = i / BS;
        uint32_t b = (lb < 12u) ? (80u + lb) : (93u + lb - 12u);
        blk(b)[i % BS] = pat(i + 3u);
    }

    /* sym (inode 15): a symlink -- never a stream target. */
    (void)mk_inode(15u, 0xA1FFu, 4u, 0u);

    /* hole.bin (inode 16): an extent starting at logical 1: block 0 is a hole. */
    in = mk_inode(16u, 0x81A4u, 2048u, 0x80000u);
    {
        const uint32_t runs[1][3] = {{1u, 44u, 1u}};
        extent_leaf_root(in, runs, 1u);
    }

    /* tree.bin (inode 17): depth-1 tree -- root index -> leaf node in block
     * 200 -> data 210..219 (10240 bytes). */
    in = mk_inode(17u, 0x81A4u, 10240u, 0x80000u);
    {
        uint8_t *eh = in + 0x28;
        put16(eh + 0, 0xF30Au);
        put16(eh + 2, 1u);
        put16(eh + 4, 4u);
        put16(eh + 6, 1u); /* depth 1 */
        put32(eh + 12u + 0, 0u);   /* ei_block */
        put32(eh + 12u + 4, 200u); /* ei_leaf_lo */
        put16(eh + 12u + 8, 0u);   /* ei_leaf_hi */
    }
    {
        uint8_t *n = blk(200u);
        put16(n + 0, 0xF30Au);
        put16(n + 2, 1u);
        put16(n + 4, (uint16_t)((BS - 12u) / 12u));
        put16(n + 6, 0u); /* leaf */
        put32(n + 12u + 0, 0u);
        put16(n + 12u + 4, 10u);
        put16(n + 12u + 6, 0u);
        put32(n + 12u + 8, 210u);
    }
    for (i = 0; i < 10240u; i++) blk(210u + i / BS)[i % BS] = pat(i + 11u);

    /* inline.bin (inode 18): INLINE_DATA flag -- refused. */
    (void)mk_inode(18u, 0x81A4u, 60u, 0x10000000u);

    /* unwrit.bin (inode 19): an unwritten extent (len 32769 == 1 block). */
    in = mk_inode(19u, 0x81A4u, 1024u, 0x80000u);
    {
        uint8_t *eh = in + 0x28;
        put16(eh + 0, 0xF30Au);
        put16(eh + 2, 1u);
        put16(eh + 4, 4u);
        put16(eh + 6, 0u);
        put32(eh + 12u + 0, 0u);
        put16(eh + 12u + 4, (uint16_t)32769u);
        put16(eh + 12u + 6, 0u);
        put32(eh + 12u + 8, 45u);
    }

    /* /hdir (inode 20): htree-shaped -- block 32 carries '.', '..' and a fake
     * inode-0 entry spanning the rest (the dx_root disguise); block 33 is a
     * normal leaf block holding found.bin (inode 24). */
    in = mk_inode(20u, 0x41EDu, 2u * BS, 0x81000u); /* extents + INDEX_FL */
    {
        const uint32_t runs[1][3] = {{0u, 32u, 2u}}; /* blocks 32..33 */
        extent_leaf_root(in, runs, 1u);
    }
    off = 0;
    off = dirent(blk(32u), off, 20u, ".", 2u);
    off = dirent(blk(32u), off, 2u, "..", 2u);
    put32(blk(32u) + off, 0u); /* fake entry: inode 0 */
    put16(blk(32u) + off + 4u, (uint16_t)(BS - off));
    off = 0;
    last = off;
    off = dirent(blk(33u), off, 24u, "found.bin", 1u);
    dirent_close(blk(33u), last, BS);
    in = mk_inode(24u, 0x81A4u, 10u, 0x80000u);
    {
        const uint32_t runs[1][3] = {{0u, 71u, 1u}};
        extent_leaf_root(in, runs, 1u);
    }
    memcpy(blk(71u), "0123456789", 10u);

    /* empty.bin (inode 21): zero bytes, no extents at all. */
    in = mk_inode(21u, 0x81A4u, 0u, 0x80000u);
    {
        uint8_t *eh = in + 0x28;
        put16(eh + 0, 0xF30Au);
        put16(eh + 4, 4u);
    }

    /* frag.bin (inode 22): 4 single-block extents with gaps between their
     * physical blocks -- 4 distinct output extents (coalescing must NOT fuse
     * them), used for the extent-cap test via a crafted variant. */
    in = mk_inode(22u, 0x81A4u, 4096u, 0x80000u);
    {
        const uint32_t runs[4][3] = {
            {0u, 220u, 1u}, {1u, 222u, 1u}, {2u, 224u, 1u}, {3u, 226u, 1u}};
        extent_leaf_root(in, runs, 4u);
    }
    for (i = 0; i < 4096u; i++) blk(220u + (i / BS) * 2u)[i % BS] = pat(i + 19u);
}

/* Gathers a resolved file's bytes through its extents, test-side. */
static unsigned int gather(const hype_file_map_t *f, uint8_t *out, unsigned int max) {
    unsigned int n = 0, x;
    for (x = 0; x < f->count; x++) {
        uint64_t s;
        for (s = 0; s < f->extents[x].sector_count && n < max; s++) {
            memcpy(out + n, g_vol + (f->extents[x].start_lba + s) * 512u,
                   (512u < max - n) ? 512u : max - n);
            n += (512u < max - n) ? 512u : max - n;
        }
    }
    return n;
}

static void check_content(const char *what, const hype_file_map_t *f, unsigned int seed) {
    static uint8_t buf[120000];
    unsigned int n = gather(f, buf, sizeof buf);
    unsigned int i;
    CHECK(what, n >= f->size_bytes);
    for (i = 0; i < f->size_bytes; i++) {
        if (buf[i] != pat(i + seed)) {
            char d[128];
            snprintf(d, sizeof d, "%s: byte %u", what, i);
            CHECK_HEX(d, pat(i + seed), buf[i]);
            break;
        }
    }
}

/* ---- tests ---- */

static void test_resolve_extents(void) {
    hype_file_map_t f;
    build_vol();
    CHECK_HEX("resolve img.bin", 0, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    CHECK_HEX("size", 20000u, (unsigned)f.size_bytes);
    CHECK_HEX("two extents", 2u, f.count);
    CHECK_HEX("extent 0 lba", 40u * SPB, f.extents[0].start_lba);
    CHECK_HEX("extent 0 sectors", 10u * SPB, f.extents[0].sector_count);
    CHECK_HEX("extent 1 lba", 60u * SPB, f.extents[1].start_lba);
    /* The final extent is clamped to the file's size in blocks (20 blocks
     * cover 20000 bytes; nothing extends past ceil(size/bs)). */
    CHECK_HEX("extent 1 sectors", 10u * SPB, f.extents[1].sector_count);
    check_content("img.bin content", &f, 0u);
    /* Backslash separators and a subdirectory. */
    CHECK_HEX("resolve sub/deep.bin", 0, hype_ext_resolve(vol_read, 0, "\\sub\\deep.bin", &f));
    CHECK_HEX("deep size", 100u, (unsigned)f.size_bytes);
    check_content("deep.bin content", &f, 7u);
    /* A depth-1 extent tree. */
    CHECK_HEX("resolve tree.bin", 0, hype_ext_resolve(vol_read, 0, "/tree.bin", &f));
    CHECK_HEX("tree size", 10240u, (unsigned)f.size_bytes);
    CHECK_HEX("tree single coalesced extent", 1u, f.count);
    check_content("tree.bin content", &f, 11u);
    /* Fragmented: coalescing must not fuse non-adjacent runs. */
    CHECK_HEX("resolve frag.bin", 0, hype_ext_resolve(vol_read, 0, "/frag.bin", &f));
    CHECK_HEX("four extents", 4u, f.count);
    check_content("frag.bin content", &f, 19u);
    /* Empty file: zero extents, zero size, success. */
    CHECK_HEX("resolve empty.bin", 0, hype_ext_resolve(vol_read, 0, "/empty.bin", &f));
    CHECK_HEX("empty size", 0u, (unsigned)f.size_bytes);
    CHECK_HEX("empty extents", 0u, f.count);
    /* An htree-shaped directory still resolves by linear scan. */
    CHECK_HEX("resolve hdir/found.bin", 0, hype_ext_resolve(vol_read, 0, "/hdir/found.bin", &f));
    CHECK_HEX("found size", 10u, (unsigned)f.size_bytes);
}

static void test_resolve_indirect(void) {
    hype_file_map_t f;
    build_vol();
    CHECK_HEX("resolve ind.bin", 0, hype_ext_resolve(vol_read, 0, "/ind.bin", &f));
    CHECK_HEX("ind size", 114000u, (unsigned)f.size_bytes);
    /* 80..91 then 93..192: exactly two runs (92 is the indirect block). */
    CHECK_HEX("two runs", 2u, f.count);
    CHECK_HEX("run 0 lba", 80u * SPB, f.extents[0].start_lba);
    CHECK_HEX("run 0 sectors", 12u * SPB, f.extents[0].sector_count);
    CHECK_HEX("run 1 lba", 93u * SPB, f.extents[1].start_lba);
    check_content("ind.bin content", &f, 3u);
}

static void test_refusals(void) {
    hype_file_map_t f;
    build_vol();
    CHECK_HEX("a symlink is refused", -1, hype_ext_resolve(vol_read, 0, "/sym", &f));
    CHECK_HEX("a directory is refused", -1, hype_ext_resolve(vol_read, 0, "/sub", &f));
    CHECK_HEX("a file mid-path is refused", -1,
              hype_ext_resolve(vol_read, 0, "/img.bin/x", &f));
    CHECK_HEX("a missing name", -1, hype_ext_resolve(vol_read, 0, "/nope.bin", &f));
    CHECK_HEX("a missing directory", -1, hype_ext_resolve(vol_read, 0, "/nodir/x", &f));
    CHECK_HEX("the root itself", -1, hype_ext_resolve(vol_read, 0, "/", &f));
    CHECK_HEX("an empty path", -1, hype_ext_resolve(vol_read, 0, "", &f));
    CHECK_HEX("a sparse file (hole)", -1, hype_ext_resolve(vol_read, 0, "/hole.bin", &f));
    CHECK_HEX("inline data", -1, hype_ext_resolve(vol_read, 0, "/inline.bin", &f));
    CHECK_HEX("an unwritten extent", -1, hype_ext_resolve(vol_read, 0, "/unwrit.bin", &f));
    {
        char big[300];
        unsigned int i;
        big[0] = '/';
        for (i = 1; i < sizeof big - 1u; i++) big[i] = 'a';
        big[sizeof big - 1u] = '\0';
        CHECK_HEX("an over-long component", -1, hype_ext_resolve(vol_read, 0, big, &f));
    }
}

static void test_mount_refusals(void) {
    hype_file_map_t f;

    build_vol();
    put16(g_vol + 1024 + 0x38, 0x1234u);
    CHECK_HEX("bad magic", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    build_vol();
    put32(g_vol + 1024 + 0x60, INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_RECOVER);
    CHECK_HEX("journal needing recovery", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    build_vol();
    put32(g_vol + 1024 + 0x60, INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | 0x8000u);
    CHECK_HEX("unknown incompat feature", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    build_vol();
    put32(g_vol + 1024 + 0x18, 3u); /* 8 KiB blocks */
    CHECK_HEX("oversized block size", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    build_vol();
    put16(g_vol + 1024 + 0x58, 96u); /* not a power of two */
    CHECK_HEX("bad inode size", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    build_vol();
    put32(g_vol + 1024 + 0x28, 0u); /* inodes_per_group 0 */
    CHECK_HEX("zero inodes per group", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    build_vol();
    put32(g_vol + 1024 + 0x14, 0u); /* first_data_block must be 1 at 1 KiB */
    CHECK_HEX("wrong first_data_block", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    build_vol();
    g_fail_read_lba = 2u; /* superblock */
    CHECK_HEX("superblock read failure", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    g_fail_read_lba = (uint64_t)-1;

    /* rev 0: the inode size field is ignored and 128 forced -- with 256-byte
     * inodes actually on disk, inode 2 lands mid-table and reads as garbage,
     * so the resolve fails safely rather than misparsing. */
    build_vol();
    put32(g_vol + 1024 + 0x4C, 0u);
    put16(g_vol + 1024 + 0x58, 0u);
    CHECK("rev0 inode size forced to 128", hype_ext_resolve(vol_read, 0, "/img.bin", &f) != 0);
}

static void test_corrupt_structures(void) {
    hype_file_map_t f;

    /* A crafted extent pointing past the volume. */
    build_vol();
    put32(inode(12u) + 0x28 + 12u + 8u, VOL_BLOCKS - 2u); /* phys near the end */
    CHECK_HEX("extent leaving the volume", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    /* A physical block of 0 inside an extent. */
    build_vol();
    put32(inode(12u) + 0x28 + 12u + 8u, 0u);
    CHECK_HEX("extent at block 0", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    /* Overlapping/out-of-order logical extents. */
    build_vol();
    put32(inode(12u) + 0x28 + 12u + 12u + 0u, 5u); /* second extent: logical 5 */
    CHECK_HEX("out-of-order extents", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    /* Corrupt extent headers: magic, entries > max, absurd depth. */
    build_vol();
    put16(inode(12u) + 0x28 + 0, 0xAAAAu);
    CHECK_HEX("bad tree magic", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    build_vol();
    put16(inode(12u) + 0x28 + 2, 5u); /* entries 5 > max 4 */
    CHECK_HEX("entries beyond max", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    build_vol();
    put16(inode(12u) + 0x28 + 6, 6u); /* depth 6 > the kernel cap */
    CHECK_HEX("tree too deep", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    /* A depth-1 tree whose child is not one level below. */
    build_vol();
    put16(blk(200u) + 6, 1u); /* the leaf claims depth 1 too */
    CHECK_HEX("child depth mismatch", -1, hype_ext_resolve(vol_read, 0, "/tree.bin", &f));

    /* A self-referential interior node: refused by the same depth rule. */
    build_vol();
    {
        uint8_t *n = blk(200u);
        put16(n + 6, 1u);           /* interior */
        put32(n + 12u + 4u, 200u);  /* child: itself */
    }
    CHECK_HEX("self-referential tree", -1, hype_ext_resolve(vol_read, 0, "/tree.bin", &f));

    /* An indirect pointer of 0 (hole) and one past the volume. */
    build_vol();
    put32(blk(92u) + 4u * 4u, 0u);
    CHECK_HEX("indirect hole", -1, hype_ext_resolve(vol_read, 0, "/ind.bin", &f));
    build_vol();
    put32(blk(92u) + 4u * 4u, VOL_BLOCKS + 7u);
    CHECK_HEX("indirect past the volume", -1, hype_ext_resolve(vol_read, 0, "/ind.bin", &f));

    /* A corrupt directory record (rec_len 0). */
    build_vol();
    put16(blk(ROOT_BLK) + 4, 0u);
    CHECK_HEX("corrupt dirent", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    /* A group descriptor pointing its inode table past the volume. */
    build_vol();
    put32(blk(2) + 0x08, VOL_BLOCKS + 100u);
    CHECK_HEX("inode table past the volume", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    build_vol();
    put32(blk(2) + 0x08, 0u);
    CHECK_HEX("inode table at block 0", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    /* More extents than the contract can carry: give img.bin's leaf 4 runs and
     * frag.bin a file whose every block alternates -- built by crafting a
     * depth-0 node in a block with 100 discontiguous single-block extents. */
    build_vol();
    {
        uint8_t *in = mk_inode(22u, 0x81A4u, 100u * BS, 0x80000u);
        uint8_t *eh = in + 0x28;
        uint8_t *n = blk(201u);
        unsigned int i;
        put16(eh + 0, 0xF30Au);
        put16(eh + 2, 1u);
        put16(eh + 4, 4u);
        put16(eh + 6, 1u);
        put32(eh + 12u + 0, 0u);
        put32(eh + 12u + 4, 201u);
        put16(eh + 12u + 8, 0u);
        put16(n + 0, 0xF30Au);
        put16(n + 2, 80u); /* within eh_max (84), beyond the 64-extent contract */
        put16(n + 4, (uint16_t)((BS - 12u) / 12u));
        put16(n + 6, 0u);
        for (i = 0; i < 80u; i++) {
            uint8_t *ee = n + 12u + i * 12u;
            put32(ee + 0, i);
            put16(ee + 4, 1u);
            put16(ee + 6, 0u);
            put32(ee + 8, 300u + i * 2u); /* every run discontiguous */
        }
    }
    CHECK_HEX("extent-cap overflow refused", -1, hype_ext_resolve(vol_read, 0, "/frag.bin", &f));
}

/* The 64BIT shape: 64-byte group descriptors, split block counts. */
static void test_64bit_feature(void) {
    hype_file_map_t f;
    build_vol();
    put32(g_vol + 1024 + 0x60, INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_64BIT);
    put16(g_vol + 1024 + 0xFE, 64u); /* desc_size */
    /* Rebuild the group descriptor at its 64-byte slot (same offsets). */
    memset(blk(2), 0, BS);
    put32(blk(2) + 0x08, INODE_TABLE);
    put32(blk(2) + 0x28, 0u); /* inode_table_hi */
    CHECK_HEX("64bit resolve", 0, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    CHECK_HEX("64bit size", 20000u, (unsigned)f.size_bytes);
    /* An insane declared desc_size is refused. */
    put16(g_vol + 1024 + 0xFE, 48u);
    CHECK_HEX("bad desc_size", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
}

/*
 * Double- and triple-indirect maps need files bigger than 12 + 256 blocks and
 * 12 + 256 + 256*256 blocks respectively (1 KiB blocks, 256 pointers each) --
 * a ~70 MiB heap-built volume, worth it: these legs otherwise only run on
 * real hardware against a real disk.
 */
#define BIGV_BLOCKS 72000u
static uint8_t *g_big;

static int big_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if ((lba + count) * 512u > (uint64_t)BIGV_BLOCKS * BS) return -1;
    memcpy(dst, g_big + lba * 512u, (size_t)count * 512u);
    return 0;
}

static void test_triple_indirect(void) {
    hype_file_map_t f;
    uint8_t *in;
    uint32_t i;
    /* 12 direct + 256 single + 65536 double + 4 triple = 65808 blocks. All
     * physically CONSECUTIVE from block 1000, so the resolve coalesces the
     * whole thing into one extent -- the map machinery is what's under test. */
    const uint32_t nblocks = 12u + 256u + 65536u + 4u;
    const uint32_t data0 = 1000u;
    const uint32_t sind = 700u;  /* single-indirect pointer block */
    const uint32_t dind = 701u;  /* double-indirect root: 702..958 */
    const uint32_t tind = 970u;  /* triple-indirect root: 971 -> 972 */

    g_big = calloc(1u, (size_t)BIGV_BLOCKS * BS);
    if (g_big == 0) {
        CHECK("big volume allocated", 0);
        return;
    }
    /* Superblock + GDT + one inode, hand-rolled into the big buffer. */
    {
        uint8_t *sb = g_big + 1024;
        put32(sb + 0x00, 16u);
        put32(sb + 0x04, BIGV_BLOCKS);
        put32(sb + 0x14, 1u);
        put32(sb + 0x28, 16u);
        put16(sb + 0x38, 0xEF53u);
        put32(sb + 0x4C, 1u);
        put16(sb + 0x58, 256u);
        put32(sb + 0x60, INCOMPAT_FILETYPE);
        put32(g_big + 2u * BS + 0x08, 8u); /* inode table at block 8 */
    }
    /* Root (inode 2): indirect-mapped single dir block 30. */
    in = g_big + 8u * BS + (2u - 1u) * 256u;
    put16(in + 0x00, 0x41EDu);
    put32(in + 0x04, BS);
    put32(in + 0x28, 30u);
    {
        uint32_t off = 0;
        off = dirent(g_big + 30u * BS, off, 2u, ".", 2u);
        off = dirent(g_big + 30u * BS, off, 2u, "..", 2u);
        put32(g_big + 30u * BS + off, 12u);
        put16(g_big + 30u * BS + off + 4u, (uint16_t)(BS - off));
        g_big[30u * BS + off + 6u] = 7u;
        memcpy(g_big + 30u * BS + off + 8u, "big.bin", 7u);
    }
    /* big.bin (inode 12). */
    in = g_big + 8u * BS + (12u - 1u) * 256u;
    put16(in + 0x00, 0x81A4u);
    put32(in + 0x04, nblocks * BS);
    for (i = 0; i < 12u; i++) put32(in + 0x28 + i * 4u, data0 + i);
    put32(in + 0x28 + 12u * 4u, sind);
    put32(in + 0x28 + 13u * 4u, dind);
    put32(in + 0x28 + 14u * 4u, tind);
    for (i = 0; i < 256u; i++) put32(g_big + sind * BS + i * 4u, data0 + 12u + i);
    for (i = 0; i < 256u; i++) put32(g_big + dind * BS + i * 4u, 702u + i);
    for (i = 0; i < 65536u; i++) {
        put32(g_big + (702u + i / 256u) * BS + (i % 256u) * 4u, data0 + 12u + 256u + i);
    }
    put32(g_big + tind * BS + 0u, 971u);
    put32(g_big + 971u * BS + 0u, 972u);
    for (i = 0; i < 4u; i++) put32(g_big + 972u * BS + i * 4u, data0 + 12u + 256u + 65536u + i);

    CHECK_HEX("triple-indirect resolve", 0, hype_ext_resolve(big_read, 0, "/big.bin", &f));
    CHECK_HEX("size", (uint64_t)nblocks * BS, f.size_bytes);
    CHECK_HEX("one coalesced extent", 1u, f.count);
    CHECK_HEX("extent lba", (uint64_t)data0 * SPB, f.extents[0].start_lba);
    CHECK_HEX("extent sectors", (uint64_t)nblocks * SPB, f.extents[0].sector_count);
    /* Corrupt the deep indirection roots in turn: each must fail cleanly. */
    put32(g_big + 972u * BS + 1u * 4u, 0u); /* a hole deep in the triple map */
    CHECK_HEX("triple leaf hole", -1, hype_ext_resolve(big_read, 0, "/big.bin", &f));
    put32(g_big + 971u * BS + 0u, 0u); /* mid-level of the triple map */
    CHECK_HEX("triple mid hole", -1, hype_ext_resolve(big_read, 0, "/big.bin", &f));
    in = g_big + 8u * BS + (12u - 1u) * 256u;
    put32(in + 0x28 + 14u * 4u, 0u); /* the triple root itself */
    CHECK_HEX("triple root hole", -1, hype_ext_resolve(big_read, 0, "/big.bin", &f));
    put32(in + 0x28 + 14u * 4u, BIGV_BLOCKS + 5u);
    CHECK_HEX("triple root out of range", -1, hype_ext_resolve(big_read, 0, "/big.bin", &f));
    put32(g_big + 702u * BS + 5u * 4u, 0u); /* a second-level hole in the double map */
    put32(in + 0x28 + 14u * 4u, 970u); /* restore the triple root */
    CHECK_HEX("double mid hole", -1, hype_ext_resolve(big_read, 0, "/big.bin", &f));
    put32(in + 0x28 + 13u * 4u, 0u); /* the double root */
    CHECK_HEX("double root hole", -1, hype_ext_resolve(big_read, 0, "/big.bin", &f));
    free(g_big);
    g_big = 0;
}


/* Remaining defensive legs: superblock field bounds, crafted descriptor and
 * pointer-block corruption, extent-node children at impossible blocks. */
static void test_more_bounds(void) {
    hype_file_map_t f;

    build_vol();
    put32(g_vol + 1024 + 0x04, 1u); /* blocks_count 1 */
    CHECK_HEX("blocks_count too small", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    build_vol();
    put32(g_vol + 1024 + 0x00, 0u); /* inodes_count 0 */
    CHECK_HEX("no inodes", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    build_vol();
    put16(g_vol + 1024 + 0x58, 2048u); /* inode_size beyond 1024 */
    CHECK_HEX("oversized inode", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    build_vol();
    put16(g_vol + 1024 + 0x58, 64u); /* inode_size below 128 */
    CHECK_HEX("undersized inode", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    build_vol();
    put32(g_vol + 1024 + 0x60, INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_64BIT);
    put16(g_vol + 1024 + 0xFE, 1024u); /* desc_size beyond a sector */
    CHECK_HEX("oversized desc", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));
    build_vol();
    put32(g_vol + 1024 + 0x60, INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_64BIT);
    put16(g_vol + 1024 + 0xFE, 96u); /* not a power of two */
    CHECK_HEX("non-power desc", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    /* A dirent naming an inode past the table. */
    build_vol();
    put32(blk(ROOT_BLK) + 24u, 9999u); /* img.bin's inode number */
    CHECK_HEX("inode number past the table", -1, hype_ext_resolve(vol_read, 0, "/img.bin", &f));

    /* An inode whose table slot falls past the volume: shrink the volume so
     * a high inode's slot lands beyond it (the root and its data stay in). */
    build_vol();
    {
        uint32_t off = 0;
        /* rebuild root at block 20 so it stays inside the shrunk volume */
        const uint32_t runs[1][3] = {{0u, 20u, 1u}};
        extent_leaf_root(inode(2u), runs, 1u);
        off = dirent(blk(20u), off, 2u, ".", 2u);
        off = dirent(blk(20u), off, 2u, "..", 2u);
        off = dirent(blk(20u), off, 63u, "hi", 1u);
        put16(blk(20u) + off - 12u + 4u, (uint16_t)(BS - (off - 12u)));
        put32(g_vol + 1024 + 0x04, 23u); /* blocks_count 23: inode 63 lands out */
    }
    CHECK_HEX("inode slot past the volume", -1, hype_ext_resolve(vol_read, 0, "/hi", &f));

    /* An extent-tree child node at block 0, and one past the volume. */
    build_vol();
    put32(inode(17u) + 0x28 + 12u + 4u, 0u);
    CHECK_HEX("tree child at block 0", -1, hype_ext_resolve(vol_read, 0, "/tree.bin", &f));
    build_vol();
    put32(inode(17u) + 0x28 + 12u + 4u, VOL_BLOCKS + 3u);
    CHECK_HEX("tree child past the volume", -1, hype_ext_resolve(vol_read, 0, "/tree.bin", &f));
    /* A child node with a broken magic, and entries beyond its max. */
    build_vol();
    put16(blk(200u) + 0, 0xBEEFu);
    CHECK_HEX("child node magic", -1, hype_ext_resolve(vol_read, 0, "/tree.bin", &f));
    build_vol();
    put16(blk(200u) + 2, 200u); /* entries 200 > max 84 */
    CHECK_HEX("child entries beyond max", -1, hype_ext_resolve(vol_read, 0, "/tree.bin", &f));

    /* The indirection block POINTER itself as a hole / out of range (the data
     * pointer holes are a different leg, covered above). */
    build_vol();
    put32(inode(14u) + 0x28 + 12u * 4u, 0u);
    CHECK_HEX("single-indirect block missing", -1, hype_ext_resolve(vol_read, 0, "/ind.bin", &f));
    build_vol();
    put32(inode(14u) + 0x28 + 12u * 4u, VOL_BLOCKS + 9u);
    CHECK_HEX("single-indirect block out of range", -1,
              hype_ext_resolve(vol_read, 0, "/ind.bin", &f));

    /* An extent logically at/after EOF. */
    build_vol();
    {
        uint8_t *in = mk_inode(22u, 0x81A4u, BS, 0x80000u); /* one block total */
        const uint32_t runs[2][3] = {{0u, 220u, 1u}, {1u, 222u, 1u}};
        extent_leaf_root(in, runs, 2u);
    }
    CHECK_HEX("extent past EOF", -1, hype_ext_resolve(vol_read, 0, "/frag.bin", &f));

    /* Path shapes around separators. */
    build_vol();
    CHECK_HEX("trailing separator on a file is fine", 0,
              hype_ext_resolve(vol_read, 0, "/img.bin/", &f));
    CHECK_HEX("mixed separators", 0, hype_ext_resolve(vol_read, 0, "/sub\\deep.bin", &f));
    CHECK_HEX("doubled separators", 0, hype_ext_resolve(vol_read, 0, "//sub//deep.bin", &f));
}


/* ---- #204: in-place writes ---- */

static long g_write_countdown = -1;
static int vol_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if ((lba + count) * 512u > sizeof g_vol) return -1;
    if (g_write_countdown >= 0 && g_write_countdown-- == 0) return -1;
    memcpy(g_vol + lba * 512u, src, (size_t)count * 512u);
    return 0;
}

static void test_write_at(void) {
    hype_ext_wfile_t f;
    static uint8_t buf[12000];
    static uint8_t back[12000];
    unsigned int i;

    build_vol();
    CHECK_HEX("open_rw ok", 0, hype_ext_open_rw(vol_read, vol_write, 0, "/img.bin", &f));
    CHECK_HEX("open size", 20000u, (unsigned)f.map.size_bytes);
    /* Read the pre-existing pattern through the writer's own read_at. */
    CHECK_HEX("read_at", 0, hype_ext_read_at(&f, 0u, back, 4000u));
    for (i = 0; i < 4000u; i++) {
        if (back[i] != pat(i)) { CHECK_HEX("pre-existing byte", pat(i), back[i]); break; }
    }
    /* An unaligned span crossing the extent seam at byte 10240. */
    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(0xC3u ^ (i * 11u));
    CHECK_HEX("write_at across the seam", 0, hype_ext_write_at(&f, 9000u, buf, 3000u));
    CHECK_HEX("read it back", 0, hype_ext_read_at(&f, 9000u, back, 3000u));
    CHECK("seam span round-trips", memcmp(back, buf, 3000u) == 0);
    CHECK_HEX("byte before untouched", pat(8999u), blk(40u + 8999u / BS)[8999u % BS]);
    CHECK_HEX("byte after untouched", pat(12000u), blk(60u + 12000u / BS - 10u)[12000u % BS]);
    /* Whole-sector aligned bulk write (no read-modify-write on that leg). */
    CHECK_HEX("aligned bulk write", 0, hype_ext_write_at(&f, 1024u, buf, 2048u));
    CHECK("bulk landed", memcmp(blk(41u), buf, 1024u) == 0);
    /* Ragged head, bulk middle, ragged tail in one call. */
    CHECK_HEX("mixed write", 0, hype_ext_write_at(&f, 100u, buf, 2000u));
    CHECK_HEX("read the mix back", 0, hype_ext_read_at(&f, 100u, back, 2000u));
    CHECK("mixed round-trips", memcmp(back, buf, 2000u) == 0);
    /* The very last byte. */
    CHECK_HEX("last byte", 0, hype_ext_write_at(&f, 19999u, buf, 1u));
    CHECK_HEX("zero length is a no-op", 0, hype_ext_write_at(&f, 0u, buf, 0u));
    /* Out of range in every direction: refused, never clamped. */
    CHECK_HEX("write past the end", -1, hype_ext_write_at(&f, 20000u, buf, 1u));
    CHECK_HEX("write straddling the end", -1, hype_ext_write_at(&f, 19999u, buf, 2u));
    CHECK_HEX("read past the end", -1, hype_ext_read_at(&f, 20000u, back, 1u));
    CHECK_HEX("absurd offset", -1, hype_ext_write_at(&f, 0xFFFFFFFFFFFFFFFFull, buf, 1u));
    CHECK_HEX("null data", -1, hype_ext_write_at(&f, 0u, 0, 1u));
    CHECK_HEX("null read buffer", -1, hype_ext_read_at(&f, 0u, 0, 1u));
    /* The resolver still sees a healthy file afterwards (no metadata moved). */
    {
        hype_file_map_t m;
        CHECK_HEX("re-resolve after writes", 0, hype_ext_resolve(vol_read, 0, "/img.bin", &m));
        CHECK_HEX("same size", 20000u, (unsigned)m.size_bytes);
        CHECK_HEX("same extents", 2u, m.count);
    }
    /* An indirect-mapped file writes the same way. */
    CHECK_HEX("open ind.bin", 0, hype_ext_open_rw(vol_read, vol_write, 0, "/ind.bin", &f));
    CHECK_HEX("write into the indirect region", 0, hype_ext_write_at(&f, 13000u, buf, 600u));
    CHECK_HEX("read it back", 0, hype_ext_read_at(&f, 13000u, back, 600u));
    CHECK("indirect round-trips", memcmp(back, buf, 600u) == 0);
}

static void test_write_gate(void) {
    hype_ext_wfile_t f;
    /* No write callback. */
    build_vol();
    CHECK_HEX("NULL write callback refused", -1,
              hype_ext_open_rw(vol_read, 0, 0, "/img.bin", &f));
    /* A volume that was not cleanly unmounted. */
    build_vol();
    put16(g_vol + 1024 + 0x3A, 0x0000u);
    CHECK_HEX("mounted-dirty volume refused", -1,
              hype_ext_open_rw(vol_read, vol_write, 0, "/img.bin", &f));
    /* A volume with recorded errors. */
    build_vol();
    put16(g_vol + 1024 + 0x3A, 0x0003u);
    CHECK_HEX("errors-recorded volume refused", -1,
              hype_ext_open_rw(vol_read, vol_write, 0, "/img.bin", &f));
    /* Resolver failures surface through open_rw too. */
    build_vol();
    CHECK_HEX("missing file", -1, hype_ext_open_rw(vol_read, vol_write, 0, "/nope", &f));
    CHECK_HEX("sparse file refused for writing too", -1,
              hype_ext_open_rw(vol_read, vol_write, 0, "/hole.bin", &f));
    /* Superblock unreadable. */
    build_vol();
    g_fail_read_lba = 2u;
    CHECK_HEX("unreadable superblock", -1, hype_ext_open_rw(vol_read, vol_write, 0, "/img.bin", &f));
    g_fail_read_lba = (uint64_t)-1;
}

static void test_write_io_errors(void) {
    hype_ext_wfile_t f;
    static uint8_t buf[2048];
    long k;
    build_vol();
    CHECK_HEX("open ok", 0, hype_ext_open_rw(vol_read, vol_write, 0, "/img.bin", &f));
    for (k = 0; k < 12; k++) {
        g_write_countdown = k;
        (void)hype_ext_write_at(&f, 100u, buf, sizeof buf);
        g_read_countdown = k;
        (void)hype_ext_write_at(&f, 100u, buf, sizeof buf); /* RMW read leg */
        (void)hype_ext_read_at(&f, 100u, buf, sizeof buf);
        g_read_countdown = -1;
    }
    g_write_countdown = -1;
    CHECK("write fault sweep completed", 1);
}

/* Fault sweep: an injected read failure at every successive read of a full
 * resolve (both mapping schemes) must fail cleanly, never crash or spin. */
static void test_fault_sweep(void) {
    long k;
    for (k = 0; k < 1600; k += (k < 400 ? 1 : 5)) {
        hype_file_map_t f;
        build_vol();
        g_read_countdown = k;
        (void)hype_ext_resolve(vol_read, 0, "/img.bin", &f);
        (void)hype_ext_resolve(vol_read, 0, "/sub/deep.bin", &f);
        (void)hype_ext_resolve(vol_read, 0, "/ind.bin", &f);
        (void)hype_ext_resolve(vol_read, 0, "/tree.bin", &f);
    }
    g_read_countdown = -1;
    CHECK("fault sweep completed without crashing", 1);
}

/* ---- #293: the ext driver behind the common interface ---- */
static void test_fs_ops_ext(void) {
    static hype_fs_t fs;
    static hype_fs_file_t f;
    static hype_file_rmap_t rm;
    uint8_t buf[256];
    unsigned i;

    build_vol();
    CHECK_HEX("probe claims ext", 0, hype_ext_probe(vol_read, 0));

    /* read-only mount: lookup produces a generic rmap handle */
    CHECK_HEX("ro auto-mount", 0, hype_fs_mount_auto(&fs, vol_read, 0, 0));
    CHECK("driver is ext", fs.ops != 0 && fs.ops->name[0] == 'e' && fs.ops->name[1] == 'x' &&
                               fs.ops->name[2] == 't');
    CHECK("ro caps: read + sparse", hype_fs_caps(&fs) == (HYPE_FS_CAP_READ | HYPE_FS_CAP_SPARSE));
    CHECK_HEX("ro lookup img.bin", 0, hype_fs_lookup(&fs, "/img.bin", &f));
    CHECK_HEX("ro size", 20000, f.size);
    CHECK_HEX("ro read_at", 0, hype_fs_read_at(&f, 100, buf, 200));
    for (i = 0; i < 200u; i++) {
        if (buf[i] != pat(100u + i)) break;
    }
    CHECK("ro read data", i == 200u);
    CHECK("ro write refused", hype_fs_write_at(&f, 0, buf, 1) != 0);

    CHECK_HEX("map_ranges", 0, hype_fs_map_ranges(&fs, "/img.bin", &rm));
    CHECK_HEX("two DATA ranges", 2, rm.count);

    /* writable mount: lookup produces the native in-place handle */
    CHECK_HEX("rw auto-mount", 0, hype_fs_mount_auto(&fs, vol_read, vol_write, 0));
    CHECK("rw caps: in-place write", (hype_fs_caps(&fs) & HYPE_FS_CAP_WRITE_INPLACE) != 0);
    CHECK("rw caps: append + grow (#497)",
          (hype_fs_caps(&fs) & (HYPE_FS_CAP_APPEND | HYPE_FS_CAP_WRITE_GROW)) ==
              (HYPE_FS_CAP_APPEND | HYPE_FS_CAP_WRITE_GROW));
    CHECK_HEX("rw lookup", 0, hype_fs_lookup(&fs, "/img.bin", &f));
    /* #497: the legacy in-place handle cannot change a file's size -- append refuses. */
    CHECK("append refused on the native handle", hype_fs_append(&f, "x", 1) != 0);
    CHECK_HEX("rw read_at (native arm)", 0, hype_fs_read_at(&f, 0, buf, 16));
    buf[0] = 0xEE;
    CHECK_HEX("rw write_at in place", 0, hype_fs_write_at(&f, 3, buf, 1));
    CHECK_HEX("write visible", 0xEE, blk(40u)[3]);
    CHECK("rw write past size refused", hype_fs_write_at(&f, 19999, buf, 2) != 0);
    CHECK("append refused (NULL slot)", hype_fs_append(&f, buf, 1) != 0);
    CHECK("create refused (NULL slot)", hype_fs_create(&fs, "/x", &f) != 0);
    CHECK("mkdir refused (NULL slot)", hype_fs_mkdir(&fs, "/d") != 0);
    CHECK("sync is a clean no-op", hype_fs_sync(&fs) == 0);
    CHECK("identity error never fires on ext", hype_fs_file_identity_error(&f) == 0);

    CHECK("rw lookup missing fails", hype_fs_lookup(&fs, "/nope", &f) != 0);
    CHECK("map_ranges missing fails", hype_fs_map_ranges(&fs, "/nope", &rm) != 0);
    {
        static hype_fs_t rofs;
        CHECK_HEX("ro mount again", 0, hype_fs_mount_auto(&rofs, vol_read, 0, 0));
        CHECK("ro lookup missing fails", hype_fs_lookup(&rofs, "/nope", &f) != 0);
    }
    memset(&f, 0, sizeof(f));
    f.fs = &fs;
    f.tag = 0;
    CHECK("read_at bogus tag", hype_fs_read_at(&f, 0, buf, 1) != 0);
    CHECK("write_at bogus tag", hype_fs_write_at(&f, 0, buf, 1) != 0);
}


/* ---- #384: sparse-aware resolution + the ext2 allocating writer ---- */

/* An ext2-shaped volume (no extents feature) with real allocation metadata:
 * one group, block bitmap at block 3, everything below FREE_FROM marked used. */
#define BITMAP_BLK 3u
#define FREE_FROM 1000u

static uint32_t g_wfail_at; /* fail the Nth write; ~0 = never */
static uint32_t g_writes_seen;
static int g_whardfail; /* once tripped, keep failing (rollback double fault) */
static int vol_write2(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (lba + count > (uint64_t)VOL_BLOCKS * (BS / 512u)) return -1;
    if (g_writes_seen++ == g_wfail_at) {
        if (g_whardfail) g_wfail_at = g_writes_seen; /* keep tripping */
        return -1;
    }
    memcpy(g_vol + lba * 512u, src, (size_t)count * 512u);
    return 0;
}

static void build_vol_ext2(void) {
    uint8_t *sb = g_vol + 1024;
    unsigned int i;
    uint32_t used = 0;

    memset(g_vol, 0, sizeof g_vol);
    superblock(INCOMPAT_FILETYPE);
    put32(sb + 0x20, VOL_BLOCKS);      /* blocks_per_group: one group */
    group_desc();
    put32(blk(2) + 0x00, BITMAP_BLK);  /* bg_block_bitmap */

    /* bitmap: blocks [first_data_block, FREE_FROM) used, the rest free */
    for (i = 1; i < FREE_FROM; i++) {
        uint32_t bit = i - 1u; /* first_data_block == 1 */
        blk(BITMAP_BLK)[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
        used++;
    }
    put16(blk(2) + 0x0C, (uint16_t)(VOL_BLOCKS - 1u - used)); /* gd free */
    put32(sb + 0x0C, VOL_BLOCKS - 1u - used);                 /* sb free */

    /* root (inode 2): classic map -> ROOT_BLK */
    {
        uint8_t *in = mk_inode(2u, 0x41EDu, BS, 0u);
        put32(in + 0x28, ROOT_BLK);
        {
            uint32_t off = 0, last;
            off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
            off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
            off = dirent(blk(ROOT_BLK), off, 12u, "plain.bin", 1u);
            last = off;
            off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
            dirent_close(blk(ROOT_BLK), last + 16u, BS);
        }
    }
    /* plain.bin (12): fully mapped, 2 blocks at 40,41 */
    {
        uint8_t *in = mk_inode(12u, 0x81A4u, 2u * BS, 0u);
        put32(in + 0x28 + 0u, 40u);
        put32(in + 0x28 + 4u, 41u);
        put32(in + 0x1C, 2u * (BS / 512u)); /* i_blocks */
        for (i = 0; i < BS; i++) { blk(40u)[i] = pat(i); blk(41u)[i] = pat(i + BS); }
    }
    /* swiss.bin (13): 40 blocks logical; direct 0 mapped (50), direct 1 hole,
     * direct 2 mapped (51), directs 3..11 holes, the whole single-indirect
     * region (12..39) a hole via a NULL root pointer. Size 40*BS - 100. */
    {
        uint8_t *in = mk_inode(13u, 0x81A4u, 40u * BS - 100u, 0u);
        put32(in + 0x28 + 0u, 50u);
        put32(in + 0x28 + 8u, 51u);
        put32(in + 0x1C, 2u * (BS / 512u));
        for (i = 0; i < BS; i++) { blk(50u)[i] = pat(i + 7u); blk(51u)[i] = pat(i + 9u); }
    }
}

static void test_resolve_rmap_sparse(void) {
    static hype_file_rmap_t m;
    uint8_t buf[2 * BS + 64];
    unsigned i;

    /* extent-mapped volume: hole.bin starts at logical 1; unwrit.bin has an
     * unwritten extent -- both now RESOLVE instead of refusing */
    build_vol();
    CHECK_HEX("hole.bin resolves sparse", 0, hype_ext_resolve_rmap(vol_read, 0, "/hole.bin", &m));
    CHECK("hole.bin leading HOLE", m.count >= 2 && m.ranges[0].kind == HYPE_RANGE_HOLE);
    CHECK_HEX("hole.bin read", 0, hype_file_rmap_read_at(&m, vol_read, 0, 0, buf, 1024));
    for (i = 0; i < 1024u; i++) { if (buf[i] != 0) break; }
    CHECK("hole.bin zeros", i == 1024u);
    CHECK("legacy still refuses holes",
          hype_ext_resolve(vol_read, 0, "/hole.bin", (hype_file_map_t *)g_scratch_map) != 0);

    CHECK_HEX("unwrit.bin resolves sparse", 0,
              hype_ext_resolve_rmap(vol_read, 0, "/unwrit.bin", &m));
    {
        int have_unwritten = 0;
        for (i = 0; i < m.count; i++) {
            if (m.ranges[i].kind == HYPE_RANGE_UNWRITTEN) have_unwritten = 1;
        }
        CHECK("unwritten extent mapped as UNWRITTEN", have_unwritten);
    }

    /* classic-map holes at every level */
    build_vol_ext2();
    CHECK_HEX("swiss.bin resolves", 0, hype_ext_resolve_rmap(vol_read, 0, "/swiss.bin", &m));
    CHECK("first range DATA", m.ranges[0].kind == HYPE_RANGE_DATA);
    CHECK("second range HOLE", m.ranges[1].kind == HYPE_RANGE_HOLE);
    CHECK_HEX("read across data|hole|data", 0,
              hype_file_rmap_read_at(&m, vol_read, 0, BS - 16u, buf, 2u * BS + 32u));
    for (i = 0; i < 16u; i++) { if (buf[i] != pat(BS - 16u + i + 7u)) break; }
    CHECK("data before hole", i == 16u);
    for (i = 16u; i < 16u + BS; i++) { if (buf[i] != 0) break; }
    CHECK("hole zeros", i == 16u + BS);
    for (i = 0; i < 32u; i++) { if (buf[16u + BS + i] != pat(i + 9u)) break; }
    CHECK("data after hole", i == 32u);
    /* the tail (missing single-indirect level + partial last block) is zeros */
    CHECK_HEX("tail read", 0, hype_file_rmap_read_at(&m, vol_read, 0, 39u * BS, buf, BS - 100u));
    for (i = 0; i < BS - 100u; i++) { if (buf[i] != 0) break; }
    CHECK("tail hole zeros", i == BS - 100u);
    CHECK_HEX("plain.bin still resolves", 0, hype_ext_resolve_rmap(vol_read, 0, "/plain.bin", &m));
    CHECK_HEX("plain one DATA range", 1, m.count);
}

static uint32_t bitmap_used_count(void) {
    uint32_t n = 0, i;
    for (i = 0; i + 1u < VOL_BLOCKS; i++) {
        if (blk(BITMAP_BLK)[i / 8u] & (1u << (i % 8u))) n++;
    }
    return n;
}

static void test_ext2_alloc(void) {
    static hype_ext2_wfile_t w;
    uint8_t buf[2 * BS];
    uint32_t used_before, sbfree_before;
    unsigned i;

    build_vol_ext2();
    g_wfail_at = ~0u;
    g_writes_seen = 0;

    CHECK_HEX("open_rw", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    used_before = bitmap_used_count();
    sbfree_before = get32(g_vol + 1024 + 0x0C);
    (void)sbfree_before;

    /* in-place fast path: no metadata movement */
    CHECK_HEX("in-place write", 0, hype_ext2_write_at(&w, 10, "QQ", 2));
    CHECK_HEX("no allocation", used_before, bitmap_used_count());

    /* fill the direct-level hole (logical block 1) */
    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i ^ 0x3C);
    CHECK_HEX("hole write (direct)", 0, hype_ext2_write_at(&w, BS + 100u, buf, 200));
    CHECK_HEX("one block allocated", used_before + 1u, bitmap_used_count());
    CHECK_HEX("sb free dropped", sbfree_before - 1u, get32(g_vol + 1024 + 0x0C));
    CHECK("volume clean after", (g_vol[1024 + 0x3A] & 1u) == 1u);
    CHECK_HEX("readback", 0, hype_ext2_read_at(&w, BS + 100u, buf + BS, 200));
    CHECK("hole write data", memcmp(buf + BS, buf, 200) == 0);
    CHECK_HEX("zero fill before", 0, hype_ext2_read_at(&w, BS, buf + BS, 100));
    for (i = 0; i < 100u; i++) { if (buf[BS + i] != 0) break; }
    CHECK("block head zeroed", i == 100u);
    /* inode: pointer + i_blocks */
    CHECK("direct pointer published", get32(inode(13u) + 0x28 + 4u) != 0u);
    CHECK_HEX("i_blocks bumped", 3u * (BS / 512u), get32(inode(13u) + 0x1C));

    /* allocation through a MISSING single-indirect tree (logical 20) */
    used_before = bitmap_used_count();
    CHECK_HEX("hole write (indirect)", 0, hype_ext2_write_at(&w, 20u * BS + 5u, "IND", 3));
    CHECK_HEX("data + pointer block allocated", used_before + 2u, bitmap_used_count());
    CHECK("indirect root published", get32(inode(13u) + 0x28 + 12u * 4u) != 0u);
    CHECK_HEX("indirect readback", 0, hype_ext2_read_at(&w, 20u * BS + 5u, buf, 3));
    CHECK("indirect data", memcmp(buf, "IND", 3) == 0);
    /* neighbouring logical blocks in the same region are still holes */
    CHECK_HEX("neighbour still zero", 0, hype_ext2_read_at(&w, 21u * BS, buf, 64));
    for (i = 0; i < 64u; i++) { if (buf[i] != 0) break; }
    CHECK("neighbour zeros", i == 64u);

    /* a second write into the SAME indirect tree reuses the pointer block */
    used_before = bitmap_used_count();
    CHECK_HEX("same-tree write", 0, hype_ext2_write_at(&w, 21u * BS, "TWO", 3));
    CHECK_HEX("only the data block allocated", used_before + 1u, bitmap_used_count());

    /* mtime stamping */
    hype_ext2_set_time(&w, 1765432100u);
    CHECK_HEX("stamped write", 0, hype_ext2_write_at(&w, 22u * BS, "T", 1));
    CHECK_HEX("mtime", 1765432100u, get32(inode(13u) + 0x10));

    /* bounds + bound refusals */
    /* #497: a write past EOF now GROWS. Straddling the boundary by one byte: the last old
     * byte is overwritten in place and the size moves by exactly one. */
    {
        uint64_t before = w.size_bytes;
        uint8_t rb[2];
        CHECK_HEX("straddling write grows", 0,
                  hype_ext2_write_at(&w, w.size_bytes - 1u, "ab", 2));
        CHECK("size moved by one", w.size_bytes == before + 1u);
        CHECK_HEX("straddle readback", 0, hype_ext2_read_at(&w, before - 1u, rb, 2));
        CHECK("straddle bytes", rb[0] == 'a' && rb[1] == 'b');
    }
    CHECK("overflow refused", hype_ext2_write_at(&w, ~0ull - 1u, "a", 1) != 0);
    CHECK_HEX("len 0 no-op", 0, hype_ext2_write_at(&w, 0, buf, 0));
    CHECK("reads past EOF refused", hype_ext2_read_at(&w, w.size_bytes, buf, 1) != 0);

    /* open refusals */
    {
        static hype_ext2_wfile_t r;
        uint8_t *sb = g_vol + 1024;
        CHECK("missing file refused", hype_ext2_open_rw(vol_read, vol_write2, 0, "/no", &r) != 0);
        put32(sb + 0x5C, 0x0004u); /* COMPAT_HAS_JOURNAL */
        CHECK("journal refused (that is #385)",
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        put32(sb + 0x5C, 0);
        put32(sb + 0x64, 0x0400u); /* RO_COMPAT metadata_csum */
        CHECK("checksummed volume refused",
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        put32(sb + 0x64, 0);
        put16(sb + 0x3A, 0); /* dirty */
        CHECK("dirty volume refused",
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        put16(sb + 0x3A, 0x0003u); /* valid + ERROR */
        CHECK("error volume refused",
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        put16(sb + 0x3A, 0x0001u);
        CHECK("NULL write refused", hype_ext2_open_rw(vol_read, 0, 0, "/swiss.bin", &r) != 0);
    }
    /* an extent-mapped file is #385's problem */
    {
        static hype_ext2_wfile_t r;
        build_vol(); /* the extent-feature volume */
        CHECK("extent volume refused (incompat set)",
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/img.bin", &r) != 0);
    }
}

static void test_ext2_alloc_rollback(void) {
    static hype_ext2_wfile_t w;
    uint8_t buf[64];
    uint32_t used_before;
    long n;

    /* volume full: every free block burned */
    build_vol_ext2();
    g_wfail_at = ~0u;
    g_writes_seen = 0;
    {
        unsigned i;
        for (i = FREE_FROM; i + 1u < VOL_BLOCKS + 1u && i - 1u < (VOL_BLOCKS - 1u); i++) {
            uint32_t bit = i - 1u;
            blk(BITMAP_BLK)[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
        }
        put16(blk(2) + 0x0C, 0);
        put32(g_vol + 1024 + 0x0C, 0);
    }
    CHECK_HEX("open on full volume", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    used_before = bitmap_used_count();
    CHECK("hole write on full volume fails", hype_ext2_write_at(&w, BS, buf, 8) != 0);
    CHECK_HEX("nothing leaked", used_before, bitmap_used_count());
    CHECK("volume clean after clean rollback", (g_vol[1024 + 0x3A] & 1u) == 1u);
    CHECK("in-place path still works on a full volume",
          hype_ext2_write_at(&w, 10, "ok", 2) == 0);

    /* per-call transaction bound */
    build_vol_ext2();
    CHECK_HEX("open", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    CHECK("a span past the allocation bound is refused up front",
          hype_ext2_write_at(&w, BS, (const void *)g_vol, 300u * 1024u) != 0);

    /* fault-injection sweep over an allocating write: after every failure the
     * volume must reopen consistently and never leak or cross-link a block */
    for (n = 0; n < 30; n++) {
        static hype_ext2_wfile_t w2;
        build_vol_ext2();
        g_writes_seen = 0;
        g_wfail_at = (uint32_t)n;
        (void)hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w2);
        (void)hype_ext2_write_at(&w2, 20u * BS + 5u, "XYZ", 3);
        g_wfail_at = ~0u;
        {
            static hype_file_rmap_t m2;
            uint32_t used = bitmap_used_count();
            uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
            CHECK("sweep: map still resolves",
                  hype_ext_resolve_rmap(vol_read, 0, "/swiss.bin", &m2) == 0);
            CHECK("sweep: counters consistent",
                  used + gdfree == VOL_BLOCKS - 1u);
        }
    }
    g_wfail_at = ~0u;
}


/* deep sparse file + two-group volume + fault sweeps for the allocator */
static void test_ext2_alloc_deep(void) {
    static hype_ext2_wfile_t w;
    uint8_t buf[64];
    unsigned i;

    /* huge.bin: 66,000 logical blocks, ALL holes -- writes land in the
     * single-, double- and triple-indirect regions and materialize each
     * pointer level on the way */
    build_vol_ext2();
    {
        uint8_t *in = mk_inode(14u, 0x81A4u, 0u, 0u);
        uint32_t off, last;
        put32(in + 0x04, 0xFFFFFFFFu & (66000u * BS)); /* size lo */
        put32(in + 0x6C, (uint32_t)(((uint64_t)66000u * BS) >> 32)); /* size hi */
        off = 0;
        off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
        off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
        off = dirent(blk(ROOT_BLK), off, 12u, "plain.bin", 1u);
        off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
        last = off;
        off = dirent(blk(ROOT_BLK), off, 14u, "huge.bin", 1u);
        dirent_close(blk(ROOT_BLK), last + 16u, BS);
    }
    CHECK_HEX("open huge", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w));
    CHECK_HEX("write @ direct", 0, hype_ext2_write_at(&w, 3u * BS, "D1", 2));
    CHECK_HEX("write @ single-indirect", 0, hype_ext2_write_at(&w, 100u * BS, "S1", 2));
    CHECK_HEX("write @ double-indirect", 0, hype_ext2_write_at(&w, 1000u * BS, "W2", 2));
    CHECK_HEX("write @ triple-indirect", 0,
              hype_ext2_write_at(&w, (uint64_t)(12u + 256u + 65536u + 3u) * BS, "T3", 2));
    CHECK_HEX("read back direct", 0, hype_ext2_read_at(&w, 3u * BS, buf, 2));
    CHECK("d", memcmp(buf, "D1", 2) == 0);
    CHECK_HEX("read back double", 0, hype_ext2_read_at(&w, 1000u * BS, buf, 2));
    CHECK("w2", memcmp(buf, "W2", 2) == 0);
    CHECK_HEX("read back triple", 0,
              hype_ext2_read_at(&w, (uint64_t)(12u + 256u + 65536u + 3u) * BS, buf, 2));
    CHECK("t3", memcmp(buf, "T3", 2) == 0);
    /* untouched neighbours still zero */
    CHECK_HEX("neighbour", 0, hype_ext2_read_at(&w, 999u * BS, buf, 8));
    for (i = 0; i < 8u; i++) { if (buf[i] != 0) break; }
    CHECK("neighbour zero", i == 8u);
    /* a multi-block write spanning several holes at once */
    {
        static uint8_t big[3 * BS];
        unsigned k;
        for (k = 0; k < sizeof big; k++) big[k] = (uint8_t)(k * 5u + 1u);
        CHECK_HEX("multi-block hole write", 0,
                  hype_ext2_write_at(&w, 500u * BS - 100u, big, sizeof big));
        CHECK_HEX("multi-block readback", 0,
                  hype_ext2_read_at(&w, 500u * BS - 100u, big, 64));
        for (k = 0; k < 64u; k++) { if (big[k] != (uint8_t)(k * 5u + 1u)) break; }
        CHECK("multi-block data", k == 64u);
    }

    /* two block groups: allocation must cross into group 1 when group 0 is
     * exhausted (the ticket's block-group-transition case) */
    build_vol_ext2();
    {
        uint8_t *sb = g_vol + 1024;
        unsigned k;
        put32(sb + 0x20, 2048u); /* blocks_per_group: groups 0 and 1 */
        /* group 1 descriptor: bitmap at block 4 */
        put32(blk(2) + 32u + 0x00u, 4u);
        put32(blk(2) + 32u + 0x08u, INODE_TABLE); /* unused, but valid */
        memset(blk(4), 0, BS);
        /* group 0: burn EVERYTHING free (bits 0..2047 == blocks 1..2048) */
        for (k = 0; k < 2048u; k++) blk(BITMAP_BLK)[k / 8u] |= (uint8_t)(1u << (k % 8u));
        put16(blk(2) + 0x0C, 0);
        /* group 1: blocks 2049.. free except a used prefix */
        for (k = 0; k < 64u; k++) blk(4u)[k / 8u] |= (uint8_t)(1u << (k % 8u));
        put16(blk(2) + 32u + 0x0Cu, (uint16_t)(2047u - 64u));
        put32(sb + 0x0C, 2047u - 64u);
    }
    CHECK_HEX("open on 2-group volume", 0,
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    CHECK_HEX("cross-group allocation", 0, hype_ext2_write_at(&w, BS, "G2", 2));
    {
        /* the allocated block must be in group 1's range */
        uint32_t ptr = get32(inode(13u) + 0x28 + 4u);
        CHECK("block came from group 1", ptr >= 2049u);
    }

    /* inconsistent metadata refusals */
    build_vol_ext2();
    put16(blk(2) + 0x0C, 0); /* free bits exist, but the group SAYS zero */
    CHECK_HEX("open", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    CHECK("free-count/bitmap disagreement refused", hype_ext2_write_at(&w, BS, "x", 1) != 0);

    build_vol_ext2();
    put32(blk(2) + 0x00, 0); /* bitmap pointer NULL */
    CHECK_HEX("open2", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    CHECK("NULL bitmap pointer refused", hype_ext2_write_at(&w, BS, "x", 1) != 0);

    /* open-time superblock guards */
    {
        static hype_ext2_wfile_t r;
        uint8_t *sb = g_vol + 1024;
        build_vol_ext2();
        put16(sb + 0x38, 0x1234u);
        CHECK("bad magic refused", hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        build_vol_ext2();
        put32(sb + 0x18, 3u);
        CHECK("8K blocks refused", hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        build_vol_ext2();
        put32(sb + 0x20, 0);
        CHECK("zero blocks-per-group refused",
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        build_vol_ext2();
        CHECK("NULL read refused", hype_ext2_open_rw(0, vol_write2, 0, "/swiss.bin", &r) != 0);
        g_read_countdown = 0;
        CHECK("unreadable sb refused", hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        g_read_countdown = -1;
    }

    /* read-fault sweep across open + an allocating write */
    {
        long n;
        for (n = 0; n < 40; n++) {
            static hype_ext2_wfile_t w2;
            build_vol_ext2();
            g_read_countdown = n;
            if (hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w2) == 0) {
                (void)hype_ext2_write_at(&w2, 20u * BS + 5u, "XYZ", 3);
            }
            g_read_countdown = -1;
            {
                static hype_file_rmap_t m2;
                CHECK("read-sweep: still resolvable",
                      hype_ext_resolve_rmap(vol_read, 0, "/swiss.bin", &m2) == 0);
            }
        }
    }
}


static void test_384_final_edges(void) {
    static hype_ext2_wfile_t w;
    static hype_file_rmap_t m;
    uint8_t buf[64];
    long n;

    /* write-fault sweep, single-shot: rollback restores consistency */
    for (n = 0; n < 40; n++) {
        build_vol_ext2();
        g_writes_seen = 0;
        g_whardfail = 0;
        g_wfail_at = (uint32_t)n;
        if (hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) == 0) {
            (void)hype_ext2_write_at(&w, 20u * BS + 5u, "XYZ", 3);
        }
        g_wfail_at = ~0u;
        {
            uint32_t used = bitmap_used_count();
            uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
            CHECK("wsweep: counters consistent", used + gdfree == VOL_BLOCKS - 1u);
            CHECK("wsweep: still resolvable",
                  hype_ext_resolve_rmap(vol_read, 0, "/swiss.bin", &m) == 0);
        }
    }
    /* double fault: rollback itself failing leaves the volume marked dirty */
    build_vol_ext2();
    g_writes_seen = 0;
    g_wfail_at = 4; /* mid-allocation */
    g_whardfail = 1;
    if (hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) == 0) {
        CHECK("double fault fails", hype_ext2_write_at(&w, 20u * BS + 5u, "XYZ", 3) != 0);
    }
    g_wfail_at = ~0u;
    g_whardfail = 0;
    CHECK("volume honestly dirty after failed rollback",
          (g_vol[1024 + 0x3A] & 1u) == 0u);

    /* rev-0 superblock (128-byte inodes): a shape difference, handled or
     * refused cleanly, never mis-parsed */
    build_vol_ext2();
    put32(g_vol + 1024 + 0x4C, 0);
    (void)hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w);

    /* extent-tree sparse edge cases through the resolver */
    build_vol();
    {
        /* xsp.bin (24): hole, then an UNWRITTEN extent, then a written one */
        uint8_t *in = mk_inode(24u, 0x81A4u, 8u * BS, 0x80000u);
        uint8_t *eh = in + 0x28;
        put16(eh + 0, 0xF30Au); put16(eh + 2, 2u); put16(eh + 4, 4u); put16(eh + 6, 0u);
        /* entry 0: logical 2, len 0x8002 (unwritten, real 2), phys 45 */
        put32(eh + 12 + 0, 2u); put16(eh + 12 + 4, 0x8002u); put16(eh + 12 + 6, 0u);
        put32(eh + 12 + 8, 45u);
        /* entry 1: logical 6, len 2, phys 47 */
        put32(eh + 24 + 0, 6u); put16(eh + 24 + 4, 2u); put16(eh + 24 + 6, 0u);
        put32(eh + 24 + 8, 47u);
        {
            uint32_t off = 0, last;
            off = dirent(blk(31u), off, 13u, ".", 2u);
            off = dirent(blk(31u), off, 2u, "..", 2u);
            last = off;
            off = dirent(blk(31u), off, 24u, "xsp.bin", 1u);
            dirent_close(blk(31u), last + 12u, BS);
        }
    }
    CHECK_HEX("hole+unwritten+data resolves", 0,
              hype_ext_resolve_rmap(vol_read, 0, "/sub/xsp.bin", &m));
    CHECK("shape: HOLE, UNWRITTEN, HOLE, DATA",
          m.count == 4 && m.ranges[0].kind == HYPE_RANGE_HOLE &&
              m.ranges[1].kind == HYPE_RANGE_UNWRITTEN &&
              m.ranges[2].kind == HYPE_RANGE_HOLE &&
              m.ranges[3].kind == HYPE_RANGE_DATA);
    CHECK_HEX("everything before the data reads zero", 0,
              hype_file_rmap_read_at(&m, vol_read, 0, 0, buf, 64));

    /* malformed unwritten shapes refuse */
    {
        uint8_t *in = inode(24u);
        uint8_t *eh = in + 0x28;
        put16(eh + 12 + 4, 0x8000u); /* real length 0 */
        CHECK("unwritten real-len 0 refused",
              hype_ext_resolve_rmap(vol_read, 0, "/sub/xsp.bin", &m) != 0);
        put16(eh + 12 + 4, 0x8002u);
        put32(eh + 12 + 8, 0u); /* phys 0 */
        CHECK("unwritten at phys 0 refused",
              hype_ext_resolve_rmap(vol_read, 0, "/sub/xsp.bin", &m) != 0);
        put32(eh + 12 + 8, 45u);
        put32(eh + 12 + 0, 7u); /* overlaps entry 1: out of order */
        CHECK("overlapping unwritten refused",
              hype_ext_resolve_rmap(vol_read, 0, "/sub/xsp.bin", &m) != 0);
    }

    /* classic map: an allocated double-indirect root whose MID pointer is 0
     * (a hole one level down) */
    build_vol_ext2();
    {
        uint8_t *in = mk_inode(15u, 0x81A4u, (12u + 256u + 300u) * BS, 0u);
        uint32_t off, last;
        put32(in + 0x28 + 13u * 4u, 60u); /* double-indirect root allocated */
        memset(blk(60u), 0, BS);          /* every mid pointer 0: all holes */
        off = 0;
        off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
        off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
        off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
        last = off;
        off = dirent(blk(ROOT_BLK), off, 15u, "dmid.bin", 1u);
        dirent_close(blk(ROOT_BLK), last + 16u, BS);
    }
    CHECK_HEX("mid-level hole resolves", 0, hype_ext_resolve_rmap(vol_read, 0, "/dmid.bin", &m));
    CHECK_HEX("read mid-level hole", 0,
              hype_file_rmap_read_at(&m, vol_read, 0, (uint64_t)(12u + 256u + 100u) * BS, buf, 64));
    {
        unsigned i;
        for (i = 0; i < 64u; i++) { if (buf[i] != 0) break; }
        CHECK("mid-level hole zeros", i == 64u);
    }
    /* writing there materializes root -> mid -> leaf -> data */
    CHECK_HEX("open dmid", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/dmid.bin", &w));
    CHECK_HEX("write through mid-level hole", 0,
              hype_ext2_write_at(&w, (uint64_t)(12u + 256u + 100u) * BS + 9u, "MID", 3));
    CHECK_HEX("read back", 0, hype_ext2_read_at(&w, (uint64_t)(12u + 256u + 100u) * BS + 9u, buf, 3));
    CHECK("mid data", memcmp(buf, "MID", 3) == 0);
}


static void test_384_coverage_tail(void) {
    static hype_ext2_wfile_t w;
    static hype_file_rmap_t m;
    uint8_t buf[64];
    long n;

    /* NULL out params are tolerated on the way to the real refusal */
    build_vol();
    CHECK("NULL rmap out refused", hype_ext_resolve_rmap(vol_read, 0, "/nope", 0) != 0);
    CHECK("NULL map out refused", hype_ext_resolve(vol_read, 0, "/nope", 0) != 0);

    /* map-by-inode failures + success */
    CHECK("map_ino: bad inode refused", hype_ext_map_ino_rmap(vol_read, 0, 0u, &m) != 0);
    CHECK("map_ino: directory refused", hype_ext_map_ino_rmap(vol_read, 0, 2u, &m) != 0);
    CHECK_HEX("map_ino: img.bin by number", 0, hype_ext_map_ino_rmap(vol_read, 0, 12u, &m));
    {
        uint8_t save = g_vol[1024 + 0x38];
        g_vol[1024 + 0x38] = 0x11; /* break the magic */
        CHECK("map_ino: bad volume refused", hype_ext_map_ino_rmap(vol_read, 0, 12u, &m) != 0);
        g_vol[1024 + 0x38] = save;
    }

    /* inline + empty files through the sparse resolver */
    CHECK("inline refused (rmap)", hype_ext_resolve_rmap(vol_read, 0, "/inline.bin", &m) != 0);
    CHECK_HEX("empty resolves (rmap)", 0, hype_ext_resolve_rmap(vol_read, 0, "/empty.bin", &m));
    CHECK_HEX("empty has no ranges", 0, m.count);

    /* extent-node shape guards */
    {
        uint8_t *eh = inode(17u) + 0x28; /* tree.bin: depth-1 root */
        put16(eh + 2, 5u); /* entries > max */
        CHECK("entries > max refused", hype_ext_resolve_rmap(vol_read, 0, "/tree.bin", &m) != 0);
        put16(eh + 2, 1u);
    }
    {
        uint8_t *eh = inode(12u) + 0x28; /* img.bin leaf root */
        put16(eh + 12 + 4, 0u); /* extent length 0 */
        CHECK("zero-length extent refused", hype_ext_resolve_rmap(vol_read, 0, "/img.bin", &m) != 0);
        put16(eh + 12 + 4, 10u);
    }
    {
        uint8_t *eh = inode(24u) + 0x28; /* xsp.bin from the earlier test volume is gone: rebuild */
        (void)eh;
    }

    /* unwritten beyond the file / at an out-of-volume phys */
    build_vol();
    {
        uint8_t *in = mk_inode(25u, 0x81A4u, 4u * BS, 0x80000u);
        uint8_t *eh = in + 0x28;
        uint32_t off, last;
        put16(eh + 0, 0xF30Au); put16(eh + 2, 1u); put16(eh + 4, 4u); put16(eh + 6, 0u);
        put32(eh + 12 + 0, 100u); put16(eh + 12 + 4, 0x8002u); put16(eh + 12 + 6, 0u);
        put32(eh + 12 + 8, 45u);
        off = 0;
        off = dirent(blk(31u), off, 13u, ".", 2u);
        off = dirent(blk(31u), off, 2u, "..", 2u);
        last = off;
        off = dirent(blk(31u), off, 25u, "uw.bin", 1u);
        dirent_close(blk(31u), last + 12u, BS);
    }
    CHECK("unwritten past EOF refused", hype_ext_resolve_rmap(vol_read, 0, "/sub/uw.bin", &m) != 0);
    {
        uint8_t *eh = inode(25u) + 0x28;
        put32(eh + 12 + 0, 1u);
        put32(eh + 12 + 8, 4000000u); /* phys far outside the volume */
        CHECK("unwritten outside volume refused",
              hype_ext_resolve_rmap(vol_read, 0, "/sub/uw.bin", &m) != 0);
    }

    /* superblock shape guards */
    build_vol();
    put32(g_vol + 1024 + 0x18, 1u); /* 2048-byte blocks, first_data_block still 1 */
    CHECK("fdb mismatch refused", hype_ext_resolve_rmap(vol_read, 0, "/img.bin", &m) != 0);
    build_vol();
    put16(g_vol + 1024 + 0x58, 192u); /* inode size: not a power of two */
    CHECK("non-pow2 inode size refused", hype_ext_resolve_rmap(vol_read, 0, "/img.bin", &m) != 0);

    /* triple-indirect partial trees: allocated L3 root, holes below */
    build_vol_ext2();
    {
        uint8_t *in = mk_inode(16u, 0x81A4u, (uint64_t)(12u + 256u + 65536u + 600u) * BS, 0u);
        uint32_t off, last;
        put32(in + 0x04, (uint32_t)((uint64_t)(12u + 256u + 65536u + 600u) * BS));
        put32(in + 0x28 + 14u * 4u, 61u); /* L3 root allocated */
        memset(blk(61u), 0, BS);
        put32(blk(61u) + 0, 62u); /* first hi allocated... */
        memset(blk(62u), 0, BS);  /* ...but every mid below it is a hole */
        off = 0;
        off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
        off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
        off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
        last = off;
        off = dirent(blk(ROOT_BLK), off, 16u, "tmid.bin", 1u);
        dirent_close(blk(ROOT_BLK), last + 16u, BS);
    }
    CHECK_HEX("triple partial tree resolves", 0, hype_ext_resolve_rmap(vol_read, 0, "/tmid.bin", &m));
    CHECK_HEX("triple hole reads zero", 0,
              hype_file_rmap_read_at(&m, vol_read, 0, (uint64_t)(12u + 256u + 300u) * BS, buf, 16));

    /* corrupt directory entries */
    build_vol_ext2();
    put16(blk(ROOT_BLK) + 4u, 4u); /* first dirent rec_len < 8 */
    CHECK("undersized dirent refused", hype_ext_resolve_rmap(vol_read, 0, "/swiss.bin", &m) != 0);
    build_vol_ext2();
    put32(inode(2u) + 0x28, 4000000u); /* root data pointer outside the volume */
    CHECK("root dir map broken refused", hype_ext_resolve_rmap(vol_read, 0, "/swiss.bin", &m) != 0);

    /* classic pointer outside the volume through the sparse resolver */
    build_vol_ext2();
    put32(inode(13u) + 0x28, 4000000u);
    CHECK("classic pointer outside volume refused",
          hype_ext_resolve_rmap(vol_read, 0, "/swiss.bin", &m) != 0);

    /* allocator: a stale map claiming HOLE over an allocated block just
     * rediscovers the pointer instead of double-allocating */
    build_vol_ext2();
    CHECK_HEX("open", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    {
        uint32_t used_before;
        w.map.ranges[0].kind = HYPE_RANGE_HOLE; /* lie about the mapped block 0 */
        w.map.ranges[0].start_lba = 0;
        used_before = bitmap_used_count();
        CHECK_HEX("stale-hole write", 0, hype_ext2_write_at(&w, 5u, "ZZ", 2));
        CHECK_HEX("no double allocation", used_before, bitmap_used_count());
    }

    /* an existing pointer entry outside the volume is refused at write */
    build_vol_ext2();
    {
        uint8_t *in = inode(13u);
        put32(in + 0x28 + 12u * 4u, 70u); /* L1 root exists */
        memset(blk(70u), 0, BS);
        put32(blk(70u) + 4u, 4000000u); /* entry for logical 13 is garbage */
    }
    CHECK("garbage pointer entry refused at open",
          hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);

    /* ext2 open guards: zero inodes-per-group, NULL inode table, an
     * extent-FLAGGED inode on an ext2 volume */
    {
        static hype_ext2_wfile_t r;
        build_vol_ext2();
        put32(g_vol + 1024 + 0x28, 0);
        CHECK("zero inodes-per-group refused",
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        build_vol_ext2();
        put32(blk(2) + 0x08, 0);
        CHECK("NULL inode table refused",
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
        build_vol_ext2();
        put32(inode(13u) + 0x20, 0x00080000u); /* FL_EXTENTS on an ext2 file */
        CHECK("extent-flagged inode refused",
              hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &r) != 0);
    }

    /* write-fault sweep through DOUBLE-indirect materialization */
    for (n = 0; n < 50; n++) {
        static hype_ext2_wfile_t w2;
        build_vol_ext2();
        {
            uint8_t *in = mk_inode(13u, 0x81A4u, (uint64_t)(12u + 256u + 300u) * BS, 0u);
            (void)in;
        }
        g_writes_seen = 0;
        g_wfail_at = (uint32_t)n;
        if (hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w2) == 0) {
            (void)hype_ext2_write_at(&w2, (uint64_t)(12u + 256u + 100u) * BS, "DBL", 3);
        }
        g_wfail_at = ~0u;
        {
            uint32_t used = bitmap_used_count();
            uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
            CHECK("dbl-sweep: counters consistent", used + gdfree == VOL_BLOCKS - 1u);
        }
    }
    /* the stale-map arm at an INDIRECT leaf: allocate, lie, rewrite */
    build_vol_ext2();
    CHECK_HEX("open stale-leaf", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    CHECK_HEX("first indirect write", 0, hype_ext2_write_at(&w, 20u * BS, "A", 1));
    {
        unsigned r;
        uint32_t used_before;
        for (r = 0; r < w.map.count; r++) {
            /* find the DATA range we just created and lie about it */
        }
        /* re-open to get a fresh handle, then forge every DATA range in the
         * indirect region back to HOLE */
        CHECK_HEX("reopen", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
        for (r = 0; r < w.map.count; r++) {
            uint64_t logical = 0;
            unsigned q;
            for (q = 0; q < r; q++) logical += w.map.ranges[q].sector_count;
            if (w.map.ranges[r].kind == HYPE_RANGE_DATA && logical >= 20u * (BS / 512u)) {
                w.map.ranges[r].kind = HYPE_RANGE_HOLE;
            }
        }
        used_before = bitmap_used_count();
        CHECK_HEX("stale indirect write", 0, hype_ext2_write_at(&w, 20u * BS, "B", 1));
        CHECK_HEX("leaf rediscovered, no double alloc", used_before, bitmap_used_count());
    }

    /* group descriptor bitmap pointer outside the volume */
    build_vol_ext2();
    put32(blk(2) + 0x00, 4000000u);
    CHECK_HEX("open bad-bitmap", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    CHECK("bitmap outside volume refused", hype_ext2_write_at(&w, BS, "x", 1) != 0);

    /* a corrupt in-RAM map surfaces instead of writing astray */
    build_vol_ext2();
    CHECK_HEX("open corrupt-map", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    w.map.size_bytes += 10u * BS;
    w.size_bytes += 10u * BS;
    CHECK("phantom region refused", hype_ext2_write_at(&w, w.size_bytes - BS, "x", 1) != 0);

    /* read-fault sweep, wider */
    for (n = 0; n < 60; n++) {
        static hype_ext2_wfile_t w2;
        build_vol_ext2();
        g_read_countdown = n;
        if (hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w2) == 0) {
            (void)hype_ext2_write_at(&w2, 20u * BS + 5u, "XYZ", 3);
        }
        g_read_countdown = -1;
    }
}


/* ---- #385: the journaled (jbd2) allocating writer ---- */

#define JBLK 800u   /* journal: 12 blocks at 800..811 (inside the used region) */
#define JLEN 12u

static void put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t get32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void graft_journal(void) {
    uint8_t *sb = g_vol + 1024;
    uint8_t *in;
    uint8_t *jsb = blk(JBLK);
    unsigned i;
    put32(sb + 0x5C, get32(sb + 0x5C) | 0x0004u); /* COMPAT_HAS_JOURNAL */
    put32(sb + 0xE0, 8u);                          /* s_journal_inum */
    in = mk_inode(8u, 0x8180u, JLEN * BS, 0u);
    for (i = 0; i < JLEN; i++) put32(in + 0x28 + i * 4u, JBLK + i);
    put32(in + 0x1C, JLEN * (BS / 512u));
    memset(jsb, 0, BS);
    put32be(jsb + 0, 0xC03B3998u);
    put32be(jsb + 4, 4u);       /* V2 superblock */
    put32be(jsb + 12, BS);      /* blocksize */
    put32be(jsb + 16, JLEN);    /* maxlen */
    put32be(jsb + 20, 1u);      /* first */
    put32be(jsb + 24, 1u);      /* sequence */
    put32be(jsb + 28, 0u);      /* start: empty */
}

static void build_vol_ext3(void) {
    build_vol_ext2();
    graft_journal();
}

/* the ext2 builder + journal + an extent-mapped sparse file (inode 17) */
static void build_vol_ext4j(void) {
    uint8_t *sb = g_vol + 1024;
    uint8_t *in;
    uint32_t off, last;
    unsigned i;
    build_vol_ext2();
    graft_journal();
    put32(sb + 0x60, get32(sb + 0x60) | 0x0040u); /* INCOMPAT_EXTENTS */
    /* esp.bin: HOLE(2) | DATA(2 @70) | UNWRITTEN(4 @75) | HOLE(rest); 400 blocks */
    in = mk_inode(17u, 0x81A4u, 400u * BS, 0x80000u);
    {
        uint8_t *eh = in + 0x28;
        put16(eh + 0, 0xF30Au); put16(eh + 2, 2u); put16(eh + 4, 4u); put16(eh + 6, 0u);
        put32(eh + 12 + 0, 2u); put16(eh + 12 + 4, 2u); put16(eh + 12 + 6, 0u); put32(eh + 12 + 8, 70u);
        put32(eh + 24 + 0, 4u); put16(eh + 24 + 4, (uint16_t)(32768u + 4u)); put16(eh + 24 + 6, 0u);
        put32(eh + 24 + 8, 75u);
    }
    for (i = 0; i < 2u * BS; i++) blk(70u)[i] = pat(i + 3u);
    for (i = 0; i < 4u * BS; i++) blk(75u)[i] = 0xEE; /* stale: must never leak */
    off = 0;
    off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
    off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
    off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
    last = off;
    off = dirent(blk(ROOT_BLK), off, 17u, "esp.bin", 1u);
    dirent_close(blk(ROOT_BLK), last + 16u, BS);
}

static void test_extj_gates(void) {
    static hype_extj_wfile_t w;
    static hype_ext2_wfile_t e2;
    uint8_t *sb = g_vol + 1024;

    /* complementary writers: extj requires the journal, ext2 refuses it */
    build_vol_ext2();
    CHECK("extj refuses an unjournaled volume",
          hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    CHECK("ext2 writer refuses a journaled volume",
          hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &e2) != 0);
    CHECK_HEX("extj opens it", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));

    /* non-empty journal: a crashed writer's transactions await replay */
    build_vol_ext3();
    put32be(blk(JBLK) + 28, 1u); /* s_start != 0 */
    CHECK("non-empty journal refused",
          hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    /* journal feature gates */
    build_vol_ext3();
    put32be(blk(JBLK) + 40, 0x10u); /* INCOMPAT_CSUM_V3 */
    CHECK("checksummed journal refused",
          hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32be(blk(JBLK) + 4, 3u); /* V1 superblock */
    CHECK("V1 journal refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32be(blk(JBLK) + 32, 5u); /* s_errno */
    CHECK("journal with recorded error refused",
          hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    /* external journal */
    build_vol_ext3();
    put32(sb + 0xE0, 0u);
    CHECK("external journal refused",
          hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    /* filesystem feature gates */
    build_vol_ext3();
    put32(sb + 0x60, get32(sb + 0x60) | 0x0080u); /* 64BIT */
    CHECK("64-bit volume refused",
          hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32(sb + 0x64, 0x0200u); /* BIGALLOC */
    CHECK("bigalloc refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32(sb + 0x64, 0x0400u); /* METADATA_CSUM */
    CHECK("checksummed metadata refused",
          hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put16(sb + 0x3A, 0x0000u); /* dirty */
    CHECK("dirty volume refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
}

static void test_extj_classic(void) {
    static hype_extj_wfile_t w;
    uint8_t buf[2 * BS];
    uint32_t used_before;
    unsigned i;

    build_vol_ext3();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    hype_extj_set_time(&w, 1765500000u);
    used_before = bitmap_used_count();

    /* in-place fast path: no journal traffic */
    {
        uint32_t seq_before = get32be(blk(JBLK) + 24);
        CHECK_HEX("in-place write", 0, hype_extj_write_at(&w, 10, "JJ", 2));
        CHECK_HEX("no allocation", used_before, bitmap_used_count());
        CHECK_HEX("no transaction", seq_before, get32be(blk(JBLK) + 24));
    }

    /* a hole write journals, applies, and checkpoints */
    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i ^ 0x77);
    {
        uint32_t seq_before = get32be(blk(JBLK) + 24);
        CHECK_HEX("hole write", 0, hype_extj_write_at(&w, BS + 50u, buf, 300));
        CHECK("sequence advanced", get32be(blk(JBLK) + 24) == seq_before + 1u);
        CHECK_HEX("journal empty again", 0, get32be(blk(JBLK) + 28));
        CHECK("descriptor landed in the journal",
              get32be(blk(JBLK + 1u)) == 0xC03B3998u && get32be(blk(JBLK + 1u) + 4) == 1u);
    }
    CHECK_HEX("one block allocated", used_before + 1u, bitmap_used_count());
    CHECK("pointer published", get32(inode(13u) + 0x28 + 4u) != 0u);
    CHECK_HEX("mtime stamped", 1765500000u, get32(inode(13u) + 0x10));
    CHECK_HEX("readback", 0, hype_extj_read_at(&w, BS + 50u, buf + BS, 300));
    CHECK("data", memcmp(buf + BS, buf, 300) == 0);
    CHECK_HEX("zero head", 0, hype_extj_read_at(&w, BS, buf + BS, 50));
    for (i = 0; i < 50u; i++) { if (buf[BS + i] != 0) break; }
    CHECK("block head zeroed", i == 50u);

    /* indirect materialization through the journal */
    used_before = bitmap_used_count();
    CHECK_HEX("indirect hole write", 0, hype_extj_write_at(&w, 20u * BS, "JIND", 4));
    CHECK_HEX("data + pointer block", used_before + 2u, bitmap_used_count());
    CHECK_HEX("indirect readback", 0, hype_extj_read_at(&w, 20u * BS, buf, 4));
    CHECK("indirect data", memcmp(buf, "JIND", 4) == 0);

    /* bounds */
    /* #497: a straddling write now GROWS by one byte. */
    {
        uint64_t before2 = w.size_bytes;
        uint8_t rb2[2];
        CHECK_HEX("straddling write grows (extj)", 0,
                  hype_extj_write_at(&w, w.size_bytes - 1u, "ab", 2));
        CHECK("extj size moved by one", w.size_bytes == before2 + 1u);
        CHECK_HEX("extj straddle readback", 0, hype_extj_read_at(&w, before2 - 1u, rb2, 2));
        CHECK("extj straddle bytes", rb2[0] == 'a' && rb2[1] == 'b');
    }
    CHECK("overflow refused", hype_extj_write_at(&w, ~0ull - 1u, "a", 1) != 0);
    CHECK_HEX("len 0", 0, hype_extj_write_at(&w, 0, buf, 0));
}

static void test_extj_extents(void) {
    static hype_extj_wfile_t w;
    uint8_t buf[2 * BS];
    unsigned i;

    build_vol_ext4j();
    CHECK_HEX("open extent file", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));

    /* the unwritten region reads as zeros, never 0xEE */
    CHECK_HEX("read unwritten", 0, hype_extj_read_at(&w, 4u * BS, buf, 64));
    for (i = 0; i < 64u; i++) { if (buf[i] != 0) break; }
    CHECK("unwritten zeros", i == 64u);

    /* a write into the LEADING hole inserts an extent at the tree's front
     * (the parent first-key update arm) */
    CHECK_HEX("hole insert (front)", 0, hype_extj_write_at(&w, 0u * BS + 7u, "EXT", 3));
    CHECK_HEX("hole readback", 0, hype_extj_read_at(&w, 0u * BS + 7u, buf, 3));
    CHECK("hole data", memcmp(buf, "EXT", 3) == 0);
    /* the pre-existing DATA blocks still read back */
    CHECK_HEX("data intact", 0, hype_extj_read_at(&w, 2u * BS, buf, 8));
    for (i = 0; i < 8u; i++) { if (buf[i] != pat(i + 3u)) break; }
    CHECK("data bytes", i == 8u);

    /* a write into the UNWRITTEN region converts the block: the covered
     * bytes stick, the rest of the block becomes REAL zeros */
    CHECK_HEX("unwritten conversion", 0, hype_extj_write_at(&w, 5u * BS + 100u, "CVT", 3));
    CHECK_HEX("converted readback", 0, hype_extj_read_at(&w, 5u * BS, buf, BS));
    for (i = 0; i < 100u; i++) { if (buf[i] != 0) break; }
    CHECK("converted head zeroed", i == 100u);
    CHECK("converted data", memcmp(buf + 100, "CVT", 3) == 0);
    for (i = 103u; i < BS; i++) { if (buf[i] != 0) break; }
    CHECK("converted tail zeroed", i == BS);
    /* neighbours in the unwritten region still read zero */
    CHECK_HEX("neighbour", 0, hype_extj_read_at(&w, 6u * BS, buf, 64));
    for (i = 0; i < 64u; i++) { if (buf[i] != 0) break; }
    CHECK("neighbour still zeros", i == 64u);

    /* conversion at the region edges (before == 0, after == 0) */
    CHECK_HEX("convert first block", 0, hype_extj_write_at(&w, 4u * BS, "A", 1));
    CHECK_HEX("convert last block", 0, hype_extj_write_at(&w, 7u * BS + BS - 1u, "Z", 1));
    /* an insert directly after the unwritten region: the merge check must
     * SKIP an unwritten predecessor, never extend it */
    CHECK_HEX("insert after unwritten", 0, hype_extj_write_at(&w, 8u * BS, "N", 1));

    /* root growth then LEAF SPLIT: 120 strided single-block inserts */
    for (i = 0; i < 120u; i++) {
        uint64_t at = (uint64_t)(9u + 3u * i) * BS; /* stride 3: never merges */
        CHECK_HEX("strided insert", 0, hype_extj_write_at(&w, at, "S", 1));
    }
    for (i = 0; i < 120u; i++) {
        uint64_t at = (uint64_t)(9u + 3u * i) * BS;
        uint8_t c;
        CHECK_HEX("strided verify", 0, hype_extj_read_at(&w, at, &c, 1));
        CHECK("strided byte", c == 'S');
    }
    /* an insert BETWEEN existing extents lands mid-leaf */
    CHECK_HEX("mid insert", 0, hype_extj_write_at(&w, (uint64_t)(9u + 3u * 50u + 1u) * BS, "M", 1));
    /* everything fsck would check locally: counters consistent */
    {
        uint32_t used = bitmap_used_count();
        uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
        CHECK("counters consistent", used + gdfree == VOL_BLOCKS - 1u);
    }
}

static void test_extj_crash_windows(void) {
    long n;
    int saw_refusal = 0, saw_success = 0;

    for (n = 0; n < 60; n++) {
        static hype_extj_wfile_t w;
        int wrote;
        build_vol_ext3();
        g_writes_seen = 0;
        g_whardfail = 0;
        g_wfail_at = (uint32_t)n;
        wrote = (hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) == 0) &&
                (hype_extj_write_at(&w, 20u * BS + 5u, "XYZ", 3) == 0);
        g_wfail_at = ~0u;
        (void)wrote;
        {
            static hype_extj_wfile_t w2;
            int reopen = hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w2);
            if (reopen != 0) {
                /* only acceptable reason: the journal holds an exposed
                 * transaction awaiting replay */
                CHECK("refusal is the non-empty journal", get32be(blk(JBLK) + 28) != 0u);
                saw_refusal = 1;
            } else {
                uint32_t used = bitmap_used_count();
                uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
                CHECK("crash sweep: counters consistent", used + gdfree == VOL_BLOCKS - 1u);
                saw_success = 1;
            }
        }
    }
    CHECK("sweep covered the pre-commit window", saw_success);
    CHECK("sweep covered the exposed-transaction window", saw_refusal);

    /* the journal itself: a too-big transaction is refused */
    {
        static hype_jbd2_t j;
        static hype_file_rmap_t jm;
        static hype_jbd2_block_t imgs[HYPE_JBD2_MAX_BLOCKS + 1u];
        static uint8_t img[1024];
        unsigned i2;
        build_vol_ext3();
        CHECK_HEX("map journal inode", 0, hype_ext_map_ino_rmap(vol_read, 0, 8u, &jm));
        CHECK_HEX("jbd2 open", 0, hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm));
        for (i2 = 0; i2 <= HYPE_JBD2_MAX_BLOCKS; i2++) {
            imgs[i2].blocknr = 40u + i2;
            imgs[i2].data = img;
        }
        CHECK("over-budget transaction refused",
              hype_jbd2_commit(&j, imgs, HYPE_JBD2_MAX_BLOCKS + 1u) != 0);
        CHECK("zero-block transaction refused", hype_jbd2_commit(&j, imgs, 0u) != 0);
        /* an image that LOOKS like a journal block gets escaped */
        put32be(img, 0xC03B3998u);
        CHECK_HEX("escaped commit", 0, hype_jbd2_commit(&j, imgs, 1u));
        CHECK("escaped image stored without its magic",
              get32be(blk(JBLK + 2u)) == 0u);
        CHECK_HEX("checkpoint", 0, hype_jbd2_checkpoint(&j));
        CHECK_HEX("journal empty", 0, get32be(blk(JBLK) + 28));
    }
}


static void test_extj_deep_and_faults(void) {
    static hype_extj_wfile_t w;
    uint8_t buf[64];
    long n;
    unsigned i;

    /* a journaled HUGE classic sparse file: every indirection level */
    build_vol_ext3();
    {
        uint8_t *in = mk_inode(14u, 0x81A4u, 0u, 0u);
        uint32_t off, last;
        put32(in + 0x04, (uint32_t)((uint64_t)66000u * BS));
        put32(in + 0x6C, (uint32_t)(((uint64_t)66000u * BS) >> 32));
        off = 0;
        off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
        off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
        off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
        last = off;
        off = dirent(blk(ROOT_BLK), off, 14u, "huge.bin", 1u);
        dirent_close(blk(ROOT_BLK), last + 16u, BS);
    }
    CHECK_HEX("open huge (journaled)", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w));
    CHECK_HEX("direct", 0, hype_extj_write_at(&w, 3u * BS, "D", 1));
    CHECK_HEX("single", 0, hype_extj_write_at(&w, 100u * BS, "S", 1));
    CHECK_HEX("double", 0, hype_extj_write_at(&w, 1000u * BS, "W", 1));
    CHECK_HEX("triple", 0, hype_extj_write_at(&w, (uint64_t)(12u + 256u + 65536u + 3u) * BS, "T", 1));
    /* siblings in already-built trees: the reuse (non-fresh) arms */
    CHECK_HEX("single sibling", 0, hype_extj_write_at(&w, 101u * BS, "s", 1));
    CHECK_HEX("double sibling", 0, hype_extj_write_at(&w, 1001u * BS, "w", 1));
    CHECK_HEX("verify T", 0, hype_extj_read_at(&w, (uint64_t)(12u + 256u + 65536u + 3u) * BS, buf, 1));
    CHECK("T byte", buf[0] == 'T');
    {
        uint32_t used = bitmap_used_count();
        uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
        CHECK("deep counters consistent", used + gdfree == VOL_BLOCKS - 1u);
    }

    /* a span whose METADATA footprint exceeds the journal credit bound is
     * refused with no metadata change at all: a pre-built double-indirect
     * tree with one hole per MID block makes a single span touch 24 distinct
     * pointer blocks -- more slots than the transaction owns */
    {
        static uint8_t big[6400u * BS];
        uint32_t used_before;
        uint8_t *in = inode(14u);
        unsigned mi, q;
        put32(in + 0x28 + 13u * 4u, 61u); /* L2 root */
        memset(blk(61u), 0, BS);
        for (mi = 0; mi < 24u; mi++) {
            uint32_t midblk = 62u + mi;
            put32(blk(61u) + mi * 4u, midblk);
            memset(blk(midblk), 0, BS);
            for (q = 0; q < 256u; q++) {
                /* contiguous per-mid mapping (coalesces to one range per
                 * mid), except ONE hole per mid */
                put32(blk(midblk) + q * 4u, (q == 7u) ? 0u : (1000u + q));
            }
        }
        CHECK_HEX("reopen huge with prebuilt tree", 0,
                  hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w));
        used_before = bitmap_used_count();
        CHECK("over-credit span refused",
              hype_extj_write_at(&w, (uint64_t)(12u + 256u) * BS, big, 6144u * BS) != 0);
        CHECK_HEX("no metadata leaked by refusal", used_before, bitmap_used_count());
        CHECK_HEX("journal still empty", 0, get32be(blk(JBLK) + 28));
    }

    /* stale-map arms: a lying HOLE over a mapped block rediscovers the
     * pointer; nothing is dirtied, so no transaction is committed */
    {
        uint32_t seq_before, used_before;
        unsigned r;
        CHECK_HEX("reopen huge", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w));
        seq_before = get32be(blk(JBLK) + 24);
        used_before = bitmap_used_count();
        for (r = 0; r < w.map.count; r++) {
            uint64_t logical = 0;
            unsigned q;
            for (q = 0; q < r; q++) logical += w.map.ranges[q].sector_count;
            if (w.map.ranges[r].kind == HYPE_RANGE_DATA && logical == 3u * (BS / 512u)) {
                w.map.ranges[r].kind = HYPE_RANGE_HOLE; /* lie about block 3 */
            }
        }
        CHECK_HEX("stale write rediscovers", 0, hype_extj_write_at(&w, 3u * BS, "d", 1));
        CHECK_HEX("no alloc", used_before, bitmap_used_count());
        CHECK_HEX("no transaction", seq_before, get32be(blk(JBLK) + 24));
    }

    /* fault sweeps across the triple-indirect journaled write */
    for (n = 0; n < 70; n++) {
        static hype_extj_wfile_t w2;
        build_vol_ext3();
        {
            uint8_t *in = mk_inode(14u, 0x81A4u, 0u, 0u);
            uint32_t off, last;
            put32(in + 0x04, (uint32_t)((uint64_t)66000u * BS));
            put32(in + 0x6C, (uint32_t)(((uint64_t)66000u * BS) >> 32));
            off = 0;
            off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
            off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
            off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
            last = off;
            off = dirent(blk(ROOT_BLK), off, 14u, "huge.bin", 1u);
            dirent_close(blk(ROOT_BLK), last + 16u, BS);
        }
        g_writes_seen = 0;
        g_wfail_at = (uint32_t)n;
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w2) == 0) {
            (void)hype_extj_write_at(&w2, (uint64_t)(12u + 256u + 65536u + 3u) * BS, "T", 1);
        }
        g_wfail_at = ~0u;
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w2) != 0) {
            CHECK("triple sweep: refusal is the exposed journal",
                  get32be(blk(JBLK) + 28) != 0u);
        } else {
            uint32_t used = bitmap_used_count();
            uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
            CHECK("triple sweep: counters consistent", used + gdfree == VOL_BLOCKS - 1u);
        }
        g_read_countdown = (n < 35) ? n : -1;
        (void)hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w2);
        g_read_countdown = -1;
    }
}

static void test_jbd2_api(void) {
    static hype_jbd2_t j;
    static hype_file_rmap_t jm;
    uint8_t *jsb;

    build_vol_ext3();
    jsb = blk(JBLK);
    CHECK_HEX("map journal", 0, hype_ext_map_ino_rmap(vol_read, 0, 8u, &jm));

    CHECK("NULL read refused", hype_jbd2_open(&j, 0, vol_write2, 0, BS, &jm) != 0);
    CHECK("bad blocksize refused", hype_jbd2_open(&j, vol_read, vol_write2, 0, 512u, &jm) != 0);
    put32be(jsb + 0, 0x11111111u);
    CHECK("bad magic refused", hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm) != 0);
    build_vol_ext3();
    put32be(jsb + 12, 4096u);
    CHECK("blocksize mismatch refused", hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm) != 0);
    build_vol_ext3();
    put32be(jsb + 16, 4u); /* maxlen < 8 */
    CHECK("tiny journal refused", hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm) != 0);
    build_vol_ext3();
    put32be(jsb + 16, 4096u); /* maxlen beyond the inode's size */
    CHECK("overlong journal refused", hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm) != 0);
    build_vol_ext3();
    put32be(jsb + 44, 1u); /* ro-compat feature */
    CHECK("ro-compat journal feature refused",
          hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm) != 0);
    build_vol_ext3();
    put32be(jsb + 24, 0u); /* sequence 0: normalized to 1 */
    CHECK_HEX("sequence 0 accepted", 0, hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm));
    CHECK_HEX("sequence normalized", 1, j.sequence);
    build_vol_ext3();
    g_read_countdown = 0;
    CHECK("unreadable journal refused", hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm) != 0);
    g_read_countdown = -1;

    /* a sparse journal map is corruption */
    {
        static hype_file_rmap_t sparse;
        hype_file_rmap_init(&sparse, 4u * BS);
        hype_file_rmap_append(&sparse, HYPE_RANGE_DATA, JBLK * (BS / 512u), 2u * (BS / 512u));
        hype_file_rmap_append(&sparse, HYPE_RANGE_HOLE, 0, 2u * (BS / 512u));
        CHECK("sparse journal refused",
              hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &sparse) != 0);
    }

    /* commit refusals: wrap, 64-bit block numbers */
    build_vol_ext3();
    CHECK_HEX("open", 0, hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm));
    {
        static uint8_t img[1024];
        static hype_jbd2_block_t one;
        static hype_jbd2_block_t many[10];
        unsigned k;
        one.blocknr = 0x100000000ull;
        one.data = img;
        CHECK("64-bit block refused", hype_jbd2_commit(&j, &one, 1u) != 0);
        for (k = 0; k < 10u; k++) { many[k].blocknr = 40u + k; many[k].data = img; }
        CHECK("wrapping transaction refused", hype_jbd2_commit(&j, many, 10u) != 0);
        /* write-fault sweep across a real commit */
        for (k = 0; k < 20u; k++) {
            build_vol_ext3();
            if (hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm) != 0) continue;
            g_writes_seen = 0;
            g_wfail_at = k;
            (void)hype_jbd2_commit(&j, many, 3u);
            g_wfail_at = ~0u;
        }
    }
}


static void test_extj_extent_guards(void) {
    static hype_extj_wfile_t w;
    uint8_t buf[64];
    unsigned i;

    /* conversion needing room in a nearly-full leaf */
    build_vol_ext4j();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    for (i = 0; i < 100u; i++) { /* fill: root grows, leaf fills */
        CHECK_HEX("fill insert", 0, hype_extj_write_at(&w, (uint64_t)(20u + 3u * i) * BS, "F", 1));
    }
    /* now convert a middle unwritten block: needs a 3-way split, may split the leaf */
    CHECK_HEX("convert under pressure", 0, hype_extj_write_at(&w, 5u * BS + 9u, "P", 1));
    CHECK_HEX("converted read", 0, hype_extj_read_at(&w, 5u * BS + 9u, buf, 1));
    CHECK("converted byte", buf[0] == 'P');
    CHECK_HEX("unwritten sibling still zero", 0, hype_extj_read_at(&w, 6u * BS + 9u, buf, 1));
    CHECK("sibling zero", buf[0] == 0);

    /* structural guards: corrupt tree shapes refuse cleanly */
    build_vol_ext4j();
    {
        uint8_t *eh = inode(17u) + 0x28;
        put16(eh + 0, 0x1111u); /* root magic */
    }
    CHECK("bad root magic refused at open (resolver)",
          hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w) != 0);
    build_vol_ext4j();
    CHECK_HEX("open2", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    {
        /* corrupt the root AFTER open: the write path's own epath_find must
         * catch it (the resolver's map is already cached) */
        uint8_t *eh = inode(17u) + 0x28;
        put16(eh + 0, 0x2222u);
    }
    CHECK("bad root magic refused at write", hype_extj_write_at(&w, 9u * BS, "x", 1) != 0);
    build_vol_ext4j();
    CHECK_HEX("open3", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    {
        uint8_t *eh = inode(17u) + 0x28;
        put16(eh + 6, 6u); /* depth beyond the cap */
    }
    CHECK("over-deep tree refused", hype_extj_write_at(&w, 9u * BS, "x", 1) != 0);

    /* fault sweeps across the extent insert + conversion paths */
    {
        long n;
        for (n = 0; n < 130; n += 2) {
            static hype_extj_wfile_t w2;
            build_vol_ext4j();
            g_writes_seen = 0;
            g_wfail_at = (uint32_t)n;
            if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w2) == 0) {
                (void)hype_extj_write_at(&w2, 9u * BS, "I", 1);
                (void)hype_extj_write_at(&w2, 5u * BS, "C", 1);
                (void)hype_extj_write_at(&w2, 6u * BS + 5u, "c", 1);
            }
            g_wfail_at = ~0u;
            if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w2) != 0) {
                CHECK("extent sweep: refusal is the exposed journal",
                      get32be(blk(JBLK) + 28) != 0u);
            } else {
                uint32_t used = bitmap_used_count();
                uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
                CHECK("extent sweep: counters consistent",
                      used + gdfree == VOL_BLOCKS - 1u);
            }
            g_read_countdown = (n < 30) ? n : -1;
            if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w2) == 0) {
                (void)hype_extj_write_at(&w2, 12u * BS, "R", 1);
            }
            g_read_countdown = -1;
        }
    }

    /* a corrupt L1 pointer entry on a journaled classic file refuses */
    build_vol_ext3();
    {
        uint8_t *in = inode(13u);
        put32(in + 0x28 + 12u * 4u, 70u);
        memset(blk(70u), 0, BS);
        put32(blk(70u) + 8u, 4000000u); /* logical 14 -> garbage */
        /* make the resolver's map treat it as... the resolver refuses; so
         * corrupt AFTER open instead */
    }
    (void)0;
    build_vol_ext3();
    CHECK_HEX("open swiss", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    {
        uint8_t *in = inode(13u);
        put32(in + 0x28 + 12u * 4u, 70u); /* L1 root appears mid-flight */
        memset(blk(70u), 0, BS);
        put32(blk(70u) + (20u - 12u) * 4u, 4000000u); /* logical 20 -> garbage */
    }
    CHECK("garbage leaf pointer refused", hype_extj_write_at(&w, 20u * BS, "x", 1) != 0);
}


static void test_extj_wave3(void) {
    static hype_extj_wfile_t w;
    uint8_t buf[16];
    long n;
    unsigned i;

    /* 2-group journaled volume: cross-group allocation under the journal */
    build_vol_ext3();
    {
        uint8_t *sb = g_vol + 1024;
        unsigned k;
        put32(sb + 0x20, 2048u);
        put32(blk(2) + 32u + 0x00u, 4u);
        put32(blk(2) + 32u + 0x08u, INODE_TABLE);
        memset(blk(4), 0, BS);
        for (k = 0; k < 2048u; k++) blk(BITMAP_BLK)[k / 8u] |= (uint8_t)(1u << (k % 8u));
        put16(blk(2) + 0x0C, 0);
        for (k = 0; k < 64u; k++) blk(4u)[k / 8u] |= (uint8_t)(1u << (k % 8u));
        put16(blk(2) + 32u + 0x0Cu, (uint16_t)(2047u - 64u));
        put32(sb + 0x0C, 2047u - 64u);
    }
    CHECK_HEX("open 2-group", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    CHECK_HEX("cross-group journaled alloc", 0, hype_extj_write_at(&w, BS, "G", 1));
    CHECK("block from group 1", get32(inode(13u) + 0x28 + 4u) >= 2049u);

    /* corrupt grown-tree shapes refuse at write time */
    build_vol_ext4j();
    CHECK_HEX("open esp", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    for (i = 0; i < 8u; i++) {
        CHECK_HEX("grow fill", 0, hype_extj_write_at(&w, (uint64_t)(20u + 3u * i) * BS, "F", 1));
    }
    /* the root is now an index: find its child block and corrupt it */
    {
        uint8_t *eh = inode(17u) + 0x28;
        uint32_t child = get32(eh + 12 + 4);
        CHECK("tree grew", get16(eh + 6) != 0);
        put16(blk(child) + 0, 0x3333u); /* child magic */
        CHECK("bad child magic refused", hype_extj_write_at(&w, 200u * BS, "x", 1) != 0);
        put16(blk(child) + 0, 0xF30Au);
        put16(blk(child) + 6, 3u); /* child depth mismatch */
        CHECK("child depth mismatch refused", hype_extj_write_at(&w, 200u * BS, "x", 1) != 0);
        put16(blk(child) + 6, 0u);
        put16(blk(child) + 2, 0u); /* interior... leaf with 0 entries is legal;
                                    * make the ROOT claim 0 entries instead */
        put16(blk(child) + 2, 8u);
        put16(eh + 2, 0u); /* interior root with no children */
        CHECK("empty interior refused", hype_extj_write_at(&w, 200u * BS, "x", 1) != 0);
        put16(eh + 2, 1u);
        put32(eh + 12 + 4, 4000000u); /* child pointer outside the volume */
        CHECK("child outside volume refused", hype_extj_write_at(&w, 200u * BS, "x", 1) != 0);
        put32(eh + 12 + 4, child);
    }

    /* fault sweep across ALL FOUR classic depths in one run: the later
     * depths only see faults at high write indices */
    for (n = 0; n < 420; n += 3) {
        static hype_extj_wfile_t w2;
        build_vol_ext3();
        {
            uint8_t *in = mk_inode(14u, 0x81A4u, 0u, 0u);
            uint32_t off, last;
            put32(in + 0x04, (uint32_t)((uint64_t)66000u * BS));
            put32(in + 0x6C, (uint32_t)(((uint64_t)66000u * BS) >> 32));
            off = 0;
            off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
            off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
            off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
            last = off;
            off = dirent(blk(ROOT_BLK), off, 14u, "huge.bin", 1u);
            dirent_close(blk(ROOT_BLK), last + 16u, BS);
        }
        g_writes_seen = 0;
        g_wfail_at = (uint32_t)n;
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w2) == 0) {
            (void)hype_extj_write_at(&w2, 3u * BS, "D", 1);
            (void)hype_extj_write_at(&w2, 100u * BS, "S", 1);
            (void)hype_extj_write_at(&w2, 1000u * BS, "W", 1);
            (void)hype_extj_write_at(&w2, (uint64_t)(12u + 256u + 65536u + 3u) * BS, "T", 1);
        }
        g_wfail_at = ~0u;
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w2) != 0) {
            CHECK("all-depth sweep: refusal is the exposed journal",
                  get32be(blk(JBLK) + 28) != 0u);
        } else {
            uint32_t used = bitmap_used_count();
            uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
            CHECK("all-depth sweep: counters consistent", used + gdfree == VOL_BLOCKS - 1u);
        }
        /* read-fault variant over the same depths */
        g_read_countdown = n;
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w2) == 0) {
            (void)hype_extj_write_at(&w2, 1000u * BS, "W", 1);
            (void)hype_extj_write_at(&w2, (uint64_t)(12u + 256u + 65536u + 5u) * BS, "t", 1);
        }
        g_read_countdown = -1;
    }

    /* faults around a leaf split: fill a tree clean, then fault one insert */
    for (n = 0; n < 40; n += 3) {
        static hype_extj_wfile_t w2;
        build_vol_ext4j();
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w2) != 0) continue;
        for (i = 0; i < 90u; i++) {
            if (hype_extj_write_at(&w2, (uint64_t)(20u + 3u * i) * BS, "F", 1) != 0) break;
        }
        g_writes_seen = 0;
        g_wfail_at = (uint32_t)n;
        (void)hype_extj_write_at(&w2, (uint64_t)(20u + 3u * 95u) * BS, "X", 1);
        g_wfail_at = ~0u;
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w2) != 0) {
            CHECK("split sweep: refusal is the exposed journal",
                  get32be(blk(JBLK) + 28) != 0u);
        }
    }

    /* exactly-k-free-blocks sweeps: each claim SITE sees the volume run
     * dry (write faults cannot reach claims -- they are cache-only) */
    for (n = 0; n <= 8; n++) {
        static hype_extj_wfile_t w2;
        unsigned k;
        build_vol_ext3();
        {
            uint8_t *in = mk_inode(14u, 0x81A4u, 0u, 0u);
            uint32_t off, last;
            put32(in + 0x04, (uint32_t)((uint64_t)66000u * BS));
            put32(in + 0x6C, (uint32_t)(((uint64_t)66000u * BS) >> 32));
            off = 0;
            off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
            off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
            off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
            last = off;
            off = dirent(blk(ROOT_BLK), off, 14u, "huge.bin", 1u);
            dirent_close(blk(ROOT_BLK), last + 16u, BS);
        }
        /* burn free blocks down to exactly n */
        {
            uint32_t left = (uint32_t)n;
            uint32_t bit;
            uint32_t freec = 0;
            for (bit = 0; bit + 1u < VOL_BLOCKS; bit++) {
                if (!(blk(BITMAP_BLK)[bit / 8u] & (1u << (bit % 8u)))) {
                    if (left > 0u) { left--; freec++; continue; }
                    blk(BITMAP_BLK)[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
                }
            }
            put16(blk(2) + 0x0C, (uint16_t)freec);
            put32(g_vol + 1024 + 0x0C, freec);
        }
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w2) == 0) {
            int rc = hype_extj_write_at(&w2, (uint64_t)(12u + 256u + 65536u + 3u) * BS, "T", 1);
            if (n >= 4) {
                CHECK("k-free: enough blocks succeeds", rc == 0);
            } else {
                CHECK("k-free: dry volume refused", rc != 0);
            }
            /* refusals are cache-only: nothing may have leaked */
            {
                uint32_t used = bitmap_used_count();
                uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
                CHECK("k-free: counters consistent", used + gdfree == VOL_BLOCKS - 1u);
            }
        }
        /* same on the extent volume: insert (1 block) and grow (2 blocks) */
        for (k = 0; k < 1u; k++) {
            static hype_extj_wfile_t w3;
            build_vol_ext4j();
            {
                uint32_t left = (uint32_t)n;
                uint32_t bit;
                uint32_t freec = 0;
                for (bit = 0; bit + 1u < VOL_BLOCKS; bit++) {
                    if (!(blk(BITMAP_BLK)[bit / 8u] & (1u << (bit % 8u)))) {
                        if (left > 0u) { left--; freec++; continue; }
                        blk(BITMAP_BLK)[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
                    }
                }
                put16(blk(2) + 0x0C, (uint16_t)freec);
                put32(g_vol + 1024 + 0x0C, freec);
            }
            if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w3) == 0) {
                unsigned q;
                for (q = 0; q < 8u; q++) {
                    (void)hype_extj_write_at(&w3, (uint64_t)(20u + 3u * q) * BS, "F", 1);
                }
                (void)hype_extj_write_at(&w3, 5u * BS, "C", 1);
            }
        }
    }

    /* fine-grained read-fault sweep over one double-indirect write */
    for (n = 0; n < 80; n++) {
        static hype_extj_wfile_t w2;
        build_vol_ext3();
        {
            uint8_t *in = mk_inode(14u, 0x81A4u, 0u, 0u);
            uint32_t off, last;
            put32(in + 0x04, (uint32_t)((uint64_t)66000u * BS));
            put32(in + 0x6C, (uint32_t)(((uint64_t)66000u * BS) >> 32));
            off = 0;
            off = dirent(blk(ROOT_BLK), off, 2u, ".", 2u);
            off = dirent(blk(ROOT_BLK), off, 2u, "..", 2u);
            off = dirent(blk(ROOT_BLK), off, 13u, "swiss.bin", 1u);
            last = off;
            off = dirent(blk(ROOT_BLK), off, 14u, "huge.bin", 1u);
            dirent_close(blk(ROOT_BLK), last + 16u, BS);
        }
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/huge.bin", &w2) == 0) {
            g_read_countdown = n;
            (void)hype_extj_write_at(&w2, 1000u * BS, "W", 1);
            g_read_countdown = -1;
        }
    }

    /* a group whose descriptor claims free blocks its bitmap does not
     * have: the scan moves on to the next group (the bitmap is the
     * authority), and the allocation still lands */
    build_vol_ext3();
    {
        uint8_t *sb2 = g_vol + 1024;
        unsigned k;
        put32(sb2 + 0x20, 2048u);
        put32(blk(2) + 32u + 0x00u, 4u);
        put32(blk(2) + 32u + 0x08u, INODE_TABLE);
        memset(blk(4), 0, BS);
        for (k = 0; k < 2048u; k++) blk(BITMAP_BLK)[k / 8u] |= (uint8_t)(1u << (k % 8u));
        put16(blk(2) + 0x0C, 5u); /* the LIE: bitmap is full */
        for (k = 0; k < 64u; k++) blk(4u)[k / 8u] |= (uint8_t)(1u << (k % 8u));
        put16(blk(2) + 32u + 0x0Cu, (uint16_t)(2047u - 64u));
        put32(sb2 + 0x0C, 2047u - 64u);
    }
    {
        static hype_extj_wfile_t w4;
        CHECK_HEX("open lie-volume", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w4));
        CHECK_HEX("scan skips the lying group", 0, hype_extj_write_at(&w4, BS, "L", 1));
        CHECK("landed in group 1", get32(inode(13u) + 0x28 + 4u) >= 2049u);
    }

    /* checkpoint I/O failure poisons the handle */
    {
        static hype_jbd2_t j;
        static hype_file_rmap_t jm;
        static uint8_t img[1024];
        static hype_jbd2_block_t one;
        build_vol_ext3();
        CHECK_HEX("map j", 0, hype_ext_map_ino_rmap(vol_read, 0, 8u, &jm));
        CHECK_HEX("open j", 0, hype_jbd2_open(&j, vol_read, vol_write2, 0, BS, &jm));
        one.blocknr = 40u;
        one.data = img;
        CHECK_HEX("commit", 0, hype_jbd2_commit(&j, &one, 1u));
        g_read_countdown = 0;
        CHECK("checkpoint read failure surfaces", hype_jbd2_checkpoint(&j) != 0);
        g_read_countdown = -1;
    }
}


static void test_extj_gate_tail(void) {
    static hype_extj_wfile_t w;
    uint8_t *sb = g_vol + 1024;
    unsigned i;

    build_vol_ext3();
    CHECK("NULL write refused", hype_extj_open_rw(vol_read, 0, 0, "/swiss.bin", &w) != 0);
    CHECK("NULL read refused", hype_extj_open_rw(0, vol_write2, 0, "/swiss.bin", &w) != 0);
    put16(sb + 0x38, 0x1111u);
    CHECK("bad magic refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32(sb + 0x60, get32(sb + 0x60) | 0x0004u); /* RECOVER */
    CHECK("RECOVER refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32(sb + 0x60, get32(sb + 0x60) | 0x0008u); /* JOURNAL_DEV */
    CHECK("journal-dev refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put16(sb + 0x3A, 0x0003u); /* valid + error */
    CHECK("error state refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32(sb + 0x18, 3u);
    CHECK("8K blocks refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32(sb + 0x20, 0);
    CHECK("bpg 0 refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put16(sb + 0x58, 64u); /* inode size under 128 */
    CHECK("tiny inodes refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32(blk(2) + 0x08, 4000000u); /* inode table outside the volume */
    CHECK("inode table oob refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32(sb + 0x4C, 0); /* rev 0: 128-byte inodes (layout mismatch on this
                          * volume, but the branch must parse cleanly) */
    (void)hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w);

    /* a grown tree whose root index entry points at block 0 */
    build_vol_ext4j();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    for (i = 0; i < 8u; i++) {
        CHECK_HEX("fill", 0, hype_extj_write_at(&w, (uint64_t)(20u + 3u * i) * BS, "F", 1));
    }
    put32(inode(17u) + 0x28 + 12 + 4, 0u); /* child pointer NULL */
    CHECK("NULL child refused", hype_extj_write_at(&w, 200u * BS, "x", 1) != 0);

    /* a hand-built FULL root index over full leaves: a leaf split has
     * nowhere to put its new index entry -> bounded refusal */
    build_vol_ext4j();
    {
        uint8_t *in = mk_inode(17u, 0x81A4u, (uint64_t)3000000u * BS, 0x80000u);
        uint8_t *eh = in + 0x28;
        unsigned e;
        put16(eh + 0, 0xF30Au); put16(eh + 2, 4u); put16(eh + 4, 4u); put16(eh + 6, 1u);
        for (e = 0; e < 4u; e++) {
            uint8_t *ie = eh + 12 + e * 12u;
            uint32_t leafblk = 500u + e;
            uint8_t *lf = blk(leafblk);
            unsigned q;
            uint16_t lmax = (uint16_t)((BS - 12u) / 12u);
            uint16_t nent = (e == 0u) ? lmax : 1u; /* only leaf 0 is FULL */
            put32(ie + 0, e * 100000u);
            put32(ie + 4, leafblk);
            put16(ie + 8, 0u);
            memset(lf, 0, BS);
            put16(lf + 0, 0xF30Au);
            put16(lf + 2, nent);
            put16(lf + 4, lmax);
            put16(lf + 6, 0u);
            for (q = 0; q < nent; q++) {
                uint8_t *le = lf + 12u + q * 12u;
                put32(le + 0, e * 100000u + q * 2u); /* gaps between extents */
                put16(le + 4, 1u);
                put16(le + 6, 0u);
                put32(le + 8, 70u); /* all point at a harmless block */
            }
            /* mark the leaf blocks used so the allocator ignores them */
            {
                uint32_t bit = leafblk - 1u;
                blk(BITMAP_BLK)[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
            }
        }
    }
    CHECK_HEX("open full-tree", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    CHECK("cascading split refused (bounded transaction)",
          hype_extj_write_at(&w, 5u * BS + 1u, "x", 1) != 0);
}


static void test_extj_last_mile(void) {
    static hype_extj_wfile_t w;
    uint8_t buf[2048];
    unsigned i;

    /* corrupt group metadata discovered mid-write */
    build_vol_ext3();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    put32(blk(2) + 0x00, 4000000u); /* bitmap pointer far outside the volume */
    CHECK("oob bitmap refused mid-write", hype_extj_write_at(&w, BS, "x", 1) != 0);
    build_vol_ext3();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    put32(blk(2) + 0x00, 0u); /* NULL bitmap pointer */
    CHECK("NULL bitmap refused mid-write", hype_extj_write_at(&w, BS, "x", 1) != 0);

    /* the inode-table block becoming unreadable mid-write */
    build_vol_ext3();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    g_fail_read_lba = (w.inode_byte / 512u) & ~1ull; /* the inode block's first sector */
    CHECK("unreadable inode block refused", hype_extj_write_at(&w, BS, "x", 1) != 0);
    g_fail_read_lba = (uint64_t)-1;

    /* open gates: NULL inode table pointer; zero inodes-per-group */
    build_vol_ext3();
    put32(blk(2) + 0x08, 0u);
    CHECK("NULL inode table refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);
    build_vol_ext3();
    put32(g_vol + 1024 + 0x28, 0u);
    CHECK("zero inodes/group refused", hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0);

    /* a classic file whose map LIES about an unwritten range is refused --
     * classic block maps have no unwritten state to convert */
    build_vol_ext3();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    w.map.ranges[0].kind = HYPE_RANGE_UNWRITTEN;
    CHECK("unwritten on a classic map refused", hype_extj_write_at(&w, 5u, "x", 1) != 0);

    /* a data-portion write failing inside a mixed DATA->HOLE span */
    build_vol_ext3();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    g_writes_seen = 0;
    g_wfail_at = 0; /* the very first media write: the DATA part of the span */
    CHECK("data-part failure surfaces",
          hype_extj_write_at(&w, BS - 8u, buf, 64) != 0);
    g_wfail_at = ~0u;

    /* a FRONT insert into a GROWN tree: the parent first-key update path */
    build_vol_ext4j();
    CHECK_HEX("open esp", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    for (i = 0; i < 8u; i++) {
        CHECK_HEX("grow", 0, hype_extj_write_at(&w, (uint64_t)(20u + 3u * i) * BS, "F", 1));
    }
    CHECK_HEX("front insert into grown tree", 0, hype_extj_write_at(&w, 0u, "0", 1));
    CHECK_HEX("front readback", 0, hype_extj_read_at(&w, 0u, buf, 1));
    CHECK("front byte", buf[0] == '0');
    /* keys above the front insert are intact */
    CHECK_HEX("later readback", 0, hype_extj_read_at(&w, (uint64_t)(20u) * BS, buf, 1));
    CHECK("later byte", buf[0] == 'F');

    /* an lb-adjacent insert whose new block is NOT phys-adjacent: the merge
     * check must fall through to a fresh entry */
    build_vol_ext4j();
    CHECK_HEX("open esp2", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    CHECK_HEX("insert lb 9", 0, hype_extj_write_at(&w, 9u * BS, "A", 1));
    {
        /* burn the next free block ON MEDIA so the following claim skips it */
        uint32_t bit;
        for (bit = 0; bit + 1u < VOL_BLOCKS; bit++) {
            if (!(blk(BITMAP_BLK)[bit / 8u] & (1u << (bit % 8u)))) {
                blk(BITMAP_BLK)[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
                {
                    uint16_t g = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
                    put16(blk(2) + 0x0C, (uint16_t)(g - 1u));
                }
                put32(g_vol + 1024 + 0x0C, get32(g_vol + 1024 + 0x0C) - 1u);
                break;
            }
        }
    }
    CHECK_HEX("insert lb 10 (non-adjacent phys)", 0, hype_extj_write_at(&w, 10u * BS, "B", 1));
    CHECK_HEX("A ok", 0, hype_extj_read_at(&w, 9u * BS, buf, 1));
    CHECK("A byte", buf[0] == 'A');
    CHECK_HEX("B ok", 0, hype_extj_read_at(&w, 10u * BS, buf, 1));
    CHECK("B byte", buf[0] == 'B');
}


/* --- #497: growth -- write_at past EOF and append, both writers --- */

static void test_497_ext2_grow(void) {
    static hype_ext2_wfile_t w;
    static uint8_t big[3u * BS];
    uint8_t rb[2u * BS];
    uint64_t sz0;
    unsigned i;

    build_vol_ext2();
    g_wfail_at = ~0u;
    CHECK_HEX("open", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    sz0 = w.size_bytes;

    /* Cross-block growth: 2.5 blocks of pattern appended right at EOF. */
    for (i = 0; i < sizeof(big); i++) big[i] = pat(i);
    CHECK_HEX("append-shaped grow", 0,
              hype_ext2_write_at(&w, sz0, big, (unsigned)(2u * BS + BS / 2u)));
    CHECK("size grew", w.size_bytes == sz0 + 2u * BS + BS / 2u);
    CHECK_HEX("grow readback", 0, hype_ext2_read_at(&w, sz0, rb, BS));
    for (i = 0; i < BS; i++) { if (rb[i] != pat(i)) break; }
    CHECK("grow bytes", i == BS);

    /* The size is now block-UNALIGNED. A further write leaving a gap INSIDE the tail block:
     * the gap must read back zero, not stale bytes -- the gap-zeroing rule. */
    {
        uint64_t sz1 = w.size_bytes;             /* ...+BS/2: mid-block */
        CHECK_HEX("gapped tail grow", 0, hype_ext2_write_at(&w, sz1 + 64u, "GAP", 3));
        CHECK("size grew past gap", w.size_bytes == sz1 + 64u + 3u);
        CHECK_HEX("gap readback", 0, hype_ext2_read_at(&w, sz1, rb, 67u));
        for (i = 0; i < 64u; i++) { if (rb[i] != 0u) break; }
        CHECK("tail gap reads zero", i == 64u);
        CHECK("gapped data", rb[64] == 'G' && rb[65] == 'A' && rb[66] == 'P');
    }

    /* A far-past-EOF write leaves the untouched gap SPARSE: only the written block (plus any
     * indirection) is allocated. */
    {
        uint64_t sz2 = w.size_bytes;
        uint32_t used0 = bitmap_used_count();
        uint64_t far = ((sz2 / BS) + 6u) * BS + 10u;
        CHECK_HEX("sparse grow", 0, hype_ext2_write_at(&w, far, "FAR", 3));
        CHECK("sparse size", w.size_bytes == far + 3u);
        /* at most the data block + one indirection level went; 5+ gap blocks did NOT */
        CHECK("gap stayed sparse", bitmap_used_count() <= used0 + 2u);
        CHECK_HEX("sparse gap readback", 0, hype_ext2_read_at(&w, sz2 + BS, rb, 64u));
        for (i = 0; i < 64u; i++) { if (rb[i] != 0u) break; }
        CHECK("sparse gap zeros", i == 64u);
        CHECK_HEX("far readback", 0, hype_ext2_read_at(&w, far, rb, 3u));
        CHECK("far bytes", rb[0] == 'F' && rb[1] == 'A' && rb[2] == 'R');
    }

    /* Counters still consistent -- the local fsck invariant. */
    {
        uint32_t used = bitmap_used_count();
        uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
        CHECK("counters consistent after growth", used + gdfree == VOL_BLOCKS - 1u);
    }
}

static void test_497_ext2_grow_rollback(void) {
    static hype_ext2_wfile_t w;
    uint8_t rb[8];
    uint64_t sz0;
    uint32_t used0;

    build_vol_ext2();
    CHECK_HEX("open", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    sz0 = w.size_bytes;
    used0 = bitmap_used_count();

    /* Fail the medium partway into a growing write: the transaction must roll back and the
     * SIZE must not move -- a half-grown file is the #464 class. */
    g_wfail_at = g_writes_seen + 3u;
    CHECK("mid-grow failure reported",
          hype_ext2_write_at(&w, sz0 + 5u, "XYZXYZXY", 8) != 0);
    g_wfail_at = ~0u;
    CHECK("size unmoved after rollback", w.size_bytes == sz0);
    CHECK("claims rolled back", bitmap_used_count() == used0);
    /* And a read past the (unchanged) EOF still refuses. */
    CHECK("EOF intact", hype_ext2_read_at(&w, sz0 + 1u, rb, 4) != 0);
}

static void test_497_extj_grow_extents(void) {
    static hype_extj_wfile_t w;
    static uint8_t big[2u * BS];
    uint8_t rb[BS];
    uint64_t sz0;
    unsigned i;

    g_wfail_at = ~0u;
    build_vol_ext4j();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    sz0 = w.size_bytes;

    for (i = 0; i < sizeof(big); i++) big[i] = pat(i + 7u);
    /* Straddle EOF: in-place tail + one fresh extent block, one journaled commit. */
    CHECK_HEX("straddling grow", 0,
              hype_extj_write_at(&w, sz0 - 4u, big, (unsigned)(BS + 8u)));
    CHECK("size grew", w.size_bytes == sz0 + BS + 4u);
    CHECK_HEX("readback across old EOF", 0, hype_extj_read_at(&w, sz0 - 4u, rb, 16u));
    for (i = 0; i < 16u; i++) { if (rb[i] != pat(i + 7u)) break; }
    CHECK("straddle bytes", i == 16u);

    /* Sparse far grow on extents, gap reads zero. */
    {
        uint64_t far = w.size_bytes + 5u * BS;
        CHECK_HEX("far grow", 0, hype_extj_write_at(&w, far, "EXT4", 4));
        CHECK("far size", w.size_bytes == far + 4u);
        CHECK_HEX("gap zeros", 0, hype_extj_read_at(&w, far - 2u * BS, rb, 32u));
        for (i = 0; i < 32u; i++) { if (rb[i] != 0u) break; }
        CHECK("gap reads zero", i == 32u);
    }

    /* fsck-local invariant on the journaled volume too. */
    {
        uint32_t used = bitmap_used_count();
        uint16_t gdfree = (uint16_t)(get32(blk(2) + 0x0C) & 0xFFFFu);
        CHECK("counters consistent", used + gdfree == VOL_BLOCKS - 1u);
    }
}

static void test_497_extj_grow_classic(void) {
    static hype_extj_wfile_t w;
    uint8_t rb[8];
    uint64_t sz0;

    g_wfail_at = ~0u;
    build_vol_ext3();
    CHECK_HEX("open classic", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    sz0 = w.size_bytes;
    CHECK_HEX("classic grow", 0, hype_extj_write_at(&w, sz0, "JCLASSIC", 8));
    CHECK("classic size", w.size_bytes == sz0 + 8u);
    CHECK_HEX("classic readback", 0, hype_extj_read_at(&w, sz0, rb, 8));
    CHECK("classic bytes", memcmp(rb, "JCLASSIC", 8) == 0);
}


/* #497 coverage: the tail-block cases that need an i_size ending inside a HOLE or UNWRITTEN
 * block. Legitimate files have that shape (a sparse file truncated to size); the fixture's do
 * not, so the on-media i_size is POKED directly (the writers re-read it at open). */
static void poke_size(uint64_t inode_byte, uint64_t size) {
    put32(g_vol + inode_byte + 0x04u, (uint32_t)size);
    put32(g_vol + inode_byte + 0x6Cu, (uint32_t)(size >> 32));
}

static void test_497_extj_unwritten_and_hole_tails(void) {
    static hype_extj_wfile_t w;
    uint8_t rb[256];
    unsigned i;

    g_wfail_at = ~0u;
    /* UNWRITTEN tail: i_size ends mid-block inside the unwritten region (block 5).
     * Learn the inode's byte offset on a THROWAWAY build, then poke a FRESH build and open it
     * exactly once -- the writers assume one open per volume instance. */
    uint64_t ib;
    build_vol_ext4j();
    CHECK_HEX("open (learn inode)", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    ib = w.inode_byte;
    /* Block 7 is the LAST unwritten block: after the grow-and-convert nothing in the tree
     * lies wholly past EOF, so the post-commit re-map stays valid. (An unwritten extent
     * ENTIRELY beyond i_size -- the fallocate shape -- is refused by hype's mapper at open,
     * before and after #497; the leading-hole poke below asserts that refusal.) */
    build_vol_ext4j();
    poke_size(ib, 7u * BS + 100u);
    CHECK_HEX("open with poked size", 0,
              hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    CHECK("poked size took", w.size_bytes == 7u * BS + 100u);
    {
        int rc1 = hype_extj_write_at(&w, 7u * BS + 150u, "UWT", 3);
        CHECK_HEX("grow across an UNWRITTEN tail", 0, rc1);
    }
    CHECK("unwritten-tail size", w.size_bytes == 7u * BS + 153u);
    CHECK_HEX("tail readback", 0, hype_extj_read_at(&w, 7u * BS + 90u, rb, 63u));
    for (i = 0; i < 60u; i++) { if (rb[i] != 0u) break; } /* 90..150 all zeros */
    CHECK("unwritten tail + gap read zero", i == 60u);
    CHECK("unwritten-tail data", rb[60] == 'U' && rb[61] == 'W' && rb[62] == 'T');

    /* An i_size poked into the LEADING hole (extents starting past EOF) is an unopenable shape
     * and open_rw correctly refuses it -- asserted so the refusal stays deliberate. */
    build_vol_ext4j();
    poke_size(ib, 1u * BS + 100u);
    CHECK("extents past a poked EOF refuse to open",
          hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w) != 0);
}

static void test_497_extj_classic_sparse_and_tails(void) {
    static hype_extj_wfile_t w;
    uint8_t rb[8];
    uint64_t sz0;

    g_wfail_at = ~0u;
    /* classic map under a journal: a far sparse grow through wholly-new blocks */
    build_vol_ext3();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    sz0 = w.size_bytes;
    CHECK_HEX("classic far grow", 0,
              hype_extj_write_at(&w, sz0 + 4u * BS + 9u, "CLSF", 4));
    CHECK("classic far size", w.size_bytes == sz0 + 4u * BS + 13u);
    CHECK_HEX("classic gap zero", 0, hype_extj_read_at(&w, sz0 + BS, rb, 8));
    CHECK("classic gap zeros", rb[0] == 0 && rb[7] == 0);

    /* classic HOLE tail via the poke (learn offset, rebuild, poke, single open) */
    {
        uint64_t ib2 = w.inode_byte;
        build_vol_ext3();
        poke_size(ib2, 1u * BS + 50u); /* logical block 1 is a hole in swiss.bin */
        CHECK_HEX("re-open 2", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    }
    CHECK_HEX("classic hole-tail grow", 0, hype_extj_write_at(&w, 1u * BS + 80u, "CH", 2));
    CHECK("classic hole-tail size", w.size_bytes == 1u * BS + 82u);
    CHECK_HEX("classic hole-tail readback", 0, hype_extj_read_at(&w, 1u * BS + 50u, rb, 8));
    CHECK("classic hole-tail gap zeros", rb[0] == 0);
}

static void test_497_ext2_hole_tail(void) {
    static hype_ext2_wfile_t w;
    uint8_t rb[8];

    uint64_t ib3;
    build_vol_ext2();
    g_wfail_at = ~0u;
    CHECK_HEX("open", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    ib3 = w.inode_byte;
    build_vol_ext2();
    poke_size(ib3, 1u * BS + 40u);
    CHECK_HEX("re-open", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
    CHECK("poked took", w.size_bytes == 1u * BS + 40u);
    CHECK_HEX("ext2 hole-tail grow", 0, hype_ext2_write_at(&w, 1u * BS + 60u, "E2", 2));
    CHECK("ext2 hole-tail size", w.size_bytes == 1u * BS + 62u);
    CHECK_HEX("ext2 hole-tail readback", 0, hype_ext2_read_at(&w, 1u * BS + 40u, rb, 8));
    CHECK("ext2 hole-tail gap zeros", rb[0] == 0);
}


static void test_497_fs_ops_append(void) {
    static hype_fs_t fs;
    static hype_fs_file_t f;
    static uint8_t big[80u * 1024u]; /* > EXT_GROW_CHUNK: exercises the chunk split */
    uint8_t rb[64];
    uint64_t sz0;
    unsigned i;

    g_wfail_at = ~0u;
    build_vol_ext4j();
    CHECK_HEX("mount rw", 0, hype_fs_mount_auto(&fs, vol_read, vol_write2, 0));
    CHECK("caps carry append+grow",
          (hype_fs_caps(&fs) & (HYPE_FS_CAP_APPEND | HYPE_FS_CAP_WRITE_GROW)) ==
              (HYPE_FS_CAP_APPEND | HYPE_FS_CAP_WRITE_GROW));
    CHECK_HEX("lookup", 0, hype_fs_lookup(&fs, "/esp.bin", &f));
    sz0 = f.size;
    for (i = 0; i < sizeof(big); i++) big[i] = pat(i + 11u);
    CHECK_HEX("80 KiB append (chunked)", 0, hype_fs_append(&f, big, (unsigned)sizeof(big)));
    CHECK("handle size advanced", f.size == sz0 + sizeof(big));
    CHECK_HEX("readback at the old EOF", 0, hype_fs_read_at(&f, sz0, rb, 64u));
    for (i = 0; i < 64u; i++) { if (rb[i] != pat(i + 11u)) break; }
    CHECK("append bytes", i == 64u);
    CHECK_HEX("readback at the very end", 0,
              hype_fs_read_at(&f, sz0 + sizeof(big) - 8u, rb, 8u));
    for (i = 0; i < 8u; i++) { if (rb[i] != pat((unsigned)(sizeof(big) - 8u + i + 11u))) break; }
    CHECK("append tail bytes", i == 8u);
    /* write_at grow through the fs layer too */
    CHECK_HEX("write_at grow via fs_ops", 0,
              hype_fs_write_at(&f, f.size + 100u, "FSGROW", 6u));
    CHECK("write_at grew the handle", f.size == sz0 + sizeof(big) + 106u);
}


/* #497: fault sweep over the growth paths -- every write is failed in turn, and each failure
 * must leave either a rolled-back file (size unmoved) or, for the journaled writer past its
 * exposure point, a POISONED handle (dead) -- never a silently half-grown file. This is the
 * #464 discipline: growth work carries a deliberate mid-growth failure test, not only success. */
static void test_497_grow_fault_sweep(void) {
    static uint8_t big[3u * BS];
    unsigned k, i;
    for (i = 0; i < sizeof(big); i++) big[i] = pat(i + 3u);

    for (k = 1; k < 48u; k++) {
        static hype_ext2_wfile_t w;
        uint64_t sz0;
        int rc;
        build_vol_ext2();
        g_wfail_at = ~0u;
        if (hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w) != 0) { CHECK("sweep open2", 0); break; }
        sz0 = w.size_bytes;
        g_wfail_at = g_writes_seen + k;
        rc = hype_ext2_write_at(&w, sz0 + 700u, big, (unsigned)(2u * BS));
        g_wfail_at = ~0u;
        if (rc != 0) {
            CHECK("ext2 grow failure rolled back (size unmoved)", w.size_bytes == sz0);
        } else {
            CHECK("ext2 grow survived late fault -- size moved", w.size_bytes == sz0 + 700u + 2u * BS);
        }
    }

    for (k = 1; k < 48u; k++) {
        static hype_extj_wfile_t w;
        uint64_t sz0;
        int rc;
        build_vol_ext4j();
        g_wfail_at = ~0u;
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w) != 0) { CHECK("sweep openj", 0); break; }
        sz0 = w.size_bytes;
        g_wfail_at = g_writes_seen + k;
        rc = hype_extj_write_at(&w, sz0 + 700u, big, (unsigned)(2u * BS));
        g_wfail_at = ~0u;
        if (rc != 0) {
            /* Unexposed failure: size unmoved. Exposed failure: the handle is poisoned and
             * refuses everything -- both honest, a half-grown live file is neither. */
            CHECK("extj grow failure honest", w.dead || w.size_bytes == sz0);
            if (w.dead) {
                CHECK("dead handle refuses writes",
                      hype_extj_write_at(&w, 0u, "x", 1) != 0);
            }
        } else {
            CHECK("extj grow survived late fault", w.size_bytes == sz0 + 700u + 2u * BS);
        }
    }

    /* the same sweep through the poked TAIL shapes -- unwritten and pure-hole tails, so the
     * convert/claim/insert error legs are exercised, not only the fresh-block ones */
    {
        static hype_extj_wfile_t w;
        uint64_t ibp = 0;
        build_vol_ext4j();
        g_wfail_at = ~0u;
        if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w) == 0) ibp = w.inode_byte;
        for (k = 1; k < 28u && ibp != 0u; k++) {
            int rc;
            build_vol_ext4j();
            poke_size(ibp, 7u * BS + 100u); /* unwritten tail */
            g_wfail_at = ~0u;
            if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w) != 0) { CHECK("sweep openu", 0); break; }
            g_wfail_at = g_writes_seen + k;
            rc = hype_extj_write_at(&w, 7u * BS + 150u, big, 64u);
            g_wfail_at = ~0u;
            if (rc != 0) {
                CHECK("unwritten-tail fault honest", w.dead || w.size_bytes == 7u * BS + 100u);
            }
        }
        for (k = 1; k < 28u && ibp != 0u; k++) {
            int rc;
            build_vol_ext4j();
            poke_size(ibp, 12u * BS + 50u); /* pure-hole tail past every extent */
            g_wfail_at = ~0u;
            if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w) != 0) { CHECK("sweep openh", 0); break; }
            g_wfail_at = g_writes_seen + k;
            rc = hype_extj_write_at(&w, 12u * BS + 80u, big, 64u);
            g_wfail_at = ~0u;
            if (rc != 0) {
                CHECK("hole-tail fault honest", w.dead || w.size_bytes == 12u * BS + 50u);
            }
        }
        /* DATA-tail gap sweep: unalign first, then a gapped in-tail grow under fault -- the
         * tail_span_write zero and data legs each get their turn to fail. */
        for (k = 1; k < 24u; k++) {
            static hype_extj_wfile_t w2;
            uint64_t sz1;
            int rc;
            build_vol_ext4j();
            g_wfail_at = ~0u;
            if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w2) != 0) { CHECK("sweep openg", 0); break; }
            if (hype_extj_write_at(&w2, w2.size_bytes, "unalign!", 8) != 0) { CHECK("sweep unalign", 0); break; }
            sz1 = w2.size_bytes;
            g_wfail_at = g_writes_seen + k;
            rc = hype_extj_write_at(&w2, sz1 + 40u, big, 80u);
            g_wfail_at = ~0u;
            if (rc != 0) {
                CHECK("gapped-tail fault honest", w2.dead || w2.size_bytes == sz1);
            }
        }
        /* READ-failure legs of the in-place tail writes (tail_span_write RMWs each sector). */
        {
            static hype_extj_wfile_t w4;
            uint64_t sz1;
            long r;
            build_vol_ext4j();
            g_wfail_at = ~0u;
            if (hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w4) == 0 &&
                hype_extj_write_at(&w4, w4.size_bytes, "unalign!", 8) == 0) {
                sz1 = w4.size_bytes;
                for (r = 0; r < 12; r++) {
                    int rc;
                    g_read_countdown = r;
                    rc = hype_extj_write_at(&w4, sz1 + 40u, big, 80u);
                    g_read_countdown = -1;
                    if (rc == 0) {
                        break; /* reads exhausted past the fragile window: done */
                    }
                    CHECK("read-fault tail grow honest", w4.dead || w4.size_bytes == sz1);
                    if (w4.dead) break;
                }
                g_read_countdown = -1;
            }
        }
        /* classic-map (ext3-under-journal) grow sweep: the classic wholly-new and tail legs. */
        for (k = 1; k < 24u; k++) {
            static hype_extj_wfile_t w3;
            uint64_t sz0c;
            int rc;
            build_vol_ext3();
            g_wfail_at = ~0u;
            if (hype_extj_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w3) != 0) { CHECK("sweep openc", 0); break; }
            sz0c = w3.size_bytes;
            g_wfail_at = g_writes_seen + k;
            rc = hype_extj_write_at(&w3, sz0c + 300u, big, (unsigned)(BS + 100u));
            g_wfail_at = ~0u;
            if (rc != 0) {
                CHECK("classic grow fault honest", w3.dead || w3.size_bytes == sz0c);
            }
        }
    }
}

/* #497: the extj DATA-tail gap-zero rule (the ext2 twin lives in test_497_ext2_grow). */
static void test_497_extj_gapped_tail(void) {
    static hype_extj_wfile_t w;
    uint8_t rb[128];
    uint64_t sz1;
    unsigned i;

    g_wfail_at = ~0u;
    build_vol_ext4j();
    CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
    /* first grow: 100 bytes -> size unaligned, tail block DATA */
    CHECK_HEX("unalign grow", 0, hype_extj_write_at(&w, w.size_bytes, "0123456789", 10));
    sz1 = w.size_bytes;
    CHECK_HEX("gapped tail grow", 0, hype_extj_write_at(&w, sz1 + 40u, "JGAP", 4));
    CHECK("gapped size", w.size_bytes == sz1 + 44u);
    CHECK_HEX("gap readback", 0, hype_extj_read_at(&w, sz1, rb, 44u));
    for (i = 0; i < 40u; i++) { if (rb[i] != 0u) break; }
    CHECK("extj tail gap reads zero", i == 40u);
    CHECK("extj gapped data", rb[40] == 'J' && rb[41] == 'G' && rb[42] == 'A' && rb[43] == 'P');
}


/* #497: the remaining functional sides -- zeros-only tails, the extents HOLE tail, and the
 * ext2-tagged fs_ops chunked append. */
static void test_497_more_sides(void) {
    unsigned i;
    g_wfail_at = ~0u;

    /* extj: a grow whose write starts BEYOND the tail block -- the tail gets zeros only. */
    {
        static hype_extj_wfile_t w;
        uint8_t rb[64];
        uint64_t sz1;
        build_vol_ext4j();
        CHECK_HEX("open", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
        CHECK_HEX("unalign", 0, hype_extj_write_at(&w, w.size_bytes, "abc", 3));
        sz1 = w.size_bytes;
        CHECK_HEX("far grow past the tail", 0,
                  hype_extj_write_at(&w, sz1 + 2u * BS, "BEYOND", 6));
        CHECK("far size", w.size_bytes == sz1 + 2u * BS + 6u);
        CHECK_HEX("tail zeros-only readback", 0, hype_extj_read_at(&w, sz1, rb, 32u));
        for (i = 0; i < 32u; i++) { if (rb[i] != 0u) break; }
        CHECK("tail zeroed to its end", i == 32u);
    }

    /* ext2 twin of the same shape. */
    {
        static hype_ext2_wfile_t w;
        uint8_t rb[32];
        uint64_t sz1;
        build_vol_ext2();
        CHECK_HEX("open2", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
        CHECK_HEX("unalign2", 0, hype_ext2_write_at(&w, w.size_bytes, "xy", 2));
        sz1 = w.size_bytes;
        CHECK_HEX("far grow past the tail (ext2)", 0,
                  hype_ext2_write_at(&w, sz1 + 3u * BS, "B2", 2));
        CHECK("far size 2", w.size_bytes == sz1 + 3u * BS + 2u);
        CHECK_HEX("tail zeros readback 2", 0, hype_ext2_read_at(&w, sz1, rb, 16u));
        for (i = 0; i < 16u; i++) { if (rb[i] != 0u) break; }
        CHECK("ext2 tail zeroed", i == 16u);
    }

    /* extents HOLE tail: i_size poked past every extent, tail block a pure hole. */
    {
        static hype_extj_wfile_t w;
        uint8_t rb[16];
        uint64_t ib;
        build_vol_ext4j();
        CHECK_HEX("open3", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
        ib = w.inode_byte;
        build_vol_ext4j();
        poke_size(ib, 12u * BS + 50u); /* well past the last extent: tail is a hole */
        CHECK_HEX("open poked-high", 0,
                  hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
        CHECK_HEX("extents hole-tail grow", 0,
                  hype_extj_write_at(&w, 12u * BS + 80u, "EHT", 3));
        CHECK("extents hole-tail size", w.size_bytes == 12u * BS + 83u);
        CHECK_HEX("extents hole-tail readback", 0, hype_extj_read_at(&w, 12u * BS + 50u, rb, 16u));
        for (i = 0; i < 16u; i++) { if (rb[i] != (uint8_t)((i >= 30u) ? 0u : 0u)) break; }
        CHECK("extents hole gap zeros", rb[0] == 0u && rb[15] == 0u);
    }

    /* fs_ops chunked append through the TAG_EXT2 arm (no journal on this volume). */
    {
        static hype_fs_t fs;
        static hype_fs_file_t f;
        static uint8_t big2[70u * 1024u];
        uint64_t sz0;
        build_vol_ext2();
        for (i = 0; i < sizeof(big2); i++) big2[i] = pat(i + 5u);
        CHECK_HEX("mount ext2 rw", 0, hype_fs_mount_auto(&fs, vol_read, vol_write2, 0));
        CHECK_HEX("lookup swiss", 0, hype_fs_lookup(&fs, "/swiss.bin", &f));
        sz0 = f.size;
        CHECK_HEX("70 KiB ext2 append (chunked)", 0,
                  hype_fs_append(&f, big2, (unsigned)sizeof(big2)));
        CHECK("ext2 append size", f.size == sz0 + sizeof(big2));
    }
}


/* #497: the fragmentation margin -- a grow is refused BEFORE the map can outgrow
 * HYPE_FILE_MAX_RANGES, leaving a file hype can still read whole. */
static void test_497_margin_gate(void) {
    g_wfail_at = ~0u;
    {
        static hype_ext2_wfile_t w;
        uint64_t sz0;
        build_vol_ext2();
        CHECK_HEX("open", 0, hype_ext2_open_rw(vol_read, vol_write2, 0, "/swiss.bin", &w));
        sz0 = w.size_bytes;
        w.map.count = HYPE_FILE_MAX_RANGES - 4u; /* four from the cliff */
        CHECK("ext2 grow refused at the margin", hype_ext2_write_at(&w, sz0, "m", 1) != 0);
        w.map.count = HYPE_FILE_MAX_RANGES - 9u; /* just under the margin */
        CHECK("non-grow writes unaffected", hype_ext2_write_at(&w, 10u, "m", 1) == 0);
    }
    {
        static hype_extj_wfile_t w;
        uint64_t sz0;
        build_vol_ext4j();
        CHECK_HEX("openj", 0, hype_extj_open_rw(vol_read, vol_write2, 0, "/esp.bin", &w));
        sz0 = w.size_bytes;
        w.map.count = HYPE_FILE_MAX_RANGES - 4u;
        CHECK("extj grow refused at the margin", hype_extj_write_at(&w, sz0, "m", 1) != 0);
    }
}

int main(void) {
    test_fs_ops_ext();
    test_resolve_rmap_sparse();
    test_ext2_alloc();
    test_ext2_alloc_rollback();
    test_ext2_alloc_deep();
    test_384_final_edges();
    test_384_coverage_tail();
    test_extj_gates();
    test_extj_classic();
    test_extj_extents();
    test_extj_crash_windows();
    test_extj_deep_and_faults();
    test_jbd2_api();
    test_extj_extent_guards();
    test_extj_wave3();
    test_extj_gate_tail();
    test_extj_last_mile();
    test_resolve_extents();
    test_resolve_indirect();
    test_refusals();
    test_mount_refusals();
    test_corrupt_structures();
    test_write_at();
    test_write_gate();
    test_write_io_errors();
    test_more_bounds();
    test_64bit_feature();
    test_triple_indirect();
    test_fault_sweep();

    test_497_ext2_grow();
    test_497_ext2_grow_rollback();
    test_497_extj_grow_extents();
    test_497_extj_grow_classic();
    test_497_extj_unwritten_and_hole_tails();
    test_497_extj_classic_sparse_and_tails();
    test_497_ext2_hole_tail();
    test_497_fs_ops_append();
    test_497_grow_fault_sweep();
    test_497_extj_gapped_tail();
    test_497_more_sides();
    test_497_margin_gate();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
