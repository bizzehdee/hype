#include <stdio.h>
#include <string.h>
#include "../iso_stream.h"

static int failures = 0;
static unsigned g_reads; /* count disk reads, to check the bounce loop */

#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)
#define CHECK_INT(desc, expected, actual) CHECK_HEX(desc, expected, actual)

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Deterministic byte at absolute disk offset D -- a mixing hash so an off-by-one
 * or wrong-sector bug shows up (unlike a low-byte pattern that repeats every 256). */
static uint8_t pat(uint64_t d) {
    return (uint8_t)((d * 1103515245ull + 12345ull) >> 16);
}

#define PART_START 4096ull
#define ISO_SIZE 200000ull

/* Synthetic infinite disk: sector `lba` holds pat(lba*512 + i). */
static int fake_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    (void)ctx;
    g_reads++;
    for (i = 0; i < count * 512u; i++) {
        d[i] = pat(lba * 512ull + i);
    }
    return 0;
}

/* The correct streamed byte for logical ISO offset O = disk byte at
 * PART_START*512 + O. */
static uint8_t expect_byte(uint64_t off) {
    return pat(PART_START * 512ull + off);
}

static void check_range(const char *desc, hype_iso_stream_t *s, uint64_t off, uint32_t len) {
    static uint8_t buf[262144];
    uint32_t i;
    int mism = -1;
    CHECK_HEX(desc, 0, hype_iso_stream_read(s, off, buf, len));
    for (i = 0; i < len; i++) {
        if (buf[i] != expect_byte(off + i)) {
            mism = (int)i;
            break;
        }
    }
    CHECK_HEX(desc, -1, mism); /* -1 == no mismatch */
}

/* #327 ------------------------------------------------------------------------------------- */

/*
 * A deliberately fragmented map: three runs, out of order and non-adjacent, so a bug that assumes
 * contiguity or ignores the map cannot pass. Logical bytes 0..(4*512) come from LBA 9000,
 * the next 2 sectors from LBA 100, the next 3 from LBA 50000.
 */
static void frag_setup(hype_iso_stream_t *s) {
    unsigned i;
    for (i = 0; i < HYPE_ISO_STREAM_MAX_EXTENTS; i++) {
        s->extents[i].start_lba = 0;
        s->extents[i].sector_count = 0;
    }
    s->read = fake_read;
    s->ctx = 0;
    s->part_start_lba = 0; /* must be IGNORED once a map is present */
    s->extents[0].start_lba = 9000ull; s->extents[0].sector_count = 4ull;
    s->extents[1].start_lba = 100ull;  s->extents[1].sector_count = 2ull;
    s->extents[2].start_lba = 50000ull; s->extents[2].sector_count = 3ull;
    s->extent_count = 3u;
    s->iso_size = 9ull * 512ull;
}

/* Where logical offset O really lives, given frag_setup's map. */
static uint64_t frag_disk_byte(uint64_t off) {
    if (off < 4ull * 512ull) {
        return 9000ull * 512ull + off;
    }
    if (off < 6ull * 512ull) {
        return 100ull * 512ull + (off - 4ull * 512ull);
    }
    return 50000ull * 512ull + (off - 6ull * 512ull);
}

static void test_locate_single_run_is_unchanged(void) {
    /* extent_count == 0 is the raw-partition / one-extent case every existing caller sets up. */
    hype_iso_stream_t s;
    memset(&s, 0, sizeof(s)); /* see iso_stream.h: the struct must be zeroed */
    uint64_t lba = 0, run = 0;
    uint32_t head = 0;

    s.read = fake_read; s.ctx = 0; s.part_start_lba = PART_START; s.iso_size = ISO_SIZE;
    s.extent_count = 0u;

    CHECK_HEX("offset 0 locates", 0, hype_iso_stream_locate(&s, 0, &lba, &head, &run));
    CHECK_HEX("lba is the partition start", PART_START, lba);
    CHECK_HEX("head 0", 0, head);
    CHECK_HEX("whole ISO is one run", ISO_SIZE, run);

    CHECK_HEX("mid-sector offset locates", 0, hype_iso_stream_locate(&s, 1000, &lba, &head, &run));
    CHECK_HEX("lba advances by whole sectors", PART_START + 1u, lba);
    CHECK_HEX("head is the remainder", 1000u - 512u, head);

    /* Past the end must be refused, not clamped. */
    CHECK_HEX("off == iso_size refused", -1, hype_iso_stream_locate(&s, ISO_SIZE, &lba, &head, &run));
}

static void test_locate_walks_the_extent_map(void) {
    hype_iso_stream_t s;
    memset(&s, 0, sizeof(s)); /* see iso_stream.h: the struct must be zeroed */
    uint64_t lba = 0, run = 0;
    uint32_t head = 0;
    frag_setup(&s);

    /* First byte of each extent, and the byte just before each boundary. */
    CHECK_HEX("extent 0 start", 0, hype_iso_stream_locate(&s, 0, &lba, &head, &run));
    CHECK_HEX("  lba", 9000ull, lba);
    CHECK_HEX("  run is the whole extent", 4ull * 512ull, run);

    CHECK_HEX("last byte of extent 0", 0,
              hype_iso_stream_locate(&s, 4ull * 512ull - 1ull, &lba, &head, &run));
    CHECK_HEX("  lba is extent 0's last sector", 9003ull, lba);
    CHECK_HEX("  run is 1 byte", 1ull, run);

    /* THE case this ticket is about: the byte immediately after an extent boundary must jump. */
    CHECK_HEX("first byte of extent 1", 0,
              hype_iso_stream_locate(&s, 4ull * 512ull, &lba, &head, &run));
    CHECK_HEX("  lba jumps to extent 1, NOT 9004", 100ull, lba);
    CHECK_HEX("  head 0", 0, head);
    CHECK_HEX("  run is extent 1", 2ull * 512ull, run);

    CHECK_HEX("first byte of extent 2", 0,
              hype_iso_stream_locate(&s, 6ull * 512ull, &lba, &head, &run));
    CHECK_HEX("  lba jumps to extent 2", 50000ull, lba);

    /* part_start_lba must play no part once a map exists. */
    CHECK_HEX("map wins over part_start_lba", 0, hype_iso_stream_locate(&s, 0, &lba, &head, &run));
    CHECK_HEX("  not part_start_lba", 1, lba != s.part_start_lba || s.part_start_lba == 9000ull);

    /* Past the mapped runs is refused. */
    CHECK_HEX("past the map refused", -1,
              hype_iso_stream_locate(&s, 9ull * 512ull, &lba, &head, &run));
}

static void test_read_across_extent_boundaries(void) {
    /*
     * A read that STRADDLES a boundary is the case a naive implementation gets wrong: it would
     * read straight on into the sectors after extent 0, which belong to a different part of the
     * file. Every byte is checked against where it actually lives on disk.
     */
    hype_iso_stream_t s;
    memset(&s, 0, sizeof(s)); /* see iso_stream.h: the struct must be zeroed */
    uint8_t buf[9 * 512];
    uint64_t i;
    unsigned bad = 0;
    frag_setup(&s);

    /* Straddle boundary 0->1: 100 bytes ending 50 past it. */
    CHECK_HEX("straddling read succeeds", 0,
              hype_iso_stream_read(&s, 4ull * 512ull - 50ull, buf, 100u));
    for (i = 0; i < 100ull; i++) {
        if (buf[i] != pat(frag_disk_byte(4ull * 512ull - 50ull + i))) {
            bad++;
        }
    }
    CHECK_HEX("every byte across boundary 0->1 correct", 0, bad);

    /* The whole ISO in one call -- spans all three extents and both boundaries. */
    bad = 0;
    CHECK_HEX("whole-ISO read succeeds", 0, hype_iso_stream_read(&s, 0, buf, 9u * 512u));
    for (i = 0; i < 9ull * 512ull; i++) {
        if (buf[i] != pat(frag_disk_byte(i))) {
            bad++;
        }
    }
    CHECK_HEX("every byte of a 3-extent read correct", 0, bad);

    /* Off-the-end still refused with a map present. */
    CHECK_HEX("read past the end refused", -1, hype_iso_stream_read(&s, 9ull * 512ull - 1ull, buf, 2u));
}

static void test_read_with_many_extents(void) {
    /*
     * 64 single-sector extents in descending disk order -- the cap, and a layout where any
     * assumption of ascending or contiguous LBAs fails.
     */
    hype_iso_stream_t s;
    memset(&s, 0, sizeof(s)); /* see iso_stream.h: the struct must be zeroed */
    static uint8_t buf[HYPE_ISO_STREAM_MAX_EXTENTS * 512u];
    unsigned i;
    unsigned bad = 0;

    s.read = fake_read; s.ctx = 0; s.part_start_lba = 0;
    for (i = 0; i < HYPE_ISO_STREAM_MAX_EXTENTS; i++) {
        s.extents[i].start_lba = 70000ull - (uint64_t)i * 7ull;
        s.extents[i].sector_count = 1ull;
    }
    s.extent_count = HYPE_ISO_STREAM_MAX_EXTENTS;
    s.iso_size = (uint64_t)HYPE_ISO_STREAM_MAX_EXTENTS * 512ull;

    CHECK_HEX("64-extent read succeeds", 0,
              hype_iso_stream_read(&s, 0, buf, HYPE_ISO_STREAM_MAX_EXTENTS * 512u));
    for (i = 0; i < HYPE_ISO_STREAM_MAX_EXTENTS; i++) {
        uint64_t b;
        for (b = 0; b < 512ull; b++) {
            if (buf[i * 512u + b] != pat((70000ull - (uint64_t)i * 7ull) * 512ull + b)) {
                bad++;
            }
        }
    }
    CHECK_HEX("every byte of a 64-extent read correct", 0, bad);
}

/*
 * #327: LARGE extents, so a single request spans more than the 128-sector bounce buffer AND
 * crosses extent boundaries. The earlier cases all used 1-4 sector extents, which never made the
 * bounce cap and the extent clamp interact -- and that interaction is where a real 15-extent ISO
 * failed on hardware.
 */
static void test_read_large_extents_across_bounce_cap(void) {
    hype_iso_stream_t s;
    memset(&s, 0, sizeof(s)); /* see iso_stream.h: the struct must be zeroed */
    static uint8_t buf[3u * 256u * 512u]; /* exactly the whole mapped ISO -- sized, not guessed */
    unsigned i;
    unsigned bad = 0;
    const uint64_t esec = 256ull; /* 256 sectors = 128 KiB per extent: 2x the bounce buffer */

    for (i = 0; i < HYPE_ISO_STREAM_MAX_EXTENTS; i++) {
        s.extents[i].start_lba = 0;
        s.extents[i].sector_count = 0;
    }
    s.read = fake_read; s.ctx = 0; s.part_start_lba = 0;
    s.extents[0].start_lba = 20000ull; s.extents[0].sector_count = esec;
    s.extents[1].start_lba = 5000ull;  s.extents[1].sector_count = esec;
    s.extents[2].start_lba = 90000ull; s.extents[2].sector_count = esec;
    s.extent_count = 3u;
    s.iso_size = 3ull * esec * 512ull;

    CHECK_HEX("large-extent read succeeds", 0,
              hype_iso_stream_read(&s, 0, buf, (uint32_t)(3ull * esec * 512ull)));
    for (i = 0; i < 3u; i++) {
        uint64_t base = (i == 0) ? 20000ull : (i == 1) ? 5000ull : 90000ull;
        uint64_t b;
        for (b = 0; b < esec * 512ull; b++) {
            if (buf[(uint64_t)i * esec * 512ull + b] != pat(base * 512ull + b)) {
                bad++;
            }
        }
    }
    CHECK_HEX("every byte correct across 3 large extents", 0, bad);

    /* A single request that starts mid-extent and ends mid-extent-two, longer than the bounce. */
    bad = 0;
    CHECK_HEX("mid-to-mid read succeeds", 0,
              hype_iso_stream_read(&s, esec * 512ull - 1000ull, buf, 200000u));
    for (i = 0; i < 200000u; i++) {
        uint64_t off = esec * 512ull - 1000ull + i;
        uint64_t want;
        if (off < esec * 512ull) {
            want = 20000ull * 512ull + off;
        } else if (off < 2ull * esec * 512ull) {
            want = 5000ull * 512ull + (off - esec * 512ull);
        } else {
            want = 90000ull * 512ull + (off - 2ull * esec * 512ull);
        }
        if (buf[i] != pat(want)) {
            bad++;
        }
    }
    CHECK_HEX("every byte correct on a bounce-spanning cross-extent read", 0, bad);
}

/* A disk that always fails, to exercise the read-error path. */
static int fail_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx; (void)lba; (void)count; (void)dst;
    return -1;
}

static void test_refusals_and_error_paths(void) {
    hype_iso_stream_t s;
    memset(&s, 0, sizeof(s)); /* see iso_stream.h: the struct must be zeroed */
    uint8_t buf[64];

    /* Unset layout: no read fn, or no size. Both must refuse rather than dereference. */
    s.read = 0; s.ctx = 0; s.part_start_lba = PART_START; s.iso_size = ISO_SIZE; s.extent_count = 0;
    CHECK_HEX("no read fn refused", -1, hype_iso_stream_read(&s, 0, buf, 1u));
    s.read = fake_read; s.iso_size = 0u;
    CHECK_HEX("zero iso_size refused", -1, hype_iso_stream_read(&s, 0, buf, 1u));

    /* Bounds: off past the end, len past the end, and the overflow case. */
    s.iso_size = ISO_SIZE;
    CHECK_HEX("off past end refused", -1, hype_iso_stream_read(&s, ISO_SIZE + 1ull, buf, 1u));
    CHECK_HEX("len past end refused", -1, hype_iso_stream_read(&s, ISO_SIZE - 4ull, buf, 64u));
    CHECK_HEX("off at exactly the end with len 0 is a no-op", 0,
              hype_iso_stream_read(&s, ISO_SIZE, buf, 0u));

    /* A failing disk read must propagate, not return partial data silently. */
    s.read = fail_read;
    CHECK_HEX("disk read failure propagates", -1, hype_iso_stream_read(&s, 0, buf, 64u));

    /* Same, with an extent map, so the mapped path's error branch is covered too. */
    frag_setup(&s);
    s.read = fail_read;
    CHECK_HEX("disk failure propagates with a map", -1, hype_iso_stream_read(&s, 0, buf, 64u));
}

static void test_map_shorter_than_iso_size_is_refused_midway(void) {
    /*
     * A map that does not cover the whole declared iso_size is a resolver bug, but it must fail
     * CLEANLY at the point the map runs out rather than reading whatever LBA the arithmetic lands
     * on. This is the mid-read locate() failure path.
     */
    hype_iso_stream_t s;
    memset(&s, 0, sizeof(s)); /* see iso_stream.h: the struct must be zeroed */
    static uint8_t buf[4096];
    unsigned i;

    for (i = 0; i < HYPE_ISO_STREAM_MAX_EXTENTS; i++) {
        s.extents[i].start_lba = 0; s.extents[i].sector_count = 0;
    }
    s.read = fake_read; s.ctx = 0; s.part_start_lba = 0;
    s.extents[0].start_lba = 1000ull; s.extents[0].sector_count = 2ull; /* only 1 KiB mapped */
    s.extent_count = 1u;
    s.iso_size = 4096ull; /* but 4 KiB declared */

    /* Inside the map: fine. */
    CHECK_HEX("read inside the map succeeds", 0, hype_iso_stream_read(&s, 0, buf, 1024u));
    /* Crossing out of the map: refused, not silently wrong. */
    CHECK_HEX("read past the mapped runs refused", -1, hype_iso_stream_read(&s, 0, buf, 2048u));
    CHECK_HEX("read starting past the map refused", -1, hype_iso_stream_read(&s, 2048u, buf, 16u));
}

/*
 * #343: does hype_iso_stream_read() ever write PAST `len`?
 *
 * Why this matters far beyond the read itself: the ATAPI datapath hands this function a pointer INTO
 * GUEST RAM (guest_dma_xlate'd from a PRD and bounds-checked for exactly `chunk` bytes). If the read
 * can write even one byte beyond `len`, hype silently corrupts guest memory -- and the symptom would be
 * exactly #343: an intermittent guest page fault somewhere unrelated, with garbage pointers, appearing
 * only on the streaming path (0 panics across ~34 RAM-preload-era runs, ~50% on streaming).
 *
 * Canary-guarded destination, swept across sizes that straddle a sector boundary, an EXTENT boundary
 * and the bounce-buffer cap -- the three places the clamping arithmetic could be wrong.
 */
static void test_read_never_writes_past_len(void) {
    static uint8_t guarded[400000];
    hype_iso_stream_t s;
    static const uint64_t offs[] = {0, 1, 511, 512, 2047, 2048, 100000, 1048575, 1048576};
    static const uint32_t lens[] = {1, 7, 512, 513, 2048, 4096, 65536, 100000};
    unsigned oi, li, trial = 0;

    memset(&s, 0, sizeof(s));
    s.read = fake_read;
    s.ctx = 0;
    s.part_start_lba = PART_START;
    s.iso_size = ISO_SIZE;
    /* A multi-extent map, so the per-extent clamping is exercised, not the contiguous fast path. */
    s.extent_count = 3u;
    s.extents[0].start_lba = PART_START;
    s.extents[0].sector_count = 64u;
    s.extents[1].start_lba = PART_START + 4096u;
    s.extents[1].sector_count = 128u;
    s.extents[2].start_lba = PART_START + 8192u;
    s.extents[2].sector_count = 8192u;

    for (oi = 0; oi < sizeof(offs) / sizeof(offs[0]); oi++) {
        for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
            uint64_t off = offs[oi];
            uint32_t len = lens[li];
            unsigned long i;

            if ((uint64_t)len > sizeof(guarded)) {
                continue;
            }
            memset(guarded, 0xC5, sizeof(guarded));
            (void)hype_iso_stream_read(&s, off, guarded, len);
            trial++;
            for (i = len; i < sizeof(guarded); i++) {
                if (guarded[i] != 0xC5u) {
                    printf("FAIL: wrote PAST len: off=%llu len=%u first stray byte at +%lu\n",
                           (unsigned long long)off, len, i);
                    failures++;
                    break;
                }
            }
        }
    }
    CHECK_HEX("the sweep ran", 1u, (trial > 20u) ? 1u : 0u);
}


/* ---- #365: read-ahead retention ---- */

/*
 * The property that matters is NOT that the cache is fast -- it is that a cached
 * read is INDISTINGUISHABLE from an uncached one. A retention bug serves stale
 * bytes, which at the guest looks exactly like #343's corruption and is just as
 * hard to attribute afterwards. So every cache test below checks bytes, and the
 * hit/miss counters are only ever a secondary assertion.
 */
static void test_sequential_reads_hit_the_cache_and_return_correct_bytes(void) {
    hype_iso_stream_t s;
    uint64_t hits = 0, misses = 0, off;
    memset(&s, 0, sizeof s);
    s.read = fake_read;
    s.ctx = 0;
    s.part_start_lba = PART_START;
    s.iso_size = ISO_SIZE;

    g_reads = 0;
    /* 32 consecutive 2 KB reads -- the OpenBSD pattern that motivated this. */
    for (off = 0; off < 32ull * 2048ull; off += 2048ull) {
        check_range("sequential 2KB read", &s, off, 2048u);
    }
    hype_iso_stream_cache_stats(&s, &hits, &misses);
    CHECK("most reads were served from the buffer", hits > misses);
    CHECK("device reads far fewer than guest reads", g_reads < 32u);
}

/* A backward seek must NOT be served from a buffer that starts later. */
static void test_backward_seek_is_a_miss_and_still_correct(void) {
    hype_iso_stream_t s;
    memset(&s, 0, sizeof s);
    s.read = fake_read;
    s.part_start_lba = PART_START;
    s.iso_size = ISO_SIZE;

    check_range("forward", &s, 100000u, 2048u);
    check_range("backward", &s, 0u, 2048u);       /* before the cached range */
    check_range("far forward", &s, 150000u, 512u); /* past it */
}

/*
 * The decisive one: if the medium changes underneath, a stream that was NOT
 * invalidated may legitimately serve old bytes -- so invalidation must actually
 * work. Simulated by flipping the backing pattern.
 */
static int g_alt_backing = 0;
static int alt_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    (void)ctx;
    g_reads++;
    for (i = 0; i < count * 512u; i++) {
        d[i] = (uint8_t)(pat(lba * 512ull + i) ^ (g_alt_backing ? 0xFFu : 0x00u));
    }
    return 0;
}

static void test_invalidate_forces_a_refetch(void) {
    hype_iso_stream_t s;
    static uint8_t a[512], b[512];
    memset(&s, 0, sizeof s);
    s.read = alt_read;
    s.part_start_lba = PART_START;
    s.iso_size = ISO_SIZE;

    g_alt_backing = 0;
    CHECK_INT("first read ok", 0, hype_iso_stream_read(&s, 0u, a, sizeof a));

    /* Medium changes. Without invalidation the stream may serve the old copy --
     * that is the hazard this hook exists for. */
    g_alt_backing = 1;
    hype_iso_stream_invalidate(&s);
    CHECK_INT("read after invalidate ok", 0, hype_iso_stream_read(&s, 0u, b, sizeof b));
    CHECK("invalidate forced a refetch of the NEW contents", memcmp(a, b, sizeof a) != 0);
}

/* A failed device read must leave the buffer marked unknown, not merely old:
 * its contents after a partial/failed transfer are undefined. */
static void test_failed_read_clears_the_cache(void) {
    hype_iso_stream_t s;
    static uint8_t buf[512];
    uint64_t hits = 0, misses = 0;
    memset(&s, 0, sizeof s);
    s.read = fake_read;
    s.part_start_lba = PART_START;
    s.iso_size = ISO_SIZE;

    CHECK_INT("prime the cache", 0, hype_iso_stream_read(&s, 0u, buf, sizeof buf));
    s.read = fail_read;
    /* A DIFFERENT range: re-reading the primed one would be served from the buffer and
     * never reach the failing device at all -- which is the cache working, not a failure. */
    CHECK("read fails", hype_iso_stream_read(&s, 150000u, buf, sizeof buf) != 0);
    CHECK_INT("cache marked empty", 0u, s.cache_sectors);

    s.read = fake_read;
    hype_iso_stream_cache_stats(&s, &hits, &misses);
    CHECK_INT("and the next read is a miss, not a stale hit", 0,
              hype_iso_stream_read(&s, 0u, buf, sizeof buf));
}

static void test_cache_stats_null_safety(void) {
    uint64_t h = 99, m = 99;
    hype_iso_stream_cache_stats(0, &h, &m);
    CHECK_INT("null stream reports zero hits", 0, (long long)h);
    CHECK_INT("null stream reports zero misses", 0, (long long)m);
    hype_iso_stream_invalidate(0); /* must not fault */
}


/*
 * #365: read-ahead must be spent only where it pays.
 *
 * Measured on hardware with two guests: a sequential guest hit 95% while a seeking guest hit 13%,
 * and USB moved 55.3 MB to deliver 9.8 MB. The unconditional 128-sector fetch was serving the
 * seeking guest 64 KiB per 2 KiB request and missing again immediately.
 */
static uint64_t g_ra_sectors_read;
static uint64_t g_ra_calls;

static int ra_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    unsigned char *p = (unsigned char *)dst;
    uint64_t i;
    (void)ctx;
    g_ra_calls++;
    g_ra_sectors_read += count;
    for (i = 0; i < (uint64_t)count * HYPE_BLK_SECTOR_SIZE; i++) {
        p[i] = (unsigned char)((lba + i / HYPE_BLK_SECTOR_SIZE) & 0xFFu);
    }
    return 0;
}

static void ra_init(hype_iso_stream_t *s) {
    unsigned i;
    for (i = 0; i < sizeof(*s); i++) ((unsigned char *)s)[i] = 0;
    s->read = ra_read;
    s->ctx = 0;
    s->part_start_lba = 0;
    s->iso_size = (uint64_t)(1u << 20) * HYPE_BLK_SECTOR_SIZE;
    s->extent_count = 0; /* one contiguous run from part_start_lba */
    g_ra_sectors_read = 0;
    g_ra_calls = 0;
}

static void test_seeking_reads_do_not_fetch_read_ahead(void) {
    hype_iso_stream_t s;
    unsigned char buf[HYPE_BLK_SECTOR_SIZE];
    unsigned i;
    ra_init(&s);
    /* Ten reads scattered far apart -- nothing continues anything. */
    for (i = 0; i < 10u; i++) {
        CHECK_INT("seeking read succeeds", 0,
                  hype_iso_stream_read(&s, (uint64_t)i * 4096u * HYPE_BLK_SECTOR_SIZE, buf,
                                       sizeof(buf)));
    }
    CHECK_INT("ten device reads", 10, (int)g_ra_calls);
    /* One sector each. Before this change it fetched 128 apiece: 1280 sectors for 10 wanted. */
    CHECK_INT("no read-ahead is fetched for a seeking pattern", 10, (int)g_ra_sectors_read);
}

static void test_sequential_reads_still_get_full_read_ahead(void) {
    hype_iso_stream_t s;
    unsigned char buf[HYPE_BLK_SECTOR_SIZE];
    unsigned i;
    ra_init(&s);
    for (i = 0; i < 64u; i++) {
        CHECK_INT("sequential read succeeds", 0,
                  hype_iso_stream_read(&s, (uint64_t)i * HYPE_BLK_SECTOR_SIZE, buf, sizeof(buf)));
    }
    /* 64 contiguous sectors must come from far fewer than 64 device reads. */
    CHECK("a sequential run is served mostly from cache", g_ra_calls <= 2u);
    CHECK("and its hits dominate", s.cache_hits > s.cache_misses);
}

/* A seek followed by a sequential run must recover read-ahead, not stay penalised. */
static void test_read_ahead_recovers_after_a_seek(void) {
    hype_iso_stream_t s;
    unsigned char buf[HYPE_BLK_SECTOR_SIZE];
    unsigned i;
    uint64_t base = 500u * HYPE_BLK_SECTOR_SIZE;
    ra_init(&s);
    CHECK_INT("the seek itself succeeds", 0, hype_iso_stream_read(&s, base, buf, sizeof(buf)));
    CHECK_INT("the seek fetched one sector only", 1, (int)g_ra_sectors_read);
    for (i = 1; i < 40u; i++) {
        CHECK_INT("continuing read succeeds", 0,
                  hype_iso_stream_read(&s, base + (uint64_t)i * HYPE_BLK_SECTOR_SIZE, buf,
                                       sizeof(buf)));
    }
    CHECK("read-ahead resumed once the pattern became sequential", g_ra_calls <= 3u);
}

/* Invalidation must clear the sequential history too, or the next read after a media change is
 * treated as continuing a stream that no longer exists. */
static void test_invalidate_clears_the_sequential_history(void) {
    hype_iso_stream_t s;
    unsigned char buf[HYPE_BLK_SECTOR_SIZE];
    ra_init(&s);
    (void)hype_iso_stream_read(&s, 0, buf, sizeof(buf));
    hype_iso_stream_invalidate(&s);
    CHECK_INT("history is cleared", 0, (int)s.next_seq_lba);
}

int main(void) {
    test_sequential_reads_hit_the_cache_and_return_correct_bytes();
    test_backward_seek_is_a_miss_and_still_correct();
    test_invalidate_forces_a_refetch();
    test_failed_read_clears_the_cache();
    test_cache_stats_null_safety();
    test_read_never_writes_past_len();
    test_refusals_and_error_paths();
    test_map_shorter_than_iso_size_is_refused_midway();
    test_read_large_extents_across_bounce_cap();
    test_locate_single_run_is_unchanged();
    test_locate_walks_the_extent_map();
    test_read_across_extent_boundaries();
    test_read_with_many_extents();
    hype_iso_stream_t s;
    memset(&s, 0, sizeof(s)); /* see iso_stream.h: the struct must be zeroed */
    s.read = fake_read;
    s.ctx = 0;
    s.part_start_lba = PART_START;
    s.iso_size = ISO_SIZE;

    check_range("aligned 1 sector", &s, 0, 512);
    check_range("aligned multi-sector", &s, 1024, 2048);
    check_range("misaligned within a sector", &s, 100, 50);
    check_range("misaligned spanning a sector boundary", &s, 500, 1000);
    check_range("2048-byte CD sector at a high offset", &s, 4096 * 2048ull % ISO_SIZE, 2048);

    /* Large read must loop the 64 KiB bounce and still assemble correctly. */
    g_reads = 0;
    check_range("100000-byte read crosses the bounce buffer", &s, 0, 100000);
    CHECK_HEX("large read took multiple disk fills", 1, (g_reads > 1) ? 1 : 0);

    /* Bounds. */
    {
        static uint8_t buf[32];
        CHECK_HEX("off past end rejected", -1,
                  hype_iso_stream_read(&s, ISO_SIZE + 1u, buf, 1));
        CHECK_HEX("off+len past end rejected", -1,
                  hype_iso_stream_read(&s, ISO_SIZE - 10u, buf, 20));
        CHECK_HEX("exact-end read ok", 0,
                  hype_iso_stream_read(&s, ISO_SIZE - 16u, buf, 16));
    }

    test_seeking_reads_do_not_fetch_read_ahead();
    test_sequential_reads_still_get_full_read_ahead();
    test_read_ahead_recovers_after_a_seek();
    test_invalidate_clears_the_sequential_history();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
