#include <stdio.h>
#include <string.h>
#include "../file_range.h"

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
#define DISK_SECTORS 1024u
static uint8_t g_disk[DISK_SECTORS * SECSZ];

static unsigned g_reads;
static long g_fail_after = -1; /* if >=0, fail the read that decrements it to <0 */

static int disk_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    g_reads++;
    if (g_fail_after >= 0 && g_fail_after-- == 0) return -1;
    if (lba + count > DISK_SECTORS) return -1;
    memcpy(dst, g_disk + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}

static long g_wfail_after = -1;
static int disk_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (g_wfail_after >= 0 && g_wfail_after-- == 0) return -1;
    if (lba + count > DISK_SECTORS) return -1;
    memcpy(g_disk + lba * SECSZ, src, (size_t)count * SECSZ);
    return 0;
}
static int fail_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx; (void)lba; (void)count; (void)src;
    return -1;
}

static int forbidden_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx; (void)lba; (void)count; (void)dst;
    g_reads++;
    return -1; /* any call is a test failure: zero synthesis must not touch media */
}

static void fill_disk(void) {
    unsigned i;
    for (i = 0; i < sizeof(g_disk); i++) {
        g_disk[i] = (uint8_t)(i * 7u + (i >> 9)); /* per-sector-distinct pattern */
    }
}

static void test_append_and_coalesce(void) {
    hype_file_rmap_t m;

    hype_file_rmap_init(&m, 10 * SECSZ);
    CHECK("append DATA", hype_file_rmap_append(&m, HYPE_RANGE_DATA, 100, 2) == 0);
    CHECK("append DATA contiguous coalesces", hype_file_rmap_append(&m, HYPE_RANGE_DATA, 102, 3) == 0);
    CHECK_HEX("coalesced count", 1, m.count);
    CHECK_HEX("coalesced sectors", 5, m.ranges[0].sector_count);
    CHECK("append DATA discontiguous", hype_file_rmap_append(&m, HYPE_RANGE_DATA, 200, 1) == 0);
    CHECK_HEX("discontiguous adds range", 2, m.count);
    CHECK("append HOLE", hype_file_rmap_append(&m, HYPE_RANGE_HOLE, 999, 2) == 0);
    CHECK_HEX("HOLE stores lba 0", 0, m.ranges[2].start_lba);
    CHECK("append HOLE coalesces", hype_file_rmap_append(&m, HYPE_RANGE_HOLE, 5, 1) == 0);
    CHECK_HEX("HOLE coalesced", 3, m.count);
    CHECK("append UNWRITTEN after HOLE", hype_file_rmap_append(&m, HYPE_RANGE_UNWRITTEN, 300, 1) == 0);
    CHECK_HEX("kind change adds range", 4, m.count);

    CHECK("zero-length refused", hype_file_rmap_append(&m, HYPE_RANGE_DATA, 1, 0) == -1);
    CHECK("invalid kind refused", hype_file_rmap_append(&m, (hype_range_kind_t)7, 1, 1) == -1);
    CHECK("LBA wrap refused",
          hype_file_rmap_append(&m, HYPE_RANGE_DATA, ~0ull - 1, 4) == -1);
}

static void test_append_cap(void) {
    hype_file_rmap_t m;
    unsigned i;

    hype_file_rmap_init(&m, (2 * HYPE_FILE_MAX_RANGES + 2) * SECSZ);
    /* alternate kinds so nothing coalesces */
    for (i = 0; i < HYPE_FILE_MAX_RANGES; i++) {
        int rc = hype_file_rmap_append(&m, (i & 1) ? HYPE_RANGE_HOLE : HYPE_RANGE_DATA,
                                       i * 10, 1);
        CHECK("append under cap", rc == 0);
    }
    CHECK("cap not yet flagged", m.too_fragmented == 0);
    CHECK("append past cap refused",
          hype_file_rmap_append(&m, HYPE_RANGE_UNWRITTEN, 1, 1) == -1);
    CHECK("cap flagged", m.too_fragmented == 1);

    /* coalescing keeps working at the cap: same kind + contiguous merges into
     * the last range instead of needing a new slot */
    CHECK("coalesce at cap ok",
          hype_file_rmap_append(&m, (HYPE_FILE_MAX_RANGES & 1) ? HYPE_RANGE_DATA : HYPE_RANGE_HOLE,
                                m.ranges[m.count - 1].kind == HYPE_RANGE_HOLE
                                    ? 0
                                    : m.ranges[m.count - 1].start_lba + m.ranges[m.count - 1].sector_count,
                                1) == (m.ranges[m.count - 1].kind == HYPE_RANGE_HOLE ? 0 : 0));

    /* sector_count coalesce overflow */
    hype_file_rmap_init(&m, ~0ull);
    CHECK("first big append", hype_file_rmap_append(&m, HYPE_RANGE_HOLE, 0, ~0ull) == 0);
    CHECK("coalesce overflow refused", hype_file_rmap_append(&m, HYPE_RANGE_HOLE, 0, 1) == -1);
}

static void test_validate(void) {
    hype_file_rmap_t m;

    /* valid three-kind map: 2 DATA, 3 HOLE, 1 UNWRITTEN = 6 sectors; file is
     * 5.5 sectors so the last range extends into a partial sector */
    hype_file_rmap_init(&m, 5 * SECSZ + SECSZ / 2);
    hype_file_rmap_append(&m, HYPE_RANGE_DATA, 10, 2);
    hype_file_rmap_append(&m, HYPE_RANGE_HOLE, 0, 3);
    hype_file_rmap_append(&m, HYPE_RANGE_UNWRITTEN, 50, 1);
    CHECK("valid map validates", hype_file_rmap_validate(&m, DISK_SECTORS) == 0);

    CHECK("DATA past media refused", hype_file_rmap_validate(&m, 11) == -1);
    CHECK("UNWRITTEN past media refused", hype_file_rmap_validate(&m, 50) == -1);
    CHECK("media just big enough", hype_file_rmap_validate(&m, 51) == 0);

    /* coverage mismatches */
    m.size_bytes = 7 * SECSZ; /* needs 7, map covers 6 */
    CHECK("short coverage refused", hype_file_rmap_validate(&m, DISK_SECTORS) == -1);
    m.size_bytes = 5 * SECSZ; /* needs 5, map covers 6 */
    CHECK("long coverage refused", hype_file_rmap_validate(&m, DISK_SECTORS) == -1);
    m.size_bytes = 5 * SECSZ + 1; /* needs 6 */
    CHECK("partial tail ok", hype_file_rmap_validate(&m, DISK_SECTORS) == 0);

    /* zero-byte file */
    hype_file_rmap_init(&m, 0);
    CHECK("empty file, empty map ok", hype_file_rmap_validate(&m, DISK_SECTORS) == 0);
    hype_file_rmap_append(&m, HYPE_RANGE_DATA, 1, 1);
    CHECK("empty file with ranges refused", hype_file_rmap_validate(&m, DISK_SECTORS) == -1);

    /* hand-built malformed shapes the append API cannot produce */
    hype_file_rmap_init(&m, SECSZ);
    m.count = 1;
    m.ranges[0].kind = 9;
    m.ranges[0].start_lba = 0;
    m.ranges[0].sector_count = 1;
    CHECK("invalid kind refused", hype_file_rmap_validate(&m, DISK_SECTORS) == -1);
    m.ranges[0].kind = HYPE_RANGE_DATA;
    m.ranges[0].sector_count = 0;
    CHECK("zero-length range refused", hype_file_rmap_validate(&m, DISK_SECTORS) == -1);
    m.ranges[0].start_lba = ~0ull - 1;
    m.ranges[0].sector_count = 4;
    CHECK("LBA overflow refused", hype_file_rmap_validate(&m, DISK_SECTORS) == -1);
    m.count = HYPE_FILE_MAX_RANGES + 1;
    CHECK("count past cap refused", hype_file_rmap_validate(&m, DISK_SECTORS) == -1);

    /* logical coverage overflow: two huge HOLEs */
    hype_file_rmap_init(&m, ~0ull);
    m.count = 2;
    m.ranges[0].kind = HYPE_RANGE_HOLE;
    m.ranges[0].start_lba = 0;
    m.ranges[0].sector_count = ~0ull;
    m.ranges[1] = m.ranges[0];
    CHECK("coverage overflow refused", hype_file_rmap_validate(&m, DISK_SECTORS) == -1);
}

static void test_from_extents(void) {
    hype_file_map_t x;
    hype_file_rmap_t m;

    memset(&x, 0, sizeof(x));
    x.count = 2;
    x.extents[0].start_lba = 10;
    x.extents[0].sector_count = 4;
    x.extents[1].start_lba = 20;
    x.extents[1].sector_count = 2;
    x.size_bytes = 6 * SECSZ - 100;

    CHECK("lift ok", hype_file_rmap_from_extents(&x, &m) == 0);
    CHECK_HEX("lift count", 2, m.count);
    CHECK_HEX("lift kind", HYPE_RANGE_DATA, m.ranges[0].kind);
    CHECK_HEX("lift size", x.size_bytes, m.size_bytes);
    CHECK("lifted map validates", hype_file_rmap_validate(&m, DISK_SECTORS) == 0);

    /* adjacent extents coalesce on the way through */
    x.extents[1].start_lba = 14;
    CHECK("lift coalesces", hype_file_rmap_from_extents(&x, &m) == 0 && m.count == 1);

    /* short chain = malformed, never a hole */
    x.size_bytes = 7 * SECSZ;
    CHECK("short chain refused", hype_file_rmap_from_extents(&x, &m) == -1);

    /* empty file */
    memset(&x, 0, sizeof(x));
    CHECK("empty lift ok", hype_file_rmap_from_extents(&x, &m) == 0 && m.count == 0);

    /* too_fragmented carries through */
    x.too_fragmented = 1;
    CHECK("flag carried", hype_file_rmap_from_extents(&x, &m) == 0 && m.too_fragmented == 1);

    /* count past the physical cap */
    x.too_fragmented = 0;
    x.count = HYPE_FILE_MAX_EXTENTS + 1;
    CHECK("overlarge extent count refused", hype_file_rmap_from_extents(&x, &m) == -1);
}

static void test_lower_to_extents(void) {
    hype_file_rmap_t m;
    hype_file_map_t x;

    /* all-DATA map lowers cleanly */
    hype_file_rmap_init(&m, 6 * SECSZ);
    CHECK("append data 1", hype_file_rmap_append(&m, HYPE_RANGE_DATA, 10, 4) == 0);
    CHECK("append data 2", hype_file_rmap_append(&m, HYPE_RANGE_DATA, 20, 2) == 0);
    memset(&x, 0xAA, sizeof(x));
    CHECK("lower ok", hype_file_map_from_rmap(&m, &x) == 0);
    CHECK_HEX("lower count", 2, x.count);
    CHECK_HEX("lower lba0", 10, x.extents[0].start_lba);
    CHECK_HEX("lower size", m.size_bytes, x.size_bytes);

    /* a HOLE cannot be lowered -- no physical sectors to hand back */
    hype_file_rmap_init(&m, 6 * SECSZ);
    CHECK("append hole", hype_file_rmap_append(&m, HYPE_RANGE_HOLE, 0, 6) == 0);
    CHECK("hole refused", hype_file_map_from_rmap(&m, &x) == -1);

    /* an UNWRITTEN range cannot be lowered either -- #696: raw sector
     * passthrough cannot honor its "reads as zero" guarantee */
    hype_file_rmap_init(&m, 6 * SECSZ);
    CHECK("append unwritten", hype_file_rmap_append(&m, HYPE_RANGE_UNWRITTEN, 10, 6) == 0);
    CHECK("unwritten refused", hype_file_map_from_rmap(&m, &x) == -1);

    /* empty file lowers to zero extents */
    hype_file_rmap_init(&m, 0);
    CHECK("empty lower ok", hype_file_map_from_rmap(&m, &x) == 0 && x.count == 0);

    /* too_fragmented carries through */
    hype_file_rmap_init(&m, 0);
    m.too_fragmented = 1;
    CHECK("frag flag carried", hype_file_map_from_rmap(&m, &x) == 0 && x.too_fragmented == 1);

    /* count past HYPE_FILE_MAX_RANGES is refused outright */
    hype_file_rmap_init(&m, 0);
    m.count = HYPE_FILE_MAX_RANGES + 1;
    CHECK("overlarge range count refused", hype_file_map_from_rmap(&m, &x) == -1);
}

static void test_locate(void) {
    hype_file_rmap_t m;
    hype_range_kind_t kind;
    uint64_t lba, run;
    uint32_t head;

    /* DATA(2 sec @10) | HOLE(1 sec) | UNWRITTEN(1 sec @50); size 3.5 sectors */
    hype_file_rmap_init(&m, 3 * SECSZ + SECSZ / 2);
    hype_file_rmap_append(&m, HYPE_RANGE_DATA, 10, 2);
    hype_file_rmap_append(&m, HYPE_RANGE_HOLE, 0, 1);
    hype_file_rmap_append(&m, HYPE_RANGE_UNWRITTEN, 50, 1);

    CHECK("locate 0", hype_file_rmap_locate(&m, 0, &kind, &lba, &head, &run) == 0);
    CHECK("kind 0", kind == HYPE_RANGE_DATA);
    CHECK_HEX("lba 0", 10, lba);
    CHECK_HEX("head 0", 0, head);
    CHECK_HEX("run 0", 2 * SECSZ, run);

    CHECK("locate mid-sector", hype_file_rmap_locate(&m, SECSZ + 100, &kind, &lba, &head, &run) == 0);
    CHECK_HEX("lba mid", 11, lba);
    CHECK_HEX("head mid", 100, head);
    CHECK_HEX("run mid", SECSZ - 100, run);

    CHECK("locate hole", hype_file_rmap_locate(&m, 2 * SECSZ + 5, &kind, &lba, &head, &run) == 0);
    CHECK("hole kind", kind == HYPE_RANGE_HOLE);
    CHECK_HEX("hole lba", 0, lba);
    CHECK_HEX("hole run", SECSZ - 5, run);

    CHECK("locate unwritten tail", hype_file_rmap_locate(&m, 3 * SECSZ, &kind, &lba, &head, &run) == 0);
    CHECK("unwritten kind", kind == HYPE_RANGE_UNWRITTEN);
    CHECK_HEX("unwritten lba", 50, lba);
    CHECK_HEX("tail run capped at size", SECSZ / 2, run);

    CHECK("locate at size refused", hype_file_rmap_locate(&m, m.size_bytes, &kind, &lba, &head, &run) == -1);
    CHECK("locate past size refused", hype_file_rmap_locate(&m, ~0ull, &kind, &lba, &head, &run) == -1);

    /* malformed: size says more bytes than the ranges cover */
    m.size_bytes = 10 * SECSZ;
    CHECK("coverage gap refused", hype_file_rmap_locate(&m, 5 * SECSZ, &kind, &lba, &head, &run) == -1);

    /* malformed: range byte size overflows */
    hype_file_rmap_init(&m, ~0ull);
    m.count = 1;
    m.ranges[0].kind = HYPE_RANGE_HOLE;
    m.ranges[0].start_lba = 0;
    m.ranges[0].sector_count = ~0ull;
    CHECK("range byte overflow refused", hype_file_rmap_locate(&m, 0, &kind, &lba, &head, &run) == -1);
}

static void test_read_at(void) {
    hype_file_rmap_t m;
    uint8_t buf[4 * SECSZ];
    uint8_t expect[4 * SECSZ];
    unsigned i;

    fill_disk();

    /* DATA(2 sec @10) | HOLE(1) | DATA(1 @20) | UNWRITTEN(1 @30); 4.5 sec file */
    hype_file_rmap_init(&m, 4 * SECSZ + SECSZ / 2);
    hype_file_rmap_append(&m, HYPE_RANGE_DATA, 10, 2);
    hype_file_rmap_append(&m, HYPE_RANGE_HOLE, 0, 1);
    hype_file_rmap_append(&m, HYPE_RANGE_DATA, 20, 1);
    hype_file_rmap_append(&m, HYPE_RANGE_UNWRITTEN, 30, 1);

    /* aligned bulk read across all four kinds of seam */
    memset(buf, 0xAA, sizeof(buf));
    CHECK("read across seams", hype_file_rmap_read_at(&m, disk_read, 0, 0, buf, 4 * SECSZ) == 0);
    memcpy(expect, g_disk + 10 * SECSZ, 2 * SECSZ);
    memset(expect + 2 * SECSZ, 0, SECSZ);
    memcpy(expect + 3 * SECSZ, g_disk + 20 * SECSZ, SECSZ);
    CHECK("seam data correct", memcmp(buf, expect, 4 * SECSZ) == 0);

    /* the unwritten tail reads as zeros, never from the medium */
    memset(buf, 0xAA, sizeof(buf));
    g_reads = 0;
    CHECK("unwritten read ok",
          hype_file_rmap_read_at(&m, forbidden_read, 0, 4 * SECSZ, buf, SECSZ / 2) == 0);
    CHECK("unwritten synthesized without media", g_reads == 0);
    for (i = 0; i < SECSZ / 2; i++) {
        if (buf[i] != 0) break;
    }
    CHECK("unwritten zeros", i == SECSZ / 2);

    /* hole read likewise */
    g_reads = 0;
    CHECK("hole-only read ok",
          hype_file_rmap_read_at(&m, forbidden_read, 0, 2 * SECSZ, buf, SECSZ) == 0);
    CHECK("hole synthesized without media", g_reads == 0);

    /* ragged head + ragged tail inside one DATA sector */
    memset(buf, 0xAA, sizeof(buf));
    CHECK("ragged read ok", hype_file_rmap_read_at(&m, disk_read, 0, 100, buf, 200) == 0);
    CHECK("ragged data", memcmp(buf, g_disk + 10 * SECSZ + 100, 200) == 0);

    /* ragged read spanning a sector boundary within DATA */
    memset(buf, 0xAA, sizeof(buf));
    CHECK("straddle read ok", hype_file_rmap_read_at(&m, disk_read, 0, SECSZ - 50, buf, 100) == 0);
    CHECK("straddle data", memcmp(buf, g_disk + 10 * SECSZ + SECSZ - 50, 100) == 0);

    /* ragged read crossing a DATA -> HOLE seam mid-request */
    memset(buf, 0xAA, sizeof(buf));
    CHECK("data->hole ragged ok",
          hype_file_rmap_read_at(&m, disk_read, 0, 2 * SECSZ - 30, buf, 60) == 0);
    CHECK("data part", memcmp(buf, g_disk + 11 * SECSZ + SECSZ - 30, 30) == 0);
    for (i = 30; i < 60; i++) {
        if (buf[i] != 0) break;
    }
    CHECK("hole part zeros", i == 60);

    /* bounds: refused, not clamped */
    CHECK("past-EOF refused", hype_file_rmap_read_at(&m, disk_read, 0, 4 * SECSZ, buf, SECSZ) == -1);
    CHECK("offset+len overflow refused",
          hype_file_rmap_read_at(&m, disk_read, 0, ~0ull - 10, buf, 100) == -1);
    CHECK("len 0 no-op", hype_file_rmap_read_at(&m, disk_read, 0, 0, buf, 0) == 0);
    CHECK("NULL read refused", hype_file_rmap_read_at(&m, 0, 0, 2 * SECSZ, buf, 8) == -1);

    /* injected I/O failures: first read (bulk) and a later bounce */
    g_fail_after = 0;
    CHECK("bulk read failure surfaces", hype_file_rmap_read_at(&m, disk_read, 0, 0, buf, SECSZ) == -1);
    g_fail_after = 0;
    CHECK("bounce read failure surfaces", hype_file_rmap_read_at(&m, disk_read, 0, 10, buf, 8) == -1);
    g_fail_after = -1;

    /* malformed map surfaces through read_at */
    m.size_bytes = 40 * SECSZ;
    CHECK("malformed map read refused",
          hype_file_rmap_read_at(&m, disk_read, 0, 39 * SECSZ, buf, 8) == -1);
}

static void test_write_at(void) {
    hype_file_rmap_t m;
    uint8_t buf[3 * SECSZ];
    unsigned i;

    fill_disk();
    /* DATA(2 @10) | HOLE(1) | DATA(1 @20) | UNWRITTEN(1 @30); 4.5 sec file */
    hype_file_rmap_init(&m, 4 * SECSZ + SECSZ / 2);
    hype_file_rmap_append(&m, HYPE_RANGE_DATA, 10, 2);
    hype_file_rmap_append(&m, HYPE_RANGE_HOLE, 0, 1);
    hype_file_rmap_append(&m, HYPE_RANGE_DATA, 20, 1);
    hype_file_rmap_append(&m, HYPE_RANGE_UNWRITTEN, 30, 1);

    for (i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i ^ 0x5A);

    /* bulk aligned write inside DATA */
    CHECK_HEX("bulk write", 0, hype_file_rmap_write_at(&m, disk_read, disk_write, 0, 0, buf, SECSZ));
    CHECK("bulk landed", memcmp(g_disk + 10 * SECSZ, buf, SECSZ) == 0);
    /* ragged write inside DATA (RMW) */
    CHECK_HEX("ragged write", 0, hype_file_rmap_write_at(&m, disk_read, disk_write, 0, SECSZ + 100, buf, 50));
    CHECK("ragged landed", memcmp(g_disk + 11 * SECSZ + 100, buf, 50) == 0);
    /* write spanning two DATA sectors, ragged both ends */
    CHECK_HEX("straddle write", 0,
              hype_file_rmap_write_at(&m, disk_read, disk_write, 0, SECSZ - 20, buf, 40));
    CHECK("straddle landed", memcmp(g_disk + 10 * SECSZ + SECSZ - 20, buf, 20) == 0 &&
                                 memcmp(g_disk + 11 * SECSZ, buf + 20, 20) == 0);

    /* refusals: hole, unwritten, span crossing into hole -- nothing written */
    {
        uint8_t before[SECSZ];
        memcpy(before, g_disk + 11 * SECSZ, SECSZ);
        CHECK("write into hole refused",
              hype_file_rmap_write_at(&m, disk_read, disk_write, 0, 2 * SECSZ + 8, buf, 8) != 0);
        CHECK("write into unwritten refused",
              hype_file_rmap_write_at(&m, disk_read, disk_write, 0, 4 * SECSZ, buf, 8) != 0);
        CHECK("span into hole refused",
              hype_file_rmap_write_at(&m, disk_read, disk_write, 0, 2 * SECSZ - 8, buf, 16) != 0);
        CHECK("refused span wrote nothing",
              memcmp(before, g_disk + 11 * SECSZ, SECSZ) == 0);
    }

    /* bounds + args */
    CHECK("past EOF refused",
          hype_file_rmap_write_at(&m, disk_read, disk_write, 0, 4 * SECSZ + 200, buf, 200) != 0);
    CHECK("overflow refused",
          hype_file_rmap_write_at(&m, disk_read, disk_write, 0, ~0ull - 4, buf, 16) != 0);
    CHECK("len 0 no-op", hype_file_rmap_write_at(&m, disk_read, disk_write, 0, 0, buf, 0) == 0);
    CHECK("NULL read refused", hype_file_rmap_write_at(&m, 0, disk_write, 0, 0, buf, 8) != 0);
    CHECK("NULL write refused", hype_file_rmap_write_at(&m, disk_read, 0, 0, 0, buf, 8) != 0);

    /* injected I/O failures: bulk write, RMW read, RMW write */
    g_fail_after = 0;
    CHECK("bulk write failure surfaces",
          hype_file_rmap_write_at(&m, disk_read, fail_write, 0, 0, buf, SECSZ) != 0);
    g_fail_after = 0;
    CHECK("RMW read failure surfaces",
          hype_file_rmap_write_at(&m, disk_read, disk_write, 0, 10, buf, 8) != 0);
    g_fail_after = -1;
    g_wfail_after = 0;
    CHECK("RMW write failure surfaces",
          hype_file_rmap_write_at(&m, disk_read, disk_write, 0, 10, buf, 8) != 0);
    g_wfail_after = -1;

    /* malformed map surfaces */
    m.size_bytes = 40 * SECSZ;
    CHECK("malformed map write refused",
          hype_file_rmap_write_at(&m, disk_read, disk_write, 0, 39 * SECSZ, buf, 8) != 0);
}

int main(void) {
    test_append_and_coalesce();
    test_append_cap();
    test_validate();
    test_from_extents();
    test_lower_to_extents();
    test_locate();
    test_read_at();
    test_write_at();

    if (failures == 0) {
        printf("test_file_range: all tests passed\n");
        return 0;
    }
    printf("test_file_range: %d failure(s)\n", failures);
    return 1;
}
