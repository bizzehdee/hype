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
#define RESECTOR_FAT0 RESERVED /* first sector of FAT copy 0, for the tests that walk it */
static uint8_t g_vol[VOL_SECTORS * SECSZ];
static uint64_t g_fail_write_lba = (uint64_t)-1;
static uint64_t g_fail_read_lba = (uint64_t)-1;
static uint32_t g_total_sectors = VOL_SECTORS; /* BPB total (may be shrunk per test) */
static long g_read_countdown = -1;  /* if >=0, fail the read that hits 0 */
static long g_write_countdown = -1; /* if >=0, fail the write that hits 0 */
static int g_write_hardfail;        /* once the countdown fires, keep failing */
static unsigned int g_read_calls;
static unsigned int g_write_calls;
static unsigned int g_max_write_count;
static unsigned int g_sync_calls;
static long g_sync_countdown = -1;
static int g_sync_hardfail; /* once the countdown fires, every later barrier fails too */
static int g_stale_fat0_reads;
static uint8_t g_stale_fat0[SECSZ];
static hype_fat32_wfile_t *g_corrupt_guard_on_data_write;

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    g_read_calls++;
    if (lba + count > VOL_SECTORS) return -1;
    if (lba == g_fail_read_lba) return -1;
    if (g_read_countdown >= 0) { if (g_read_countdown-- == 0) return -1; }
    if (g_stale_fat0_reads && lba == RESERVED && count == 1u) {
        memcpy(dst, g_stale_fat0, SECSZ);
        return 0;
    }
    memcpy(dst, g_vol + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}
static int vol_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    g_write_calls++;
    if (count > g_max_write_count) g_max_write_count = count;
    if (lba + count > VOL_SECTORS) return -1;
    if (lba == g_fail_write_lba) return -1;
    if (g_write_countdown >= 0) {
        if (g_write_countdown-- == 0) {
            if (g_write_hardfail) g_write_countdown = 0; /* stay failing */
            return -1;
        }
    }
    memcpy(g_vol + lba * SECSZ, src, (size_t)count * SECSZ);
    if (g_corrupt_guard_on_data_write != 0 && lba >= DATA_START) {
        g_corrupt_guard_on_data_write->first_cluster_guard ^= 1u;
        g_corrupt_guard_on_data_write = 0;
    }
    return 0;
}
static int vol_sync(void *ctx) {
    (void)ctx;
    g_sync_calls++;
    if (g_sync_countdown >= 0 && g_sync_countdown-- == 0) {
        if (g_sync_hardfail) g_sync_countdown = 0; /* stay failing */
        return -1;
    }
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
static uint8_t pat(unsigned int i);

static void build_vol(void) {
    uint8_t *bpb = g_vol;
    uint8_t *fsi;
    memset(g_vol, 0, sizeof(g_vol));
    g_read_calls = 0u;
    g_write_calls = 0u;
    g_max_write_count = 0u;
    g_sync_calls = 0u;
    g_sync_countdown = -1;
    g_sync_hardfail = 0;
    g_stale_fat0_reads = 0;
    g_write_hardfail = 0;
    g_corrupt_guard_on_data_write = 0;

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

/* #374: full sectors within one cluster must reach the block backend as one
 * multi-sector transfer. The USB backend supports this directly; splitting
 * here made filesystem throughput pay one command latency per 512 bytes. */
/*
 * #464: the invariant that keeps a volume mountable -- a directory entry must never claim more
 * bytes than its cluster chain holds, at ANY point, including after a failed growth.
 *
 * The operator's validation stick went read-only after every hardware run with Linux reporting
 * "fat_bmap_cluster: request beyond EOF", which is precisely this invariant broken. The cause
 * was growth_rollback() freeing the newly allocated clusters while leaving the on-disk entry at
 * the larger size, so the entry outlived the chain that backed it.
 */
static void test_failed_growth_leaves_entry_within_chain(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint8_t data[1500];
    unsigned int i;
    uint32_t dsz, clus, walked;

    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    hype_fat32_fs_set_sync(&fs, vol_sync);
    CHECK_HEX("create ok", 0, hype_fat32_create(&fs, "GROWFAIL.BIN", &f));
    for (i = 0; i < sizeof data; i++) data[i] = pat(i);
    CHECK_HEX("seed 600", 0, hype_fat32_append(&f, data, 600u));

    /*
     * Fail the DURABILITY BARRIER, not a data write. That is the path that corrupts:
     * flush_metadata() writes the directory entry with the NEW size, then calls sync -- so a
     * sync failure returns -1 with the larger size already on the medium, and the rollback then
     * frees the clusters that size depended on. A failing data write aborts earlier, before the
     * entry is ever published, and cannot reproduce this.
     *
     * The operator's stick has a real barrier (SCSI SYNCHRONIZE CACHE over USB), so this is the
     * live configuration, not a synthetic one.
     */
    g_sync_countdown = 0;
    CHECK("growing append reports failure", hype_fat32_append(&f, data, 900u) != 0);
    g_sync_countdown = -1;

    /* Read the invariant off the volume image, not from the in-memory file. */
    {
        const uint8_t *ent = g_vol + clba(2) * SECSZ + 0;
        dsz = hype_fat_dirent_size(ent);
        clus = hype_fat_dirent_cluster(ent);
    }
    walked = 0u;
    while (clus >= 2u && clus < 0x0FFFFFF8u && walked < 128u) {
        walked++;
        clus = fat0(clus);
    }
    CHECK("chain terminates", walked < 128u);
    CHECK_HEX("entry size fits the chain it claims", 1u, (unsigned)(dsz <= walked * SECSZ));
}

/*
 * #464: the same invariant when the barrier does not recover -- the case the operator's stick
 * actually presents.
 *
 * The first barrier of a growth succeeds, the entry is published at the NEW size, and the barrier
 * that follows it fails and keeps failing. That is not synthetic: #516 found that this stick
 * rejects SYNCHRONIZE CACHE(10) outright, and before that fix a rejected barrier was misread as
 * transport damage, so once one failed the rest of the run failed too.
 *
 * growth_rollback() must still shrink the on-disk entry under those conditions. Restoring it
 * through a DURABLE flush cannot work here: flush_metadata() issues its barrier BEFORE rewriting
 * the entry, so a still-failing barrier aborts the restore, and the chain is then freed anyway --
 * leaving exactly the entry-beyond-chain state this rollback exists to prevent.
 */
static void test_persistent_barrier_failure_never_leaves_entry_past_chain(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint8_t data[1500];
    unsigned int i;
    uint32_t dsz, clus, walked;

    build_vol();
    CHECK_HEX("hardfail mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    hype_fat32_fs_set_sync(&fs, vol_sync);
    CHECK_HEX("hardfail create", 0, hype_fat32_create(&fs, "SYNCDEAD.BIN", &f));
    for (i = 0; i < sizeof data; i++) data[i] = pat(i);
    CHECK_HEX("seed 600 while barriers work", 0, hype_fat32_append(&f, data, 600u));

    /*
     * Fail the SECOND barrier of the growing write and every barrier after it. The first one
     * (ordering the FAT links) succeeds, so the entry does reach the medium at the new size --
     * which is the only way to reach the corrupting window.
     */
    g_sync_countdown = 1;
    g_sync_hardfail = 1;
    CHECK("growing write reports failure", hype_fat32_write_at(&f, 0, data, 1500u) != 0);
    g_sync_countdown = -1;
    g_sync_hardfail = 0;

    {
        const uint8_t *ent = g_vol + clba(2) * SECSZ + 0;
        dsz = hype_fat_dirent_size(ent);
        clus = hype_fat_dirent_cluster(ent);
    }
    walked = 0u;
    while (clus >= 2u && clus < 0x0FFFFFF8u && walked < 128u) {
        walked++;
        clus = fat0(clus);
    }
    CHECK("hardfail chain terminates", walked < 128u);
    CHECK_HEX("entry never claims more than the chain holds", 1u,
              (unsigned)(dsz <= walked * SECSZ));
}

static void test_append_coalesces_contiguous_sectors(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint8_t data[1536];
    unsigned int i;

    build_vol();
    g_vol[0x0D] = 4u; /* four sectors per cluster */
    for (i = 0; i < sizeof data; i++) data[i] = pat(i);
    CHECK_HEX("mount coalescing volume", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create coalescing file", 0,
              hype_fat32_create(&fs, "COAL.BIN", &f));
    g_max_write_count = 0u;
    CHECK_HEX("append three contiguous sectors", 0,
              hype_fat32_append(&f, data, sizeof data));
    CHECK_HEX("one backend transfer covers all data sectors", 3u, g_max_write_count);
}

static uint8_t pat(unsigned int i) { return (uint8_t)(i * 7u + 3u); }

static void test_cluster_growth_uses_durability_barriers(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint8_t data[600];

    build_vol();
    memset(data, 0x5A, sizeof data);
    CHECK_HEX("mount durable volume", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    hype_fat32_fs_set_sync(&fs, vol_sync);
    CHECK_HEX("durable create", 0, hype_fat32_create(&fs, "DUR.LOG", &f));
    CHECK_HEX("create brackets metadata with two barriers", 2u, g_sync_calls);

    CHECK_HEX("first append allocates the initial cluster", 0,
              hype_fat32_append(&f, data, 400u));
    CHECK_HEX("initial cluster publication is bracketed", 4u, g_sync_calls);

    CHECK_HEX("append across cluster boundary", 0,
              hype_fat32_append(&f, data + 400u, 200u));
    CHECK_HEX("cluster extension brackets size commit", 6u, g_sync_calls);
    CHECK_HEX("extended file size committed", 600u,
              hype_fat_dirent_size(g_vol + clba(2) * SECSZ));

    /* A failed pre-metadata barrier must leave the on-disk size inside the
     * already durable chain. The newly allocated cluster may be reclaimed,
     * but fsck must never need to truncate a size beyond the chain. */
    build_vol();
    CHECK_HEX("remount barrier-failure volume", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    hype_fat32_fs_set_sync(&fs, vol_sync);
    CHECK_HEX("create before barrier failure", 0, hype_fat32_create(&fs, "FAIL.LOG", &f));
    CHECK_HEX("seed 400 bytes", 0, hype_fat32_append(&f, data, 400u));
    g_sync_countdown = 0;
    CHECK("extension surfaces failed persistence barrier",
          hype_fat32_append(&f, data + 400u, 200u) != 0);
    CHECK_HEX("failed barrier did not publish larger size", 400u,
              hype_fat_dirent_size(g_vol + clba(2) * SECSZ));
    hype_fat32_fs_set_sync(0, vol_sync); /* NULL is safe */
}

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
    CHECK_HEX("empty file has no cluster", 0u, f.first_cluster);
    CHECK_HEX("empty dirent has no cluster", 0u,
              hype_fat_dirent_cluster(g_vol + clba(2) * SECSZ));
    CHECK_HEX("empty create allocates no data cluster", 0u, fat0(3u));
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
    CHECK_HEX("recreated empty file is chainless", 0u, f.first_cluster);
    CHECK_HEX("recreate reuses dirent slot", clba(2), f.dirent_lba);
    CHECK_HEX("recreate dirent off 0", 0u, f.dirent_off);
    CHECK_HEX("dirent size reset to 0", 0u,
              hype_fat_dirent_size(g_vol + clba(2) * SECSZ + 0));

    /* A distinct second file lands in the next free dirent slot. */
    CHECK_HEX("create second ok", 0, hype_fat32_create(&fs, "B.LOG", &g));
    CHECK("second dirent distinct from first", g.dirent_off != f.dirent_off);
    CHECK_HEX("second empty file is also chainless", 0u, g.first_cluster);
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
    g_fail_write_lba = clba(3u); /* first append lazily allocates cluster 3 */
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

static void test_independent_writers_preserve_fsinfo_count(void) {
    hype_fat32_fs_t fs1, fs2;
    hype_fat32_wfile_t f1, f2;

    build_vol();
    CHECK_HEX("mount shared writer one", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs1));
    CHECK_HEX("mount shared writer two", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs2));
    CHECK_HEX("create shared file one", 0, hype_fat32_create(&fs1, "ONE.LOG", &f1));
    CHECK_HEX("create shared file two", 0, hype_fat32_create(&fs2, "TWO.LOG", &f2));

    CHECK_HEX("writer one allocates", 0, hype_fat32_append(&f1, "a", 1u));
    CHECK_HEX("first allocation decrements exact count", 123u,
              get32(g_vol + 1u * SECSZ + 0x1E8));
    CHECK_HEX("writer two refreshes before allocating", 0,
              hype_fat32_append(&f2, "b", 1u));
    CHECK_HEX("second allocation preserves both decrements", 122u,
              get32(g_vol + 1u * SECSZ + 0x1E8));

    /* fs1 still caches 123. An append that does not change allocation must not
     * write that stale count back over fs2's newer value. */
    CHECK_HEX("writer one appends within its cluster", 0,
              hype_fat32_append(&f1, "c", 1u));
    CHECK_HEX("non-allocating append does not overwrite FSInfo", 122u,
              get32(g_vol + 1u * SECSZ + 0x1E8));
}

/*
 * #377: all files on one mounted volume share one authoritative FAT-sector
 * image. The AMD stick returned an older successful FAT read after VM0 had
 * claimed the next cluster. Without the cache, HYPE then linked to VM0's root.
 * Keep returning the mount-time FAT image and prove two files still receive
 * distinct chains as the first file grows past one cluster.
 */
static void test_shared_mount_survives_stale_fat_reads(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t h, v;
    uint8_t full[SECSZ];

    build_vol();
    memset(full, 'H', sizeof full);
    memcpy(g_stale_fat0, g_vol + RESERVED * SECSZ, SECSZ);
    g_stale_fat0_reads = 1;
    CHECK_HEX("stale-read mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("stale-read create HYPE", 0, hype_fat32_create(&fs, "HYPE.LOG", &h));
    CHECK_HEX("stale-read create VM0", 0, hype_fat32_create(&fs, "VM0.LOG", &v));
    CHECK_HEX("HYPE claims and fills first cluster", 0,
              hype_fat32_append(&h, full, sizeof full));
    CHECK_HEX("VM0 claims its own root", 0, hype_fat32_append(&v, "V", 1u));
    CHECK("initial roots differ", h.first_cluster != v.first_cluster);
    CHECK_HEX("HYPE extends despite stale medium reads", 0,
              hype_fat32_append(&h, "x", 1u));
    CHECK("HYPE extension does not link to VM0 root", fat0(h.first_cluster) != v.first_cluster);
    CHECK_HEX("VM0 root remains end-of-chain", 0x0FFFFFFFu, fat0(v.first_cluster));

    g_stale_fat0_reads = 0;
}

static void test_fat_cache_failure_paths(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t f;
    uint8_t full[SECSZ];

    build_vol();
    memset(full, 0x5A, sizeof full);
    CHECK_HEX("cache-failure mount", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("cache-failure create", 0, hype_fat32_create(&fs, "FAIL.LOG", &f));
    fs.fat_cache_valid = 0;
    g_fail_read_lba = RESERVED;
    CHECK("allocation surfaces FAT cache read failure",
          hype_fat32_append(&f, "x", 1u) != 0);
    g_fail_read_lba = (uint64_t)-1;

    build_vol();
    CHECK_HEX("free-failure mount", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("free-failure create", 0, hype_fat32_create(&fs, "FREE.LOG", &f));
    CHECK_HEX("free-failure seed", 0, hype_fat32_append(&f, "x", 1u));
    fs.fat_cache_valid = 0;
    g_fail_read_lba = RESERVED;
    CHECK("truncate surfaces FAT cache read failure",
          hype_fat32_create(&fs, "FREE.LOG", &f) != 0);
    g_fail_read_lba = (uint64_t)-1;

    /* A valid cache for another FAT-sector offset must be replaced. */
    build_vol();
    CHECK_HEX("cache-offset mount", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("cache-offset create", 0, hype_fat32_create(&fs, "OFFSET.LOG", &f));
    fs.fat_cache_valid = 1;
    fs.fat_cache_off = 1u;
    CHECK_HEX("different cached FAT sector is reloaded", 0,
              hype_fat32_append(&f, full, sizeof full));

    f.tail_cluster = fs.max_cluster + 1u;
    CHECK("invalid in-memory tail fails closed", hype_fat32_append(&f, "x", 1u) != 0);

    build_vol();
    CHECK_HEX("zero-hint mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("zero-hint create", 0, hype_fat32_create(&fs, "ZERO.LOG", &f));
    CHECK_HEX("zero-hint seed", 0, hype_fat32_append(&f, "x", 1u));
    fs.next_free = 0u;
    fs.fsinfo_dirty = 1; /* preserve the injected in-memory state through free_chain */
    CHECK_HEX("truncate repairs zero next-free hint", 0,
              hype_fat32_create(&fs, "ZERO.LOG", &f));
    CHECK("freed cluster becomes the next allocation hint", fs.next_free >= 2u);

    build_vol();
    CHECK_HEX("mid-append guard mount", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("mid-append guard create", 0,
              hype_fat32_create(&fs, "GUARD.LOG", &f));
    g_corrupt_guard_on_data_write = &f;
    CHECK("metadata commit rejects a guard changed during data I/O",
          hype_fat32_append(&f, "x", 1u) != 0);
}

/* #377: the AMD capture contained two valid independent FAT chains, but
 * VM1's directory entry named VM0's first cluster. The remaining VM1 fields
 * were intact. Reproduce that exact single-field mutation and prove the
 * writer fails closed before it can publish a cross-link. */
static void test_chain_root_identity_is_immutable(void) {
    hype_fat32_fs_t fs0, fs1;
    hype_fat32_wfile_t f0, f1;
    uint8_t *ent1;
    uint32_t first0, first1;
    uint32_t saved_guard;

    build_vol();
    CHECK_HEX("identity mount 0", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs0));
    CHECK_HEX("identity mount 1", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs1));
    CHECK_HEX("identity create 0", 0, hype_fat32_create(&fs0, "VM0.LOG", &f0));
    CHECK_HEX("identity create 1", 0, hype_fat32_create(&fs1, "VM1.LOG", &f1));
    CHECK_HEX("identity append 0", 0, hype_fat32_append(&f0, "zero", 4u));
    CHECK_HEX("identity append 1", 0, hype_fat32_append(&f1, "one", 3u));
    first0 = f0.first_cluster;
    first1 = f1.first_cluster;
    ent1 = g_vol + f1.dirent_lba * SECSZ + f1.dirent_off;
    CHECK("writers received independent roots", first0 != first1);

    saved_guard = f1.first_cluster_guard;
    f1.first_cluster = first0; /* the exact observed VM1 <- VM0 mutation */
    CHECK("guard rejects RAM root substitution",
          hype_fat32_append(&f1, "must not write", 14u) != 0);
    CHECK_HEX("RAM substitution is classified as identity", HYPE_FAT32_WFILE_ERR_IDENTITY,
              f1.last_error);
    CHECK_HEX("VM1 dirent keeps its own root", first1, hype_fat_dirent_cluster(ent1));
    CHECK_HEX("VM1 size remains unchanged", 3u, hype_fat_dirent_size(ent1));

    /* Restore RAM, then independently corrupt the existing directory entry.
     * A zero-length append performs metadata validation without touching data. */
    f1.first_cluster = first1;
    f1.first_cluster_guard = saved_guard;
    hype_fat_dirent_set_cluster(ent1, first0);
    CHECK("disk root mismatch is not overwritten",
          hype_fat32_append(&f1, "", 0u) != 0);
    CHECK_HEX("disk substitution is classified as identity", HYPE_FAT32_WFILE_ERR_IDENTITY,
              f1.last_error);
    CHECK_HEX("mismatched disk root remains observable", first0,
              hype_fat_dirent_cluster(ent1));

    hype_fat_dirent_set_cluster(ent1, 0u);
    CHECK("non-empty chainless dirent is rejected",
          hype_fat32_append(&f1, "", 0u) != 0);
    hype_fat_dirent_set_cluster(ent1, first1);
    ent1[0] = 'X';
    CHECK("directory name mismatch is rejected",
          hype_fat32_append(&f1, "", 0u) != 0);
    ent1[0] = 'V';
    f1.dirent_off = SECSZ;
    CHECK("out-of-sector directory offset is rejected",
          hype_fat32_append(&f1, "", 0u) != 0);
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
    /* Fail the FAT copy-0 write so the first append's lazy allocation surfaces it. */
    g_fail_write_lba = RESERVED; /* FAT copy 0, sector 0 */
    CHECK_HEX("chainless create succeeds", 0, hype_fat32_create(&fs, "E.TXT", &f));
    CHECK("append surfaces FAT write error", hype_fat32_append(&f, "x", 1u) != 0);
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
        fs.fat_cache_valid = 0; /* test changed the mounted medium out of band */
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

    /* FSInfo that goes bad AFTER mount: allocation falls back to unknown. */
    build_vol();
    CHECK_HEX("mount ok", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    put32(g_vol + 1u * SECSZ, 0xBADBAD00u);
    CHECK_HEX("create with a corrupt FSInfo", 0, hype_fat32_create(&fs, "OK.TXT", &f));
    CHECK_HEX("append with a corrupt FSInfo", 0, hype_fat32_append(&f, "x", 1u));
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
    CHECK_HEX("allocate after low on-disk hint", 0, hype_fat32_create(&fs, "LOW.TXT", &f));
    CHECK_HEX("append after low on-disk hint", 0, hype_fat32_append(&f, "x", 1u));

    build_vol();
    put32(g_vol + 1u * SECSZ + 0x1EC, 999999u);
    CHECK_HEX("mount high-hint volume", 0,
              hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create after high on-disk hint", 0, hype_fat32_create(&fs, "HIGH.TXT", &f));
    CHECK_HEX("append after high on-disk hint", 0, hype_fat32_append(&f, "x", 1u));
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


/* ---- #382: open-existing + random-position I/O ---- */

static void set_fat_both(uint32_t cl, uint32_t val) {
    unsigned int copy;
    for (copy = 0; copy < NUM_FATS; copy++) {
        put32(g_vol + (RESERVED + copy * FATSZ) * SECSZ + cl * 4u, val);
    }
}

/* Allocated (non-zero) data-cluster entries in FAT copy 0. */
static unsigned int fat_allocated(void) {
    unsigned int n = 0, cl;
    for (cl = 2u; cl < 128u; cl++) {
        if (fat0(cl) != 0u) n++;
    }
    return n;
}

/* Volume-dirty per FAT[1] ClnShutBitMask: bit CLEAR == dirty. */
static int vol_dirty(void) { return (fat0(1u) & 0x08000000u) ? 0 : 1; }

static void mk_file(hype_fat32_fs_t *fs, const char *name, unsigned int len,
                    hype_fat32_wfile_t *w) {
    static uint8_t data[8192];
    unsigned int i;
    for (i = 0; i < len; i++) data[i] = pat(i);
    CHECK_HEX("mk create", 0, hype_fat32_create(fs, name, w));
    if (len) CHECK_HEX("mk append", 0, hype_fat32_append(w, data, len));
}

static void test_open_existing(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t w, o;
    uint8_t buf[1600];
    unsigned int i;

    build_vol();
    CHECK_HEX("mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "F.BIN", 1200u, &w);

    CHECK_HEX("open existing", 0, hype_fat32_open(&fs, "F.BIN", &o));
    CHECK_HEX("open size", 1200, o.size);
    CHECK_HEX("open first == create first", w.first_cluster, o.first_cluster);
    CHECK_HEX("open tail == create tail", w.tail_cluster, o.tail_cluster);

    CHECK_HEX("read whole", 0, hype_fat32_read_at(&o, 0, buf, 1200));
    for (i = 0; i < 1200u; i++) { if (buf[i] != pat(i)) break; }
    CHECK("read whole data", i == 1200u);
    CHECK_HEX("read straddle", 0, hype_fat32_read_at(&o, 500, buf, 200));
    for (i = 0; i < 200u; i++) { if (buf[i] != pat(500u + i)) break; }
    CHECK("read straddle data", i == 200u);
    CHECK("read past EOF refused", hype_fat32_read_at(&o, 1199, buf, 2) != 0);
    CHECK("read overflow refused", hype_fat32_read_at(&o, ~0ull - 1u, buf, 4) != 0);
    CHECK_HEX("read zero-len ok", 0, hype_fat32_read_at(&o, 0, buf, 0));

    /* in-place write: pure data, dirent + FAT untouched */
    {
        uint8_t dirent_before[SECSZ], fat_before[SECSZ];
        memcpy(dirent_before, g_vol + clba(2u) * SECSZ, SECSZ);
        memcpy(fat_before, g_vol + RESERVED * SECSZ, SECSZ);
        CHECK_HEX("write in place", 0, hype_fat32_write_at(&o, 510, "QR", 2));
        CHECK("dirent sector untouched",
              memcmp(dirent_before, g_vol + clba(2u) * SECSZ, SECSZ) == 0);
        CHECK("FAT untouched", memcmp(fat_before, g_vol + RESERVED * SECSZ, SECSZ) == 0);
        CHECK_HEX("in-place data read back", 0, hype_fat32_read_at(&o, 510, buf, 2));
        CHECK("in-place data", buf[0] == 'Q' && buf[1] == 'R');
    }

    /* refusals */
    CHECK("open missing refused", hype_fat32_open(&fs, "NOPE.BIN", &o) != 0);
    CHECK_HEX("mkdir for open test", 0, hype_fat32_mkdir(&fs, "D"));
    CHECK("open a directory refused", hype_fat32_open(&fs, "D", &o) != 0);

    /* chain shorter than size */
    set_fat_both(w.first_cluster, 0x0FFFFFFFu); /* EOC after one cluster; size 1200 needs 3 */
    fs.fat_cache_valid = 0;
    CHECK("short chain refused", hype_fat32_open(&fs, "F.BIN", &o) != 0);

    /* loop */
    set_fat_both(w.first_cluster, w.first_cluster); /* self-loop */
    fs.fat_cache_valid = 0;
    CHECK("looping chain refused", hype_fat32_open(&fs, "F.BIN", &o) != 0);

    /* free cluster mid-chain */
    set_fat_both(w.first_cluster, 0u);
    fs.fat_cache_valid = 0;
    CHECK("free mid-chain refused", hype_fat32_open(&fs, "F.BIN", &o) != 0);

    /* out-of-range cluster mid-chain */
    set_fat_both(w.first_cluster, 0x0FFFFFF0u); /* above max_cluster, below EOC */
    fs.fat_cache_valid = 0;
    CHECK("out-of-range link refused", hype_fat32_open(&fs, "F.BIN", &o) != 0);

    /* chain longer than the size justifies */
    build_vol();
    CHECK_HEX("remount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "G.BIN", 500u, &w); /* one cluster */
    set_fat_both(w.first_cluster, w.first_cluster + 1u);
    set_fat_both(w.first_cluster + 1u, 0x0FFFFFFFu);
    fs.fat_cache_valid = 0;
    CHECK("slack cluster refused", hype_fat32_open(&fs, "G.BIN", &o) != 0);

    /* size 0 with a chain */
    build_vol();
    CHECK_HEX("remount2", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "H.BIN", 0u, &w);
    {
        uint8_t *root = g_vol + clba(2u) * SECSZ; /* H.BIN is entry 0, 8.3, no LFN */
        put16(root + 26, 3u); /* DIR_FstClusLO = 3 */
    }
    CHECK("zero size with chain refused", hype_fat32_open(&fs, "H.BIN", &o) != 0);
}

static void test_write_at_growth(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t w, o;
    uint8_t buf[2100];
    uint32_t free_before;
    unsigned int i;

    build_vol();
    CHECK_HEX("mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "F.BIN", 700u, &w); /* clusters 3,4; slack 324 bytes in cluster 4 */
    /* plant stale garbage in the slack the size does not cover */
    for (i = 700u; i < 1024u; i++) g_vol[clba(4u) * SECSZ + (i - 512u)] = 0xAB;

    CHECK_HEX("open", 0, hype_fat32_open(&fs, "F.BIN", &o));
    free_before = fs.free_count;

    /* growth within the existing slack: no new cluster */
    CHECK_HEX("grow within slack", 0, hype_fat32_write_at(&o, 900, "Q", 1));
    CHECK_HEX("size grew", 901, o.size);
    CHECK_HEX("no new cluster", 0x0FFFFFFFu, fat0(4u));
    CHECK_HEX("read gap", 0, hype_fat32_read_at(&o, 700, buf, 200));
    for (i = 0; i < 200u; i++) { if (buf[i] != 0u) break; }
    CHECK("slack gap zeroed", i == 200u);
    CHECK_HEX("free count unchanged", free_before, fs.free_count);

    /* growth allocating clusters, with a gap spanning old slack + new */
    CHECK_HEX("grow with gap", 0, hype_fat32_write_at(&o, 2000, "XY", 2));
    CHECK_HEX("size", 2002, o.size);
    CHECK("chain extended", fat0(4u) != 0x0FFFFFFFu);
    CHECK_HEX("free count dropped by 2", free_before - 2u, fs.free_count);
    CHECK("volume clean after success", vol_dirty() == 0);
    /* whole-file verification: pattern, then zeros, then the payload */
    CHECK_HEX("read all", 0, hype_fat32_read_at(&o, 0, buf, 2002));
    for (i = 0; i < 700u; i++) { if (buf[i] != pat(i)) break; }
    CHECK("data prefix intact", i == 700u);
    if (buf[900] != 'Q') { printf("FAIL: mid write lost\n"); failures++; }
    for (i = 700u; i < 2000u; i++) {
        if (i == 900u) continue;
        if (buf[i] != 0u) break;
    }
    CHECK("gap zeroed end to end", i == 2000u);
    CHECK("payload landed", buf[2000] == 'X' && buf[2001] == 'Y');
    /* dirent size published */
    CHECK_HEX("reopen sees new size", 0, hype_fat32_open(&fs, "F.BIN", &o));
    CHECK_HEX("reopen size", 2002, o.size);

    /* growth from a fresh empty file: first cluster is published */
    mk_file(&fs, "E.BIN", 0u, &w);
    CHECK_HEX("open empty", 0, hype_fat32_open(&fs, "E.BIN", &o));
    CHECK_HEX("grow empty", 0, hype_fat32_write_at(&o, 600, "ZZ", 2));
    CHECK_HEX("empty grew", 602, o.size);
    CHECK_HEX("reopen empty-grown", 0, hype_fat32_open(&fs, "E.BIN", &o));
    CHECK_HEX("reopen size 602", 602, o.size);
    CHECK("first cluster published", o.first_cluster >= 2u);
    CHECK_HEX("read gap head", 0, hype_fat32_read_at(&o, 0, buf, 600));
    for (i = 0; i < 600u; i++) { if (buf[i] != 0u) break; }
    CHECK("empty-file gap zeroed", i == 600u);

    /* a fragmented chain still opens and grows: interleave two files */
    build_vol();
    CHECK_HEX("mount3", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    {
        hype_fat32_wfile_t a, b;
        uint8_t d[512];
        for (i = 0; i < 512u; i++) d[i] = pat(i);
        CHECK_HEX("create A", 0, hype_fat32_create(&fs, "A.BIN", &a));
        CHECK_HEX("create B", 0, hype_fat32_create(&fs, "B.BIN", &b));
        /* alternate appends so A's clusters interleave with B's */
        for (i = 0; i < 3u; i++) {
            CHECK_HEX("append A", 0, hype_fat32_append(&a, d, 512));
            CHECK_HEX("append B", 0, hype_fat32_append(&b, d, 512));
        }
        CHECK_HEX("open fragmented", 0, hype_fat32_open(&fs, "A.BIN", &o));
        CHECK_HEX("fragmented size", 1536, o.size);
        CHECK_HEX("fragmented read", 0, hype_fat32_read_at(&o, 1000, buf, 500));
        for (i = 0; i < 500u; i++) { if (buf[i] != pat((1000u + i) % 512u)) break; }
        CHECK("fragmented data", i == 500u);
        CHECK_HEX("fragmented growth", 0, hype_fat32_write_at(&o, 1800, "k", 1));
        CHECK_HEX("fragmented new size", 1801, o.size);
        /* B is untouched */
        CHECK_HEX("open B", 0, hype_fat32_open(&fs, "B.BIN", &o));
        CHECK_HEX("B size", 1536, o.size);
    }
}

static void test_write_at_rollback(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t o, w;
    uint8_t buf[64];
    unsigned int before_alloc;
    uint64_t huge_end;

    build_vol();
    CHECK_HEX("mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "F.BIN", 700u, &w);
    CHECK_HEX("open", 0, hype_fat32_open(&fs, "F.BIN", &o));
    before_alloc = fat_allocated();

    /* disk full: the volume has ~124 free clusters; ask for far more */
    huge_end = (uint64_t)180u * 512u * 2u; /* way past capacity */
    CHECK("oversized growth fails", hype_fat32_write_at(&o, huge_end, "x", 1) != 0);
    CHECK_HEX("size unchanged", 700, o.size);
    CHECK_HEX("allocation restored", before_alloc, fat_allocated());
    CHECK_HEX("tail EOC restored", 0x0FFFFFFFu, fat0(o.tail_cluster));
    CHECK("volume clean after clean rollback", vol_dirty() == 0);
    CHECK_HEX("file still writable", 0, hype_fat32_write_at(&o, 800, "y", 1));
    CHECK_HEX("recovered growth size", 801, o.size);

    /* absurd size: beyond FAT32 chain limits */
    CHECK("beyond-format growth refused",
          hype_fat32_write_at(&o, 0xFFFFFFFF0ull, "x", 1) != 0);

    /* identity guard: dirent tampered between open and growth */
    build_vol();
    CHECK_HEX("mount2", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "F.BIN", 700u, &w);
    CHECK_HEX("open2", 0, hype_fat32_open(&fs, "F.BIN", &o));
    before_alloc = fat_allocated();
    g_vol[clba(2u) * SECSZ + 0] = 'X'; /* rename the 8.3 entry on disk */
    CHECK("growth after tamper refused", hype_fat32_write_at(&o, 1500, "x", 1) != 0);
    CHECK_HEX("identity error reported", HYPE_FAT32_WFILE_ERR_IDENTITY, o.last_error);
    CHECK_HEX("allocation rolled back", before_alloc, fat_allocated());
    CHECK_HEX("size unchanged after tamper", 700, o.size);

    /* zero-length write is a no-op */
    CHECK_HEX("len 0 no-op", 0, hype_fat32_write_at(&o, 5000, buf, 0));
    CHECK_HEX("still old size", 700, o.size);
    /* overflow refused */
    CHECK("offset overflow refused", hype_fat32_write_at(&o, ~0ull, buf, 2) != 0);
}

/* Fail every Nth write in turn across a fixed growth operation; after every
 * failure the volume must reopen consistently at either the old or the new
 * state, with no cross-linked or lost allocation beyond what rollback frees. */
static void test_write_at_fault_sweep(void) {
    long n;
    int reached_success = 0;

    for (n = 0; n < 80 && !reached_success; n++) {
        hype_fat32_fs_t fs;
        hype_fat32_wfile_t o, w;
        int rc;

        build_vol();
        CHECK_HEX("sweep mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
        mk_file(&fs, "F.BIN", 700u, &w);
        CHECK_HEX("sweep open", 0, hype_fat32_open(&fs, "F.BIN", &o));

        g_write_countdown = n;
        rc = hype_fat32_write_at(&o, 2000, "XY", 2);
        g_write_countdown = -1;

        if (rc == 0) {
            reached_success = 1;
            CHECK_HEX("sweep success size", 2002, o.size);
        }
        /* whatever happened, a fresh mount + open must find a valid file */
        {
            hype_fat32_fs_t fs2;
            hype_fat32_wfile_t o2;
            CHECK_HEX("sweep remount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs2));
            if (hype_fat32_open(&fs2, "F.BIN", &o2) != 0) {
                printf("FAIL: sweep reopen validates at n=%ld rc=%d\n", n, rc);
                failures++;
            }
            CHECK("sweep size sane", o2.size == 700u || o2.size == 2002u);
        }
    }
    CHECK("sweep reached success", reached_success);

    /* same sweep on the read side */
    for (n = 0; n < 20; n++) {
        hype_fat32_fs_t fs;
        hype_fat32_wfile_t o, w;
        build_vol();
        CHECK_HEX("rsweep mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
        mk_file(&fs, "F.BIN", 700u, &w);
        CHECK_HEX("rsweep open", 0, hype_fat32_open(&fs, "F.BIN", &o));
        g_read_countdown = n;
        (void)hype_fat32_write_at(&o, 2000, "XY", 2); /* may fail; must not corrupt */
        g_read_countdown = -1;
        {
            hype_fat32_fs_t fs2;
            hype_fat32_wfile_t o2;
            CHECK_HEX("rsweep remount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs2));
            CHECK_HEX("rsweep reopen validates", 0, hype_fat32_open(&fs2, "F.BIN", &o2));
        }
    }
}


static void test_382_edge_branches(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t o, w;
    static uint8_t big[2048];
    uint8_t buf[1200];
    unsigned int i;

    for (i = 0; i < sizeof big; i++) big[i] = pat(i + 9u);

    /* bulk data write + bulk read through span_io */
    build_vol();
    CHECK_HEX("mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "F.BIN", 700u, &w);
    CHECK_HEX("open", 0, hype_fat32_open(&fs, "F.BIN", &o));
    CHECK_HEX("bulk growth write", 0, hype_fat32_write_at(&o, 1024, big, 2048));
    CHECK_HEX("bulk size", 3072, o.size);
    CHECK_HEX("bulk read", 0, hype_fat32_read_at(&o, 1024, buf, 1024));
    for (i = 0; i < 1024u; i++) { if (buf[i] != pat(i + 9u)) break; }
    CHECK("bulk data", i == 1024u);

    /* read failures: bulk and mid-walk */
    g_fail_read_lba = clba(o.first_cluster);
    CHECK("bulk read failure surfaces", hype_fat32_read_at(&o, 0, buf, 512) != 0);
    CHECK("ragged read failure surfaces", hype_fat32_read_at(&o, 10, buf, 8) != 0);
    g_fail_read_lba = (uint64_t)-1;
    g_read_countdown = 0; /* first FAT read of a cold walk */
    o.seek_cluster = 0u;  /* force the walk to consult the FAT */
    (void)hype_fat32_read_at(&o, 2000, buf, 8);
    g_read_countdown = -1;

    /* a corrupted handle fails the walk cleanly */
    o.seek_cluster = 0u;
    o.first_cluster = 1u; /* reserved: never a data cluster */
    CHECK("corrupt handle read refused", hype_fat32_read_at(&o, 0, buf, 8) != 0);

    /* already-dirty volume: the dirty transition is a no-op branch */
    build_vol();
    CHECK_HEX("mount2", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "F.BIN", 700u, &w);
    set_fat_both(1u, 0x07FFFFFFu); /* ClnShut CLEAR == already dirty */
    fs.fat_cache_valid = 0;
    CHECK_HEX("open dirty vol", 0, hype_fat32_open(&fs, "F.BIN", &o));
    CHECK_HEX("growth on dirty volume", 0, hype_fat32_write_at(&o, 1500, "x", 1));
    CHECK("volume clean after", vol_dirty() == 0);

    /* growth from empty failing on the SECOND allocation: old_tail == 0 arm */
    build_vol();
    CHECK_HEX("mount3", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "E.BIN", 0u, &w);
    {
        /* burn all but one data cluster (3..126 used; one left) */
        uint32_t cl;
        for (cl = 3u; cl < 127u; cl++) set_fat_both(cl, 0x0FFFFFFFu); /* leaves ONE free: 127 */
        fs.fat_cache_valid = 0;
        fs.free_count = 0xFFFFFFFFu;
        fs.fsinfo_sector = 0u; /* no FSInfo refresh to overwrite the hand edit */
    }
    CHECK_HEX("open empty", 0, hype_fat32_open(&fs, "E.BIN", &o));
    CHECK("empty growth needing 2 clusters fails", hype_fat32_write_at(&o, 600, "zz", 2) != 0);
    CHECK_HEX("file still empty", 0, o.size);
    CHECK_HEX("no chain root", 0, o.first_cluster);

    /* open() path refusals */
    build_vol();
    CHECK_HEX("mount4", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK("open empty path refused", hype_fat32_open(&fs, "", &o) != 0);
    CHECK("open root refused", hype_fat32_open(&fs, "/", &o) != 0);
    /* open-time fault sweep: no crash point may corrupt or wedge */
    mk_file(&fs, "F.BIN", 700u, &w);
    {
        long n;
        for (n = 0; n < 12; n++) {
            g_read_countdown = n;
            (void)hype_fat32_open(&fs, "F.BIN", &o);
            g_read_countdown = -1;
        }
    }
    CHECK_HEX("open after sweep", 0, hype_fat32_open(&fs, "F.BIN", &o));

    /* size > 0 with a NULL chain root in the dirent */
    {
        uint8_t *root = g_vol + clba(2u) * SECSZ;
        uint8_t save26 = root[26], save27 = root[27], save20 = root[20], save21 = root[21];
        root[26] = root[27] = root[20] = root[21] = 0; /* DIR_FstClus = 0, size stays 700 */
        CHECK("size without chain refused", hype_fat32_open(&fs, "F.BIN", &o) != 0);
        root[26] = save26; root[27] = save27; root[20] = save20; root[21] = save21;
        CHECK_HEX("restored open", 0, hype_fat32_open(&fs, "F.BIN", &o));
    }

    /* overlap growth: starts inside the file, ends past it -- no gap to zero */
    CHECK_HEX("overlap growth", 0, hype_fat32_write_at(&o, 600, big, 200));
    CHECK_HEX("overlap size", 800, o.size);
    CHECK_HEX("overlap read", 0, hype_fat32_read_at(&o, 600, buf, 200));
    for (i = 0; i < 200u; i++) { if (buf[i] != pat(i + 9u)) break; }
    CHECK("overlap data", i == 200u);

    /* bulk in-place data write failing */
    g_fail_write_lba = clba(o.first_cluster);
    CHECK("bulk in-place failure surfaces", hype_fat32_write_at(&o, 0, big, 512) != 0);
    g_fail_write_lba = (uint64_t)-1;

    /* the in-RAM identity guard itself */
    o.first_cluster_guard ^= 1u;
    CHECK("corrupt guard refused", hype_fat32_write_at(&o, 5000, "x", 1) != 0);
    CHECK_HEX("guard error code", HYPE_FAT32_WFILE_ERR_IDENTITY, o.last_error);

    /* missing parent directory */
    CHECK("open under missing dir refused", hype_fat32_open(&fs, "NODIR/X.BIN", &o) != 0);

    /* double fault: the failure that triggers rollback ALSO breaks the
     * rollback writes -- write_at must fail and leave the dirty flag SET */
    build_vol();
    CHECK_HEX("mount6", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    mk_file(&fs, "F.BIN", 700u, &w);
    CHECK_HEX("open6", 0, hype_fat32_open(&fs, "F.BIN", &o));
    g_write_hardfail = 1;
    g_write_countdown = 6; /* first failure lands mid-chain-link */
    CHECK("double fault fails", hype_fat32_write_at(&o, 2000, "xy", 2) != 0);
    g_write_hardfail = 0;
    g_write_countdown = -1;
    CHECK("volume honestly left dirty", vol_dirty() == 1);

    /* dirty-flag write failing up front: nothing else is touched */
    {
        unsigned int alloc_before = fat_allocated();
        g_fail_write_lba = RESERVED; /* FAT copy 0: volume_set_dirty cannot land */
        CHECK("growth with unwritable FAT refused",
              hype_fat32_write_at(&o, 1500, "x", 1) != 0);
        g_fail_write_lba = (uint64_t)-1;
        CHECK_HEX("nothing allocated", alloc_before, fat_allocated());
        CHECK_HEX("size untouched", 700, o.size);
    }

    /* gap zero-fill hitting a bad sector: growth rolls back */
    {
        unsigned int alloc_before;
        build_vol();
        CHECK_HEX("mount5", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
        mk_file(&fs, "F.BIN", 700u, &w);
        CHECK_HEX("open5", 0, hype_fat32_open(&fs, "F.BIN", &o));
        alloc_before = fat_allocated();
        g_fail_write_lba = clba(4u); /* the slack sector the gap zero must rewrite */
        CHECK("gap-zero failure rolls back", hype_fat32_write_at(&o, 2000, "xy", 2) != 0);
        g_fail_write_lba = (uint64_t)-1;
        CHECK_HEX("rollback allocation", alloc_before, fat_allocated());
        CHECK_HEX("rollback size", 700, o.size);
        CHECK_HEX("rollback tail EOC", 0x0FFFFFFFu, fat0(o.tail_cluster));
    }
}

/* spc == 2: the aligned-full-sector zero-fill path inside the old allocation
 * is only reachable when a cluster spans more than one sector. */
static void test_382_spc2(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t w, o;
    uint8_t buf[1200];
    unsigned int i;

    build_vol();
    g_vol[0x0D] = 2; /* sectors per cluster */
    CHECK_HEX("mount spc2", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("spc", 2, fs.spc);
    mk_file(&fs, "F.BIN", 512u, &w); /* one 1024-byte cluster, size sector-aligned */
    CHECK_HEX("open spc2", 0, hype_fat32_open(&fs, "F.BIN", &o));
    /* grow: the gap [512,1024) is a whole aligned sector in the OLD cluster */
    CHECK_HEX("grow spc2", 0, hype_fat32_write_at(&o, 3000, "W", 1));
    CHECK_HEX("size spc2", 3001, o.size);
    CHECK_HEX("read back gap", 0, hype_fat32_read_at(&o, 512, buf, 512));
    for (i = 0; i < 512u; i++) { if (buf[i] != 0u) break; }
    CHECK("aligned gap zeroed", i == 512u);
    CHECK_HEX("read data spc2", 0, hype_fat32_read_at(&o, 0, buf, 512));
    for (i = 0; i < 512u; i++) { if (buf[i] != pat(i)) break; }
    CHECK("prefix intact spc2", i == 512u);
    CHECK_HEX("payload spc2", 0, hype_fat32_read_at(&o, 3000, buf, 1));
    CHECK("payload byte", buf[0] == 'W');

    /* the aligned bulk-zero write failing mid-gap: rollback, file unchanged */
    {
        hype_fat32_wfile_t o2;
        build_vol();
        g_vol[0x0D] = 2;
        CHECK_HEX("mount spc2b", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
        mk_file(&fs, "G.BIN", 512u, &w);
        CHECK_HEX("open spc2b", 0, hype_fat32_open(&fs, "G.BIN", &o2));
        g_fail_write_lba = DATA_START + (w.first_cluster - 2u) * 2u + 1u; /* the slack sector */
        CHECK("bulk gap-zero failure rolls back",
              hype_fat32_write_at(&o2, 3000, "W", 1) != 0);
        g_fail_write_lba = (uint64_t)-1;
        CHECK_HEX("spc2 rollback size", 512, o2.size);
        CHECK_HEX("spc2 tail EOC", 0x0FFFFFFFu, fat0(o2.tail_cluster));
    }
}

/*
 * #584: TWO FILES ON ONE VOLUME MUST NEVER SHARE A CLUSTER.
 *
 * Measured on a real stick and reproduced under QEMU: HYPE.LOG's directory entry claimed 203,837
 * bytes while its chain held 94 clusters (48,128 bytes), and reading the tail gave EIO. Walking the
 * FAT showed why -- HYPE.LOG and CDTEST.LOG shared 60 clusters. CDTEST's second cluster was 37,
 * which HYPE.LOG already owned, so the two chains merged and each file's length became a fiction.
 *
 * A cross-link is the worst class of filesystem bug this writer can have: nothing fails at the time,
 * both files keep "working", and the damage is only visible later as a short read on whichever file
 * the merge orphaned. On a serial-less machine that log IS the evidence, so a silently truncated one
 * is worse than no log.
 *
 * This drives the shape the rig produced -- one fs, two files, appends interleaved, growing past
 * several cluster boundaries each -- and asserts the invariant directly by walking both chains.
 */
static void collect_chain(hype_fat32_fs_t *fs, uint32_t first, uint32_t *out, unsigned int cap,
                          unsigned int *n_out) {
    uint32_t cl = first;
    unsigned int n = 0;
    (void)fs;
    while (cl >= 2u && cl < 0x0FFFFFF8u && n < cap) {
        const uint8_t *fat = g_vol + RESECTOR_FAT0 * SECSZ;
        out[n++] = cl;
        cl = (uint32_t)(fat[cl * 4u] | ((uint32_t)fat[cl * 4u + 1] << 8) |
                        ((uint32_t)fat[cl * 4u + 2] << 16) |
                        ((uint32_t)fat[cl * 4u + 3] << 24)) & 0x0FFFFFFFu;
    }
    *n_out = n;
}

static void test_two_files_never_share_a_cluster(void) {
    hype_fat32_fs_t fs;
    hype_fat32_wfile_t a, b;
    static uint8_t payload[512];
    uint32_t ca[64], cb[64];
    unsigned int na = 0, nb = 0, i, j, shared = 0;
    unsigned int round;

    build_vol();
    memset(payload, 'a', sizeof(payload));
    CHECK_HEX("mount", 0, hype_fat32_fs_mount(vol_read, vol_write, NULL, &fs));
    CHECK_HEX("create A", 0, hype_fat32_create(&fs, "A.LOG", &a));
    /* A gets a head start, exactly as the combined log does before any per-VM sink exists. */
    for (round = 0; round < 6u; round++) {
        CHECK_HEX("append A (head start)", 0, hype_fat32_append(&a, payload, sizeof(payload)));
    }
    /* THEN the second file appears -- a per-VM sink opens when that guest first says anything. */
    CHECK_HEX("create B", 0, hype_fat32_create(&fs, "B.LOG", &b));
    for (round = 0; round < 6u; round++) {
        CHECK_HEX("append A (interleaved)", 0, hype_fat32_append(&a, payload, sizeof(payload)));
        CHECK_HEX("append B (interleaved)", 0, hype_fat32_append(&b, payload, sizeof(payload)));
    }

    collect_chain(&fs, a.first_cluster, ca, 64u, &na);
    collect_chain(&fs, b.first_cluster, cb, 64u, &nb);
    CHECK("A's chain covers its size", (uint64_t)na * SECSZ >= a.size);
    CHECK("B's chain covers its size", (uint64_t)nb * SECSZ >= b.size);
    for (i = 0; i < na; i++) {
        for (j = 0; j < nb; j++) {
            if (ca[i] == cb[j]) shared++;
        }
    }
    if (shared != 0u) {
        printf("       A: %u clusters, B: %u clusters, %u SHARED\n", na, nb, shared);
    }
    CHECK("the two chains are disjoint", shared == 0u);
}

int main(void) {
    test_open_existing();
    test_write_at_growth();
    test_write_at_rollback();
    test_write_at_fault_sweep();
    test_382_edge_branches();
    test_382_spc2();
    test_two_files_never_share_a_cluster();
    test_dirent_timestamp_from_clock();
    test_cluster_growth_uses_durability_barriers();
    test_append_coalesces_contiguous_sectors();
    test_create_append();
    test_truncate_and_second_file();
    test_reject_bad_volume();
    test_write_error_propagates();
    test_grow_root();
    test_deleted_slot_reuse();
    test_volume_full();
    test_mount_rejections();
    test_fsinfo_variants();
    test_independent_writers_preserve_fsinfo_count();
    test_shared_mount_survives_stale_fat_reads();
    test_fat_cache_failure_paths();
    test_chain_root_identity_is_immutable();
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
    test_failed_growth_leaves_entry_within_chain(); /* #464 */
    test_persistent_barrier_failure_never_leaves_entry_past_chain(); /* #464 */
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
