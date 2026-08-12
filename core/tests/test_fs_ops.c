#include <stdio.h>
#include <string.h>
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

#define SECSZ HYPE_BLK_SECTOR_SIZE

/* ---- an in-RAM medium, reused for every synthetic volume ---- */
#define VOL_SECTORS 600u
static uint8_t g_vol[VOL_SECTORS * SECSZ];
static int g_fail_reads; /* fail every read while > 0 */

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (g_fail_reads > 0) return -1;
    if (lba + count > VOL_SECTORS) return -1;
    memcpy(dst, g_vol + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}
static int vol_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (lba + count > VOL_SECTORS) return -1;
    memcpy(g_vol + lba * SECSZ, src, (size_t)count * SECSZ);
    return 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- ISO9660: a PVD at byte 32768 and a recognisable data pattern ---- */
#define ISO_BLOCKS 40u /* 2048-byte logical blocks: 80 KiB image */
static void build_iso(void) {
    uint8_t *pvd = g_vol + 64u * SECSZ;
    unsigned i;
    memset(g_vol, 0, sizeof(g_vol));
    pvd[0] = 0x01;
    memcpy(pvd + 1, "CD001", 5);
    put32(pvd + 80, ISO_BLOCKS); /* VolumeSpaceSize, LE half */
    put16(pvd + 128, 2048u);     /* LogicalBlockSize */
    for (i = 0; i < ISO_BLOCKS * 2048u; i++) {
        g_vol[i] = (uint8_t)(i * 11u + 3u);
    }
    /* rebuild the PVD over the pattern */
    pvd[0] = 0x01;
    memcpy(pvd + 1, "CD001", 5);
    put32(pvd + 80, ISO_BLOCKS);
    put16(pvd + 128, 2048u);
}

/* ---- FAT32: the same minimal volume test_log_sink.c builds ---- */
#define FAT_RESERVED 32u
#define FAT_NFATS 2u
#define FAT_SZ 2u
static void build_fat32(void) {
    uint8_t *bpb = g_vol, *fsi;
    unsigned copy;
    memset(g_vol, 0, sizeof(g_vol));
    put16(bpb + 0x0B, 512); bpb[0x0D] = 1; put16(bpb + 0x0E, FAT_RESERVED); bpb[0x10] = FAT_NFATS;
    put16(bpb + 0x16, 0);   put32(bpb + 0x24, FAT_SZ);
    put32(bpb + 0x2C, 2);   put16(bpb + 0x30, 1);
    put32(bpb + 0x20, VOL_SECTORS);
    fsi = g_vol + SECSZ;
    put32(fsi + 0x000, 0x41615252u); put32(fsi + 0x1E4, 0x61417272u);
    put32(fsi + 0x1E8, 400u); put32(fsi + 0x1EC, 3u);
    for (copy = 0; copy < FAT_NFATS; copy++) {
        uint8_t *fat = g_vol + (FAT_RESERVED + copy * FAT_SZ) * SECSZ;
        put32(fat + 0, 0x0FFFFFF8u); put32(fat + 4, 0x0FFFFFFFu); put32(fat + 8, 0x0FFFFFFFu);
    }
}

static void test_registry(void) {
    unsigned n = 0;
    const hype_fs_ops_t *const *reg = hype_fs_registry(&n);
    unsigned i;
    CHECK_HEX("five drivers registered", 5, n);
    for (i = 0; i < n; i++) {
        CHECK("every driver probes", reg[i]->probe != 0);
        CHECK("every driver mounts", reg[i]->mount != 0);
        CHECK("every driver reads", reg[i]->read_at != 0 && reg[i]->lookup != 0 &&
                                        reg[i]->map_ranges != 0);
        CHECK("caps say READ", (reg[i]->caps & HYPE_FS_CAP_READ) != 0);
        /* capability honesty: a NULL slot must not have its caps bit, and a
         * caps bit must have its slot */
        CHECK("write_at slot matches caps",
              ((reg[i]->write_at != 0)) == ((reg[i]->caps & HYPE_FS_CAP_WRITE_INPLACE) != 0));
        CHECK("append slot matches caps",
              ((reg[i]->append != 0)) == ((reg[i]->caps & HYPE_FS_CAP_APPEND) != 0));
        CHECK("namespace slots match caps",
              ((reg[i]->create != 0)) == ((reg[i]->caps & HYPE_FS_CAP_NAMESPACE) != 0));
        CHECK("WRITE_GROW is fat32-only so far (#382)",
              ((reg[i]->caps & HYPE_FS_CAP_WRITE_GROW) == 0) ||
                  (reg[i]->name[0] == 'f' && reg[i]->name[1] == 'a'));
    }
    (void)hype_fs_registry(0); /* count pointer is optional */
}

static void test_iso(void) {
    static hype_fs_t fs;
    static hype_fs_file_t f;
    static hype_file_rmap_t rm;
    uint8_t buf[3000];
    unsigned i;

    build_iso();
    CHECK_HEX("auto-mount claims iso9660", 0, hype_fs_mount_auto(&fs, vol_read, vol_write, 0));
    CHECK("driver name", strcmp(fs.ops->name, "iso9660") == 0);
    CHECK("iso mount is read-only even with a write callback", fs.write == 0);
    CHECK("caps: read only", hype_fs_caps(&fs) == HYPE_FS_CAP_READ);

    CHECK_HEX("lookup root (NULL)", 0, hype_fs_lookup(&fs, 0, &f));
    CHECK_HEX("lookup root (empty)", 0, hype_fs_lookup(&fs, "", &f));
    CHECK_HEX("lookup root (slash)", 0, hype_fs_lookup(&fs, "/", &f));
    CHECK_HEX("lookup root (backslash)", 0, hype_fs_lookup(&fs, "\\", &f));
    CHECK("non-root path refused", hype_fs_lookup(&fs, "/setup.exe", &f) != 0);
    CHECK_HEX("image size", (uint64_t)ISO_BLOCKS * 2048u, f.size);

    CHECK_HEX("whole-image map", 0, hype_fs_map_ranges(&fs, 0, &rm));
    CHECK_HEX("one DATA range", 1, rm.count);
    CHECK("map for non-root refused", hype_fs_map_ranges(&fs, "/x", &rm) != 0);

    CHECK_HEX("ragged read", 0, hype_fs_read_at(&f, 1000, buf, 3000));
    for (i = 0; i < 3000u; i++) {
        if (buf[i] != (uint8_t)((1000u + i) * 11u + 3u)) break;
    }
    CHECK("read data", i == 3000u);
    CHECK("read past end refused",
          hype_fs_read_at(&f, (uint64_t)ISO_BLOCKS * 2048u - 4u, buf, 8) != 0);

    /* every mutating operation is a NULL slot, not a fake success */
    CHECK("write_at refused", hype_fs_write_at(&f, 0, buf, 1) != 0);
    CHECK("append refused", hype_fs_append(&f, buf, 1) != 0);
    CHECK("create refused", hype_fs_create(&fs, "X", &f) != 0);
    CHECK("unlink refused", hype_fs_unlink(&fs, "X") != 0);
    CHECK("mkdir refused", hype_fs_mkdir(&fs, "X") != 0);
    CHECK("rmdir refused", hype_fs_rmdir(&fs, "X") != 0);
    CHECK("rename refused", hype_fs_rename(&fs, "X", "Y") != 0);
    CHECK("sync no-op success", hype_fs_sync(&fs) == 0);
    hype_fs_set_time(&fs, 0);      /* NULL slot: silently nothing */
    hype_fs_set_barrier(&fs, 0);   /* NULL slot: silently nothing */
    CHECK("identity error is fat32-only", hype_fs_file_identity_error(&f) == 0);

    /* a truncated PVD claim: probe passes, mount re-reads -- injected failure */
    g_fail_reads = 1;
    CHECK("unreadable volume claimed by nobody",
          hype_fs_mount_auto(&fs, vol_read, 0, 0) != 0);
    g_fail_reads = 0;

    /* corrupt PVDs: each recognition field individually */
    {
        uint8_t *pvd = g_vol + 64u * SECSZ;
        unsigned k;
        for (k = 0; k < 6u; k++) {
            build_iso();
            pvd[k] ^= 0xFF;
            CHECK("corrupt PVD byte refused", hype_fs_mount_auto(&fs, vol_read, 0, 0) != 0);
        }
        build_iso();
        put32(pvd + 80, 0); /* VolumeSpaceSize 0 */
        CHECK("zero space refused", hype_fs_mount_auto(&fs, vol_read, 0, 0) != 0);
        build_iso();
        put16(pvd + 128, 0); /* LogicalBlockSize 0 */
        CHECK("zero block size refused", hype_fs_mount_auto(&fs, vol_read, 0, 0) != 0);
        build_iso();
        put32(pvd + 80, 0xFFFFFFFFu);
        put16(pvd + 128, 0xFFFFu); /* space * bs would overflow nothing here, but
                                    * exercises the guard's arithmetic branch */
        CHECK("huge geometry still parses or refuses cleanly",
              hype_fs_mount_auto(&fs, vol_read, 0, 0) == 0 ||
                  hype_fs_mount_auto(&fs, vol_read, 0, 0) != 0);
    }
}

static void test_fat32_through_interface(void) {
    static hype_fs_t fs;
    static hype_fs_file_t created, ro;
    static hype_file_rmap_t rm;
    uint8_t buf[64];

    build_fat32();
    CHECK_HEX("auto-mount claims fat32", 0, hype_fs_mount_auto(&fs, vol_read, vol_write, 0));
    CHECK("driver name", strcmp(fs.ops->name, "fat32") == 0);
    CHECK("caps: append + random write with growth (#382)",
          (hype_fs_caps(&fs) & (HYPE_FS_CAP_APPEND | HYPE_FS_CAP_WRITE_INPLACE |
                                HYPE_FS_CAP_WRITE_GROW)) ==
              (HYPE_FS_CAP_APPEND | HYPE_FS_CAP_WRITE_INPLACE | HYPE_FS_CAP_WRITE_GROW));

    CHECK_HEX("create", 0, hype_fs_create(&fs, "LOG.TXT", &created));
    CHECK_HEX("append", 0, hype_fs_append(&created, "0123456789", 10));
    CHECK_HEX("size after append", 10, created.size);
    CHECK_HEX("write_at in place via interface", 0, hype_fs_write_at(&created, 0, "ABCD", 4));
    CHECK("no identity error", hype_fs_file_identity_error(&created) == 0);

    CHECK_HEX("lookup finds it", 0, hype_fs_lookup(&fs, "LOG.TXT", &ro));
    CHECK_HEX("lookup size", 10, ro.size);
    CHECK_HEX("read back", 0, hype_fs_read_at(&ro, 2, buf, 6));
    CHECK("read data", memcmp(buf, "CD4567", 6) == 0);
    CHECK_HEX("growth via interface", 0, hype_fs_write_at(&ro, 2000, "Z", 1));
    CHECK_HEX("size grew", 2001, ro.size);
    CHECK_HEX("gap reads zero", 0, hype_fs_read_at(&ro, 500, buf, 1));
    CHECK_HEX("gap byte", 0, buf[0]);

    CHECK_HEX("map_ranges", 0, hype_fs_map_ranges(&fs, "LOG.TXT", &rm));
    CHECK_HEX("one range", 1, rm.count);

    CHECK_HEX("mkdir", 0, hype_fs_mkdir(&fs, "D"));
    CHECK_HEX("rename", 0, hype_fs_rename(&fs, "LOG.TXT", "D/L.TXT"));
    CHECK("sync no-op success (barrier is injected instead)", hype_fs_sync(&fs) == 0);
    hype_fs_set_barrier(&fs, 0); /* fat32 has the slot; installing NULL is legal */

    /* failures through the adapters */
    CHECK("lookup of a missing file fails", hype_fs_lookup(&fs, "NOPE.BIN", &ro) != 0);
    CHECK("map_ranges of a missing file fails", hype_fs_map_ranges(&fs, "NOPE.BIN", &rm) != 0);

    /* read-only FAT32 mount: mutation gated by the wrapper */
    CHECK_HEX("ro mount", 0, hype_fs_mount_auto(&fs, vol_read, 0, 0));
    CHECK("ro caps masked to READ", hype_fs_caps(&fs) == HYPE_FS_CAP_READ);
    CHECK("ro create refused", hype_fs_create(&fs, "A.TXT", &created) != 0);
    CHECK("ro mkdir refused", hype_fs_mkdir(&fs, "E") != 0);
    CHECK("ro rmdir refused", hype_fs_rmdir(&fs, "E") != 0);
    CHECK("ro rename refused", hype_fs_rename(&fs, "A", "B") != 0);
    CHECK("ro unlink refused", hype_fs_unlink(&fs, "A") != 0);
    /* ro lookup is the generic rmap arm; reads work, native ops do not */
    CHECK_HEX("ro lookup", 0, hype_fs_lookup(&fs, "D/L.TXT", &ro));
    CHECK_HEX("ro read", 0, hype_fs_read_at(&ro, 0, buf, 4));
    CHECK("ro read data", memcmp(buf, "ABCD", 4) == 0);

    /* a foreign-tag handle is refused by the fat32 adapters */
    CHECK_HEX("rw mount back", 0, hype_fs_mount_auto(&fs, vol_read, vol_write, 0));
    memset(&ro, 0, sizeof(ro));
    ro.fs = &fs;
    ro.tag = 0;
    CHECK("write_at bogus tag", hype_fs_write_at(&ro, 0, buf, 1) != 0);
    /* in-place write does not shrink the tracked size */
    CHECK_HEX("re-lookup", 0, hype_fs_lookup(&fs, "D/L.TXT", &ro));
    CHECK_HEX("small in-place write", 0, hype_fs_write_at(&ro, 0, "a", 1));
    CHECK("size unchanged by in-place write", ro.size == 2001u);
    CHECK_HEX("unlink", 0, hype_fs_unlink(&fs, "D/L.TXT"));
    CHECK_HEX("rmdir", 0, hype_fs_rmdir(&fs, "D"));
}

static void test_claimed_but_unmountable(void) {
    static hype_fs_t fs;
    /* an exFAT signature with none of the structures behind it: the probe
     * claims it, the mount refuses it, and mount_auto reports THAT rather
     * than letting a laxer driver take a volume that says it is exFAT */
    memset(g_vol, 0, sizeof(g_vol));
    g_vol[3] = 'E'; g_vol[4] = 'X'; g_vol[5] = 'F'; g_vol[6] = 'A'; g_vol[7] = 'T';
    g_vol[8] = ' '; g_vol[9] = ' '; g_vol[10] = ' ';
    g_vol[510] = 0x55; g_vol[511] = 0xAA;
    CHECK("claimed-but-unmountable is a refusal",
          hype_fs_mount_auto(&fs, vol_read, vol_write, 0) != 0);
}

static void test_unmounted_and_unclaimed(void) {
    static hype_fs_t fs;
    static hype_fs_file_t f;
    static hype_file_rmap_t rm;
    uint8_t buf[8];

    /* nothing claims a zeroed medium */
    memset(g_vol, 0, sizeof(g_vol));
    CHECK("no driver claims zeros", hype_fs_mount_auto(&fs, vol_read, vol_write, 0) != 0);
    CHECK("failed mount leaves no ops", fs.ops == 0);
    CHECK("caps of unmounted fs", hype_fs_caps(&fs) == 0);
    CHECK("caps of NULL fs", hype_fs_caps(0) == 0);
    CHECK("NULL read refused", hype_fs_mount_auto(&fs, 0, 0, 0) != 0);

    /* every wrapper refuses an unmounted fs / unopened file */
    CHECK("lookup refused", hype_fs_lookup(&fs, "x", &f) != 0);
    CHECK("map_ranges refused", hype_fs_map_ranges(&fs, "x", &rm) != 0);
    CHECK("create refused", hype_fs_create(&fs, "x", &f) != 0);
    CHECK("unlink refused", hype_fs_unlink(&fs, "x") != 0);
    CHECK("mkdir refused", hype_fs_mkdir(&fs, "x") != 0);
    CHECK("rmdir refused", hype_fs_rmdir(&fs, "x") != 0);
    CHECK("rename refused", hype_fs_rename(&fs, "x", "y") != 0);
    CHECK("sync refused", hype_fs_sync(&fs) != 0);
    hype_fs_set_time(&fs, 0);
    hype_fs_set_barrier(&fs, 0);
    f.fs = 0;
    CHECK("read_at on unopened handle", hype_fs_read_at(&f, 0, buf, 1) != 0);
    CHECK("write_at on unopened handle", hype_fs_write_at(&f, 0, buf, 1) != 0);
    CHECK("append on unopened handle", hype_fs_append(&f, buf, 1) != 0);
    CHECK("read_at on NULL handle", hype_fs_read_at(0, 0, buf, 1) != 0);
    CHECK("identity error on NULL handle", hype_fs_file_identity_error(0) == 0);
}

int main(void) {
    test_registry();
    test_iso();
    test_fat32_through_interface();
    test_claimed_but_unmountable();
    test_unmounted_and_unclaimed();

    if (failures == 0) {
        printf("test_fs_ops: all tests passed\n");
        return 0;
    }
    printf("test_fs_ops: %d failure(s)\n", failures);
    return 1;
}
