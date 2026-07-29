#include <stdio.h>
#include <string.h>
#include "../fat_write_fs.h"
#include "../fat_write.h"

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

/* ---- Synthetic FAT32 volume in RAM (512 B/sector, spc=1, 2 FATs) ---- */
#define VOL_SECTORS 200u
#define SECSZ 512u
#define RESERVED 32u
#define NUM_FATS 2u
#define FATSZ 1u
#define DATA_START (RESERVED + NUM_FATS * FATSZ) /* 34 */
static uint8_t g_vol[VOL_SECTORS * SECSZ];
static uint64_t g_fail_write_lba = (uint64_t)-1;
static uint64_t g_fail_read_lba = (uint64_t)-1;
static uint32_t g_total_sectors = VOL_SECTORS; /* BPB total (may be shrunk per test) */
static long g_read_countdown = -1;  /* if >=0, fail the read that hits 0 */
static long g_write_countdown = -1; /* if >=0, fail the write that hits 0 */

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (lba + count > VOL_SECTORS) return -1;
    if (lba == g_fail_read_lba) return -1;
    if (g_read_countdown >= 0) { if (g_read_countdown-- == 0) return -1; }
    memcpy(dst, g_vol + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}
static int vol_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (lba + count > VOL_SECTORS) return -1;
    if (lba == g_fail_write_lba) return -1;
    if (g_write_countdown >= 0) { if (g_write_countdown-- == 0) return -1; }
    memcpy(g_vol + lba * SECSZ, src, (size_t)count * SECSZ);
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
/* Read cluster N's FAT entry directly from FAT copy 0 (test-side verification). */
static uint32_t fat0(uint32_t cl) {
    return get32(g_vol + RESERVED * SECSZ + cl * 4u) & 0x0FFFFFFFu;
}
static uint64_t clba(uint32_t cl) { return DATA_START + (cl - 2u); } /* spc == 1 */

static void build_vol(void) {
    uint8_t *bpb = g_vol;
    uint8_t *fsi;
    memset(g_vol, 0, sizeof(g_vol));

    put16(bpb + 0x0B, 512);   bpb[0x0D] = 1;          /* bytes/sector, spc */
    put16(bpb + 0x0E, RESERVED); bpb[0x10] = NUM_FATS; /* reserved, numFATs */
    put16(bpb + 0x16, 0);     put32(bpb + 0x24, FATSZ); /* FATSz16=0 (FAT32), FATSz32 */
    put32(bpb + 0x2C, 2);     put16(bpb + 0x30, 1);    /* root cluster, FSInfo sector */
    put32(bpb + 0x20, g_total_sectors);                /* total sectors 32 */

    /* FSInfo at sector 1. */
    fsi = g_vol + 1u * SECSZ;
    put32(fsi + 0x000, 0x41615252u);
    put32(fsi + 0x1E4, 0x61417272u);
    put32(fsi + 0x1E8, 124u); /* free count */
    put32(fsi + 0x1EC, 3u);   /* next free hint */

    /* Reserve FAT entries 0/1 and mark the root cluster (2) end-of-chain, both copies. */
    {
        unsigned int copy;
        for (copy = 0; copy < NUM_FATS; copy++) {
            uint8_t *fat = g_vol + (RESERVED + copy * FATSZ) * SECSZ;
            put32(fat + 0, 0x0FFFFFF8u);
            put32(fat + 4, 0x0FFFFFFFu);
            put32(fat + 8, 0x0FFFFFFFu); /* cluster 2 = root = EOC */
        }
    }
}

static uint8_t pat(unsigned int i) { return (uint8_t)(i * 7u + 3u); }

/* Walk the file's chain and gather its data into buf (up to max bytes). */
static unsigned int gather(uint32_t first, uint8_t *buf, unsigned int max) {
    uint32_t cl = first;
    unsigned int n = 0, guard = 0;
    while (cl >= 2u && cl < 0x0FFFFFF8u && guard < 128u) {
        unsigned int k;
        for (k = 0; k < SECSZ && n < max; k++) buf[n++] = g_vol[clba(cl) * SECSZ + k];
        cl = fat0(cl);
        guard++;
    }
    return n;
}

static void test_create_append(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint8_t data[1100];
    uint8_t back[2048];
    unsigned int i, got;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("spc", 1u, fs.spc);
    CHECK_HEX("root cluster", 2u, fs.root_cluster);
    CHECK_HEX("data start", DATA_START, fs.data_start);
    CHECK_HEX("max cluster (fat-capacity bound)", 127u, fs.max_cluster);

    CHECK_HEX("create ok", 0, hype_fat32_create(&fs, "HYPELOG.TXT", &f));
    CHECK_HEX("first cluster == next_free hint 3", 3u, f.first_cluster);
    CHECK_HEX("initial size 0", 0u, (unsigned)f.size);
    CHECK_HEX("dirent at root sector", clba(2), f.dirent_lba);
    CHECK_HEX("dirent off 0", 0u, f.dirent_off);

    for (i = 0; i < sizeof data; i++) data[i] = pat(i);
    CHECK_HEX("append 1000", 0, hype_fat32_append(&f, data, 1000u));
    CHECK_HEX("size after 1000", 1000u, (unsigned)f.size);
    /* 1000 bytes over 512-byte clusters -> clusters 3 (full) + 4 (488 used). */
    CHECK_HEX("FAT[3] -> 4", 4u, fat0(3));
    CHECK_HEX("FAT[4] EOC", 0x0FFFFFFFu, fat0(4));

    CHECK_HEX("append 100 more", 0, hype_fat32_append(&f, data + 1000u, 100u));
    CHECK_HEX("size after 1100", 1100u, (unsigned)f.size);
    /* 1100 bytes -> clusters 3,4 full (1024) + 5 (76 used). */
    CHECK_HEX("FAT[4] -> 5", 5u, fat0(4));
    CHECK_HEX("FAT[5] EOC", 0x0FFFFFFFu, fat0(5));

    /* Directory entry reflects the running size + first cluster. */
    {
        uint8_t *ent = g_vol + clba(2) * SECSZ + 0;
        CHECK_HEX("dirent size 1100", 1100u, hype_fat_dirent_size(ent));
        CHECK_HEX("dirent first cluster 3", 3u, hype_fat_dirent_cluster(ent));
        CHECK("dirent name HYPELOG TXT", memcmp(ent, "HYPELOG TXT", 11) == 0);
    }

    /* Content round-trips through the chain. */
    got = gather(f.first_cluster, back, sizeof back);
    CHECK("gathered >= 1100", got >= 1100u);
    for (i = 0; i < 1100u; i++) {
        if (back[i] != pat(i)) { CHECK_HEX("content byte", pat(i), back[i]); break; }
    }

    /* FSInfo free count decremented by the 3 allocated clusters (124 -> 121). */
    CHECK_HEX("fsinfo free count 121", 121u, get32(g_vol + 1u * SECSZ + 0x1E8));
}

static void test_truncate_and_second_file(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f, g;

    /* Continues from the volume state left by test_create_append. */
    CHECK_HEX("remount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));

    /* Re-creating the same name truncates: old chain (3,4,5) is freed. */
    CHECK_HEX("recreate ok", 0, hype_fat32_create(&fs, "HYPELOG.TXT", &f));
    CHECK_HEX("recreate size 0", 0u, (unsigned)f.size);
    CHECK_HEX("recreate reuses dirent slot", clba(2), f.dirent_lba);
    CHECK_HEX("recreate dirent off 0", 0u, f.dirent_off);
    CHECK_HEX("dirent size reset to 0", 0u,
              hype_fat_dirent_size(g_vol + clba(2) * SECSZ + 0));

    /* A distinct second file lands in the next free dirent slot. */
    CHECK_HEX("create second ok", 0, hype_fat32_create(&fs, "B.LOG", &g));
    CHECK("second dirent distinct from first", g.dirent_off != f.dirent_off);
    CHECK("second first-cluster distinct", g.first_cluster != f.first_cluster);
    {
        CHECK_HEX("append to second ok", 0, hype_fat32_append(&g, "hello", 5u));
        CHECK_HEX("second size 5", 5u, (unsigned)g.size);
        CHECK("second content", memcmp(g_vol + clba(g.first_cluster) * SECSZ, "hello", 5) == 0);
    }
}

static void test_reject_bad_volume(void) {
    hype_fat32_fs_t fs;
    build_vol();
    put16(g_vol + 0x0B, 4096); /* non-512 sector */
    CHECK_HEX("non-512 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    build_vol();
    put16(g_vol + 0x16, 8); /* FAT16-shaped */
    CHECK_HEX("fat16 rejected", (unsigned long long)(-1),
              (unsigned long long)hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
}

static void test_write_error_propagates(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create ok", 0, hype_fat32_create(&fs, "X.TXT", &f));
    g_fail_write_lba = clba(f.first_cluster); /* fail the data-cluster write */
    CHECK("append surfaces write error", hype_fat32_append(&f, "data", 4u) != 0);
    g_fail_write_lba = (uint64_t)-1;
}

/* Fill the root directory's single cluster with 16 valid, distinct entries so a
 * fresh create finds neither a deleted slot nor a 0x00 terminator -> forces the
 * root directory to grow by a cluster. */
static void fill_root_full(void) {
    unsigned int e;
    for (e = 0; e < SECSZ / 32u; e++) {
        uint8_t *ent = g_vol + clba(2) * SECSZ + e * 32u;
        memset(ent, ' ', 11);
        ent[0] = (uint8_t)('A' + e);
        ent[11] = 0x20u;           /* ARCHIVE, not LFN */
        put16(ent + 26, (uint16_t)(20u + e)); /* some first cluster */
    }
}

static void test_grow_root(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    build_vol();
    fill_root_full();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create forces root grow", 0, hype_fat32_create(&fs, "GROWN.TXT", &f));
    /* Root cluster 2 was full -> a new cluster was linked and holds the entry. */
    CHECK("dirent moved off the root's first cluster", f.dirent_lba != clba(2));
    CHECK_HEX("root chain extended (FAT[2] no longer EOC)", 0, (fat0(2) >= 0x0FFFFFF8u) ? 1 : 0);
    CHECK_HEX("append after grow", 0, hype_fat32_append(&f, "x", 1u));
    CHECK_HEX("grown file size 1", 1u, (unsigned)f.size);
}

static void test_deleted_slot_reuse(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    build_vol();
    /* slot 0 deleted (0xE5), slot 1 terminator (0x00) */
    g_vol[clba(2) * SECSZ + 0] = 0xE5u;
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create reuses deleted slot", 0, hype_fat32_create(&fs, "R.TXT", &f));
    CHECK_HEX("reused slot is offset 0", 0u, f.dirent_off);
}

static void test_volume_full(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    /* Only clusters 2 (root) and 3 exist -> after the file's single cluster, a
     * second-cluster append has nowhere to go. */
    g_total_sectors = DATA_START + 2u; /* data_clusters = 2 -> max_cluster = 3 */
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("max cluster 3", 3u, fs.max_cluster);
    CHECK_HEX("create ok", 0, hype_fat32_create(&fs, "F.TXT", &f)); /* takes cluster 3 */
    CHECK("append past capacity fails", hype_fat32_append(&f, g_vol, 513u) != 0);
    g_total_sectors = VOL_SECTORS; /* restore for later tests */
}

static void test_mount_rejections(void) {
    hype_fat32_fs_t fs;
    build_vol(); g_vol[0x0D] = 0; /* spc 0 */
    CHECK("spc 0 rejected", hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) != 0);
    build_vol(); g_vol[0x10] = 0; /* numFATs 0 */
    CHECK("numFATs 0 rejected", hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) != 0);
    build_vol(); put32(g_vol + 0x2C, 1); /* root cluster < 2 */
    CHECK("root<2 rejected", hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) != 0);
    build_vol(); put16(g_vol + 0x0E, 0); /* reserved 0 */
    CHECK("reserved 0 rejected", hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) != 0);
    build_vol(); put32(g_vol + 0x20, DATA_START); /* total <= data_start */
    CHECK("tiny total rejected", hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) != 0);
    build_vol(); put32(g_vol + 0x24, 0); /* FATSz32 == 0 */
    CHECK("zero FATSz32 rejected", hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) != 0);
    /* BPB read failure. */
    build_vol(); g_fail_read_lba = 0;
    CHECK("bpb read failure rejected", hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) != 0);
    g_fail_read_lba = (uint64_t)-1;
}

static void test_fsinfo_variants(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    /* No FSInfo sector -> free count stays "unknown", allocation still works. */
    build_vol(); put16(g_vol + 0x30, 0);
    CHECK_HEX("mount (no fsinfo) ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("free count unknown", 0xFFFFFFFFu, fs.free_count);
    CHECK_HEX("create (no fsinfo) ok", 0, hype_fat32_create(&fs, "N.TXT", &f));
    CHECK_HEX("append (no fsinfo) ok", 0, hype_fat32_append(&f, "hi", 2u));
    /* Invalid FSInfo signature -> also treated as unknown. */
    build_vol(); put32(g_vol + 1u * SECSZ, 0xDEADBEEFu);
    CHECK_HEX("mount (bad fsinfo) ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("bad fsinfo -> unknown", 0xFFFFFFFFu, fs.free_count);
    /* Out-of-range next-free hint is clamped back to cluster 2. */
    build_vol(); put32(g_vol + 1u * SECSZ + 0x1EC, 999999u);
    CHECK_HEX("mount (huge next_free) ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("next_free clamped to 2", 2u, fs.next_free);
}

static void test_lfn_skip(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    build_vol();
    /* An LFN component (attr 0x0F) at slot 0, terminator at slot 1: the walk must
     * skip the LFN and land the new entry in the deleted/free region after it. */
    {
        uint8_t *ent = g_vol + clba(2) * SECSZ;
        memset(ent, 0x20, 11);
        ent[0] = 0x41u;   /* first/last LFN sequence byte */
        ent[11] = 0x0Fu;  /* LFN attribute */
        g_vol[clba(2) * SECSZ + 32] = 0xE5u; /* a reusable slot after the LFN */
    }
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create past LFN ok", 0, hype_fat32_create(&fs, "L.TXT", &f));
    CHECK("entry not placed over the LFN slot", f.dirent_off != 0u);
}

static void test_fat_write_failure(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    /* Fail the FAT copy-0 write so alloc_cluster (via create) surfaces the error. */
    g_fail_write_lba = RESERVED; /* FAT copy 0, sector 0 */
    CHECK("create surfaces FAT write error", hype_fat32_create(&fs, "E.TXT", &f) != 0);
    g_fail_write_lba = (uint64_t)-1;
}

/* ---- #247: unlink, mkdir, rmdir, rename, LFN generation ---- */

/* Test-side raw dirent pointer (root cluster 2, spc == 1: index < 16). */
static uint8_t *root_ent(unsigned int i) { return g_vol + clba(2) * SECSZ + i * 32u; }

static void test_unlink_fat(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint32_t first, free_before;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    free_before = fs.free_count;
    CHECK_HEX("create ok", 0, hype_fat32_create(&fs, "DEAD.DAT", &f));
    CHECK_HEX("append ok", 0, hype_fat32_append(&f, "0123456789", 10u));
    first = f.first_cluster;
    CHECK_HEX("unlink ok", 0, hype_fat32_unlink(&fs, "\\DEAD.DAT"));
    CHECK_HEX("dirent marked deleted", 0xE5u, root_ent(0)[0]);
    CHECK_HEX("chain freed", 0u, fat0(first));
    CHECK_HEX("fsinfo free count restored", free_before,
              get32(g_vol + 1u * SECSZ + 0x1E8));
    CHECK("unlink again fails", hype_fat32_unlink(&fs, "\\DEAD.DAT") != 0);
    CHECK("unlink missing fails", hype_fat32_unlink(&fs, "\\NOPE.DAT") != 0);
    CHECK("unlink the root fails", hype_fat32_unlink(&fs, "\\") != 0);
    /* Directories are refused. */
    CHECK_HEX("mkdir ok", 0, hype_fat32_mkdir(&fs, "\\D1"));
    CHECK("unlink refuses a directory", hype_fat32_unlink(&fs, "\\D1") != 0);
}

static void test_mkdir_fat(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint32_t d1, d2;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("mkdir D1", 0, hype_fat32_mkdir(&fs, "\\D1"));
    CHECK_HEX("dirent attr DIRECTORY", HYPE_FAT_ATTR_DIRECTORY, root_ent(0)[11]);
    CHECK_HEX("dirent size 0", 0u, hype_fat_dirent_size(root_ent(0)));
    d1 = hype_fat_dirent_cluster(root_ent(0));
    CHECK("a cluster was allocated", d1 >= 3u);
    CHECK_HEX("FAT entry end-of-chain", 0x0FFFFFFFu, fat0(d1));
    /* '.' and '..' lead the new cluster; '..' is 0 because the parent is the
     * ROOT -- the classic off-by-one the ticket calls out. */
    {
        uint8_t *dot = g_vol + clba(d1) * SECSZ;
        uint8_t *dotdot = dot + 32u;
        CHECK("'.' name", memcmp(dot, ".          ", 11) == 0);
        CHECK_HEX("'.' attr", HYPE_FAT_ATTR_DIRECTORY, dot[11]);
        CHECK_HEX("'.' cluster is its own", d1, hype_fat_dirent_cluster(dot));
        CHECK("'..' name", memcmp(dotdot, "..         ", 11) == 0);
        CHECK_HEX("'..' cluster 0 for a root parent", 0u, hype_fat_dirent_cluster(dotdot));
        CHECK_HEX("terminator after them", 0x00u, dot[64]);
    }
    /* Nested: '..' must carry the REAL parent cluster. */
    CHECK_HEX("mkdir D1/D2", 0, hype_fat32_mkdir(&fs, "\\D1\\D2"));
    {
        uint8_t ent[32];
        /* D2's entry is the first in D1 (after no '.'/'..' confusion: D1's own
         * cluster starts with '.','..', then D2's entry). */
        uint8_t *e2 = g_vol + clba(d1) * SECSZ + 64u;
        CHECK_HEX("D2 attr", HYPE_FAT_ATTR_DIRECTORY, e2[11]);
        d2 = hype_fat_dirent_cluster(e2);
        memcpy(ent, g_vol + clba(d2) * SECSZ + 32u, 32u);
        CHECK_HEX("D2's '..' points at D1", d1, hype_fat_dirent_cluster(ent));
    }
    /* Files by path, two levels down. */
    CHECK_HEX("create in D1/D2", 0, hype_fat32_create(&fs, "\\D1\\D2\\F.TXT", &f));
    CHECK_HEX("append there", 0, hype_fat32_append(&f, "deep", 4u));
    CHECK("content landed", memcmp(g_vol + clba(f.first_cluster) * SECSZ, "deep", 4) == 0);
    CHECK_HEX("truncate by path", 0, hype_fat32_create(&fs, "\\D1\\D2\\F.TXT", &f));
    CHECK_HEX("truncated size", 0u, (unsigned)f.size);
    /* Refusals. */
    CHECK("mkdir over a directory", hype_fat32_mkdir(&fs, "\\D1") != 0);
    CHECK("mkdir over a file", hype_fat32_mkdir(&fs, "\\D1\\D2\\F.TXT") != 0);
    CHECK("mkdir under a missing parent", hype_fat32_mkdir(&fs, "\\NOPE\\D3") != 0);
    CHECK("mkdir under a file", hype_fat32_mkdir(&fs, "\\D1\\D2\\F.TXT\\D3") != 0);
    CHECK("mkdir of the root", hype_fat32_mkdir(&fs, "\\") != 0);
    CHECK("mkdir with a bad name", hype_fat32_mkdir(&fs, "\\D?1") != 0);
    CHECK("mkdir '.'", hype_fat32_mkdir(&fs, "\\.") != 0);
}

static void test_rmdir_fat(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint32_t d1;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("mkdir ok", 0, hype_fat32_mkdir(&fs, "\\D1"));
    d1 = hype_fat_dirent_cluster(root_ent(0));
    CHECK_HEX("create inside", 0, hype_fat32_create(&fs, "\\D1\\A.TXT", &f));
    CHECK("rmdir refuses a non-empty directory", hype_fat32_rmdir(&fs, "\\D1") != 0);
    CHECK_HEX("unlink the content", 0, hype_fat32_unlink(&fs, "\\D1\\A.TXT"));
    CHECK_HEX("rmdir once empty ('.' and '..' don't count)", 0, hype_fat32_rmdir(&fs, "\\D1"));
    CHECK_HEX("its dirent deleted", 0xE5u, root_ent(0)[0]);
    CHECK_HEX("its cluster freed", 0u, fat0(d1));
    CHECK("rmdir of a missing name", hype_fat32_rmdir(&fs, "\\D1") != 0);
    CHECK("rmdir of the root", hype_fat32_rmdir(&fs, "\\") != 0);
    CHECK_HEX("create a plain file", 0, hype_fat32_create(&fs, "\\F.TXT", &f));
    CHECK("rmdir of a file", hype_fat32_rmdir(&fs, "\\F.TXT") != 0);
}

/* A name that cannot be 8.3 gets a spec-shaped LFN run over a "~N" short name;
 * a second colliding long name gets "~2". */
static void test_lfn_generation(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    /* 20 characters -> 2 LFN entries + the 8.3 entry. */
    CHECK_HEX("create long-named", 0, hype_fat32_create(&fs, "A Long FileName.txt", &f));
    {
        uint8_t *l1 = root_ent(0); /* physical first: logically-last piece */
        uint8_t *l2 = root_ent(1);
        uint8_t *de = root_ent(2);
        uint8_t chk = hype_fat_shortname_checksum(de);
        CHECK_HEX("run head sequence | LAST", 0x42u, l1[0]);
        CHECK_HEX("run head attr", 0x0Fu, l1[11]);
        CHECK_HEX("run head checksum", chk, l1[13]);
        CHECK_HEX("second piece sequence", 0x01u, l2[0]);
        CHECK_HEX("second piece checksum", chk, l2[13]);
        CHECK("short name is ALONGF~1.TXT", memcmp(de, "ALONGF~1TXT", 11) == 0);
        /* Chars land at their spec offsets: piece 1 carries 'A',' ','L'... */
        CHECK_HEX("piece 1 char 0", 'A', l2[1]);
        CHECK_HEX("piece 1 char 1", ' ', l2[3]);
        /* The terminator sits right after the name's last char (the name is 19
         * chars, so within-piece index 6 == spec offset 16), 0xFFFF after. */
        CHECK_HEX("terminator after the last char", 0x0000u,
                  (unsigned)((unsigned)l1[16] | ((unsigned)l1[17] << 8)));
        CHECK_HEX("0xFFFF fill after the terminator", 0xFFFFu,
                  (unsigned)((unsigned)l1[18] | ((unsigned)l1[19] << 8)));
    }
    CHECK_HEX("append via the long name", 0, hype_fat32_append(&f, "hello", 5u));
    /* A different long name colliding on the same stem takes ~2. */
    CHECK_HEX("create colliding long name", 0, hype_fat32_create(&fs, "A Long FileNamf.txt", &f));
    CHECK("second short name is ALONGF~2.TXT", memcmp(root_ent(5), "ALONGF~2TXT", 11) == 0);
    /* Both resolve independently -- and deleting one takes its WHOLE run. */
    CHECK_HEX("unlink the first by long name", 0, hype_fat32_unlink(&fs, "\\A Long FileName.txt"));
    CHECK_HEX("LFN piece 1 deleted", 0xE5u, root_ent(0)[0]);
    CHECK_HEX("LFN piece 2 deleted", 0xE5u, root_ent(1)[0]);
    CHECK_HEX("dirent deleted", 0xE5u, root_ent(2)[0]);
    CHECK("its long name no longer resolves",
          hype_fat32_unlink(&fs, "\\A Long FileName.txt") != 0);
    CHECK("the second survives (case-insensitive lookup)",
          hype_fat32_unlink(&fs, "\\a long filenamF.TXT") == 0);
    /* Truncating an LFN file by its long name reuses the same slot. */
    CHECK_HEX("recreate long-named", 0, hype_fat32_create(&fs, "A Long FileName.txt", &f));
    CHECK_HEX("recreate again (truncate path)", 0,
              hype_fat32_create(&fs, "A LONG FILENAME.TXT", &f));
}

static void test_rename_fat(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint32_t first, d1, d2;
    uint8_t before[32];

    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create ok", 0, hype_fat32_create(&fs, "OLD.TXT", &f));
    CHECK_HEX("append ok", 0, hype_fat32_append(&f, "payload!", 8u));
    first = f.first_cluster;
    memcpy(before, root_ent(0), 32u);

    /* 8.3 -> 8.3: everything but the name survives byte-for-byte. */
    CHECK_HEX("rename 8.3 to 8.3", 0, hype_fat32_rename(&fs, "\\OLD.TXT", "\\NEW.TXT"));
    CHECK_HEX("old slot deleted", 0xE5u, root_ent(0)[0]);
    {
        uint8_t *ne = root_ent(1);
        CHECK("new name", memcmp(ne, "NEW     TXT", 11) == 0);
        CHECK("attrs + stamps + cluster + size preserved", memcmp(ne + 11, before + 11, 21) == 0);
    }
    CHECK("old name gone", hype_fat32_unlink(&fs, "\\OLD.TXT") != 0);

    /* 8.3 -> long name (grows an LFN run), then back (shrinks it away). */
    CHECK_HEX("rename to a long name", 0,
              hype_fat32_rename(&fs, "\\NEW.TXT", "\\A Much Longer Name.dat"));
    CHECK("old 8.3 slot deleted", root_ent(1)[0] == 0xE5u);
    CHECK_HEX("rename back to 8.3", 0,
              hype_fat32_rename(&fs, "\\A Much Longer Name.dat", "\\BACK.TXT"));
    {
        /* The data followed the entry through both renames. */
        uint8_t got[8];
        memcpy(got, g_vol + clba(first) * SECSZ, 8u);
        CHECK("content intact", memcmp(got, "payload!", 8) == 0);
    }

    /* Refusals. */
    CHECK_HEX("create bystander", 0, hype_fat32_create(&fs, "OTHER.TXT", &f));
    CHECK("rename onto an existing name", hype_fat32_rename(&fs, "\\OTHER.TXT", "\\BACK.TXT") != 0);
    CHECK("rename a missing source", hype_fat32_rename(&fs, "\\NOPE.TXT", "\\X.TXT") != 0);
    CHECK("case-only rename is 'exists'", hype_fat32_rename(&fs, "\\OTHER.TXT", "\\other.txt") != 0);
    CHECK("rename to a bad name", hype_fat32_rename(&fs, "\\OTHER.TXT", "\\o<o") != 0);
    CHECK("rename to a missing parent", hype_fat32_rename(&fs, "\\OTHER.TXT", "\\NODIR\\O.TXT") != 0);
    CHECK("rename the root", hype_fat32_rename(&fs, "\\", "\\R") != 0);

    /* Moves. A directory moved between parents gets its '..' re-pointed. */
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("mkdir D1", 0, hype_fat32_mkdir(&fs, "\\D1"));
    CHECK_HEX("mkdir D2", 0, hype_fat32_mkdir(&fs, "\\D2"));
    d1 = hype_fat_dirent_cluster(root_ent(0));
    d2 = hype_fat_dirent_cluster(root_ent(1));
    CHECK_HEX("create in D2", 0, hype_fat32_create(&fs, "\\D2\\F.TXT", &f));
    CHECK_HEX("'..' of D2 starts at root (0)", 0u,
              hype_fat_dirent_cluster(g_vol + clba(d2) * SECSZ + 32u));
    CHECK_HEX("move D2 into D1", 0, hype_fat32_rename(&fs, "\\D2", "\\D1\\D2"));
    CHECK_HEX("'..' re-pointed at D1", d1,
              hype_fat_dirent_cluster(g_vol + clba(d2) * SECSZ + 32u));
    CHECK_HEX("children reachable at the new path", 0,
              hype_fat32_unlink(&fs, "\\D1\\D2\\F.TXT"));
    CHECK("old path gone", hype_fat32_unlink(&fs, "\\D2\\F.TXT") != 0);
    /* Cycle guard. */
    CHECK("move into itself", hype_fat32_rename(&fs, "\\D1", "\\D1\\SUB") != 0);
    CHECK("move into a descendant", hype_fat32_rename(&fs, "\\D1", "\\D1\\D2\\SUB") != 0);
    /* Move a file into a subdirectory and rename it at once. */
    CHECK_HEX("create in root", 0, hype_fat32_create(&fs, "\\MOVE.ME", &f));
    CHECK_HEX("append", 0, hype_fat32_append(&f, "xyz", 3u));
    CHECK_HEX("move + rename", 0, hype_fat32_rename(&fs, "\\MOVE.ME", "\\D1\\Long Moved Name.bin"));
    CHECK_HEX("unlink at the destination", 0, hype_fat32_unlink(&fs, "\\D1\\Long Moved Name.bin"));
}


/* Corrupt volumes and boundary shapes: every defensive leg the clean-path
 * tests cannot reach. A broken chain must fail fast, never spin or "succeed". */
static void test_corrupt_and_boundary(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;

    /* A looping root chain: create must fail, quickly. */
    build_vol();
    put32(g_vol + RESERVED * SECSZ + 2u * 4u, 3u);
    put32(g_vol + RESERVED * SECSZ + 3u * 4u, 2u); /* 2 -> 3 -> 2 */
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK("create over a looping root fails", hype_fat32_create(&fs, "X.TXT", &f) != 0);

    /* A root chain pointing at a nonsense cluster. */
    build_vol();
    put32(g_vol + RESERVED * SECSZ + 2u * 4u, 999u);
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK("create over a broken root fails", hype_fat32_create(&fs, "X.TXT", &f) != 0);
    CHECK("mkdir over a broken root fails", hype_fat32_mkdir(&fs, "\\D") != 0);

    /* rmdir of a directory whose own chain loops: refused, not "empty". */
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("mkdir ok", 0, hype_fat32_mkdir(&fs, "\\D1"));
    {
        uint32_t d1 = hype_fat_dirent_cluster(root_ent(0));
        put32(g_vol + RESERVED * SECSZ + d1 * 4u, d1); /* self-loop */
        CHECK("rmdir of a looping directory refused", hype_fat32_rmdir(&fs, "\\D1") != 0);
    }

    /* Crafted LFN pieces that must be ignored: sequence 0 with the LAST bit,
     * and a sequence past the name maximum. The 8.3 entry after them still
     * resolves by its short name. */
    build_vol();
    {
        uint8_t name11[11];
        memcpy(name11, "REAL    TXT", 11);
        memset(root_ent(0), 0, 32); root_ent(0)[0] = 0x40u; root_ent(0)[11] = 0x0Fu;
        memset(root_ent(1), 0, 32); root_ent(1)[0] = 0x7Fu; root_ent(1)[11] = 0x0Fu;
        memcpy(root_ent(2), name11, 11); root_ent(2)[11] = 0x20u;
        /* A volume label after it is skipped, not matched. */
        memcpy(root_ent(3), "NOLABEL    ", 11); root_ent(3)[11] = 0x08u;
    }
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("entry behind junk LFN pieces resolves", 0,
              hype_fat32_rename(&fs, "\\REAL.TXT", "\\STILL.TXT"));
    CHECK("volume label not matched as a file", hype_fat32_unlink(&fs, "\\NOLABEL") != 0);

    /* An out-of-order LFN run (wrong sequence) is not credited to the entry. */
    build_vol();
    {
        uint8_t chk;
        memcpy(root_ent(2), "WRONG   TXT", 11); root_ent(2)[11] = 0x20u;
        chk = hype_fat_shortname_checksum(root_ent(2));
        hype_fat_lfn_entry_build(root_ent(0), "wrongname.txt", 13u, 2u, 1, chk);
        hype_fat_lfn_entry_build(root_ent(1), "wrongname.txt", 13u, 2u, 0, chk); /* 2 again */
    }
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK("broken run does not name the entry",
          hype_fat32_unlink(&fs, "\\wrongname.txt") != 0);
    CHECK_HEX("its short name still works", 0, hype_fat32_unlink(&fs, "\\WRONG.TXT"));

    /* A checksum-mismatched run is orphaned: ignored for matching. */
    build_vol();
    {
        memcpy(root_ent(1), "MISM    TXT", 11); root_ent(1)[11] = 0x20u;
        hype_fat_lfn_entry_build(root_ent(0), "mismatch.txt", 12u, 1u, 1, 0xEEu); /* bad chk */
    }
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK("orphan run not matched", hype_fat32_unlink(&fs, "\\mismatch.txt") != 0);
    /* Deleting by short name must NOT take the orphan run with it. */
    CHECK_HEX("unlink by short name", 0, hype_fat32_unlink(&fs, "\\MISM.TXT"));
    CHECK("orphan LFN piece left alone", root_ent(0)[0] != 0xE5u);

    /* A file entry with cluster 0 (empty file shape): unlink has no chain to
     * free; truncate-create must not call free_chain either. */
    build_vol();
    {
        memcpy(root_ent(0), "ZERO    DAT", 11); root_ent(0)[11] = 0x20u; /* cluster 0, size 0 */
    }
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("truncate over a chainless entry", 0, hype_fat32_create(&fs, "ZERO.DAT", &f));
    CHECK_HEX("unlink it", 0, hype_fat32_unlink(&fs, "\\ZERO.DAT"));

    /* rmdir of a directory dirent whose cluster field is junk. */
    build_vol();
    {
        memcpy(root_ent(0), "BADDIR     ", 11); root_ent(0)[11] = 0x10u;
        put16(root_ent(0) + 26, 999u);
    }
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK("rmdir of an out-of-range directory refused", hype_fat32_rmdir(&fs, "\\BADDIR") != 0);
    CHECK("descending through it fails too", hype_fat32_create(&fs, "\\BADDIR\\F.TXT", &f) != 0);

    /* A moved directory whose '.'/'..' pair was destroyed: the move still
     * completes -- there is no '..' to re-point -- rather than corrupting
     * whatever sits in those bytes. */
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("mkdir A", 0, hype_fat32_mkdir(&fs, "\\A"));
    CHECK_HEX("mkdir B", 0, hype_fat32_mkdir(&fs, "\\B"));
    {
        uint32_t a = hype_fat_dirent_cluster(root_ent(0));
        memset(g_vol + clba(a) * SECSZ, 0, 64u); /* wipe '.' and '..' */
        CHECK_HEX("move still completes", 0, hype_fat32_rename(&fs, "\\A", "\\B\\A"));
    }

    /* FSInfo that goes bad AFTER mount: flushes are skipped, ops still work. */
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    put32(g_vol + 1u * SECSZ, 0xBADBAD00u);
    CHECK_HEX("create with a corrupt FSInfo", 0, hype_fat32_create(&fs, "OK.TXT", &f));
    CHECK_HEX("unlink with a corrupt FSInfo", 0, hype_fat32_unlink(&fs, "\\OK.TXT"));

    /* The dirent size field saturates at 4 GiB - 1. */
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create ok", 0, hype_fat32_create(&fs, "BIG.BIN", &f));
    f.size = 0x100000001ull;
    CHECK_HEX("append(0) still flushes", 0, hype_fat32_append(&f, "", 0u));
    CHECK_HEX("size clamped in the dirent", 0xFFFFFFFFu, hype_fat_dirent_size(root_ent(0)));

    /* 16-bit BPB total-sector field takes precedence when non-zero. */
    build_vol();
    put16(g_vol + 0x13, (uint16_t)VOL_SECTORS);
    put32(g_vol + 0x20, 0u);
    CHECK_HEX("mount with TotSec16", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("same geometry", 127u, fs.max_cluster);

    /* spc > 1 with too few data sectors for even one cluster. */
    build_vol();
    g_vol[0x0D] = 8u;
    put32(g_vol + 0x20, DATA_START + 7u); /* 7 data sectors < one 8-sector cluster */
    CHECK("no-cluster volume rejected", hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) != 0);

    /* Rename with no final component on either side. */
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create ok", 0, hype_fat32_create(&fs, "R.TXT", &f));
    CHECK("rename to the root", hype_fat32_rename(&fs, "\\R.TXT", "\\") != 0);

    /* Stale free-cluster hints are tolerated by the allocator. */
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    fs.next_free = 0u; /* out of range: the scan restarts from 2 */
    CHECK_HEX("alloc with a junk hint", 0, hype_fat32_create(&fs, "H.TXT", &f));
    fs.next_free = 0u;
    CHECK_HEX("free with a zero hint", 0, hype_fat32_unlink(&fs, "\\H.TXT"));
    /* FSInfo carrying a next-free below 2 is clamped at mount. */
    build_vol();
    put32(g_vol + 1u * SECSZ + 0x1EC, 1u);
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("low next_free clamped", 2u, fs.next_free);
}


/* Remaining defensive legs: allocator hints, forward slashes, over-long
 * components, and LFN runs broken in yet other ways. */
static void test_more_edges(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    char big[300];
    unsigned int i;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    /* Forward slashes are separators too. */
    CHECK_HEX("mkdir with forward slashes", 0, hype_fat32_mkdir(&fs, "/FWD"));
    CHECK_HEX("create with forward slashes", 0, hype_fat32_create(&fs, "/FWD/F.TXT", &f));
    CHECK_HEX("unlink with forward slashes", 0, hype_fat32_unlink(&fs, "/FWD/F.TXT"));
    /* create refuses to clobber a directory, and refuses the root. */
    CHECK("create over a directory", hype_fat32_create(&fs, "FWD", &f) != 0);
    CHECK("create of the root", hype_fat32_create(&fs, "\\", &f) != 0);
    /* A path component longer than any legal name. */
    big[0] = '\\';
    for (i = 1; i < sizeof big - 1u; i++) big[i] = 'a';
    big[sizeof big - 1u] = '\0';
    CHECK("over-long leaf rejected", hype_fat32_create(&fs, big, &f) != 0);
    big[260] = '\\'; big[261] = 'f'; big[262] = '\0';
    CHECK("over-long mid component rejected", hype_fat32_create(&fs, big, &f) != 0);
    /* Allocator hint out of range, and a free count already at zero. */
    fs.next_free = 200u;
    fs.free_count = 0u;
    CHECK_HEX("alloc survives both", 0, hype_fat32_create(&fs, "HINT.TXT", &f));
    /* A dirent whose chain starts past the volume: freeing it is a no-op walk. */
    {
        /* Slot 2 is the first slot before the 0x00 terminator region. */
        memcpy(root_ent(2), "OOR     DAT", 11); root_ent(2)[11] = 0x20u;
        put16(root_ent(2) + 26, 200u); /* > max_cluster 127 */
    }
    CHECK_HEX("unlink an out-of-range chain", 0, hype_fat32_unlink(&fs, "\\OOR.DAT"));
    /* A root cluster number the FAT cannot address: mount accepts the BPB shape
     * but every directory walk must refuse it. */
    build_vol();
    put32(g_vol + 0x2C, 999u);
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK("walks refuse an out-of-range root", hype_fat32_create(&fs, "X.TXT", &f) != 0);
    /* An LFN run whose SECOND piece carries the wrong checksum byte. */
    build_vol();
    {
        uint8_t chk;
        memcpy(root_ent(2), "CHKM    TXT", 11); root_ent(2)[11] = 0x20u;
        chk = hype_fat_shortname_checksum(root_ent(2));
        hype_fat_lfn_entry_build(root_ent(0), "chk mismatch.txt", 16u, 2u, 1, chk);
        hype_fat_lfn_entry_build(root_ent(1), "chk mismatch.txt", 16u, 1u, 0, (uint8_t)(chk ^ 0xFFu));
    }
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK("mid-run checksum break orphans the run",
          hype_fat32_unlink(&fs, "\\chk mismatch.txt") != 0);
    CHECK_HEX("short name still resolves", 0, hype_fat32_unlink(&fs, "\\CHKM.TXT"));
}

/* Sweep a read/write failure across successive I/O operations of a full
 * create + multi-cluster append + truncate cycle, exercising every defensive
 * "I/O failed" error leg. Results are intentionally ignored -- the point is
 * that no path crashes and each failing branch is taken at least once. */
static void run_cycle(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    static uint8_t buf[1500];
    if (hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) != 0) return;
    if (hype_fat32_create(&fs, "SWEEP.TXT", &f) != 0) return;
    if (hype_fat32_append(&f, buf, sizeof buf) != 0) return; /* spans 3 clusters */
    (void)hype_fat32_create(&fs, "SWEEP.TXT", &f); /* re-create -> free_chain path */
    (void)hype_fat32_mkdir(&fs, "SWPDIR");
    (void)hype_fat32_create(&fs, "\\SWPDIR\\A Long Sweep Name.dat", &f);
    (void)hype_fat32_rename(&fs, "\\SWPDIR\\A Long Sweep Name.dat", "\\SWPDIR\\S.DAT");
    (void)hype_fat32_rename(&fs, "\\SWEEP.TXT", "\\SWPDIR\\SWEEP.TXT");
    (void)hype_fat32_unlink(&fs, "\\SWPDIR\\S.DAT");
    (void)hype_fat32_unlink(&fs, "\\SWPDIR\\SWEEP.TXT");
    (void)hype_fat32_rmdir(&fs, "\\SWPDIR");
}
static void test_fault_sweep(void) {
    long k;
    for (k = 0; k < 1400; k += (k < 300 ? 1 : 7)) {
        build_vol();
        g_read_countdown = k; g_write_countdown = -1;
        run_cycle();
        build_vol();
        g_read_countdown = -1; g_write_countdown = k;
        run_cycle();
    }
    /* Same sweep, but with a pre-full root so each cycle grows the root
     * directory -- exercising the grow path's own I/O error legs. */
    for (k = 0; k < 40; k++) {
        hype_fat32_fs_t fs;
        hype_fat32_wfile_t f;
        build_vol(); fill_root_full();
        g_read_countdown = k; g_write_countdown = -1;
        if (hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) == 0)
            (void)hype_fat32_create(&fs, "GROW.TXT", &f);
        build_vol(); fill_root_full();
        g_read_countdown = -1; g_write_countdown = k;
        if (hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs) == 0)
            (void)hype_fat32_create(&fs, "GROW.TXT", &f);
    }
    g_read_countdown = -1; g_write_countdown = -1;
    CHECK("fault sweep completed without crashing", 1);
}


/*
 * #198 follow-up: a real timestamp must reach the directory entry. Before the
 * RTC existed these fields were always zero, which is why hype's own log showed
 * as the Unix epoch. Checks the encoded bytes at their spec offsets, and that
 * clearing the clock restores the old unset behaviour.
 */
static void test_dirent_timestamp_from_clock(void) {
    hype_fat32_fs_t fs;
    hype_rtc_time_t now;
    uint8_t ent[32];
    uint8_t name[11];
    uint16_t d, t;

    now.year = 2026; now.month = 7; now.day = 28;
    now.hour = 14; now.minute = 35; now.second = 6;

    hype_fat_shortname_83("HYPEFULL.LOG", name);
    hype_fat_dirent_build(ent, name, HYPE_FAT_ATTR_ARCHIVE, 3, 100, &now);
    d = (uint16_t)((uint16_t)ent[16] | ((uint16_t)ent[17] << 8));
    t = (uint16_t)((uint16_t)ent[14] | ((uint16_t)ent[15] << 8));
    CHECK_HEX("dirent CrtDate", (46u << 9) | (7u << 5) | 28u, d);
    CHECK_HEX("dirent CrtTime", (14u << 11) | (35u << 5) | 3u, t);
    /* WrtDate/WrtTime and LstAccDate carry the same stamp. */
    CHECK_HEX("dirent WrtDate", d, (uint16_t)((uint16_t)ent[24] | ((uint16_t)ent[25] << 8)));
    CHECK_HEX("dirent WrtTime", t, (uint16_t)((uint16_t)ent[22] | ((uint16_t)ent[23] << 8)));
    CHECK_HEX("dirent LstAccDate", d, (uint16_t)((uint16_t)ent[18] | ((uint16_t)ent[19] << 8)));

    /* No clock -> zeroes, exactly as before. */
    hype_fat_dirent_build(ent, name, HYPE_FAT_ATTR_ARCHIVE, 3, 100, 0);
    CHECK_HEX("dirent no clock -> zero date", 0,
              (uint16_t)((uint16_t)ent[16] | ((uint16_t)ent[17] << 8)));

    /* set_time stores it; passing 0 invalidates it again. */
    fs.now.year = 0;
    hype_fat32_fs_set_time(&fs, &now);
    CHECK_HEX("set_time stored year", 2026, fs.now.year);
    CHECK_HEX("set_time stored second", 6, fs.now.second);
    hype_fat32_fs_set_time(&fs, 0);
    CHECK_HEX("set_time(0) invalidates", 0, fs.now.year);
    hype_fat32_fs_set_time(0, &now); /* must not crash */
}

int main(void) {
    test_dirent_timestamp_from_clock();
    test_create_append();
    test_truncate_and_second_file();
    test_reject_bad_volume();
    test_write_error_propagates();
    test_grow_root();
    test_deleted_slot_reuse();
    test_volume_full();
    test_mount_rejections();
    test_fsinfo_variants();
    test_lfn_skip();
    test_fat_write_failure();
    test_unlink_fat();
    test_mkdir_fat();
    test_rmdir_fat();
    test_lfn_generation();
    test_rename_fat();
    test_corrupt_and_boundary();
    test_more_edges();
    test_fault_sweep();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
