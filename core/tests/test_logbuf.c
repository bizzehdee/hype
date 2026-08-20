#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../logbuf.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual)                                                    \
    do {                                                                                     \
        long long e_ = (long long)(expected), a_ = (long long)(actual);                      \
        if (e_ != a_) {                                                                      \
            printf("FAIL: %s (expected %lld, got %lld)\n", (desc), e_, a_);                  \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static void test_append_accumulates_in_order(void) {
    hype_logbuf_reset();
    hype_logbuf_append("hello ");
    hype_logbuf_append("world\n");
    CHECK_INT("len is sum of appends", 12, hype_logbuf_len());
    CHECK_INT("not truncated", 0, hype_logbuf_truncated());
    CHECK_INT("content matches", 0,
              memcmp(hype_logbuf_data(), "hello world\n", 12));
}

static void test_reset_clears(void) {
    hype_logbuf_append("stuff");
    hype_logbuf_reset();
    CHECK_INT("len 0 after reset", 0, hype_logbuf_len());
    CHECK_INT("not truncated after reset", 0, hype_logbuf_truncated());
}

static void test_null_append_is_noop(void) {
    hype_logbuf_reset();
    hype_logbuf_append(0);
    CHECK_INT("NULL append leaves len 0", 0, hype_logbuf_len());
}

static void test_truncates_at_capacity(void) {
    /* Fill exactly to capacity, then one more byte must be dropped and
     * latch the truncated flag; the retained content stays intact. */
    static char big[HYPE_LOGBUF_CAPACITY + 16];
    unsigned int i;
    hype_logbuf_reset();
    for (i = 0; i < sizeof(big) - 1; i++) {
        big[i] = 'A';
    }
    big[sizeof(big) - 1] = '\0';
    hype_logbuf_append(big);
    CHECK_INT("len capped at capacity", (long long)HYPE_LOGBUF_CAPACITY, hype_logbuf_len());
    CHECK_INT("truncated flag latched", 1, hype_logbuf_truncated());
    CHECK_INT("first retained byte intact", 'A', hype_logbuf_data()[0]);
    CHECK_INT("last retained byte intact", 'A', hype_logbuf_data()[HYPE_LOGBUF_CAPACITY - 1]);
    /* A further append stays dropped. */
    hype_logbuf_append("x");
    CHECK_INT("still capped after extra append", (long long)HYPE_LOGBUF_CAPACITY, hype_logbuf_len());
}

/* Lay down a header at the struct's field offsets (magic@0, version@8,
 * len@12, truncated@16, checksum@20, data@24) so find()/validate() can be
 * exercised against a synthetic region without a full 2MB struct. */
/*
 * Field offsets come from the STRUCT, not from arithmetic in this function.
 *
 * They used to be hand-written (`p + 24` for the data), and #585 adding a uint64_t to the header
 * moved data[] -- so six tests in this file started failing for a reason that had nothing to do
 * with what they were testing, and one of them (`bad checksum rejected`) failed by ACCEPTING a bad
 * checksum, because the bytes it meant to corrupt landed in padding. A test that encodes a layout
 * it does not own breaks every time that layout legitimately changes, and the failure points at the
 * wrong thing. The compiler knows where the fields are.
 */
static void put_header(unsigned char *p, uint64_t magic, uint32_t ver, uint32_t len,
                       uint32_t trunc, uint32_t cksum, const char *data) {
    memcpy(p + __builtin_offsetof(hype_logbuf_t, magic), &magic, sizeof(magic));
    memcpy(p + __builtin_offsetof(hype_logbuf_t, version), &ver, sizeof(ver));
    memcpy(p + __builtin_offsetof(hype_logbuf_t, len), &len, sizeof(len));
    memcpy(p + __builtin_offsetof(hype_logbuf_t, truncated), &trunc, sizeof(trunc));
    memcpy(p + __builtin_offsetof(hype_logbuf_t, checksum), &cksum, sizeof(cksum));
    if (data && len) {
        memcpy(p + __builtin_offsetof(hype_logbuf_t, data), data, len);
    }
}

static void test_reset_stamps_header(void) {
    hype_logbuf_reset();
    const hype_logbuf_t *h = hype_logbuf_get();
    CHECK_INT("reset stamps magic", (long long)HYPE_LOGBUF_MAGIC, (long long)h->magic);
    CHECK_INT("reset stamps version", (long long)HYPE_LOGBUF_VERSION, (long long)h->version);
    CHECK_INT("empty buffer validates", 1, hype_logbuf_validate(h));
}

static void test_live_buffer_validates_and_is_findable(void) {
    hype_logbuf_reset();
    hype_logbuf_append("abc");
    const hype_logbuf_t *h = hype_logbuf_get();
    CHECK_INT("checksum tracks appended bytes", (long long)('a' + 'b' + 'c'), (long long)h->checksum);
    CHECK_INT("populated buffer validates", 1, hype_logbuf_validate(h));
    /* The real RT-1b path: scan a region starting at the header, find it. */
    CHECK_INT("find locates the live buffer at offset 0", 1,
              hype_logbuf_find(h, sizeof(hype_logbuf_t), 8u) == h);
}

static void test_find_at_offset_and_rejections(void) {
    static unsigned char buf[512] __attribute__((aligned(8)));
    /* Valid header at a non-zero 8-aligned offset with 3 data bytes. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 3, 0, 'a' + 'b' + 'c', "abc");
    CHECK_INT("find locates a header at a nonzero offset", 1,
              hype_logbuf_find(buf, sizeof(buf), 8u) == (const hype_logbuf_t *)(buf + 64));

    /* Wrong magic -> not found. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, 0xDEADBEEFDEADBEEFULL, HYPE_LOGBUF_VERSION, 0, 0, 0, 0);
    CHECK_INT("wrong magic is not found", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    /* Right magic, wrong version -> validate fails, not found. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION + 1u, 0, 0, 0, 0);
    CHECK_INT("wrong version rejected", 0,
              hype_logbuf_validate((const hype_logbuf_t *)(buf + 64)));
    CHECK_INT("wrong version not found", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    /* Right magic/version, checksum doesn't match the data -> rejected. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 3, 0, 0 /*wrong*/, "abc");
    CHECK_INT("bad checksum rejected", 0,
              hype_logbuf_validate((const hype_logbuf_t *)(buf + 64)));
    CHECK_INT("bad checksum not found", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    /* A zeroed region has no magic anywhere. */
    memset(buf, 0, sizeof(buf));
    CHECK_INT("zeroed region yields nothing", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    /* Claimed len runs past the scanned region -> not found (no over-read). */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 100000u, 0, 0, 0);
    CHECK_INT("oversized len past region not found", 1, hype_logbuf_find(buf, sizeof(buf), 8u) == 0);

    CHECK_INT("NULL base not found", 1, hype_logbuf_find(0, sizeof(buf), 8u) == 0);
    CHECK_INT("NULL header does not validate", 0, hype_logbuf_validate(0));
}

/* RT-1d: the real RT-1b sweep steps by HYPE_LOGBUF_SCAN_ALIGN (4 KB) over
 * page-aligned RAM. Verify a header on a page boundary is found at that
 * stride, that a too-small stride is clamped (still finds an 8-aligned
 * header), and that the page stride correctly skips a header sitting off a
 * page boundary (the alignment contract the fast scan relies on). */
static void test_find_honours_stride(void) {
    static unsigned char buf[3 * 4096] __attribute__((aligned(4096)));

    /* Header exactly on the second page -> found by a 4 KB stride. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 4096, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 3, 0, 'a' + 'b' + 'c', "abc");
    CHECK_INT("page-strided find locates a page-aligned header", 1,
              hype_logbuf_find(buf, sizeof(buf), HYPE_LOGBUF_SCAN_ALIGN) ==
                  (const hype_logbuf_t *)(buf + 4096));

    /* Same header, but a stride below 8 is clamped up to 8 and still finds
     * it (4096 is a multiple of 8). */
    CHECK_INT("sub-8 stride is clamped and still finds it", 1,
              hype_logbuf_find(buf, sizeof(buf), 1u) == (const hype_logbuf_t *)(buf + 4096));

    /* Header off a page boundary -> a 4 KB stride steps past it (documents
     * the contract: the fast scan only works because the buffer is
     * page-aligned). An 8-byte stride still catches it. */
    memset(buf, 0, sizeof(buf));
    put_header(buf + 4096 + 64, HYPE_LOGBUF_MAGIC, HYPE_LOGBUF_VERSION, 3, 0, 'a' + 'b' + 'c', "abc");
    CHECK_INT("page stride skips an off-page header", 1,
              hype_logbuf_find(buf, sizeof(buf), HYPE_LOGBUF_SCAN_ALIGN) == 0);
    CHECK_INT("8-byte stride still finds the off-page header", 1,
              hype_logbuf_find(buf, sizeof(buf), 8u) == (const hype_logbuf_t *)(buf + 4096 + 64));
}

/* ---- #338: concurrent appends must not lose or tear records ---- */

#include <pthread.h>
#include <string.h>

#define CONC_THREADS 4
#define CONC_RECORDS 25000

/* Each thread appends its own fixed-length record repeatedly. Fixed length matters: it makes a torn
 * or lost record detectable by simple counting, with no parsing. */
static const char *const g_conc_rec[CONC_THREADS] = {
    "AAAAAAAAAAAAAAAA\n", "BBBBBBBBBBBBBBBB\n", "CCCCCCCCCCCCCCCC\n", "DDDDDDDDDDDDDDDD\n"
};

/* Start barrier: without it a fast thread can finish before the next is scheduled, and the appends
 * never actually overlap -- which is how the first version of this test passed with the lock REMOVED
 * and therefore proved nothing. */
static volatile int g_conc_go;

static void *conc_worker(void *arg) {
    unsigned t = (unsigned)(unsigned long)arg;
    int i;
    while (__atomic_load_n(&g_conc_go, __ATOMIC_ACQUIRE) == 0) {
    }
    for (i = 0; i < CONC_RECORDS; i++) {
        hype_logbuf_append(g_conc_rec[t]);
    }
    return 0;
}

static void test_concurrent_appends_lose_nothing(void) {
    pthread_t th[CONC_THREADS];
    unsigned long t;
    const char *data;
    unsigned long len, i;
    int counts[CONC_THREADS];
    unsigned long expect_bytes = (unsigned long)CONC_THREADS * CONC_RECORDS * 17ul;

    hype_logbuf_reset();
    g_conc_go = 0;
    for (t = 0; t < CONC_THREADS; t++) {
        counts[t] = 0;
        pthread_create(&th[t], 0, conc_worker, (void *)t);
    }
    __atomic_store_n(&g_conc_go, 1, __ATOMIC_RELEASE);
    for (t = 0; t < CONC_THREADS; t++) {
        pthread_join(th[t], 0);
    }

    data = hype_logbuf_data();
    len = (unsigned long)hype_logbuf_len();

    /*
     * The assertion that catches the shared-index bug: with an unsynchronised len, two threads write
     * to the same offset and the total comes out SHORT. Byte count alone proves nothing was lost.
     */
    CHECK_INT("no bytes lost to a racing append", (int)expect_bytes, (int)len);

    /* And nothing torn: every 17-byte slot must be one whole record, not a mix. */
    for (i = 0; i + 17ul <= len; i += 17ul) {
        unsigned t2;
        int matched = 0;
        for (t2 = 0; t2 < CONC_THREADS; t2++) {
            if (memcmp(data + i, g_conc_rec[t2], 17) == 0) {
                counts[t2]++;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            CHECK_INT("a record was TORN by a concurrent append", 1, 0);
            break;
        }
    }
    for (t = 0; t < CONC_THREADS; t++) {
        CHECK_INT("every record from this writer survived exactly once", CONC_RECORDS, counts[t]);
    }
}


static void test_panic_path_never_blocks(void) {
    /* The deadlock this guards: a core faults INSIDE the critical section, so the lock is still held
     * when hype_fatal() runs. The panic path must write anyway rather than spin forever -- a silent
     * hang is indistinguishable from the fault itself. */
    hype_logbuf_reset();
    CHECK_INT("lock is free to start", 1, hype_logbuf_try_lock());
    /* Now held, exactly as a faulted core would leave it. */
    CHECK_INT("a second try_lock reports FAILURE rather than blocking", 0, hype_logbuf_try_lock());
    hype_logbuf_append_unlocked("PANIC: still written\n");
    CHECK_INT("the panic text landed despite the lock being held", 21, hype_logbuf_len());
    hype_logbuf_unlock();
    CHECK_INT("and the lock is releasable afterwards", 1, hype_logbuf_try_lock());
    hype_logbuf_unlock();
}


static void test_capacity_truncation_is_flagged(void) {
    /* The buffer is finite and a hypervisor that logs enough WILL reach the end. Overflow must set
     * the flag rather than wrap or write past it -- a silently short log is how a debug run looks
     * complete while missing the part that mattered. */
    unsigned long i;
    char chunk[1025];

    hype_logbuf_reset();
    for (i = 0; i < 1024ul; i++) {
        chunk[i] = 'x';
    }
    chunk[1024] = '\0';
    CHECK_INT("not truncated to start", 0, hype_logbuf_truncated());
    for (i = 0; i < (HYPE_LOGBUF_CAPACITY / 1024u) + 2ul; i++) {
        hype_logbuf_append(chunk);
    }
    CHECK_INT("capacity is respected", (int)HYPE_LOGBUF_CAPACITY, (int)hype_logbuf_len());
    CHECK_INT("and overflow is FLAGGED, not silent", 1, hype_logbuf_truncated());
    hype_logbuf_reset();
    CHECK_INT("reset clears the flag", 0, hype_logbuf_truncated());
}


static void test_null_and_empty_appends_are_safe(void) {
    hype_logbuf_reset();
    hype_logbuf_append(0);  /* callers do pass NULL: a format that produced nothing */
    CHECK_INT("a NULL append is a no-op, not a crash", 0, hype_logbuf_len());
    hype_logbuf_append("");
    CHECK_INT("an empty append is a no-op", 0, hype_logbuf_len());
    hype_logbuf_append_unlocked(0);
    CHECK_INT("...and on the panic path too", 0, hype_logbuf_len());
}


/* ---- #585: reclaim ---- */

/*
 * The whole point: drop the already-written prefix, keep the rest, and leave the buffer VALID -- a
 * next-boot scanner has to be able to trust what is left, and the checksum is maintained by
 * subtraction rather than recomputed, so an error there is silent until a scan rejects the region.
 */
static void test_reclaim_drops_prefix_and_slides(void) {
    hype_logbuf_reset();
    hype_logbuf_append("AAAABBBBCCCC");
    CHECK_INT("dropped what was asked", 4, hype_logbuf_reclaim_unlocked(4));
    CHECK_INT("len shrank", 8, hype_logbuf_len());
    CHECK_INT("the residue slid to the front", 0, memcmp(hype_logbuf_data(), "BBBBCCCC", 8));
    CHECK_INT("reclaimed total", 4, (int)hype_logbuf_reclaimed());
    CHECK_INT("still a valid, self-describing region", 1,
              hype_logbuf_validate(hype_logbuf_get()));

    /* Again, so the total accumulates rather than being overwritten. */
    CHECK_INT("second reclaim", 4, hype_logbuf_reclaim_unlocked(4));
    CHECK_INT("len shrank again", 4, hype_logbuf_len());
    CHECK_INT("residue is the tail", 0, memcmp(hype_logbuf_data(), "CCCC", 4));
    CHECK_INT("reclaimed accumulates", 8, (int)hype_logbuf_reclaimed());
    CHECK_INT("still valid", 1, hype_logbuf_validate(hype_logbuf_get()));
}

/* Appending after a reclaim must land after the residue, not overwrite it -- the buffer is still a
 * linear append region, just with a shorter history. */
static void test_append_after_reclaim_continues(void) {
    hype_logbuf_reset();
    hype_logbuf_append("0123456789");
    (void)hype_logbuf_reclaim_unlocked(6);
    hype_logbuf_append("abc");
    CHECK_INT("len is residue + new", 7, hype_logbuf_len());
    CHECK_INT("content is residue then new", 0, memcmp(hype_logbuf_data(), "6789abc", 7));
    CHECK_INT("valid", 1, hype_logbuf_validate(hype_logbuf_get()));
}

/*
 * A reclaim of everything, and of MORE than everything. An over-large `upto` is clamped rather than
 * sliding bytes in from beyond the buffer -- the caller derives it from sink cursors, and a cursor
 * that has run ahead of the buffer is a bug elsewhere that must not become a memory error here.
 */
static void test_reclaim_clamps_and_empties(void) {
    hype_logbuf_reset();
    hype_logbuf_append("12345");
    CHECK_INT("clamped to len", 5, hype_logbuf_reclaim_unlocked(9999));
    CHECK_INT("empty", 0, hype_logbuf_len());
    CHECK_INT("valid when empty", 1, hype_logbuf_validate(hype_logbuf_get()));
    CHECK_INT("reclaimed counted the clamped amount", 5, (int)hype_logbuf_reclaimed());
    CHECK_INT("reclaiming an empty buffer is a no-op", 0, hype_logbuf_reclaim_unlocked(1));
}

/* Zero means zero. The driver calls this whenever the buffer is under pressure, and with no live
 * sink the watermark is 0 -- that path must not move a byte. */
static void test_reclaim_zero_is_a_noop(void) {
    hype_logbuf_reset();
    hype_logbuf_append("keep me");
    CHECK_INT("no bytes dropped", 0, hype_logbuf_reclaim_unlocked(0));
    CHECK_INT("len unchanged", 7, hype_logbuf_len());
    CHECK_INT("content unchanged", 0, memcmp(hype_logbuf_data(), "keep me", 7));
    CHECK_INT("reclaimed still zero", 0, (int)hype_logbuf_reclaimed());
}

/* reset() clears the total, or a second boot would report the first boot's reclaim and every record
 * offset would start at a lie. */
static void test_reset_clears_reclaimed(void) {
    hype_logbuf_reset();
    hype_logbuf_append("xyz");
    (void)hype_logbuf_reclaim_unlocked(2);
    CHECK_INT("reclaimed before reset", 2, (int)hype_logbuf_reclaimed());
    hype_logbuf_reset();
    CHECK_INT("reclaimed cleared", 0, (int)hype_logbuf_reclaimed());
    CHECK_INT("len cleared", 0, hype_logbuf_len());
}

/*
 * TRUNCATED IS NOT CLEARED BY RECLAIM, and this is the test that says why. `truncated` means output
 * was LOST; giving the buffer room afterwards does not un-lose it. A run that filled up before any
 * sink existed produced an incomplete log, and a later reclaim must not make it look fine.
 */
static void test_reclaim_does_not_clear_truncated(void) {
    unsigned int i;
    hype_logbuf_reset();
    for (i = 0; i < (HYPE_LOGBUF_CAPACITY / 8u) + 2u; i++) {
        hype_logbuf_append("XXXXXXXX");
    }
    CHECK_INT("filled and latched", 1, hype_logbuf_truncated());
    (void)hype_logbuf_reclaim_unlocked(1024);
    CHECK_INT("room made", 1, hype_logbuf_len() < HYPE_LOGBUF_CAPACITY);
    CHECK_INT("but the loss is still reported", 1, hype_logbuf_truncated());
}

/*
 * A reclaimed buffer must survive the next-boot SCAN, not merely validate() -- hype_logbuf_find()
 * checks that the claimed data fits inside the region it was given, and #585 added a field ahead of
 * data[], so a hand-counted header size would have let it validate a candidate whose bytes ran past
 * what was checked. Scanning a real region containing a reclaimed buffer is what catches that.
 */
static void test_reclaimed_buffer_is_still_findable(void) {
    const hype_logbuf_t *found;
    hype_logbuf_reset();
    hype_logbuf_append("find me after a reclaim\n");
    (void)hype_logbuf_reclaim_unlocked(5);
    found = hype_logbuf_find(hype_logbuf_get(), sizeof(hype_logbuf_t), 8u);
    CHECK_INT("found", 1, found == hype_logbuf_get());
    CHECK_INT("and it reports what went", 5, (int)(found ? found->reclaimed : 0));
    CHECK_INT("residue is the tail", 0,
              memcmp(hype_logbuf_data(), "me after a reclaim\n", 19));
}

int main(void) {
    test_append_accumulates_in_order();
    test_reset_clears();
    test_null_append_is_noop();
    test_truncates_at_capacity();
    test_reset_stamps_header();
    test_live_buffer_validates_and_is_findable();
    test_find_at_offset_and_rejections();
    test_find_honours_stride();
    test_concurrent_appends_lose_nothing();
    test_panic_path_never_blocks();
    test_capacity_truncation_is_flagged();
    test_null_and_empty_appends_are_safe();
    test_reclaim_drops_prefix_and_slides();
    test_append_after_reclaim_continues();
    test_reclaim_clamps_and_empties();
    test_reclaim_zero_is_a_noop();
    test_reset_clears_reclaimed();
    test_reclaim_does_not_clear_truncated();
    test_reclaimed_buffer_is_still_findable();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
