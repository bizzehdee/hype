#include <stdio.h>
#include <string.h>
#include "../blk_image_sparse.h"

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

/* A fake host disk, same shape as test_blk_image.c's: the DATA portion of the sparse file
 * lives at a fixed base LBA on it. */
#define DISK_SECTORS 4096u
#define SECSZ HYPE_BLK_SECTOR_SIZE
#define DATA_BASE_LBA 1000u
static uint8_t g_disk[DISK_SECTORS * SECSZ];

static int g_fail_disk_write;

static int disk_read(void *hw, uint64_t lba, uint32_t count, void *buf) {
    (void)hw;
    if (lba + count > DISK_SECTORS) return -1;
    memcpy(buf, g_disk + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}
static int disk_write(void *hw, uint64_t lba, uint32_t count, const void *buf) {
    (void)hw;
    if (g_fail_disk_write) return -1;
    if (lba + count > DISK_SECTORS) return -1;
    memcpy(g_disk + lba * SECSZ, buf, (size_t)count * SECSZ);
    return 0;
}

/*
 * A fake growable filesystem: the "file" is just the DATA_BASE_LBA-based run on g_disk, grown
 * one sector at a time by fake_write_at, up to g_virtual_bytes. map_ranges reports [0,
 * g_grown_bytes) as DATA and the rest, up to g_virtual_bytes, as one HOLE -- exactly the shape
 * ext's own sparse resolver produces for a file that has been partly written.
 */
static uint64_t g_grown_bytes;
static uint64_t g_virtual_bytes;
static int g_fail_write;       /* simulate volume-full */
static int g_fail_map_refresh; /* simulate a post-write map_ranges failure */
static unsigned g_write_at_calls;

static int fake_write_at(hype_fs_file_t *f, uint64_t offset, const void *src, unsigned int len) {
    uint64_t disk_off = DATA_BASE_LBA * (uint64_t)SECSZ + offset;
    (void)f;
    g_write_at_calls++;
    if (g_fail_write) return -1;
    if (offset % SECSZ != 0 || len % SECSZ != 0) return -1; /* backend always sector-aligns */
    if (disk_off + len > DISK_SECTORS * (uint64_t)SECSZ) return -1;
    memcpy(g_disk + disk_off, src, len);
    if (offset + len > g_grown_bytes) g_grown_bytes = offset + len;
    return 0;
}

static int fake_map_ranges(hype_fs_t *fs, const char *path, hype_file_rmap_t *out) {
    (void)fs;
    (void)path;
    if (g_fail_map_refresh) return -1;
    hype_file_rmap_init(out, g_virtual_bytes);
    if (g_grown_bytes > 0u) {
        CHECK_HEX("fake fs stays sector-aligned", 0u, g_grown_bytes % SECSZ);
        (void)hype_file_rmap_append(out, HYPE_RANGE_DATA, DATA_BASE_LBA,
                                    g_grown_bytes / SECSZ);
    }
    if (g_grown_bytes < g_virtual_bytes) {
        (void)hype_file_rmap_append(out, HYPE_RANGE_HOLE, 0,
                                    (g_virtual_bytes - g_grown_bytes) / SECSZ);
    }
    return 0;
}

static const hype_fs_ops_t g_fake_ops = {
    "fake-sparse",
    HYPE_FS_CAP_READ | HYPE_FS_CAP_WRITE_GROW,
    0,               /* probe */
    0,               /* mount */
    0,               /* lookup */
    fake_map_ranges, /* map_ranges */
    0,               /* read_at */
    fake_write_at,   /* write_at */
    0, 0, 0, 0, 0, 0, /* append/create/unlink/mkdir/rmdir/rename */
    0,               /* sync */
    0,               /* set_time */
    0,               /* set_barrier */
};

static const hype_fs_ops_t g_fake_ops_no_grow = {
    "fake-nogrow", HYPE_FS_CAP_READ, 0, 0, 0, fake_map_ranges, 0, fake_write_at,
    0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static void reset_fixture(uint64_t initial_grown_bytes, uint64_t virtual_bytes) {
    memset(g_disk, 0, sizeof g_disk);
    g_grown_bytes = initial_grown_bytes;
    g_virtual_bytes = virtual_bytes;
    g_fail_write = 0;
    g_fail_map_refresh = 0;
    g_fail_disk_write = 0;
    g_write_at_calls = 0;
}

static void init_sparse(hype_blk_image_sparse_t *img, hype_blk_backend_t *be, hype_fs_t *fs,
                        hype_fs_file_t *file, const hype_fs_ops_t *ops, const char *path) {
    hype_file_rmap_t rmap;
    fs->ops = ops;
    fs->read = disk_read;
    fs->write = disk_write;
    fs->ctx = 0;
    file->fs = fs;
    (void)fake_map_ranges(fs, path, &rmap);
    CHECK_HEX("sparse init ok",
             0, hype_blk_image_sparse_init(img, be, &rmap, 0u, disk_read, disk_write, 0, fs, file,
                                           path));
}

/* Fresh sparse image: 8 KiB virtual, nothing written yet -- capacity reports the FULL virtual
 * size even though zero sectors are allocated. */
static void test_capacity_before_any_write(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;

    reset_fixture(0, 8192);
    init_sparse(&img, &be, &fs, &file, &g_fake_ops, "/sparse.img");
    CHECK_HEX("capacity = 16 sectors", 16ull, be.total_sectors);
}

/* A read into a never-written region synthesizes zeros and never calls write_at. */
static void test_read_hole_is_zero(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    uint8_t buf[SECSZ];
    unsigned i;

    reset_fixture(0, 8192);
    init_sparse(&img, &be, &fs, &file, &g_fake_ops, "/sparse.img");
    memset(buf, 0xEE, sizeof buf);
    CHECK_HEX("read ok", 0, be.read(be.ctx, 4, 1, buf));
    for (i = 0; i < sizeof buf; i++) {
        if (buf[i] != 0u) { CHECK("hole reads as zero", 0); break; }
    }
    CHECK_HEX("no write_at for a read", 0u, g_write_at_calls);
}

/* A write into a hole grows the file (one write_at call), and the SAME data reads back
 * afterward through the now-fast DATA path -- no second write_at, since the second read is a
 * plain host disk read, not a filesystem call. */
static void test_write_into_hole_grows_and_persists(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    uint8_t wbuf[SECSZ], rbuf[SECSZ];
    unsigned i;

    reset_fixture(0, 8192);
    init_sparse(&img, &be, &fs, &file, &g_fake_ops, "/sparse.img");
    for (i = 0; i < sizeof wbuf; i++) wbuf[i] = (uint8_t)(i * 7u + 3u);

    CHECK_HEX("write into hole ok", 0, be.write(be.ctx, 0, 1, wbuf));
    CHECK_HEX("exactly one write_at for the growth", 1u, g_write_at_calls);
    CHECK_HEX("capacity unchanged by growth", 16ull, be.total_sectors);

    memset(rbuf, 0, sizeof rbuf);
    CHECK_HEX("read back ok", 0, be.read(be.ctx, 0, 1, rbuf));
    CHECK_HEX("readback matches", 0, memcmp(wbuf, rbuf, sizeof wbuf));

    /* A second write to the SAME (now-allocated) sector takes the fast DATA path: no further
     * write_at call. */
    for (i = 0; i < sizeof wbuf; i++) wbuf[i] = (uint8_t)(i + 1u);
    CHECK_HEX("second write ok", 0, be.write(be.ctx, 0, 1, wbuf));
    CHECK_HEX("second write used the fast path", 1u, g_write_at_calls);
    CHECK_HEX("read back second write", 0, be.read(be.ctx, 0, 1, rbuf));
    CHECK_HEX("second readback matches", 0, memcmp(wbuf, rbuf, sizeof wbuf));
}

/* A write spanning an already-grown DATA prefix and a HOLE tail issues exactly one write_at,
 * for the HOLE portion only -- the DATA portion goes through the fast in-place path. */
static void test_write_spans_data_and_hole(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    uint8_t wbuf[3 * SECSZ], rbuf[3 * SECSZ];
    unsigned i;

    reset_fixture(SECSZ, 4 * SECSZ); /* sector 0 already allocated; 1..3 are hole */
    init_sparse(&img, &be, &fs, &file, &g_fake_ops, "/sparse.img");
    for (i = 0; i < sizeof wbuf; i++) wbuf[i] = (uint8_t)(i + 5u);

    CHECK_HEX("mixed write ok", 0, be.write(be.ctx, 0, 3, wbuf));
    CHECK_HEX("one write_at for the hole tail", 1u, g_write_at_calls);
    CHECK_HEX("read back mixed write", 0, be.read(be.ctx, 0, 3, rbuf));
    CHECK_HEX("mixed readback matches", 0, memcmp(wbuf, rbuf, sizeof wbuf));
}

/* Volume full: write_at fails, the guest write fails cleanly, and no partial growth is left
 * claiming success. */
static void test_write_grow_failure_is_clean(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    uint8_t wbuf[SECSZ];

    reset_fixture(0, 8192);
    init_sparse(&img, &be, &fs, &file, &g_fake_ops, "/sparse.img");
    memset(wbuf, 0x11, sizeof wbuf);
    g_fail_write = 1;
    CHECK_HEX("write into full volume fails", -1, be.write(be.ctx, 0, 1, wbuf));
}

/* A read-only sparse view (no growth handle) refuses a write into a hole rather than
 * discarding it silently. */
static void test_no_growth_handle_refuses_hole_write(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_file_rmap_t rmap;
    uint8_t wbuf[SECSZ];

    reset_fixture(0, 8192);
    hype_file_rmap_init(&rmap, 8192);
    (void)hype_file_rmap_append(&rmap, HYPE_RANGE_HOLE, 0, 16);
    CHECK_HEX("init without growth handle ok",
             0, hype_blk_image_sparse_init(&img, &be, &rmap, 0u, disk_read, disk_write, 0, 0, 0,
                                           0));
    memset(wbuf, 0x22, sizeof wbuf);
    CHECK_HEX("hole write refused with no growth handle", -1, be.write(be.ctx, 0, 1, wbuf));
}

/* A filesystem without HYPE_FS_CAP_WRITE_GROW cannot back a sparse image at all -- refused at
 * setup, not at the first hole. */
static void test_grow_handle_needs_write_grow_cap(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    hype_file_rmap_t rmap;

    reset_fixture(0, 8192);
    fs.ops = &g_fake_ops_no_grow;
    fs.read = disk_read;
    fs.write = disk_write;
    fs.ctx = 0;
    file.fs = &fs;
    (void)fake_map_ranges(&fs, "/sparse.img", &rmap);
    CHECK_HEX("init refused without WRITE_GROW",
             -1, hype_blk_image_sparse_init(&img, &be, &rmap, 0u, disk_read, disk_write, 0, &fs,
                                            &file, "/sparse.img"));
}

/* A partially-NULL growth-handle triple is refused rather than silently treated as "no
 * growth". */
static void test_growth_handle_must_be_all_or_nothing(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_file_rmap_t rmap;

    reset_fixture(0, 8192);
    fs.ops = &g_fake_ops;
    hype_file_rmap_init(&rmap, 8192);
    (void)hype_file_rmap_append(&rmap, HYPE_RANGE_HOLE, 0, 16);
    CHECK_HEX("fs set but file/path NULL is refused",
             -1, hype_blk_image_sparse_init(&img, &be, &rmap, 0u, disk_read, disk_write, 0, &fs,
                                            0, 0));
}

/* If the post-growth map refresh itself fails, the bytes are already safely persisted (the
 * write_at succeeded), but the backend marks itself broken and refuses every further write --
 * never silently keeps serving a map it can no longer trust. */
static void test_map_refresh_failure_marks_broken(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    uint8_t wbuf[SECSZ];

    reset_fixture(0, 8192);
    init_sparse(&img, &be, &fs, &file, &g_fake_ops, "/sparse.img");
    memset(wbuf, 0x33, sizeof wbuf);
    g_fail_map_refresh = 1;
    CHECK_HEX("write with broken refresh reports failure", -1, be.write(be.ctx, 0, 1, wbuf));
    CHECK("grow_broken latched", img.grow_broken);
    g_fail_map_refresh = 0;
    CHECK_HEX("further writes refused once broken", -1, be.write(be.ctx, 1, 1, wbuf));
}

/* A write into an UNWRITTEN range is refused outright, never promoted to a growth attempt. */
static void test_unwritten_range_refuses_write(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    hype_file_rmap_t rmap;
    uint8_t wbuf[SECSZ];

    reset_fixture(0, 8192);
    fs.ops = &g_fake_ops;
    fs.read = disk_read;
    fs.write = disk_write;
    fs.ctx = 0;
    file.fs = &fs;
    hype_file_rmap_init(&rmap, 8192);
    (void)hype_file_rmap_append(&rmap, HYPE_RANGE_UNWRITTEN, DATA_BASE_LBA, 16);
    CHECK_HEX("init with unwritten range ok",
             0, hype_blk_image_sparse_init(&img, &be, &rmap, 0u, disk_read, disk_write, 0, &fs,
                                           &file, "/sparse.img"));
    memset(wbuf, 0x44, sizeof wbuf);
    CHECK_HEX("unwritten write refused, not grown", -1, be.write(be.ctx, 0, 1, wbuf));
    CHECK_HEX("no growth attempted", 0u, g_write_at_calls);
}

/* partition_lba folds into DATA ranges but never into a HOLE's (meaningless) start_lba. */
static void test_partition_offset_applies_to_data_only(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    hype_file_rmap_t rmap;

    reset_fixture(SECSZ, 2 * SECSZ);
    fs.ops = &g_fake_ops;
    fs.read = disk_read;
    fs.write = disk_write;
    fs.ctx = 0;
    file.fs = &fs;
    (void)fake_map_ranges(&fs, "/sparse.img", &rmap);
    CHECK_HEX("init with partition offset ok",
             0, hype_blk_image_sparse_init(&img, &be, &rmap, 500u, disk_read, disk_write, 0, &fs,
                                           &file, "/sparse.img"));
    CHECK_HEX("DATA range shifted by partition_lba", (uint64_t)(DATA_BASE_LBA + 500u),
             img.map.ranges[0].start_lba);
}

/* write_sectors == 0 makes a read-only backend, exactly like hype_blk_image_t: be->write stays
 * NULL even though growth is otherwise wired up. */
static void test_read_only_when_write_sectors_null(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    hype_file_rmap_t rmap;

    reset_fixture(0, 8192);
    fs.ops = &g_fake_ops;
    fs.read = disk_read;
    fs.write = disk_write;
    fs.ctx = 0;
    file.fs = &fs;
    (void)fake_map_ranges(&fs, "/sparse.img", &rmap);
    CHECK_HEX("init with no write_sectors ok",
             0, hype_blk_image_sparse_init(&img, &be, &rmap, 0u, disk_read, 0, 0, &fs, &file,
                                           "/sparse.img"));
    CHECK("write fn is NULL for a read-only backend", be.write == 0);
}

/* Invalid-argument refusals: every required pointer, and a zero-capacity rmap. */
static void test_init_refuses_bad_args(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_file_rmap_t rmap, zero_rmap;

    hype_file_rmap_init(&rmap, 8192);
    (void)hype_file_rmap_append(&rmap, HYPE_RANGE_HOLE, 0, 16);
    CHECK_HEX("NULL img refused",
             -1, hype_blk_image_sparse_init(0, &be, &rmap, 0u, disk_read, disk_write, 0, 0, 0, 0));
    CHECK_HEX("NULL be refused",
             -1, hype_blk_image_sparse_init(&img, 0, &rmap, 0u, disk_read, disk_write, 0, 0, 0, 0));
    CHECK_HEX("NULL rmap refused",
             -1, hype_blk_image_sparse_init(&img, &be, 0, 0u, disk_read, disk_write, 0, 0, 0, 0));
    CHECK_HEX("NULL read_sectors refused",
             -1, hype_blk_image_sparse_init(&img, &be, &rmap, 0u, 0, disk_write, 0, 0, 0, 0));

    hype_file_rmap_init(&zero_rmap, 0);
    CHECK_HEX("zero-capacity rmap refused",
             -1, hype_blk_image_sparse_init(&img, &be, &zero_rmap, 0u, disk_read, disk_write, 0,
                                            0, 0, 0));
}

/* A DATA-range write that fails at the host disk (not the filesystem) fails cleanly too --
 * the fast in-place path has the same "guest write fails" contract as the growth path. */
static void test_data_write_disk_failure_is_clean(void) {
    hype_blk_image_sparse_t img;
    hype_blk_backend_t be;
    hype_fs_t fs;
    hype_fs_file_t file;
    uint8_t wbuf[SECSZ];

    reset_fixture(SECSZ, 2 * SECSZ); /* sector 0 already allocated (DATA) */
    init_sparse(&img, &be, &fs, &file, &g_fake_ops, "/sparse.img");
    memset(wbuf, 0x55, sizeof wbuf);
    g_fail_disk_write = 1;
    CHECK_HEX("data write fails when the disk does", -1, be.write(be.ctx, 0, 1, wbuf));
}

int main(void) {
    test_capacity_before_any_write();
    test_read_hole_is_zero();
    test_write_into_hole_grows_and_persists();
    test_write_spans_data_and_hole();
    test_write_grow_failure_is_clean();
    test_no_growth_handle_refuses_hole_write();
    test_grow_handle_needs_write_grow_cap();
    test_growth_handle_must_be_all_or_nothing();
    test_map_refresh_failure_marks_broken();
    test_unwritten_range_refuses_write();
    test_partition_offset_applies_to_data_only();
    test_read_only_when_write_sectors_null();
    test_init_refuses_bad_args();
    test_data_write_disk_failure_is_clean();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d failure(s)\n", failures);
    return 1;
}
