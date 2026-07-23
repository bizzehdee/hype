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
}
static void test_fault_sweep(void) {
    long k;
    for (k = 0; k < 60; k++) {
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

int main(void) {
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
    test_fault_sweep();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
