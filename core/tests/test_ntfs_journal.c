#include <stdio.h>
#include <string.h>
#include "../ntfs.h"
#include "../ntfs_journal.h"

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

/* Minimal synthetic volume: just enough for hype_ntfs_mount() to succeed --
 * $MFT (record 0), $Volume/$VOLUME_INFORMATION (record 3), $UpCase (record
 * 10). No directories: this file tests the write-side record/dirty-flag/
 * txn/USN primitives, not path resolution (test_ntfs.c already covers that). */
#define SECSZ 512u
#define VOL_SECTORS 2048u
#define SPC 1u
#define REC_SIZE 1024u
#define MFT_LCN 100u
#define MFT_RECORDS 16u
#define UPCASE_LCN 200u
#define MFTMIRR_LCN 150u /* backs up records 0..3 -- 4 KiB, 8 clusters at SPC=1 */

static uint8_t g_vol[VOL_SECTORS * SECSZ];

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
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

static long g_write_fail_after = -1; /* -1: never fail; N: fail on the (N+1)th call */

static int vol_write_fails(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    if (g_write_fail_after >= 0 && g_write_fail_after-- == 0) return -1;
    return vol_write(ctx, lba, count, src);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static uint8_t *rec_ptr(unsigned n) { return g_vol + (MFT_LCN + n * (REC_SIZE / SECSZ)) * SECSZ; }

static void rec_init(unsigned n) {
    uint8_t *r = rec_ptr(n);
    memset(r, 0, REC_SIZE);
    r[0] = 'F'; r[1] = 'I'; r[2] = 'L'; r[3] = 'E';
    put16(r + 4, 0x30);
    put16(r + 6, 3);
    put16(r + 0x14, 0x38);
    put16(r + 0x16, 0x0001u);
    put32(r + 0x1C, REC_SIZE);
    put32(r + 0x38, 0xFFFFFFFFu);
    put32(r + 0x18, 0x40);
}

static uint32_t attr_add(unsigned n, uint32_t type, int non_res, const uint8_t *body,
                         uint32_t body_len) {
    uint8_t *r = rec_ptr(n);
    uint32_t off = 0x38;
    uint32_t total;
    while (off + 4u <= REC_SIZE) {
        uint32_t t = (uint32_t)r[off] | ((uint32_t)r[off + 1] << 8) |
                     ((uint32_t)r[off + 2] << 16) | ((uint32_t)r[off + 3] << 24);
        if (t == 0xFFFFFFFFu) break;
        off += (uint32_t)r[off + 4] | ((uint32_t)r[off + 5] << 8);
    }
    total = ((non_res ? 0x40u : 0x18u) + body_len + 7u) & ~7u;
    put32(r + off, type);
    put32(r + off + 4, total);
    r[off + 8] = (uint8_t)non_res;
    put16(r + off + 0x0C, 0);
    if (non_res) {
        put16(r + off + 0x20, 0x40);
        memcpy(r + off + 0x40, body, body_len);
    } else {
        put32(r + off + 0x10, body_len);
        put16(r + off + 0x14, 0x18);
        memcpy(r + off + 0x18, body, body_len);
    }
    put32(r + off + total, 0xFFFFFFFFu);
    put32(r + 0x18, off + total + 8u);
    return off;
}

static void nonres_sizes(unsigned n, uint32_t attr_off, uint64_t alloc, uint64_t real,
                         uint64_t init) {
    uint8_t *r = rec_ptr(n);
    put64(r + attr_off + 0x10, 0);
    put64(r + attr_off + 0x28, alloc);
    put64(r + attr_off + 0x30, real);
    put64(r + attr_off + 0x38, init);
}

static void rec_fixup(unsigned n) {
    uint8_t *r = rec_ptr(n);
    uint16_t usn = 0x0001;
    unsigned s;
    put16(r + 0x30, usn);
    for (s = 0; s < REC_SIZE / SECSZ; s++) {
        uint8_t *tail = r + (s + 1u) * SECSZ - 2u;
        put16(r + 0x30 + (s + 1u) * 2u, (uint16_t)(tail[0] | (tail[1] << 8)));
        put16(tail, usn);
    }
}

static void build_vol(int dirty) {
    uint8_t v[16];
    uint8_t rl[16];
    uint32_t off, n;
    unsigned i;

    memset(g_vol, 0, sizeof(g_vol));

    g_vol[3] = 'N'; g_vol[4] = 'T'; g_vol[5] = 'F'; g_vol[6] = 'S';
    g_vol[7] = ' '; g_vol[8] = ' '; g_vol[9] = ' '; g_vol[10] = ' ';
    put16(g_vol + 0x0B, 512);
    g_vol[0x0D] = SPC;
    put64(g_vol + 0x28, VOL_SECTORS);
    put64(g_vol + 0x30, MFT_LCN);
    g_vol[0x40] = 0xF6; /* -10: 1024-byte records */
    put16(g_vol + 0x1FE, 0xAA55);

    /* record 0: $MFT, $DATA = 32 clusters at MFT_LCN (covers MFT_RECORDS) */
    rec_init(0);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 32; put16(rl + n, MFT_LCN); n += 2; rl[n++] = 0;
    off = attr_add(0, 0x80, 1, rl, n);
    nonres_sizes(0, off, 32u * SECSZ, (uint64_t)MFT_RECORDS * REC_SIZE,
                (uint64_t)MFT_RECORDS * REC_SIZE);
    rec_fixup(0);

    /* record 1: $MFTMirr -- backs up records 0..3 (4 KiB @ MFTMIRR_LCN).
     * Content doesn't need to match $MFT yet; hype_ntfs_record_write's mirror
     * path always overwrites whatever's there when it writes a mirrored record. */
    rec_init(1);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 8; put16(rl + n, MFTMIRR_LCN); n += 2; rl[n++] = 0;
    off = attr_add(1, 0x80, 1, rl, n);
    nonres_sizes(1, off, 8u * SECSZ, 4u * REC_SIZE, 4u * REC_SIZE);
    rec_fixup(1);

    /* record 3: $Volume with $VOLUME_INFORMATION */
    rec_init(3);
    memset(v, 0, 12);
    v[8] = 3; v[9] = 1;
    put16(v + 10, dirty ? 0x0001 : 0x0000);
    attr_add(3, 0x70, 0, v, 12);
    rec_fixup(3);

    /* record 10: $UpCase -- 4 clusters (2 KiB, > the 512-byte cache floor) */
    rec_init(10);
    n = 0;
    rl[n++] = 0x21; rl[n++] = 4; put16(rl + n, UPCASE_LCN); n += 2; rl[n++] = 0;
    off = attr_add(10, 0x80, 1, rl, n);
    nonres_sizes(10, off, 4u * SECSZ, 2048, 2048);
    rec_fixup(10);
    for (i = 0; i < 1024u; i++) {
        uint16_t up = (uint16_t)i;
        if (i >= 'a' && i <= 'z') up = (uint16_t)(i - 'a' + 'A');
        put16(g_vol + UPCASE_LCN * SECSZ + i * 2u, up);
    }
}

/* ---- record read/write + fixup stamp ---- */

static void test_record_roundtrip(void) {
    hype_ntfs_t fs;
    uint8_t rec[REC_SIZE];

    build_vol(0);
    CHECK("mount succeeds on a clean volume", hype_ntfs_mount(vol_read, 0, &fs) == 0);

    CHECK("record 3 reads back", hype_ntfs_record_read(&fs, 3, rec) == 0);
    CHECK("read record has FILE magic", rec[0] == 'F' && rec[1] == 'I' && rec[2] == 'L' &&
                                            rec[3] == 'E');

    /* Mutate a byte outside any attribute header (safe scratch: right after
     * the record header, before attrs start at 0x38) and write it back. */
    rec[0x20] = 0x42;
    CHECK_HEX("record write succeeds", 0, hype_ntfs_record_write(&fs, vol_write, 3, rec, 7u));

    memset(rec, 0, sizeof(rec));
    CHECK_HEX("re-read after write succeeds", 0, hype_ntfs_record_read(&fs, 3, rec));
    CHECK_HEX("the write's mutation persisted", 0x42, rec[0x20]);
}

static void test_record_write_rejects_reserved_usn(void) {
    hype_ntfs_t fs;
    uint8_t rec[REC_SIZE];

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    CHECK("record 3 reads", hype_ntfs_record_read(&fs, 3, rec) == 0);

    CHECK("USN 0 is refused", hype_ntfs_record_write(&fs, vol_write, 3, rec, 0u) != 0);
    CHECK("USN 0xFFFF is refused", hype_ntfs_record_write(&fs, vol_write, 3, rec, 0xFFFFu) != 0);
}

static void test_fixup_stamp_roundtrips_with_apply(void) {
    hype_ntfs_t fs;
    uint8_t rec[REC_SIZE];

    /* fixup_apply (read-side) is static in ntfs.c; the roundtrip is proven
     * indirectly: record_read applies+verifies fixups on every call, so a
     * record freshly stamped and written by us must read back clean. */
    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    CHECK("record 0 reads", hype_ntfs_record_read(&fs, 0, rec) == 0);
    CHECK_HEX("stamp+write+re-read roundtrips", 0,
             hype_ntfs_record_write(&fs, vol_write, 0, rec, 99u));
    CHECK_HEX("record 0 still reads clean after re-stamp", 0, hype_ntfs_record_read(&fs, 0, rec));
}

static void test_record_write_mirrors_into_mftmirr(void) {
    hype_ntfs_t fs;
    uint8_t rec[REC_SIZE];
    uint8_t mirrored[REC_SIZE];

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    CHECK("record 3 reads", hype_ntfs_record_read(&fs, 3, rec) == 0);
    rec[0x20] = 0x55;
    CHECK_HEX("record 3 write succeeds", 0, hype_ntfs_record_write(&fs, vol_write, 3, rec, 9u));

    /* Record 3 is inside $MFTMirr's 4-record backup range in this fixture:
     * the mirrored copy at MFTMIRR_LCN's 4th slot must carry the same byte. */
    memcpy(mirrored, g_vol + (MFTMIRR_LCN + 3u * (REC_SIZE / SECSZ)) * SECSZ, REC_SIZE);
    CHECK_HEX("the mirror copy got the same mutation", 0x55, mirrored[0x20]);
}

static void test_record_write_skips_mirror_past_mirrored_range(void) {
    hype_ntfs_t fs;
    uint8_t rec[REC_SIZE];
    uint8_t before[8u * SECSZ];

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    memcpy(before, g_vol + MFTMIRR_LCN * SECSZ, sizeof(before));

    /* Record 10 ($UpCase) is well past $MFTMirr's 4-record range in this
     * fixture -- writing it must succeed and must NOT touch $MFTMirr at all. */
    CHECK("record 10 reads", hype_ntfs_record_read(&fs, 10, rec) == 0);
    rec[0x20] = 0x77;
    CHECK_HEX("record 10 write succeeds (unmirrored)", 0,
             hype_ntfs_record_write(&fs, vol_write, 10, rec, 11u));
    CHECK("$MFTMirr's bytes are untouched", memcmp(before, g_vol + MFTMIRR_LCN * SECSZ,
                                                   sizeof(before)) == 0);
}

static void test_record_write_fails_with_no_mftmirr(void) {
    hype_ntfs_t fs;
    uint8_t rec[REC_SIZE];

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    CHECK("record 3 reads", hype_ntfs_record_read(&fs, 3, rec) == 0);

    /* Destroy $MFTMirr's own record structurally (bad magic) so stream_map
     * can no longer resolve it -- any mirrored write must now fail closed
     * rather than silently leave $MFT and $MFTMirr disagreeing. */
    rec_ptr(1)[0] = 'X';
    CHECK("record write fails when $MFTMirr cannot be resolved",
         hype_ntfs_record_write(&fs, vol_write, 3, rec, 13u) != 0);
}

/* ---- dirty flag ---- */

static void test_dirty_get_set(void) {
    hype_ntfs_t fs;

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    CHECK_HEX("freshly-mounted clean volume reads clean", 0, hype_ntfs_volume_dirty_get(&fs));

    CHECK_HEX("setting dirty succeeds", 0, hype_ntfs_volume_dirty_set(&fs, vol_write, 1, 5u));
    CHECK_HEX("dirty flag now reads dirty", 1, hype_ntfs_volume_dirty_get(&fs));

    CHECK_HEX("clearing dirty succeeds", 0, hype_ntfs_volume_dirty_set(&fs, vol_write, 0, 6u));
    CHECK_HEX("dirty flag now reads clean", 0, hype_ntfs_volume_dirty_get(&fs));
}

static void test_dirty_get_on_structural_failure(void) {
    hype_ntfs_t fs;
    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    /* corrupt record 3's magic so record_read fails inside dirty_get */
    rec_ptr(3)[0] = 'X';
    CHECK_HEX("dirty_get on a broken record 3 fails closed (-1)", (uint32_t)-1,
             (uint32_t)hype_ntfs_volume_dirty_get(&fs));
}

/* ---- owner-guarded txn bracket (#596/decision-57 lesson) ---- */

#define BSP_APIC 0u
#define AP_APIC 1u

static void test_txn_open_close_bracket(void) {
    hype_ntfs_t fs;
    hype_ntfs_txn_t txn;

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    hype_ntfs_txn_init(&txn, BSP_APIC);

    CHECK_HEX("txn_open by the owner succeeds", 0, hype_ntfs_txn_open(&fs, vol_write, &txn, BSP_APIC));
    CHECK_HEX("volume is dirty once the txn is open", 1, hype_ntfs_volume_dirty_get(&fs));

    CHECK_HEX("txn_close by the owner succeeds", 0, hype_ntfs_txn_close(&fs, vol_write, &txn, BSP_APIC));
    CHECK_HEX("volume is clean once the txn is closed", 0, hype_ntfs_volume_dirty_get(&fs));
}

static void test_txn_open_refuses_non_owner_core(void) {
    hype_ntfs_t fs;
    hype_ntfs_txn_t txn;

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    hype_ntfs_txn_init(&txn, BSP_APIC);

    CHECK("txn_open from a non-owner core is refused", hype_ntfs_txn_open(&fs, vol_write, &txn, AP_APIC) != 0);
    CHECK_HEX("the volume stays clean -- the guard fired before any write", 0,
             hype_ntfs_volume_dirty_get(&fs));
}

static void test_txn_open_refuses_already_dirty(void) {
    hype_ntfs_t fs;
    hype_ntfs_txn_t txn;

    /* A volume already dirty at open time (a prior session ended uncleanly,
     * or another driver has it open) -- refuse, matching ntfs-3g's own
     * behaviour (plan.md decision 64), never guess. */
    build_vol(1);
    CHECK("mount refuses a dirty volume (unaffected, #337's own gate)",
         hype_ntfs_mount(vol_read, 0, &fs) != 0);

    /* Exercise txn_open's own dirty check directly against a volume that
     * mounted read-only-refused but whose structures are otherwise valid --
     * simulate by mounting the boot-sector-and-MFT parts without going
     * through the (correctly) refusing hype_ntfs_mount. Simpler: build clean,
     * then hand-set dirty before opening. */
    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    CHECK_HEX("hand-set dirty", 0, hype_ntfs_volume_dirty_set(&fs, vol_write, 1, 3u));

    hype_ntfs_txn_init(&txn, BSP_APIC);
    CHECK("txn_open on an already-dirty volume is refused", hype_ntfs_txn_open(&fs, vol_write, &txn, BSP_APIC) != 0);
}

static void test_txn_close_refuses_when_not_open(void) {
    hype_ntfs_t fs;
    hype_ntfs_txn_t txn;

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    hype_ntfs_txn_init(&txn, BSP_APIC);

    CHECK("closing a never-opened txn is refused", hype_ntfs_txn_close(&fs, vol_write, &txn, BSP_APIC) != 0);
}

static void test_txn_close_refuses_non_owner_core(void) {
    hype_ntfs_t fs;
    hype_ntfs_txn_t txn;

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    hype_ntfs_txn_init(&txn, BSP_APIC);
    CHECK_HEX("txn_open succeeds", 0, hype_ntfs_txn_open(&fs, vol_write, &txn, BSP_APIC));

    CHECK("txn_close from a non-owner core is refused", hype_ntfs_txn_close(&fs, vol_write, &txn, AP_APIC) != 0);
    CHECK_HEX("volume stays dirty -- the honest state, not silently cleared", 1,
             hype_ntfs_volume_dirty_get(&fs));
}

static void test_txn_open_fails_when_dirty_set_write_fails(void) {
    hype_ntfs_t fs;
    hype_ntfs_txn_t txn;

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    hype_ntfs_txn_init(&txn, BSP_APIC);

    g_write_fail_after = 0; /* the very write that would set dirty=1 fails */
    CHECK("txn_open is refused when the dirty-set write itself fails",
         hype_ntfs_txn_open(&fs, vol_write_fails, &txn, BSP_APIC) != 0);
    g_write_fail_after = -1;
    CHECK_HEX("volume is still clean -- the failed write never landed", 0,
             hype_ntfs_volume_dirty_get(&fs));
}

static void test_txn_close_fails_when_dirty_clear_write_fails(void) {
    hype_ntfs_t fs;
    hype_ntfs_txn_t txn;

    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    hype_ntfs_txn_init(&txn, BSP_APIC);
    CHECK_HEX("txn_open succeeds", 0, hype_ntfs_txn_open(&fs, vol_write, &txn, BSP_APIC));

    g_write_fail_after = 0; /* the write that would clear dirty=0 fails */
    CHECK("txn_close is refused when the dirty-clear write itself fails",
         hype_ntfs_txn_close(&fs, vol_write_fails, &txn, BSP_APIC) != 0);
    g_write_fail_after = -1;
    CHECK_HEX("volume stays dirty -- the honest state after a failed clear", 1,
             hype_ntfs_volume_dirty_get(&fs));
}

static void test_txn_null_guards(void) {
    hype_ntfs_t fs;
    hype_ntfs_txn_t txn;
    build_vol(0);
    CHECK("mount succeeds", hype_ntfs_mount(vol_read, 0, &fs) == 0);
    hype_ntfs_txn_init(&txn, BSP_APIC);

    CHECK("txn_open refuses a NULL fs", hype_ntfs_txn_open(0, vol_write, &txn, BSP_APIC) != 0);
    CHECK("txn_open refuses a NULL write fn", hype_ntfs_txn_open(&fs, 0, &txn, BSP_APIC) != 0);
    CHECK("txn_open refuses a NULL txn", hype_ntfs_txn_open(&fs, vol_write, 0, BSP_APIC) != 0);
    CHECK("txn_close refuses a NULL fs", hype_ntfs_txn_close(0, vol_write, &txn, BSP_APIC) != 0);
    hype_ntfs_txn_init(0, BSP_APIC); /* must not crash */
    CHECK_HEX("next_usn on a NULL txn returns the safe default", 1, hype_ntfs_txn_next_usn(0));
}

static void test_txn_next_usn_wraps(void) {
    hype_ntfs_txn_t txn;
    uint16_t first;
    int i;
    hype_ntfs_txn_init(&txn, BSP_APIC);
    first = hype_ntfs_txn_next_usn(&txn);
    CHECK_HEX("first USN is 1", 1, first);
    txn.next_usn = 0xFFFEu;
    CHECK_HEX("USN just below the wrap point", 0xFFFEu, hype_ntfs_txn_next_usn(&txn));
    /* next_usn is now 0xFFFF internally -- the getter must never RETURN that
     * or 0, only advance past it */
    for (i = 0; i < 3; i++) {
        uint16_t v = hype_ntfs_txn_next_usn(&txn);
        CHECK("USN never returns the reserved 0 or 0xFFFF sentinels", v != 0u && v != 0xFFFFu);
    }
}

/* ---- USN record encoding ---- */

static void test_usn_encode_decode_fields(void) {
    uint8_t buf[128];
    uint16_t name[4] = { 'a', 'b', 'c', 0 };
    uint8_t name_bytes[8];
    uint32_t len;
    unsigned i;

    for (i = 0; i < 3; i++) { name_bytes[i * 2] = (uint8_t)name[i]; name_bytes[i * 2 + 1] = 0; }

    len = hype_ntfs_usn_encode(buf, sizeof buf, 0x1000000000005ULL, 0x1000000000006ULL, 777ULL,
                               0x01D0000000000000ULL, HYPE_USN_REASON_FILE_CREATE, 0x20u,
                               name_bytes, 6u);
    CHECK("encode succeeds (nonzero length)", len != 0u);
    CHECK_HEX("RecordLength matches hype_ntfs_usn_record_size", hype_ntfs_usn_record_size(6u), len);
    CHECK_HEX("RecordLength field", len, (uint32_t)(buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24)));
    CHECK_HEX("MajorVersion is 2", 2u, (uint32_t)(buf[4] | (buf[5] << 8)));
    CHECK_HEX("FileReferenceNumber", 0x1000000000005ULL,
             (unsigned long long)(buf[8] | ((uint64_t)buf[9] << 8) | ((uint64_t)buf[10] << 16) |
                                  ((uint64_t)buf[11] << 24) | ((uint64_t)buf[12] << 32) |
                                  ((uint64_t)buf[13] << 40) | ((uint64_t)buf[14] << 48) |
                                  ((uint64_t)buf[15] << 56)));
    CHECK_HEX("Reason field", HYPE_USN_REASON_FILE_CREATE,
             (uint32_t)(buf[0x28] | (buf[0x29] << 8) | (buf[0x2A] << 16) | (buf[0x2B] << 24)));
    CHECK_HEX("FileNameLength field", 6u, (uint32_t)(buf[0x38] | (buf[0x39] << 8)));
    CHECK_HEX("FileNameOffset field", 0x3Cu, (uint32_t)(buf[0x3A] | (buf[0x3B] << 8)));
    CHECK_HEX("name bytes copied at FileNameOffset", 'a', buf[0x3C]);
    CHECK_HEX("total length is DWORD-aligned", 0u, len % 8u);
}

static void test_usn_encode_rejects_undersized_buffer(void) {
    uint8_t buf[8];
    uint8_t name_bytes[4] = { 0, 0, 0, 0 };
    CHECK_HEX("encode into a too-small buffer returns 0", 0u,
             hype_ntfs_usn_encode(buf, sizeof buf, 1, 2, 3, 4, 0, 0, name_bytes, 4u));
    CHECK_HEX("encode with a NULL out returns 0", 0u,
             hype_ntfs_usn_encode(0, 128u, 1, 2, 3, 4, 0, 0, name_bytes, 4u));
}

static void test_usn_record_size_alignment(void) {
    CHECK_HEX("a name of 0 bytes still aligns", 0u, hype_ntfs_usn_record_size(0u) % 8u);
    CHECK_HEX("a name of 6 bytes aligns", 0u, hype_ntfs_usn_record_size(6u) % 8u);
    CHECK("size grows with name length", hype_ntfs_usn_record_size(20u) > hype_ntfs_usn_record_size(2u));
}

/* ---- USN append: owner guard + no-active-journal no-op ---- */

static void test_usn_append_no_active_journal_is_ok(void) {
    hype_ntfs_txn_t txn;
    uint8_t rec[16] = {0};
    hype_ntfs_txn_init(&txn, BSP_APIC);
    txn.open = 1;
    CHECK_HEX("append with no $UsnJrnl stream is a silent no-op success", 0,
             hype_ntfs_usn_append(&txn, BSP_APIC, 0, vol_read, vol_write, 0, 0, rec, sizeof rec));
}

static void test_usn_append_refuses_when_txn_not_open(void) {
    hype_ntfs_txn_t txn;
    uint8_t rec[16] = {0};
    hype_ntfs_txn_init(&txn, BSP_APIC);
    CHECK("append before txn_open is refused", hype_ntfs_usn_append(&txn, BSP_APIC, 0, vol_read, vol_write, 0, 0, rec, sizeof rec) != 0);
}

static void test_usn_append_refuses_non_owner_core(void) {
    hype_ntfs_txn_t txn;
    uint8_t rec[16] = {0};
    hype_ntfs_txn_init(&txn, BSP_APIC);
    txn.open = 1;
    CHECK("append from a non-owner core is refused", hype_ntfs_usn_append(&txn, AP_APIC, 0, vol_read, vol_write, 0, 0, rec, sizeof rec) != 0);
}

/* USN journal streams are just $DATA ranges under the hood -- build one
 * directly with the #381 range-map API (bypassing path resolution, which
 * this fixture's minimal volume has no directory tree for) to exercise the
 * real append-and-write path. */
#define USNJRNL_LCN 300u

static void test_usn_append_writes_a_real_record(void) {
    hype_ntfs_txn_t txn;
    hype_file_rmap_t stream;
    uint8_t rec[96];
    uint16_t name[3] = { 'a', 'b', 'c' };
    uint8_t name_bytes[6];
    uint32_t len;
    unsigned i;

    memset(g_vol + USNJRNL_LCN * SECSZ, 0, 4u * SECSZ);
    hype_file_rmap_init(&stream, 4u * SECSZ);
    CHECK_HEX("range map for the $J stream builds", 0,
             hype_file_rmap_append(&stream, HYPE_RANGE_DATA, (uint64_t)USNJRNL_LCN, 4u));

    for (i = 0; i < 3; i++) { name_bytes[i * 2] = (uint8_t)name[i]; name_bytes[i * 2 + 1] = 0; }
    len = hype_ntfs_usn_encode(rec, sizeof rec, 5ULL, 6ULL, 0ULL, 0ULL,
                               HYPE_USN_REASON_FILE_CREATE, 0u, name_bytes, 6u);
    CHECK("record encodes", len != 0u);

    hype_ntfs_txn_init(&txn, BSP_APIC);
    txn.open = 1;
    CHECK_HEX("append into an active $J stream succeeds", 0,
             hype_ntfs_usn_append(&txn, BSP_APIC, &stream, vol_read, vol_write, 0, 0, rec, len));

    CHECK("the encoded record's bytes actually reached the medium",
         memcmp(g_vol + USNJRNL_LCN * SECSZ, rec, len) == 0);
}

static void test_usn_append_null_guards(void) {
    hype_ntfs_txn_t txn;
    uint8_t rec[16] = {0};
    hype_ntfs_txn_init(&txn, BSP_APIC);
    txn.open = 1;
    CHECK("append refuses a NULL txn", hype_ntfs_usn_append(0, BSP_APIC, 0, vol_read, vol_write, 0, 0, rec, sizeof rec) != 0);
    CHECK("append refuses a NULL read fn", hype_ntfs_usn_append(&txn, BSP_APIC, 0, 0, vol_write, 0, 0, rec, sizeof rec) != 0);
    CHECK("append refuses a NULL write fn", hype_ntfs_usn_append(&txn, BSP_APIC, 0, vol_read, 0, 0, 0, rec, sizeof rec) != 0);
    CHECK("append refuses a NULL record", hype_ntfs_usn_append(&txn, BSP_APIC, 0, vol_read, vol_write, 0, 0, 0, sizeof rec) != 0);
}

int main(void) {
    test_record_roundtrip();
    test_record_write_rejects_reserved_usn();
    test_fixup_stamp_roundtrips_with_apply();
    test_record_write_mirrors_into_mftmirr();
    test_record_write_skips_mirror_past_mirrored_range();
    test_record_write_fails_with_no_mftmirr();
    test_dirty_get_set();
    test_dirty_get_on_structural_failure();
    test_txn_open_close_bracket();
    test_txn_open_refuses_non_owner_core();
    test_txn_open_refuses_already_dirty();
    test_txn_close_refuses_when_not_open();
    test_txn_open_fails_when_dirty_set_write_fails();
    test_txn_close_fails_when_dirty_clear_write_fails();
    test_txn_close_refuses_non_owner_core();
    test_txn_null_guards();
    test_txn_next_usn_wraps();
    test_usn_encode_decode_fields();
    test_usn_encode_rejects_undersized_buffer();
    test_usn_record_size_alignment();
    test_usn_append_writes_a_real_record();
    test_usn_append_no_active_journal_is_ok();
    test_usn_append_refuses_when_txn_not_open();
    test_usn_append_refuses_non_owner_core();
    test_usn_append_null_guards();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
