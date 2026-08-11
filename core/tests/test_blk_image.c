#include <stdio.h>
#include <string.h>
#include "../blk_image.h"

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
 * A fake host disk: 4096 sectors of RAM, plus a log of the (lba,count) commands
 * the backend issues, so the tests can assert both the DATA and the SPLITTING
 * (one command per contiguous extent run -- a guest transfer spanning a
 * fragmented image must not become one bogus command).
 */
#define DISK_SECTORS 4096u
#define SECSZ HYPE_BLK_SECTOR_SIZE
static uint8_t g_disk[DISK_SECTORS * SECSZ];

#define MAX_CMDS 64
static struct { uint64_t lba; uint32_t count; int is_write; } g_cmds[MAX_CMDS];
static unsigned g_cmd_count;
static long g_fail_after = -1; /* if >=0, fail the command that hits 0 */

static void cmds_reset(void) { g_cmd_count = 0; g_fail_after = -1; }

static int record(uint64_t lba, uint32_t count, int is_write) {
    if (g_cmd_count < MAX_CMDS) {
        g_cmds[g_cmd_count].lba = lba;
        g_cmds[g_cmd_count].count = count;
        g_cmds[g_cmd_count].is_write = is_write;
    }
    g_cmd_count++;
    if (g_fail_after >= 0 && g_fail_after-- == 0) return -1;
    return 0;
}

static int disk_read(void *hw, uint64_t lba, uint32_t count, void *buf) {
    (void)hw;
    if (record(lba, count, 0) != 0) return -1;
    if (lba + count > DISK_SECTORS) return -1;
    memcpy(buf, g_disk + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}
static int disk_write(void *hw, uint64_t lba, uint32_t count, const void *buf) {
    (void)hw;
    if (record(lba, count, 1) != 0) return -1;
    if (lba + count > DISK_SECTORS) return -1;
    memcpy(g_disk + lba * SECSZ, buf, (size_t)count * SECSZ);
    return 0;
}

static uint8_t pat(uint64_t i) { return (uint8_t)(i * 37u + 11u); }

static void fill_disk(void) {
    uint64_t i;
    for (i = 0; i < sizeof g_disk; i++) g_disk[i] = pat(i);
}

/* A two-extent, deliberately out-of-order image: file sectors 0..15 live at
 * disk 200..215, file sectors 16..31 at disk 100..115. Non-contiguous AND
 * backwards, so any accidental "just add lba" arithmetic fails loudly. */
static void map_two_extents(hype_file_map_t *m) {
    m->count = 2;
    m->extents[0].start_lba = 200;
    m->extents[0].sector_count = 16;
    m->extents[1].start_lba = 100;
    m->extents[1].sector_count = 16;
    m->size_bytes = 32ull * SECSZ;
}

static void test_locate(void) {
    hype_blk_image_t img;
    hype_blk_backend_t be;
    hype_file_map_t m;
    uint64_t lba, run;

    map_two_extents(&m);
    CHECK_HEX("init ok", 0, hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));
    CHECK_HEX("capacity = 32 sectors", 32ull, be.total_sectors);

    CHECK_HEX("locate 0", 0, hype_blk_image_locate(&img, 0, &lba, &run));
    CHECK_HEX("  lba", 200ull, lba);
    CHECK_HEX("  run", 16ull, run);
    CHECK_HEX("locate 15 (last of extent 0)", 0, hype_blk_image_locate(&img, 15, &lba, &run));
    CHECK_HEX("  lba", 215ull, lba);
    CHECK_HEX("  run", 1ull, run);
    CHECK_HEX("locate 16 (first of extent 1)", 0, hype_blk_image_locate(&img, 16, &lba, &run));
    CHECK_HEX("  lba", 100ull, lba);
    CHECK_HEX("  run", 16ull, run);
    CHECK_HEX("locate 31 (last sector)", 0, hype_blk_image_locate(&img, 31, &lba, &run));
    CHECK_HEX("  lba", 115ull, lba);
    CHECK_HEX("past the end refused", -1, hype_blk_image_locate(&img, 32, &lba, &run));

    /* A partition offset shifts every extent. */
    CHECK_HEX("init with partition base", 0,
              hype_blk_image_init(&img, &be, &m, 2048u, disk_read, disk_write, 0));
    CHECK_HEX("locate 0 with base", 0, hype_blk_image_locate(&img, 0, &lba, &run));
    CHECK_HEX("  lba includes base", 2248ull, lba);
}

static void test_read_write_roundtrip(void) {
    hype_blk_image_t img;
    hype_blk_backend_t be;
    hype_file_map_t m;
    static uint8_t buf[32 * SECSZ];
    static uint8_t back[32 * SECSZ];
    unsigned i;

    fill_disk();
    map_two_extents(&m);
    CHECK_HEX("init ok", 0, hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));

    /* Guest sector 0 must read disk sector 200. */
    cmds_reset();
    CHECK_HEX("read guest sector 0", 0, hype_blk_backend_read(&be, 0, 1, back));
    CHECK("data is disk sector 200", memcmp(back, g_disk + 200 * SECSZ, SECSZ) == 0);
    CHECK_HEX("one command", 1u, g_cmd_count);
    CHECK_HEX("  at lba 200", 200ull, g_cmds[0].lba);

    /* Guest sector 16 must read disk sector 100 (the backwards extent). */
    cmds_reset();
    CHECK_HEX("read guest sector 16", 0, hype_blk_backend_read(&be, 16, 1, back));
    CHECK("data is disk sector 100", memcmp(back, g_disk + 100 * SECSZ, SECSZ) == 0);

    /* A transfer spanning the extent seam must SPLIT into two commands. */
    cmds_reset();
    CHECK_HEX("read across the seam", 0, hype_blk_backend_read(&be, 14, 4, back));
    CHECK_HEX("two commands", 2u, g_cmd_count);
    CHECK_HEX("  cmd0 lba", 214ull, g_cmds[0].lba);
    CHECK_HEX("  cmd0 count", 2u, g_cmds[0].count);
    CHECK_HEX("  cmd1 lba", 100ull, g_cmds[1].lba);
    CHECK_HEX("  cmd1 count", 2u, g_cmds[1].count);
    CHECK("first half from extent 0", memcmp(back, g_disk + 214 * SECSZ, 2 * SECSZ) == 0);
    CHECK("second half from extent 1",
          memcmp(back + 2 * SECSZ, g_disk + 100 * SECSZ, 2 * SECSZ) == 0);

    /* Whole-image write then read back: proves persistence through the map. */
    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(0x5Au ^ (i * 7u));
    cmds_reset();
    CHECK_HEX("write the whole image", 0, hype_blk_backend_write(&be, 0, 32, buf));
    CHECK_HEX("two commands (one per extent)", 2u, g_cmd_count);
    CHECK_HEX("read the whole image", 0, hype_blk_backend_read(&be, 0, 32, back));
    CHECK("round-trips", memcmp(back, buf, sizeof buf) == 0);
    /* And it landed at the right disk sectors, not somewhere plausible. */
    CHECK("extent 0 on disk", memcmp(g_disk + 200 * SECSZ, buf, 16 * SECSZ) == 0);
    CHECK("extent 1 on disk", memcmp(g_disk + 100 * SECSZ, buf + 16 * SECSZ, 16 * SECSZ) == 0);

    /* Sectors OUTSIDE the image are untouched -- the guest cannot reach them. */
    CHECK("disk sector 216 untouched",
          g_disk[216 * SECSZ] == pat(216ull * SECSZ));
    CHECK("disk sector 116 untouched",
          g_disk[116 * SECSZ] == pat(116ull * SECSZ));
    CHECK("disk sector 199 untouched",
          g_disk[199 * SECSZ] == pat(199ull * SECSZ));
}

static void test_bounds(void) {
    hype_blk_image_t img;
    hype_blk_backend_t be;
    hype_file_map_t m;
    static uint8_t buf[4 * SECSZ];

    fill_disk();
    map_two_extents(&m);
    CHECK_HEX("init ok", 0, hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));

    /* The dispatcher's VALID-3 gate: refused, never clamped. */
    CHECK_HEX("read past capacity", -1, hype_blk_backend_read(&be, 32, 1, buf));
    CHECK_HEX("read straddling the end", -1, hype_blk_backend_read(&be, 31, 2, buf));
    CHECK_HEX("write past capacity", -1, hype_blk_backend_write(&be, 32, 1, buf));
    CHECK_HEX("count 0", -1, hype_blk_backend_read(&be, 0, 0, buf));
    CHECK_HEX("absurd lba", -1, hype_blk_backend_read(&be, 0xFFFFFFFFFFFFFFFFull, 1, buf));
    /* Last valid sector still works. */
    CHECK_HEX("last sector ok", 0, hype_blk_backend_read(&be, 31, 1, buf));
}

static void test_read_only(void) {
    hype_blk_image_t img;
    hype_blk_backend_t be;
    hype_file_map_t m;
    static uint8_t buf[SECSZ];

    map_two_extents(&m);
    CHECK_HEX("init read-only", 0, hype_blk_image_init(&img, &be, &m, 0u, disk_read, 0, 0));
    CHECK("write fn is NULL", be.write == 0);
    CHECK_HEX("guest write rejected", -1, hype_blk_backend_write(&be, 0, 1, buf));
    CHECK_HEX("read still works", 0, hype_blk_backend_read(&be, 0, 1, buf));
}

static void test_init_refusals(void) {
    hype_blk_image_t img;
    hype_blk_backend_t be;
    hype_file_map_t m;

    /* No read callback. */
    map_two_extents(&m);
    CHECK_HEX("NULL read refused", -1, hype_blk_image_init(&img, &be, &m, 0u, 0, disk_write, 0));
    CHECK_HEX("NULL map refused", -1,
              hype_blk_image_init(&img, &be, 0, 0u, disk_read, disk_write, 0));

    /* A zero-byte file is not a disk. */
    m.count = 0;
    m.size_bytes = 0;
    CHECK_HEX("empty file refused", -1,
              hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));

    /* Extents shorter than the file's size: a hole at the end. Serving this
     * would fail mid-install instead of at attach. */
    map_two_extents(&m);
    m.size_bytes = 64ull * SECSZ; /* claims 64 sectors, extents cover 32 */
    CHECK_HEX("short extent list refused", -1,
              hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));

    /* A zero-length run. */
    map_two_extents(&m);
    m.extents[1].sector_count = 0;
    m.size_bytes = 16ull * SECSZ;
    CHECK_HEX("zero-length extent refused", -1,
              hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));

    /* More extents than the contract holds. */
    map_two_extents(&m);
    m.count = HYPE_FILE_MAX_EXTENTS + 1u;
    CHECK_HEX("too many extents refused", -1,
              hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));

    /* A trailing partial sector is unreachable, not an error. */
    map_two_extents(&m);
    m.size_bytes = 32ull * SECSZ + 100u;
    CHECK_HEX("partial trailing sector ok", 0,
              hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));
    CHECK_HEX("capacity floors to 32", 32ull, be.total_sectors);
}

/* A single-extent contiguous image -- the common case a freshly created,
 * unfragmented image file produces -- transfers in ONE command. */
static void test_contiguous_single_command(void) {
    hype_blk_image_t img;
    hype_blk_backend_t be;
    hype_file_map_t m;
    static uint8_t buf[64 * SECSZ];

    fill_disk();
    m.count = 1;
    m.extents[0].start_lba = 1000;
    m.extents[0].sector_count = 64;
    m.size_bytes = 64ull * SECSZ;
    CHECK_HEX("init ok", 0, hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));

    cmds_reset();
    CHECK_HEX("read all 64", 0, hype_blk_backend_read(&be, 0, 64, buf));
    CHECK_HEX("exactly one command", 1u, g_cmd_count);
    CHECK_HEX("  count 64", 64u, g_cmds[0].count);
    CHECK("data matches the disk", memcmp(buf, g_disk + 1000 * SECSZ, sizeof buf) == 0);
}

/* An I/O error on any chunk must surface, not be silently short. */
static void test_io_errors(void) {
    hype_blk_image_t img;
    hype_blk_backend_t be;
    hype_file_map_t m;
    static uint8_t buf[32 * SECSZ];
    long k;

    fill_disk();
    map_two_extents(&m);
    CHECK_HEX("init ok", 0, hype_blk_image_init(&img, &be, &m, 0u, disk_read, disk_write, 0));

    for (k = 0; k < 2; k++) {
        cmds_reset();
        g_fail_after = k;
        CHECK_HEX("read error surfaces", -1, hype_blk_backend_read(&be, 0, 32, buf));
        cmds_reset();
        g_fail_after = k;
        CHECK_HEX("write error surfaces", -1, hype_blk_backend_write(&be, 0, 32, buf));
    }
    g_fail_after = -1;
}

int main(void) {
    test_locate();
    test_read_write_roundtrip();
    test_bounds();
    test_read_only();
    test_init_refusals();
    test_contiguous_single_command();
    test_io_errors();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
