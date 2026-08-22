/* #498: unit tests for core/ext_dirent.c, core/ext2_namespace.c,
 * core/extj_namespace.c and core/ext_namespace.c's dispatcher, plus the
 * core/fs_ops.c wiring and core/ext.c's hype_ext_resolve_dir_ino and
 * core/rtc.c's hype_rtc_to_unix. Synthetic in-memory volumes, hand-laid the
 * same way core/tests/test_ext.c already does -- no real mkfs tool
 * dependency (that is tools/498/run-498.sh's job, real media only). */
#include <stdio.h>
#include <string.h>
#include "../ext.h"
#include "../ext_dirent.h"
#include "../ext_namespace.h"
#include "../ext_csum.h"
#include "../fs_ops.h"
#include "../rtc.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

/* ---------------------------------------------------------------------
 * ext_dirent.c: pure, no volume needed.
 * --------------------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

static void test_dirent_basic(void) {
    uint8_t blk[256];
    uint32_t off, ino;

    memset(blk, 0xAA, sizeof blk);
    hype_extd_block_init(blk, sizeof blk, 0);
    CHECK("init: one free slot spans the block", get16(blk + 4) == sizeof blk);
    CHECK("validate: fresh block ok", hype_extd_validate(blk, sizeof blk, 0) == 0);

    CHECK("insert .", hype_extd_insert(blk, sizeof blk, 0, 2u, ".", 1u, HYPE_EXTD_FT_DIR) == 0);
    CHECK("insert ..", hype_extd_insert(blk, sizeof blk, 0, 2u, "..", 2u, HYPE_EXTD_FT_DIR) == 0);
    CHECK("insert foo", hype_extd_insert(blk, sizeof blk, 0, 12u, "foo", 3u, HYPE_EXTD_FT_REG) == 0);
    CHECK("validate after inserts", hype_extd_validate(blk, sizeof blk, 0) == 0);

    CHECK("find foo", hype_extd_find(blk, sizeof blk, 0, "foo", 3u, &off, &ino) == 1);
    CHECK("find foo ino", ino == 12u);
    CHECK("find missing", hype_extd_find(blk, sizeof blk, 0, "bar", 3u, &off, &ino) == 0);
    CHECK("only_dots false with foo present", hype_extd_only_dots(blk, sizeof blk, 0) == 0);

    /* remove foo (has a predecessor ".." in the same block): merges into it */
    CHECK("remove foo", hype_extd_remove(blk, sizeof blk, 0, "foo", 3u) == 1);
    CHECK("validate after remove", hype_extd_validate(blk, sizeof blk, 0) == 0);
    CHECK("find foo gone", hype_extd_find(blk, sizeof blk, 0, "foo", 3u, &off, &ino) == 0);
    CHECK("only_dots true again", hype_extd_only_dots(blk, sizeof blk, 0) == 1);
    CHECK("remove missing returns 0", hype_extd_remove(blk, sizeof blk, 0, "foo", 3u) == 0);

    /* re-insert into the freed (merged) space */
    CHECK("re-insert foo", hype_extd_insert(blk, sizeof blk, 0, 13u, "foo2", 4u, HYPE_EXTD_FT_REG) ==
                              0);
    CHECK("find foo2", hype_extd_find(blk, sizeof blk, 0, "foo2", 4u, &off, &ino) == 1);

    /* removing the FIRST entry in a block: stays present, inode forced 0 */
    {
        uint8_t b2[64];
        memset(b2, 0, sizeof b2);
        hype_extd_block_init(b2, sizeof b2, 0);
        CHECK("b2 insert only", hype_extd_insert(b2, sizeof b2, 0, 5u, "x", 1u, HYPE_EXTD_FT_REG) ==
                                   0);
        CHECK("b2 remove first", hype_extd_remove(b2, sizeof b2, 0, "x", 1u) == 1);
        CHECK("b2 first entry ino zeroed", get32(b2 + 0) == 0u);
        CHECK("b2 rec_len untouched", get16(b2 + 4) == sizeof b2);
    }
}

static void test_dirent_full_and_grow_refusal(void) {
    /* a tiny block that fits exactly one 12-byte entry ("."): a second
     * insert of the same size must fail (no room), proving hype_extd_insert
     * never silently corrupts on overflow. */
    uint8_t blk[12];
    memset(blk, 0, sizeof blk);
    hype_extd_block_init(blk, sizeof blk, 0);
    CHECK("tiny insert ok", hype_extd_insert(blk, sizeof blk, 0, 2u, ".", 1u, HYPE_EXTD_FT_DIR) == 0);
    CHECK("tiny insert overflow refused",
         hype_extd_insert(blk, sizeof blk, 0, 3u, "y", 1u, HYPE_EXTD_FT_REG) != 0);
}

static void test_dirent_csum_tail(void) {
    uint8_t blk[64];
    uint32_t seed = 0x12345678u;
    uint32_t expect;
    memset(blk, 0, sizeof blk);
    hype_extd_block_init(blk, sizeof blk, 1);
    CHECK("tail: usable shrinks by 12", get16(blk + 4) == sizeof blk - 12u);
    CHECK("validate with tail ok", hype_extd_validate(blk, sizeof blk, 1) == 0);
    CHECK("insert with tail", hype_extd_insert(blk, sizeof blk, 1, 2u, ".", 1u, HYPE_EXTD_FT_DIR) ==
                                 0);
    hype_extd_csum_finalize(blk, sizeof blk, 1, seed);
    expect = hype_ext_crc32c(seed, blk, sizeof blk - 12u);
    CHECK("tail checksum matches independent recompute", get32(blk + sizeof blk - 4u) == expect);
    CHECK("tail marker fields intact", blk[sizeof blk - 12u + 6u] == 0u &&
                                          blk[sizeof blk - 12u + 7u] == 0xDEu);
    CHECK("validate still ok after finalize", hype_extd_validate(blk, sizeof blk, 1) == 0);

    /* corrupt the tail: validate must refuse rather than guess */
    blk[sizeof blk - 12u + 7u] = 0x00u;
    CHECK("corrupt tail refused", hype_extd_validate(blk, sizeof blk, 1) != 0);
}

static void test_dirent_corrupt_chain(void) {
    uint8_t blk[32];
    memset(blk, 0, sizeof blk);
    /* a zero rec_len must never be accepted (would spin the scanner) */
    put16(blk + 4, 0u);
    CHECK("zero rec_len refused", hype_extd_validate(blk, sizeof blk, 0) != 0);
    /* an unaligned rec_len refused */
    memset(blk, 0, sizeof blk);
    put16(blk + 4, 15u);
    CHECK("unaligned rec_len refused", hype_extd_validate(blk, sizeof blk, 0) != 0);
    /* a chain that overruns the block refused */
    memset(blk, 0, sizeof blk);
    put16(blk + 4, (uint16_t)(sizeof blk + 4u));
    CHECK("overrunning rec_len refused", hype_extd_validate(blk, sizeof blk, 0) != 0);
    /* a name_len claiming more bytes than the usable region has, even though
     * this record's own rec_len is small and in-bounds */
    memset(blk, 0, sizeof blk);
    put32(blk + 0, 5u);   /* a real entry (inode != 0) */
    put16(blk + 4, 12u);  /* rec_len: valid, in-bounds */
    blk[6] = 250u;        /* name_len: claims far more than fits */
    CHECK("oversized name_len refused", hype_extd_validate(blk, sizeof blk, 0) != 0);

    /* a block too small to hold even one record header */
    CHECK("undersized block refused", hype_extd_validate(blk, 4u, 0) != 0);
    /* too small for a metadata_csum tail specifically */
    CHECK("undersized block with tail refused", hype_extd_validate(blk, 16u, 1) != 0);
}

/* ---------------------------------------------------------------------
 * Synthetic ext2/ext3/ext4-shaped volumes, hand-laid like test_ext.c's.
 * One group, 1024-byte blocks, 128-byte inodes, first_ino=11.
 *
 *   block 1: superblock        block 2: group descriptor
 *   block 3: block bitmap      block 4: inode bitmap
 *   blocks 5-8: inode table (32 inodes x 128 B)
 *   block 9: root dir data     block 10: /htree dir data (INDEX_FL set)
 *   blocks 20-27: journal (ext3/4 variant only)
 *   everything else: free
 * --------------------------------------------------------------------- */

#define BS 1024u
#define SPB 2u
#define VOL_BLOCKS 1024u
#define INODES_PER_GROUP 256u /* generous: the directory-growth test alone
                               * needs room for ~90 inodes past the reserved
                               * + fixture-preexisting ones */
#define INODE_SIZE 128u
#define INODE_TABLE 5u /* 256 inodes * 128 B = 32 KiB = 32 blocks: 5..36 */
#define ROOT_BLK 37u
#define HTREE_BLK 38u
#define BITMAP_BLK 3u
#define IBITMAP_BLK 4u
#define JIND_BLK 39u /* the journal inode's single-indirect pointer block */
#define JBLK 40u
#define JLEN 48u /* generous headroom: rename can touch ~10 distinct metadata
                  * blocks in one transaction, and hype_jbd2_commit requires
                  * maxlen - first >= count + 2. Past 12 blocks the classic
                  * map needs single indirection (JIND_BLK), same as any
                  * other classic-mapped file this size. */
#define FIRST_INO 11u
#define HTREE_INO 20u
#define JOURNAL_INO 8u

static uint8_t g_vol[VOL_BLOCKS * BS];

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if ((lba + count) * 512u > sizeof g_vol) return -1;
    memcpy(dst, g_vol + lba * 512u, (size_t)count * 512u);
    return 0;
}
static int vol_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if ((lba + count) * 512u > sizeof g_vol) return -1;
    memcpy(g_vol + lba * 512u, src, (size_t)count * 512u);
    return 0;
}

/* Fault-injecting wrappers: the Nth read+write call combined fails, every
 * other call behaves normally. Drives the fault sweep below through every
 * "if (an I/O call failed) return -1" unwind path both backends carry. */
static long g_fail_at = -1;
static long g_io_calls = 0;
static int vol_read_flaky(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    if (g_fail_at >= 0 && g_io_calls++ == g_fail_at) return -1;
    return vol_read(ctx, lba, count, dst);
}
static int vol_write_flaky(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    if (g_fail_at >= 0 && g_io_calls++ == g_fail_at) return -1;
    return vol_write(ctx, lba, count, src);
}

static uint8_t *blk(uint32_t b) { return g_vol + (size_t)b * BS; }
static uint8_t *ino_ptr(uint32_t ino) { return blk(INODE_TABLE) + (size_t)(ino - 1u) * INODE_SIZE; }

static void bitmap_set(uint8_t *bm, uint32_t bit) { bm[bit / 8u] |= (uint8_t)(1u << (bit % 8u)); }

static void put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* has_journal: build ext3 (no checksum). has_csum: also enable
 * RO_COMPAT_METADATA_CSUM (only meaningful when has_journal). */
static void build_vol(int has_journal, int has_csum) {
    uint8_t *sb = g_vol + 1024;
    uint32_t incompat = 0x0002u; /* INCOMPAT_FILETYPE */
    uint32_t compat = 0;
    uint32_t rocompat = 0;
    uint32_t used_blocks;

    memset(g_vol, 0, sizeof g_vol);

    if (has_journal) {
        compat |= 0x0004u; /* COMPAT_HAS_JOURNAL */
        incompat |= 0x0040u; /* INCOMPAT_EXTENTS: exercise the extent-mapped path */
        if (has_csum) rocompat |= 0x0400u; /* RO_COMPAT_METADATA_CSUM */
    }

    memset(sb, 0, 1024);
    put32(sb + 0x00, INODES_PER_GROUP); /* inodes_count (one group == inodes_per_group) */
    put32(sb + 0x04, VOL_BLOCKS);       /* blocks_count */
    put32(sb + 0x0C, 0u);               /* free_blocks (fixed up below) */
    put32(sb + 0x10, 0u);               /* free_inodes (fixed up below) */
    put32(sb + 0x14, 1u);               /* first_data_block */
    put32(sb + 0x18, 0u);               /* log_block_size: 1024 */
    put32(sb + 0x20, VOL_BLOCKS);       /* blocks_per_group: one group */
    put32(sb + 0x28, INODES_PER_GROUP); /* inodes_per_group */
    put16(sb + 0x38, 0xEF53u);          /* magic */
    put16(sb + 0x3A, 0x0001u);          /* state: VALID */
    put32(sb + 0x4C, 1u);               /* rev_level: dynamic */
    put32(sb + 0x54, FIRST_INO);        /* s_first_ino */
    put16(sb + 0x58, INODE_SIZE);       /* inode_size */
    put32(sb + 0x5C, compat);
    put32(sb + 0x60, incompat);
    put32(sb + 0x64, rocompat);
    if (has_journal) put32(sb + 0xE0, JOURNAL_INO); /* s_journal_inum */
    if (has_csum) {
        unsigned i;
        for (i = 0; i < 16u; i++) sb[0x68 + i] = (uint8_t)(0x10u + i); /* s_uuid */
        sb[0x175] = 1u; /* s_checksum_type: crc32c */
    }

    /* group descriptor */
    put32(blk(2) + 0x00, BITMAP_BLK);   /* bg_block_bitmap */
    put32(blk(2) + 0x04, IBITMAP_BLK);  /* bg_inode_bitmap */
    put32(blk(2) + 0x08, INODE_TABLE);  /* bg_inode_table */
    put16(blk(2) + 0x10, 1u);           /* bg_used_dirs_count: root */

    /* block bitmap: blocks 1..HTREE_BLK used (super, gd, 2 bitmaps, the
     * inode table, root dir, htree dir); +journal range when journaled */
    {
        uint32_t b;
        for (b = 1; b <= HTREE_BLK; b++) bitmap_set(blk(BITMAP_BLK), b - 1u);
        used_blocks = HTREE_BLK;
        if (has_journal) {
            for (b = JBLK; b < JBLK + JLEN; b++) bitmap_set(blk(BITMAP_BLK), b - 1u);
            bitmap_set(blk(BITMAP_BLK), JIND_BLK - 1u);
            used_blocks += JLEN + 1u;
        }
    }
    put32(sb + 0x0C, VOL_BLOCKS - 1u - used_blocks);
    put16(blk(2) + 0x0C, (uint16_t)(VOL_BLOCKS - 1u - used_blocks));

    /* inode bitmap: 1..10 reserved, + inode HTREE_INO (20), + inode 8 (journal) */
    {
        uint32_t i, used_inodes = 10u;
        for (i = 1; i <= 10u; i++) bitmap_set(blk(IBITMAP_BLK), i - 1u);
        bitmap_set(blk(IBITMAP_BLK), HTREE_INO - 1u);
        used_inodes++;
        if (has_journal) {
            bitmap_set(blk(IBITMAP_BLK), JOURNAL_INO - 1u);
            used_inodes++;
        }
        put32(sb + 0x10, INODES_PER_GROUP - used_inodes);
        put16(blk(2) + 0x0E, (uint16_t)(INODES_PER_GROUP - used_inodes));
    }

    /* root inode (2): classic map, one block, "." + ".." + a pre-existing
     * "htree" subdirectory entry -- built with the SAME hype_extd_insert
     * this ticket's writers use, so the fixture's shape is exactly what a
     * real insert sequence produces. */
    {
        uint8_t *in = ino_ptr(2u);
        memset(in, 0, INODE_SIZE);
        put16(in + 0x00, 0x41EDu);
        put16(in + 0x1A, 3u); /* links_count: self + the htree subdir's ".." */
        put32(in + 0x04, BS); /* size */
        put32(in + 0x1C, SPB); /* i_blocks */
        put32(in + 0x28, ROOT_BLK);
        hype_extd_block_init(blk(ROOT_BLK), BS, has_csum);
        (void)hype_extd_insert(blk(ROOT_BLK), BS, has_csum, 2u, ".", 1u, HYPE_EXTD_FT_DIR);
        (void)hype_extd_insert(blk(ROOT_BLK), BS, has_csum, 2u, "..", 2u, HYPE_EXTD_FT_DIR);
        (void)hype_extd_insert(blk(ROOT_BLK), BS, has_csum, HTREE_INO, "htree", 5u,
                               HYPE_EXTD_FT_DIR);
        if (has_csum) {
            /* i_csum_seed for inode 2, generation 0 (this fixture never sets
             * i_generation) -- same formula core/extj_namespace.c uses. */
            uint8_t uuid[16], inum_le[4], gen_le[4] = {0, 0, 0, 0};
            uint32_t fs_seed, seed;
            unsigned k;
            for (k = 0; k < 16u; k++) uuid[k] = (uint8_t)(0x10u + k);
            fs_seed = hype_ext_crc32c(0xFFFFFFFFu, uuid, 16u);
            put32(inum_le, 2u);
            seed = hype_ext_crc32c(fs_seed, inum_le, 4u);
            seed = hype_ext_crc32c(seed, gen_le, 4u);
            hype_extd_csum_finalize(blk(ROOT_BLK), BS, 1, seed);
        }
    }

    /* /htree (inode 20): a directory carrying EXT4_INDEX_FL, one data block
     * with "." + ".." only -- refusal is about the FLAG, not real htree
     * shape, so this is enough to test the gate. */
    {
        uint8_t *in = ino_ptr(HTREE_INO);
        memset(in, 0, INODE_SIZE);
        put16(in + 0x00, 0x41EDu);
        put16(in + 0x1A, 2u);
        put32(in + 0x04, BS);
        put32(in + 0x1C, SPB);
        put32(in + 0x20, 0x00001000u); /* EXT4_INDEX_FL */
        put32(in + 0x28, HTREE_BLK);
        hype_extd_block_init(blk(HTREE_BLK), BS, has_csum);
        (void)hype_extd_insert(blk(HTREE_BLK), BS, has_csum, HTREE_INO, ".", 1u, HYPE_EXTD_FT_DIR);
        (void)hype_extd_insert(blk(HTREE_BLK), BS, has_csum, 2u, "..", 2u, HYPE_EXTD_FT_DIR);
        if (has_csum) {
            uint8_t uuid[16], inum_le[4], gen_le[4] = {0, 0, 0, 0};
            uint32_t fs_seed, seed;
            unsigned k;
            for (k = 0; k < 16u; k++) uuid[k] = (uint8_t)(0x10u + k);
            fs_seed = hype_ext_crc32c(0xFFFFFFFFu, uuid, 16u);
            put32(inum_le, HTREE_INO);
            seed = hype_ext_crc32c(fs_seed, inum_le, 4u);
            seed = hype_ext_crc32c(seed, gen_le, 4u);
            hype_extd_csum_finalize(blk(HTREE_BLK), BS, 1, seed);
        }
    }

    if (has_journal) {
        uint8_t *in = ino_ptr(JOURNAL_INO);
        uint8_t *jsb = blk(JBLK);
        uint32_t i;
        memset(in, 0, INODE_SIZE);
        put16(in + 0x00, 0x8180u);
        put32(in + 0x04, JLEN * BS);
        put32(in + 0x1C, (JLEN + 1u) * SPB); /* +1: the indirect pointer block itself */
        /* classic block map: 12 direct, then single indirect for the rest --
         * JLEN=48 exceeds the 12 direct pointers, same as any other
         * classic-mapped file this size (never write past i_block[14]!). */
        memset(blk(JIND_BLK), 0, BS);
        for (i = 0; i < JLEN; i++) {
            if (i < 12u) {
                put32(in + 0x28 + i * 4u, JBLK + i);
            } else {
                put32(blk(JIND_BLK) + (i - 12u) * 4u, JBLK + i);
            }
        }
        put32(in + 0x28 + 12u * 4u, JIND_BLK);
        memset(jsb, 0, BS);
        put32be(jsb + 0, 0xC03B3998u);
        put32be(jsb + 4, 4u);
        put32be(jsb + 12, BS);
        put32be(jsb + 16, JLEN);
        put32be(jsb + 20, 1u);
        put32be(jsb + 24, 1u);
        put32be(jsb + 28, 0u);
    }
}

/* ---- fixture accessors ---- */

static uint32_t sb_free_blocks(void) { return get32(g_vol + 1024 + 0x0C); }
static uint32_t sb_free_inodes(void) { return get32(g_vol + 1024 + 0x10); }
static uint16_t gdt_free_blocks(void) { return get16(blk(2) + 0x0C); }
static uint16_t gdt_free_inodes(void) { return get16(blk(2) + 0x0E); }
static uint16_t gdt_used_dirs(void) { return get16(blk(2) + 0x10); }
static uint16_t inode_mode(uint32_t ino) { return get16(ino_ptr(ino) + 0x00); }
static uint16_t inode_links(uint32_t ino) { return get16(ino_ptr(ino) + 0x1A); }
static uint32_t inode_size_of(uint32_t ino) { return get32(ino_ptr(ino) + 0x04); }
static int inode_bit(uint32_t ino) {
    uint32_t b = ino - 1u;
    return (blk(IBITMAP_BLK)[b / 8u] >> (b % 8u)) & 1u;
}
/* resolve_ino is FILE-only and resolve_dir_ino is DIRECTORY-only (see
 * ext.c) -- these test helpers try whichever the caller's path actually
 * names, so a directory path never falsely reads as "gone". */
static int exists(const char *path) {
    uint32_t ino;
    if (hype_ext_resolve_ino(vol_read, 0, path, &ino) == 0) return 1;
    return hype_ext_resolve_dir_ino(vol_read, 0, path, &ino) == 0;
}
static uint32_t ino_of(const char *path) {
    uint32_t ino = 0;
    (void)hype_ext_resolve_ino(vol_read, 0, path, &ino);
    return ino;
}
static uint32_t dir_ino_of(const char *path) {
    uint32_t ino = 0;
    (void)hype_ext_resolve_dir_ino(vol_read, 0, path, &ino);
    return ino;
}

/* ---------------------------------------------------------------------
 * Full create/unlink/mkdir/rmdir/rename lifecycle -- run against ext2
 * (has_journal=0), ext3-shaped (1,0) and ext4-with-metadata_csum (1,1) so
 * both backends AND the checksum path share one test.
 * --------------------------------------------------------------------- */

static void test_ns_lifecycle(int has_journal, int has_csum) {
    uint32_t root_links0, foo_ino;
    uint16_t free_i0, free_b0;

    build_vol(has_journal, has_csum);
    root_links0 = inode_links(2u);
    free_i0 = gdt_free_inodes();
    free_b0 = gdt_free_blocks();

    CHECK("create refuses a missing write callback", hype_ext_ns_create(vol_read, 0, 0, "/x", 0) != 0);

    CHECK("create /foo.txt", hype_ext_ns_create(vol_read, vol_write, 0, "/foo.txt", 1000u) == 0);
    CHECK("foo.txt resolves", exists("/foo.txt"));
    foo_ino = ino_of("/foo.txt");
    CHECK("foo.txt is a regular file", (inode_mode(foo_ino) & 0xF000u) == 0x8000u);
    CHECK("foo.txt links == 1", inode_links(foo_ino) == 1u);
    CHECK("foo.txt size == 0", inode_size_of(foo_ino) == 0u);
    CHECK("foo.txt inode bit set", inode_bit(foo_ino) == 1);
    CHECK("free inodes decremented", gdt_free_inodes() == free_i0 - 1u);
    CHECK("free blocks unchanged (0-byte file)", gdt_free_blocks() == free_b0);
    CHECK("sb free blocks matches gd", sb_free_blocks() == gdt_free_blocks());
    CHECK("sb free inodes matches gd", sb_free_inodes() == gdt_free_inodes());

    CHECK("create refuses an existing name",
         hype_ext_ns_create(vol_read, vol_write, 0, "/foo.txt", 0) != 0);
    CHECK("create refuses under an htree parent",
         hype_ext_ns_create(vol_read, vol_write, 0, "/htree/x.txt", 0) != 0);
    CHECK("mkdir refuses under an htree parent",
         hype_ext_ns_mkdir(vol_read, vol_write, 0, "/htree/subdir", 0) != 0);
    CHECK("create refuses a missing parent",
         hype_ext_ns_create(vol_read, vol_write, 0, "/nosuchdir/x.txt", 0) != 0);

    CHECK("mkdir /sub", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/sub", 1001u) == 0);
    CHECK("/sub resolves as a directory", (inode_mode(dir_ino_of("/sub")) & 0xF000u) == 0x4000u);
    CHECK("/sub links == 2", inode_links(dir_ino_of("/sub")) == 2u);
    CHECK("root links +1 for /sub's ..", inode_links(2u) == root_links0 + 1u);
    CHECK("used_dirs +1", gdt_used_dirs() == 2u);
    CHECK("mkdir refuses an existing name", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/sub", 0) != 0);
    CHECK("create refuses a name mkdir already used",
         hype_ext_ns_create(vol_read, vol_write, 0, "/sub", 0) != 0);

    CHECK("rmdir refuses a non-empty parent behaving as a dir target: root",
         hype_ext_ns_rmdir(vol_read, vol_write, 0, "/", 0) != 0);
    CHECK("rmdir refuses an htree-flagged directory", hype_ext_ns_rmdir(vol_read, vol_write, 0, "/htree", 0) != 0);

    CHECK("rename /foo.txt -> /sub/foo.txt",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/foo.txt", "/sub/foo.txt", 1002u) == 0);
    CHECK("old path gone", !exists("/foo.txt"));
    CHECK("new path resolves to the SAME inode", ino_of("/sub/foo.txt") == foo_ino);
    CHECK("plain-file rename never touches link counts", inode_links(2u) == root_links0 + 1u);

    CHECK("rename never clobbers an existing target",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/sub", "/htree", 0) != 0);
    CHECK("rename refuses an htree destination parent",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/sub/foo.txt", "/htree/foo.txt", 0) != 0);

    CHECK("mkdir /sub/inner (nested, for the cycle guard)",
         hype_ext_ns_mkdir(vol_read, vol_write, 0, "/sub/inner", 0) == 0);
    CHECK("rename refuses a directory moved into its own subtree",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/sub", "/sub/inner/x", 0) != 0);
    CHECK("rename refuses a directory moved onto itself",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/sub", "/sub/inner", 0) != 0);
    CHECK("the cycle guard touched nothing: /sub/inner still resolves",
         exists("/sub/inner"));

    /* a genuine cross-parent DIRECTORY rename: /sub/inner -> /sub2/inner,
     * exercising the ".." relocation and both parents' link-count math. */
    CHECK("mkdir /sub2 (rename destination)", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/sub2", 0) ==
                                                 0);
    {
        uint32_t inner_ino = dir_ino_of("/sub/inner");
        uint16_t sub_links_before = inode_links(dir_ino_of("/sub"));
        uint16_t sub2_links_before = inode_links(dir_ino_of("/sub2"));
        CHECK("rename /sub/inner -> /sub2/inner (cross-parent dir move)",
             hype_ext_ns_rename(vol_read, vol_write, 0, "/sub/inner", "/sub2/inner", 1005u) == 0);
        CHECK("old nested path gone", !exists("/sub/inner"));
        CHECK("new nested path resolves to the same inode",
             dir_ino_of("/sub2/inner") == inner_ino);
        CHECK("source parent (/sub) lost the ..-backlink",
             inode_links(dir_ino_of("/sub")) == sub_links_before - 1u);
        CHECK("dest parent (/sub2) gained the ..-backlink",
             inode_links(dir_ino_of("/sub2")) == sub2_links_before + 1u);
    }
    CHECK("rmdir /sub2/inner (cleanup)",
         hype_ext_ns_rmdir(vol_read, vol_write, 0, "/sub2/inner", 0) == 0);
    CHECK("rmdir /sub2 (cleanup)", hype_ext_ns_rmdir(vol_read, vol_write, 0, "/sub2", 0) == 0);

    CHECK("rmdir refuses a non-empty directory",
         hype_ext_ns_rmdir(vol_read, vol_write, 0, "/sub", 0) != 0);
    CHECK("unlink refuses a directory", hype_ext_ns_unlink(vol_read, vol_write, 0, "/sub", 0) != 0);
    CHECK("unlink refuses a missing file",
         hype_ext_ns_unlink(vol_read, vol_write, 0, "/sub/nope.txt", 0) != 0);

    CHECK("unlink /sub/foo.txt", hype_ext_ns_unlink(vol_read, vol_write, 0, "/sub/foo.txt", 1003u) == 0);
    CHECK("/sub/foo.txt gone", !exists("/sub/foo.txt"));
    CHECK("its inode is freed", inode_bit(foo_ino) == 0);
    /* /sub itself is still allocated at this point -- one inode short of
     * the original baseline until it is rmdir'd below. */
    CHECK("free inodes reflect foo.txt's release", gdt_free_inodes() == free_i0 - 1u);

    CHECK("rmdir /sub now it is empty", hype_ext_ns_rmdir(vol_read, vol_write, 0, "/sub", 1004u) == 0);
    CHECK("/sub gone", !exists("/sub"));
    CHECK("root links restored", inode_links(2u) == root_links0);
    CHECK("used_dirs restored", gdt_used_dirs() == 1u);
    CHECK("free blocks restored", gdt_free_blocks() == free_b0);
    CHECK("free inodes fully restored", gdt_free_inodes() == free_i0);
}

/* ---------------------------------------------------------------------
 * Directory growth: enough small files in root to need a second block.
 * --------------------------------------------------------------------- */

static void test_ns_dir_growth(int has_journal, int has_csum) {
    char name[16];
    int i, n = 90; /* ~12 bytes/entry: 90 fits past one 1024-byte block */
    uint16_t blocks0;

    build_vol(has_journal, has_csum);
    blocks0 = gdt_free_blocks();
    for (i = 0; i < n; i++) {
        snprintf(name, sizeof name, "/f%d", i);
        CHECK("growth: create ok", hype_ext_ns_create(vol_read, vol_write, 0, name, 0) == 0);
    }
    CHECK("growth: a second block was claimed", gdt_free_blocks() < blocks0);
    for (i = 0; i < n; i++) {
        snprintf(name, sizeof name, "/f%d", i);
        CHECK("growth: every file still resolves", exists(name));
    }
    /* and every one can be unlinked back out again, from both blocks */
    for (i = 0; i < n; i++) {
        snprintf(name, sizeof name, "/f%d", i);
        CHECK("growth: unlink ok", hype_ext_ns_unlink(vol_read, vol_write, 0, name, 0) == 0);
    }
    for (i = 0; i < n; i++) {
        snprintf(name, sizeof name, "/f%d", i);
        CHECK("growth: gone after unlink", !exists(name));
    }
}

/* ---------------------------------------------------------------------
 * Checksum self-consistency on a metadata_csum (ext4-shaped) volume.
 * --------------------------------------------------------------------- */

static void test_ns_checksum_consistency(void) {
    uint32_t ino;
    uint8_t *in;
    uint32_t crc, stored;

    build_vol(1, 1);
    CHECK("csum: create ok", hype_ext_ns_create(vol_read, vol_write, 0, "/c.txt", 42u) == 0);
    ino = ino_of("/c.txt");
    CHECK("csum: file exists", ino != 0u);
    in = ino_ptr(ino);
    /* recompute i_csum_seed the same way core/ext_jalloc.c / extj_namespace.c
     * do (fs seed chained with inode number + generation) and verify the
     * inode's OWN checksum matches, independently of the code under test. */
    {
        uint8_t uuid[16];
        uint8_t inum_le[4], gen_le[4];
        uint32_t fs_seed, seed;
        unsigned k;
        for (k = 0; k < 16u; k++) uuid[k] = (uint8_t)(0x10u + k);
        fs_seed = hype_ext_crc32c(0xFFFFFFFFu, uuid, 16u);
        put32(inum_le, ino);
        seed = hype_ext_crc32c(fs_seed, inum_le, 4u);
        memcpy(gen_le, in + 0x64, 4u); /* i_generation */
        seed = hype_ext_crc32c(seed, gen_le, 4u);
        {
            uint8_t tmp[INODE_SIZE];
            memcpy(tmp, in, INODE_SIZE);
            put16(tmp + 0x7C, 0u); /* zero i_checksum_lo for the hash, like the writer does */
            crc = hype_ext_crc32c(seed, tmp, INODE_SIZE);
        }
    }
    stored = get16(in + 0x7C);
    CHECK("csum: inode checksum matches independent recompute", (crc & 0xFFFFu) == stored);

    /* the root directory block's metadata_csum tail must also validate */
    CHECK("csum: root dir block passes validate", hype_extd_validate(blk(ROOT_BLK), BS, 1) == 0);
}

/* ---------------------------------------------------------------------
 * hype_ext_resolve_dir_ino
 * --------------------------------------------------------------------- */

static void test_resolve_dir_ino(void) {
    uint32_t ino;
    build_vol(0, 0);
    CHECK("empty path is the root", hype_ext_resolve_dir_ino(vol_read, 0, "", &ino) == 0 && ino == 2u);
    CHECK("bare slash is the root", hype_ext_resolve_dir_ino(vol_read, 0, "/", &ino) == 0 && ino == 2u);
    CHECK("nested dir resolves", hype_ext_resolve_dir_ino(vol_read, 0, "/htree", &ino) == 0 &&
                                    ino == HTREE_INO);
    CHECK("a FILE target is refused", hype_ext_ns_create(vol_read, vol_write, 0, "/plain", 0) == 0);
    CHECK("resolve_dir_ino refuses a regular file",
         hype_ext_resolve_dir_ino(vol_read, 0, "/plain", &ino) != 0);
    CHECK("resolve_dir_ino refuses a missing path",
         hype_ext_resolve_dir_ino(vol_read, 0, "/nope", &ino) != 0);
}

/* ---------------------------------------------------------------------
 * core/rtc.c: hype_rtc_to_unix
 * --------------------------------------------------------------------- */

static void test_rtc_to_unix(void) {
    hype_rtc_time_t t;
    t.year = 1980; t.month = 1; t.day = 1; t.hour = 0; t.minute = 0; t.second = 0;
    CHECK("1980-01-01 epoch", hype_rtc_to_unix(&t) == 315532800u);
    t.year = 2000; t.month = 1; t.day = 1; t.hour = 0; t.minute = 0; t.second = 0;
    CHECK("2000-01-01 epoch", hype_rtc_to_unix(&t) == 946684800u);
    t.year = 2024; t.month = 2; t.day = 29; t.hour = 0; t.minute = 0; t.second = 0;
    CHECK("leap day epoch", hype_rtc_to_unix(&t) == 1709164800u);
    t.year = 1999; t.month = 12; t.day = 31; t.hour = 23; t.minute = 59; t.second = 59;
    CHECK("just before 2000 epoch", hype_rtc_to_unix(&t) == 946684799u);
    CHECK("invalid time -> 0", hype_rtc_to_unix(0) == 0u);
}

/* ---------------------------------------------------------------------
 * core/fs_ops.c wiring: hype_fs_create/unlink/mkdir/rmdir/rename/set_time,
 * HYPE_FS_CAP_NAMESPACE, and the set_time -> write-path mtime handoff.
 * --------------------------------------------------------------------- */

static void test_fs_ops_wiring(int has_journal, int has_csum) {
    hype_fs_t fs;
    hype_fs_file_t f;
    hype_rtc_time_t now;
    uint32_t ino;
    uint8_t buf[4];

    build_vol(has_journal, has_csum);
    CHECK("mount_auto claims the ext volume", hype_fs_mount_auto(&fs, vol_read, vol_write, 0) == 0);
    CHECK("NAMESPACE capability advertised", (hype_fs_caps(&fs) & HYPE_FS_CAP_NAMESPACE) != 0u);

    now.year = 2026; now.month = 8; now.day = 22; now.hour = 12; now.minute = 0; now.second = 0;
    hype_fs_set_time(&fs, &now);

    CHECK("fs_create", hype_fs_create(&fs, "/w.txt", &f) == 0);
    CHECK("fs create returns a live, appendable handle", hype_fs_append(&f, "hi", 2u) == 0);
    ino = ino_of("/w.txt");
    CHECK("set_time stamped the new inode's ctime",
         get32(ino_ptr(ino) + 0x0C) == hype_rtc_to_unix(&now));

    CHECK("fs_mkdir", hype_fs_mkdir(&fs, "/d") == 0);
    CHECK("fs_rename", hype_fs_rename(&fs, "/w.txt", "/d/w.txt") == 0);
    CHECK("fs lookup finds it at the new path", hype_fs_lookup(&fs, "/d/w.txt", &f) == 0);
    CHECK("byte-exact after rename", hype_fs_read_at(&f, 0, buf, 2u) == 0 && buf[0] == 'h' &&
                                        buf[1] == 'i');
    CHECK("fs_unlink", hype_fs_unlink(&fs, "/d/w.txt") == 0);
    CHECK("fs_rmdir", hype_fs_rmdir(&fs, "/d") == 0);
    CHECK("fs_sync is a clean no-op success", hype_fs_sync(&fs) == 0);

    {
        hype_fs_t rofs;
        CHECK("read-only mount_auto still claims the volume",
             hype_fs_mount_auto(&rofs, vol_read, 0, 0) == 0);
        CHECK("a read-only mount loses NAMESPACE",
             (hype_fs_caps(&rofs) & HYPE_FS_CAP_NAMESPACE) == 0u);
    }
}

/* ---------------------------------------------------------------------
 * Open-gate refusals: every superblock/journal shape core/ext2_namespace.c
 * and core/extj_namespace.c refuse outright, one mutation at a time from a
 * known-good baseline.
 * --------------------------------------------------------------------- */

static void test_gates_ext2(void) {
    uint8_t *sb = g_vol + 1024;

    build_vol(0, 0);
    CHECK("ext2 gate: baseline works",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g0", 0) == 0);

    build_vol(0, 0);
    put16(sb + 0x38, 0u); /* bad magic */
    CHECK("ext2 gate: bad magic refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put32(sb + 0x60, 0u); /* INCOMPAT_FILETYPE missing */
    CHECK("ext2 gate: missing filetype refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put32(sb + 0x60, get32(sb + 0x60) | 0x00000040u); /* an incompat bit this writer refuses */
    CHECK("ext2 gate: unsupported incompat bit refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put32(sb + 0x64, 0xFFFFFFFFu); /* every rocompat bit */
    CHECK("ext2 gate: unsupported rocompat refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put16(sb + 0x3A, 0x0002u); /* STATE_ERROR */
    CHECK("ext2 gate: error state refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put16(sb + 0x3A, 0x0000u); /* not VALID */
    CHECK("ext2 gate: not-valid state refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put32(sb + 0x18, 3u); /* log_block_size > 2 (> 4096-byte blocks) */
    CHECK("ext2 gate: oversized block refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put32(sb + 0x20, 0u); /* blocks_per_group == 0 */
    CHECK("ext2 gate: zero blocks_per_group refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put32(sb + 0x28, 0u); /* inodes_per_group == 0 */
    CHECK("ext2 gate: zero inodes_per_group refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put32(sb + 0x54, 0u); /* first_ino below 1 */
    CHECK("ext2 gate: zero first_ino refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put32(sb + 0x54, INODES_PER_GROUP + 100u); /* first_ino past inodes_count */
    CHECK("ext2 gate: first_ino past inodes_count refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(0, 0);
    put16(sb + 0x58, 64u); /* inode_size < 128 */
    CHECK("ext2 gate: undersized inode refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);
}

static void test_gates_extj(void) {
    uint8_t *sb = g_vol + 1024;

    build_vol(1, 0);
    CHECK("extj gate: baseline works",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g0", 0) == 0);

    build_vol(1, 0);
    put32(sb + 0x60, get32(sb + 0x60) | 0x00000004u); /* INCOMPAT_RECOVER */
    CHECK("extj gate: unreplayed journal refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    put32(sb + 0x60, get32(sb + 0x60) | 0x00000008u); /* INCOMPAT_JOURNAL_DEV */
    CHECK("extj gate: external journal device refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    put32(sb + 0x60, 0u); /* INCOMPAT_FILETYPE missing entirely */
    CHECK("extj gate: missing filetype refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    put32(sb + 0x64, 0xFFFFFFFFu); /* every rocompat bit, incl. BIGALLOC */
    CHECK("extj gate: unsupported rocompat refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 1);
    sb[0x175] = 2u; /* s_checksum_type != crc32c */
    CHECK("extj gate: unknown checksum algorithm refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    put16(sb + 0x3A, 0x0002u); /* STATE_ERROR */
    CHECK("extj gate: error state refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    put32(sb + 0x18, 3u);
    CHECK("extj gate: oversized block refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    put32(sb + 0xE0, 99u); /* s_journal_inum != 8 */
    CHECK("extj gate: relocated journal inode refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    put32(sb + 0x20, 0u);
    CHECK("extj gate: zero blocks_per_group refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    put32(sb + 0x54, 0u);
    CHECK("extj gate: zero first_ino refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    {
        /* corrupt the journal's own superblock magic: hype_jbd2_open must
         * refuse, exercising v_open's jbd2_open failure path. */
        uint8_t *jsb = blk(JBLK);
        put32be(jsb + 0, 0u);
    }
    CHECK("extj gate: corrupt journal superblock refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);

    build_vol(1, 0);
    {
        /* the journal inode no longer looks like a regular file: jmap
         * (hype_ext_map_ino_rmap) refuses it. */
        uint8_t *in = ino_ptr(JOURNAL_INO);
        put16(in + 0x00, 0x41EDu); /* directory, not a file */
    }
    CHECK("extj gate: non-regular journal inode refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g", 0) != 0);
}

/* ---------------------------------------------------------------------
 * A file with more than one link at unlink time (as a hard link made by some
 * other tool would leave it -- hard links themselves are out of #498's
 * scope, but unlink must still only DECREMENT, never free, an inode that
 * still has one).
 * --------------------------------------------------------------------- */

static void test_unlink_extra_link(int has_journal) {
    uint32_t ino;
    build_vol(has_journal, 0);
    CHECK("extra-link: create ok", hype_ext_ns_create(vol_read, vol_write, 0, "/l.txt", 1u) == 0);
    ino = ino_of("/l.txt");
    put16(ino_ptr(ino) + 0x1A, 2u); /* simulate a second name pointing here */
    CHECK("extra-link: unlink ok", hype_ext_ns_unlink(vol_read, vol_write, 0, "/l.txt", 0) == 0);
    CHECK("extra-link: the name is gone", !exists("/l.txt"));
    CHECK("extra-link: link count only decremented", inode_links(ino) == 1u);
    CHECK("extra-link: inode NOT freed", inode_bit(ino) == 1);
}

/* ---------------------------------------------------------------------
 * RO_COMPAT_GDT_CSUM (the older crc16 group-descriptor checksum, distinct
 * from RO_COMPAT_METADATA_CSUM's crc32c) on the journaled backend.
 * --------------------------------------------------------------------- */

static void test_gdt_csum(void) {
    uint8_t *sb = g_vol + 1024;
    build_vol(1, 0);
    put32(sb + 0x64, get32(sb + 0x64) | 0x0010u); /* RO_COMPAT_GDT_CSUM */
    CHECK("gdt_csum: create ok", hype_ext_ns_create(vol_read, vol_write, 0, "/gc.txt", 1u) == 0);
    CHECK("gdt_csum: resolves", exists("/gc.txt"));
    CHECK("gdt_csum: mkdir ok", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/gcd", 1u) == 0);
    CHECK("gdt_csum: unlink ok", hype_ext_ns_unlink(vol_read, vol_write, 0, "/gc.txt", 0) == 0);
    CHECK("gdt_csum: rmdir ok", hype_ext_ns_rmdir(vol_read, vol_write, 0, "/gcd", 0) == 0);
}

/* ---------------------------------------------------------------------
 * INCOMPAT_64BIT: wider (64-byte) group descriptors, exercising every
 * lo/hi field pair core/extj_namespace.c carries for #496 compatibility.
 * All hi halves are legitimately 0 on a volume this small -- the point is
 * the WIDER descriptor shape and its checksum hash extending over the
 * added bytes, not a genuinely nonzero high half (core/ext_jalloc.c's own
 * #496 tests already cover a real nonzero high half on a synthetic huge
 * volume; this ticket only needs to prove the namespace writers carry the
 * SAME width correctly).
 * --------------------------------------------------------------------- */

static void enable_64bit_desc(void) {
    uint8_t *sb = g_vol + 1024;
    put32(sb + 0x60, get32(sb + 0x60) | 0x0080u); /* INCOMPAT_64BIT */
    put16(sb + 0xFE, 64u);                        /* s_desc_size */
}

static void test_64bit_desc(void) {
    build_vol(1, 1); /* journaled + metadata_csum */
    enable_64bit_desc();
    CHECK("64bit desc: create ok", hype_ext_ns_create(vol_read, vol_write, 0, "/six4.txt", 1u) == 0);
    CHECK("64bit desc: resolves", exists("/six4.txt"));
    CHECK("64bit desc: mkdir ok", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/six4d", 1u) == 0);
    CHECK("64bit desc: rename ok",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/six4.txt", "/six4d/six4.txt", 1u) == 0);
    CHECK("64bit desc: unlink ok",
         hype_ext_ns_unlink(vol_read, vol_write, 0, "/six4d/six4.txt", 1u) == 0);
    CHECK("64bit desc: rmdir ok", hype_ext_ns_rmdir(vol_read, vol_write, 0, "/six4d", 1u) == 0);

    build_vol(1, 0); /* journaled + the older gdt_csum (crc16), ALSO 64bit */
    {
        uint8_t *sb = g_vol + 1024;
        put32(sb + 0x64, get32(sb + 0x64) | 0x0010u); /* RO_COMPAT_GDT_CSUM */
    }
    enable_64bit_desc();
    CHECK("64bit+gdt_csum: create ok",
         hype_ext_ns_create(vol_read, vol_write, 0, "/g64.txt", 1u) == 0);
    CHECK("64bit+gdt_csum: mkdir ok", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/g64d", 1u) == 0);
}

/* ---------------------------------------------------------------------
 * A completely full volume: every allocation path's "no room" refusal.
 * --------------------------------------------------------------------- */

static void fill_all_blocks(uint8_t *bitmap, uint32_t nbits) {
    uint32_t i;
    for (i = 0; i < nbits; i++) bitmap_set(bitmap, i);
}

/* ---------------------------------------------------------------------
 * split_path's own refusals: "." / ".." as the target LEAF name, and a
 * path so long it cannot fit the scratch buffer.
 * --------------------------------------------------------------------- */

static void test_path_edges(int has_journal) {
    char longpath[600];
    unsigned i;

    build_vol(has_journal, 0);
    CHECK("create refuses a bare .", hype_ext_ns_create(vol_read, vol_write, 0, "/.", 0) != 0);
    CHECK("create refuses a bare ..", hype_ext_ns_create(vol_read, vol_write, 0, "/..", 0) != 0);
    CHECK("mkdir refuses a bare .", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/.", 0) != 0);
    CHECK("mkdir refuses a bare ..", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/..", 0) != 0);
    CHECK("rename refuses renaming onto a bare .",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/htree", "/.", 0) != 0);

    longpath[0] = '/';
    for (i = 1; i < sizeof longpath - 1u; i++) longpath[i] = 'a';
    longpath[sizeof longpath - 1u] = '\0';
    CHECK("create refuses an oversized path",
         hype_ext_ns_create(vol_read, vol_write, 0, longpath, 0) != 0);
}

/* ---------------------------------------------------------------------
 * is_ancestor_or_self's own refusals beyond the simple direct-parent case.
 * --------------------------------------------------------------------- */

static void test_ancestor_guard_depth(int has_journal) {
    build_vol(has_journal, 0);
    CHECK("mkdir /a", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/a", 0) == 0);
    CHECK("mkdir /a/b", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/a/b", 0) == 0);
    CHECK("mkdir /a/b/c", hype_ext_ns_mkdir(vol_read, vol_write, 0, "/a/b/c", 0) == 0);
    /* three levels deep: the ancestor walk must climb past /a/b to find /a */
    CHECK("rename refuses /a into its own great-grandchild",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/a", "/a/b/c/x", 0) != 0);
    CHECK("rename refuses /a/b into its own child",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/a/b", "/a/b/c/y", 0) != 0);
    /* a sibling move (not an ancestor relationship) must still be allowed */
    CHECK("mkdir /z (an unrelated destination)",
         hype_ext_ns_mkdir(vol_read, vol_write, 0, "/z", 0) == 0);
    CHECK("rename /a/b/c -> /z/c (unrelated, allowed)",
         hype_ext_ns_rename(vol_read, vol_write, 0, "/a/b/c", "/z/c", 0) == 0);
}

static void test_full_volume(int has_journal) {
    build_vol(has_journal, 0);
    /* exhaust every inode: claim_inode must refuse cleanly */
    fill_all_blocks(blk(IBITMAP_BLK), INODES_PER_GROUP);
    put32(g_vol + 1024 + 0x10, 0u);
    put16(blk(2) + 0x0E, 0u);
    CHECK("full inodes: create refused",
         hype_ext_ns_create(vol_read, vol_write, 0, "/full", 0) != 0);
    CHECK("full inodes: mkdir refused",
         hype_ext_ns_mkdir(vol_read, vol_write, 0, "/full", 0) != 0);

    build_vol(has_journal, 0);
    /* exhaust every block: claim_block must refuse cleanly (mkdir needs a
     * fresh data block even when an inode is free) */
    fill_all_blocks(blk(BITMAP_BLK), VOL_BLOCKS);
    put32(g_vol + 1024 + 0x0C, 0u);
    put16(blk(2) + 0x0C, 0u);
    CHECK("full blocks: mkdir refused",
         hype_ext_ns_mkdir(vol_read, vol_write, 0, "/full", 0) != 0);
}

/* ---------------------------------------------------------------------
 * The public dispatcher's own probe (core/ext_namespace.c).
 * --------------------------------------------------------------------- */

static void test_dispatcher(void) {
    CHECK("dispatcher create: null read refused", hype_ext_ns_create(0, vol_write, 0, "/x", 0) != 0);
    CHECK("dispatcher unlink: null read refused", hype_ext_ns_unlink(0, vol_write, 0, "/x", 0) != 0);
    CHECK("dispatcher mkdir: null read refused", hype_ext_ns_mkdir(0, vol_write, 0, "/x", 0) != 0);
    CHECK("dispatcher rmdir: null read refused", hype_ext_ns_rmdir(0, vol_write, 0, "/x", 0) != 0);
    CHECK("dispatcher rename: null read refused",
         hype_ext_ns_rename(0, vol_write, 0, "/x", "/y", 0) != 0);

    build_vol(0, 0);
    {
        uint8_t *sb = g_vol + 1024;
        put16(sb + 0x38, 0u);
        CHECK("dispatcher create: bad magic refused (probe)",
             hype_ext_ns_create(vol_read, vol_write, 0, "/x", 0) != 0);
        CHECK("dispatcher unlink: bad magic refused (probe)",
             hype_ext_ns_unlink(vol_read, vol_write, 0, "/x", 0) != 0);
        CHECK("dispatcher mkdir: bad magic refused (probe)",
             hype_ext_ns_mkdir(vol_read, vol_write, 0, "/x", 0) != 0);
        CHECK("dispatcher rmdir: bad magic refused (probe)",
             hype_ext_ns_rmdir(vol_read, vol_write, 0, "/x", 0) != 0);
        CHECK("dispatcher rename: bad magic refused (probe)",
             hype_ext_ns_rename(vol_read, vol_write, 0, "/x", "/y", 0) != 0);
    }
}

/* ---------------------------------------------------------------------
 * Fault sweep: fail the Nth I/O call, for every N up to a generous bound,
 * across a full create/mkdir/rename/unlink/rmdir sequence on every backend
 * shape. Exercises the "an I/O call failed -> return -1, unwind" branch
 * every claim/free/commit/read path carries -- there is no way to reach
 * most of them other than actually failing a specific call, the same
 * reasoning core/tests/test_ext.c's own test_fault_sweep already applies to
 * the read side. Not a correctness check (a failure mid-sequence can
 * legitimately leave later steps refused too) -- a crash-safety sweep, same
 * bar as test_ext.c's.
 * --------------------------------------------------------------------- */

/* Rebuilds a fault-free /s.txt + /sd (and, past `stage`, /sd/s.txt) so the
 * operation under test starts from a REAL, fully-formed state -- sweeping
 * all five ops at the SAME shared k (as an earlier version of this sweep
 * did) mostly exercised each op's own EARLY refusals, because at any k
 * small enough to probe one op's deep internals, the EARLIER setup ops
 * sharing that same k had usually already failed first, leaving nothing
 * real for the op under test to work on. stage: 0 = just /s.txt + /sd,
 * 1 = also renamed to /sd/s.txt. */
static void sweep_setup(int has_journal, int has_csum, int stage) {
    build_vol(has_journal, has_csum);
    g_fail_at = -1;
    (void)hype_ext_ns_create(vol_read_flaky, vol_write_flaky, 0, "/s.txt", 1u);
    (void)hype_ext_ns_mkdir(vol_read_flaky, vol_write_flaky, 0, "/sd", 1u);
    if (stage >= 1) {
        (void)hype_ext_ns_rename(vol_read_flaky, vol_write_flaky, 0, "/s.txt", "/sd/s.txt", 1u);
    }
}

static void test_fault_sweep_ns(void) {
    long k;
    int hj, hc;
    for (hj = 0; hj <= 1; hj++) {
        for (hc = 0; hc <= (hj ? 1 : 0); hc++) {
            for (k = 0; k < 320; k++) {
                sweep_setup(hj, hc, 0);
                g_io_calls = 0;
                g_fail_at = k;
                (void)hype_ext_ns_create(vol_read_flaky, vol_write_flaky, 0, "/s2.txt", 1u);

                sweep_setup(hj, hc, 0);
                g_io_calls = 0;
                g_fail_at = k;
                (void)hype_ext_ns_mkdir(vol_read_flaky, vol_write_flaky, 0, "/sd2", 1u);

                sweep_setup(hj, hc, 0);
                g_io_calls = 0;
                g_fail_at = k;
                (void)hype_ext_ns_rename(vol_read_flaky, vol_write_flaky, 0, "/s.txt", "/sd/s.txt",
                                        1u);

                sweep_setup(hj, hc, 1);
                g_io_calls = 0;
                g_fail_at = k;
                (void)hype_ext_ns_unlink(vol_read_flaky, vol_write_flaky, 0, "/sd/s.txt", 1u);

                sweep_setup(hj, hc, 0); /* /sd is empty here: rmdir's real target */
                g_io_calls = 0;
                g_fail_at = k;
                (void)hype_ext_ns_rmdir(vol_read_flaky, vol_write_flaky, 0, "/sd", 1u);

                /* a cross-parent DIRECTORY rename (the ".." relocation +
                 * both parents' link-count path) needs its own setup: /sd
                 * and /sd2 both exist, /sd/inner is what actually moves. */
                build_vol(hj, hc);
                g_fail_at = -1;
                (void)hype_ext_ns_mkdir(vol_read_flaky, vol_write_flaky, 0, "/sd", 1u);
                (void)hype_ext_ns_mkdir(vol_read_flaky, vol_write_flaky, 0, "/sd2", 1u);
                (void)hype_ext_ns_mkdir(vol_read_flaky, vol_write_flaky, 0, "/sd/inner", 1u);
                g_io_calls = 0;
                g_fail_at = k;
                (void)hype_ext_ns_rename(vol_read_flaky, vol_write_flaky, 0, "/sd/inner",
                                        "/sd2/inner", 1u);
            }
        }
    }
    g_fail_at = -1;
    CHECK("namespace fault sweep completed without crashing", 1);
}

int main(void) {
    test_dirent_basic();
    test_dirent_full_and_grow_refusal();
    test_dirent_csum_tail();
    test_dirent_corrupt_chain();

    test_ns_lifecycle(0, 0); /* ext2 */
    test_ns_lifecycle(1, 0); /* ext3-shaped: journaled, no checksum */
    test_ns_lifecycle(1, 1); /* ext4-shaped: journaled + metadata_csum */

    test_ns_dir_growth(0, 0);
    test_ns_dir_growth(1, 0);
    test_ns_dir_growth(1, 1);

    test_ns_checksum_consistency();
    test_resolve_dir_ino();
    test_rtc_to_unix();

    test_fs_ops_wiring(0, 0);
    test_fs_ops_wiring(1, 0);
    test_fs_ops_wiring(1, 1);

    test_gates_ext2();
    test_gates_extj();
    test_unlink_extra_link(0);
    test_unlink_extra_link(1);
    test_gdt_csum();
    test_64bit_desc();
    test_path_edges(0);
    test_path_edges(1);
    test_ancestor_guard_depth(0);
    test_ancestor_guard_depth(1);
    test_full_volume(0);
    test_full_volume(1);
    test_dispatcher();
    test_fault_sweep_ns();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", failures);
    return 1;
}
