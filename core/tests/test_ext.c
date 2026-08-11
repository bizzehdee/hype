#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ext.h"

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

int main(void) {
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
