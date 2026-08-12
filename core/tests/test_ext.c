#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ext.h"
#include "../fs_ops.h"

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
    CHECK("rw caps: no append (ext allocation is #384)",
          (hype_fs_caps(&fs) & HYPE_FS_CAP_APPEND) == 0);
    CHECK_HEX("rw lookup", 0, hype_fs_lookup(&fs, "/img.bin", &f));
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
    CHECK("write past EOF refused", hype_ext2_write_at(&w, w.size_bytes - 1u, "ab", 2) != 0);
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

int main(void) {
    test_fs_ops_ext();
    test_resolve_rmap_sparse();
    test_ext2_alloc();
    test_ext2_alloc_rollback();
    test_ext2_alloc_deep();
    test_384_final_edges();
    test_384_coverage_tail();
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
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
