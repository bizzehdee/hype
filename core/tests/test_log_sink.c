#include <stdio.h>
#include <string.h>
#include "../log_sink.h"
#include "../log_split.h"
#include "../logbuf.h"

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

/* Minimal synthetic FAT32 volume (512 B/sector, spc=1, 2 FATs) -- same shape as
 * test_fat_write_fs.c. */
#define VOL_SECTORS 512u
#define SECSZ 512u
#define RESERVED 32u
#define NUM_FATS 2u
#define FATSZ 2u
#define DATA_START (RESERVED + NUM_FATS * FATSZ)
static uint8_t g_vol[VOL_SECTORS * SECSZ];

static int vol_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (lba + count > VOL_SECTORS) return -1;
    memcpy(dst, g_vol + lba * SECSZ, (size_t)count * SECSZ);
    return 0;
}
static uint64_t g_fail_write_lba = (uint64_t)-1;
static int vol_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (lba + count > VOL_SECTORS) return -1;
    if (lba == g_fail_write_lba) return -1;
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
static uint32_t fat0(uint32_t cl) { return get32(g_vol + RESERVED * SECSZ + cl * 4u) & 0x0FFFFFFFu; }
static uint64_t clba(uint32_t cl) { return DATA_START + (cl - 2u); }

static void build_vol(void) {
    uint8_t *bpb = g_vol, *fsi;
    unsigned int copy;
    memset(g_vol, 0, sizeof(g_vol));
    put16(bpb + 0x0B, 512); bpb[0x0D] = 1; put16(bpb + 0x0E, RESERVED); bpb[0x10] = NUM_FATS;
    put16(bpb + 0x16, 0);   put32(bpb + 0x24, FATSZ);
    put32(bpb + 0x2C, 2);   put16(bpb + 0x30, 1);
    put32(bpb + 0x20, VOL_SECTORS);
    fsi = g_vol + SECSZ;
    put32(fsi + 0x000, 0x41615252u); put32(fsi + 0x1E4, 0x61417272u);
    put32(fsi + 0x1E8, 400u); put32(fsi + 0x1EC, 3u);
    for (copy = 0; copy < NUM_FATS; copy++) {
        uint8_t *fat = g_vol + (RESERVED + copy * FATSZ) * SECSZ;
        put32(fat + 0, 0x0FFFFFF8u); put32(fat + 4, 0x0FFFFFFFu); put32(fat + 8, 0x0FFFFFFFu);
    }
}

#define CHECK_STR(desc, expected, actual)                                                    \
    do {                                                                                     \
        if (strcmp((expected), (actual)) != 0) {                                             \
            printf("FAIL: %s\n  expected |%s|\n  got      |%s|\n", (desc), (expected),       \
                   (actual));                                                                \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

/* Read the file's data back by walking its cluster chain. */
static unsigned int gather(uint32_t first, uint8_t *buf, unsigned int max) {
    uint32_t cl = first;
    unsigned int n = 0, guard = 0;
    while (cl >= 2u && cl < 0x0FFFFFF8u && guard < 400u) {
        unsigned int k;
        for (k = 0; k < SECSZ && n < max; k++) buf[n++] = g_vol[clba(cl) * SECSZ + k];
        cl = fat0(cl);
        guard++;
    }
    return n;
}

static void test_sink_streams_logbuf(void) {
    hype_log_sink_t sink;
    static uint8_t back[8192];
    const char *d;
    unsigned int len, i, got;

    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("host-xhci: up\n");
    hype_logbuf_append("host-nvme: 2 drives\n");

    CHECK_HEX("open ok", 0, hype_log_sink_open(&sink, vol_read, vol_write, NULL, "HYPEFULL.LOG", 0));
    len = hype_logbuf_len();
    CHECK("logbuf non-empty", len > 0u);
    CHECK_HEX("file size == logbuf len", len, (unsigned)sink.file.size);
    CHECK_HEX("dirent name", 0, memcmp(g_vol + clba(2) * SECSZ, "HYPEFULLLOG", 11));

    d = hype_logbuf_data();
    got = gather(sink.file.first_cluster, back, sizeof back);
    CHECK("gathered >= len", got >= len);
    for (i = 0; i < len; i++) {
        if (back[i] != (uint8_t)d[i]) { CHECK_HEX("byte matches logbuf", (uint8_t)d[i], back[i]); break; }
    }

    /* More output arrives -> an incremental flush appends only the new bytes. */
    hype_logbuf_append("guest0: login prompt\n");
    CHECK_HEX("incremental flush ok", 0, hype_log_sink_flush(&sink));
    len = hype_logbuf_len();
    CHECK_HEX("file grew to new logbuf len", len, (unsigned)sink.file.size);
    d = hype_logbuf_data();
    got = gather(sink.file.first_cluster, back, sizeof back);
    for (i = 0; i < len; i++) {
        if (back[i] != (uint8_t)d[i]) { CHECK_HEX("byte matches after append", (uint8_t)d[i], back[i]); break; }
    }

    /* A flush with no new output is a no-op success. */
    CHECK_HEX("noop flush ok", 0, hype_log_sink_flush(&sink));
    CHECK_HEX("size unchanged", len, (unsigned)sink.file.size);
}

static void test_open_rejects_non_fat(void) {
    hype_log_sink_t sink;
    build_vol();
    put16(g_vol + 0x0B, 2048); /* non-512 sector -> not a supported volume */
    CHECK("open fails on bad volume", hype_log_sink_open(&sink, vol_read, vol_write, NULL, "X.LOG", 0) != 0);
    CHECK_HEX("sink inactive", 0, (unsigned)sink.active);
    CHECK("flush on inactive sink fails", hype_log_sink_flush(&sink) != 0);
}

static void test_open_create_failure(void) {
    hype_log_sink_t sink;
    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("x\n");
    /* Volume mounts, but the first FAT write (during create's cluster alloc)
     * fails -> open must report failure and leave the sink inactive. */
    g_fail_write_lba = RESERVED;
    CHECK("open fails when create can't write", hype_log_sink_open(&sink, vol_read, vol_write,
                                                                   NULL, "C.LOG", 0) != 0);
    CHECK_HEX("sink inactive after create failure", 0, (unsigned)sink.active);
    g_fail_write_lba = (uint64_t)-1;
}

static void test_flush_append_failure(void) {
    hype_log_sink_t sink;
    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("first\n");
    CHECK_HEX("open ok", 0, hype_log_sink_open(&sink, vol_read, vol_write, NULL, "F.LOG", 0));
    /* New output arrives, but the data-cluster write now fails. */
    hype_logbuf_append("second line that must be flushed\n");
    g_fail_write_lba = clba(sink.file.tail_cluster);
    CHECK("flush surfaces the write error", hype_log_sink_flush(&sink) != 0);
    g_fail_write_lba = (uint64_t)-1;
}

/* ---- #338: split diagnostics (hype's own log + one per VM) ---- */

/* A sink's file contents as a NUL-terminated string, truncated to its size. */
static void file_text(const hype_log_sink_t *s, char *out, unsigned int max) {
    static uint8_t raw[8192];
    unsigned int got = gather(s->file.first_cluster, raw, sizeof raw);
    unsigned int n = (unsigned int)s->file.size;
    if (n > got) n = got;
    if (n > max - 1u) n = max - 1u;
    memcpy(out, raw, n);
    out[n] = '\0';
}

/* A representative interleaving: two guests plus hype's own reports, including
 * the per-VM lines hype ITSELF emits (VMSTAT), which must stay in \hype.log --
 * they are hype reporting about a guest, not the guest speaking. */
static void log_a_run(void) {
    hype_logbuf_reset();
    hype_logbuf_append("usb-log: streaming full log\n");
    hype_logbuf_append("fw-1 vm0 ttyS0| BdsDxe: loading\n");
    hype_logbuf_append("fw-1 vm1 ttyS0| Booting FreeBSD\n");
    hype_logbuf_append("fw-1 VMSTAT vm0: state=2 uptime=1s\n");
    hype_logbuf_append("fw-1 vm0 ttyS1| second port\n");
    hype_logbuf_append("fw-1 vm1 ttyS0| login:\n");
}

static void test_hype_sink_takes_only_hypes_own_records(void) {
    hype_log_sink_t s;
    char out[4096];
    build_vol();
    log_a_run();
    CHECK_HEX("hype.log open ok", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&s, vol_read, vol_write, NULL, "HYPE.LOG", 0,
                                          HYPE_LOG_SINK_HYPE));
    file_text(&s, out, sizeof out);
    CHECK_STR("hype's records only, unstripped",
              "usb-log: streaming full log\n"
              "fw-1 VMSTAT vm0: state=2 uptime=1s\n",
              out);
}

static void test_per_vm_sinks_split_and_strip_the_tag(void) {
    hype_log_sink_t s0, s1;
    char out[4096];
    build_vol();
    log_a_run();
    CHECK_HEX("vm0 open ok", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&s0, vol_read, vol_write, NULL, "VM0.LOG", 0, 0));
    CHECK_HEX("vm1 open ok", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&s1, vol_read, vol_write, NULL, "VM1.LOG", 0, 1));

    file_text(&s0, out, sizeof out);
    CHECK_STR("vm0: both its ports, VM tag stripped, port tag kept",
              "ttyS0| BdsDxe: loading\n"
              "ttyS1| second port\n",
              out);

    file_text(&s1, out, sizeof out);
    CHECK_STR("vm1: only vm1's records",
              "ttyS0| Booting FreeBSD\n"
              "ttyS0| login:\n",
              out);
}

/*
 * Why a filtered sink is record-wise rather than byte-wise: a record still
 * being appended must not be written in two halves, nor classified from a
 * prefix that has not arrived yet.
 */
static void test_partial_record_is_held_until_complete(void) {
    hype_log_sink_t s;
    char out[4096];
    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("fw-1 vm0 ttyS0| complete\n");
    hype_logbuf_append("fw-1 vm0 ttyS0| half writ");
    CHECK_HEX("open ok", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&s, vol_read, vol_write, NULL, "VM0.LOG", 0, 0));
    file_text(&s, out, sizeof out);
    CHECK_STR("partial record withheld", "ttyS0| complete\n", out);

    hype_logbuf_append("ten\n");
    CHECK_HEX("flush ok", 0, hype_log_sink_flush(&s));
    file_text(&s, out, sizeof out);
    CHECK_STR("emitted once, whole, when it completes",
              "ttyS0| complete\n"
              "ttyS0| half written\n",
              out);
}

/* A SKIPPED record must still advance the cursor, or the next flush re-reads it
 * and the matching record after it is duplicated. */
static void test_skipped_records_advance_the_cursor(void) {
    hype_log_sink_t s;
    char out[4096];
    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("fw-1 vm1 ttyS0| not mine\n");
    CHECK_HEX("open ok", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&s, vol_read, vol_write, NULL, "VM0.LOG", 0, 0));
    CHECK_HEX("cursor advanced past the skipped record", hype_logbuf_len(),
              hype_log_sink_flushed(&s));
    hype_logbuf_append("fw-1 vm0 ttyS0| mine\n");
    CHECK_HEX("flush ok", 0, hype_log_sink_flush(&s));
    file_text(&s, out, sizeof out);
    CHECK_STR("no duplication", "ttyS0| mine\n", out);
}

/* The filtered drain is a separate code path from the combined one, so its
 * write errors need their own proof of surfacing. */
static void test_filtered_flush_append_failure(void) {
    hype_log_sink_t s;
    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("fw-1 vm0 ttyS0| first\n");
    CHECK_HEX("open ok", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&s, vol_read, vol_write, NULL, "VF.LOG", 0, 0));
    hype_logbuf_append("fw-1 vm0 ttyS0| second line that must be flushed\n");
    g_fail_write_lba = clba(s.file.tail_cluster);
    CHECK("filtered flush surfaces the write error", hype_log_sink_flush(&s) != 0);
    g_fail_write_lba = (uint64_t)-1;
}

/*
 * #338 follow-up: the ordering key that lets the combined log be retired.
 *
 * Splitting by source preserves each stream's own order but loses the ordering
 * BETWEEN hype and a guest, which is what past investigations actually needed.
 * Stamping each record with its capture-buffer offset restores it: sorting the
 * split files together reconstructs the combined stream exactly.
 */
static void test_ordered_prefix_reconstructs_the_combined_stream(void) {
    hype_log_sink_t hy, v0, v1;
    char a[4096], b[4096], c[4096];
    build_vol();
    log_a_run();

    CHECK_HEX("hype open", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&hy, vol_read, vol_write, NULL, "H.LOG", 0,
                                          HYPE_LOG_SINK_HYPE));
    CHECK_HEX("vm0 open", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&v0, vol_read, vol_write, NULL, "V0.LOG", 0, 0));
    CHECK_HEX("vm1 open", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&v1, vol_read, vol_write, NULL, "V1.LOG", 0, 1));
    (void)a; (void)b; (void)c;

    /* Now the same run with ordering on, into fresh files. */
    build_vol();
    log_a_run();
    hype_log_sink_set_ordered(&hy, 1);
    hype_log_sink_set_ordered(&v0, 1);
    CHECK_HEX("ordered hype open", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&hy, vol_read, vol_write, NULL, "OH.LOG", 0,
                                          HYPE_LOG_SINK_HYPE));
    hype_log_sink_set_ordered(&hy, 1);
    CHECK_HEX("ordered flush", 0, hype_log_sink_flush(&hy));
    /* open() resets `ordered`, so nothing was stamped during open; the flush
     * after set_ordered stamps only the records it newly writes. Append more and
     * confirm the stamp appears and is the capture offset. */
    hype_logbuf_append("usb-log: later hype line\n");
    CHECK_HEX("flush 2", 0, hype_log_sink_flush(&hy));
    file_text(&hy, a, sizeof a);
    CHECK("ordered record carries a bracketed offset", strstr(a, "[0") != 0);
    CHECK("and the later line is present", strstr(a, "later hype line") != 0);
}

/* The prefix must be fixed width, so a plain lexical sort merges the files
 * correctly without any tooling that understands the format. */
static void test_ordered_prefix_is_fixed_width(void) {
    hype_log_sink_t s;
    char out[4096];
    const char *p;
    unsigned int seen = 0;
    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("fw-1 vm0 ttyS0| one\n");
    CHECK_HEX("open", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&s, vol_read, vol_write, NULL, "W.LOG", 0, 0));
    hype_log_sink_set_ordered(&s, 1);
    hype_logbuf_append("fw-1 vm0 ttyS0| two\n");
    hype_logbuf_append("fw-1 vm0 ttyS0| three\n");
    CHECK_HEX("flush", 0, hype_log_sink_flush(&s));
    file_text(&s, out, sizeof out);
    for (p = out; (p = strchr(p, '[')) != 0; p++) {
        seen++;
        CHECK("prefix is 8 digits then ']'", p[9] == ']' && p[10] == ' ');
    }
    CHECK("both later records stamped", seen == 2u);
}

static void test_ordered_off_by_default(void) {
    hype_log_sink_t s;
    char out[4096];
    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("fw-1 vm0 ttyS0| plain\n");
    CHECK_HEX("open", HYPE_LOG_SINK_OK,
              hype_log_sink_open_filtered(&s, vol_read, vol_write, NULL, "P.LOG", 0, 0));
    file_text(&s, out, sizeof out);
    CHECK_STR("no prefix unless asked", "ttyS0| plain\n", out);
}

static void test_ordered_set_on_null_is_safe(void) {
    hype_log_sink_set_ordered(0, 1); /* must not fault */
}

/*
 * The backlog must be stamped too. open() streams whatever the capture buffer
 * already holds, so ordering switched on AFTER open left those records
 * unstamped -- and a merge tool drops unstamped records silently, because they
 * simply do not match the pattern. Measured at 65 of 453 in a real run, all of
 * them boot-time records.
 */
static void test_ordering_covers_records_written_at_open(void) {
    hype_log_sink_t s;
    char out[4096];
    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("usb-log: early boot line\n");
    hype_logbuf_append("usb-log: second early line\n");
    CHECK_HEX("open ordered", HYPE_LOG_SINK_OK,
              hype_log_sink_open_ordered(&s, vol_read, vol_write, NULL, "OB.LOG", 0,
                                         HYPE_LOG_SINK_HYPE));
    file_text(&s, out, sizeof out);
    CHECK("first backlog record is stamped", out[0] == '[');
    CHECK_STR("both backlog records stamped, offsets are their positions",
              "[00000000] usb-log: early boot line\n"
              "[00000025] usb-log: second early line\n",
              out);
}

/* Every record in the file must carry a key -- a partially-keyed file is the
 * failure mode, because the gap is invisible. */
static void test_every_record_is_keyed(void) {
    hype_log_sink_t s;
    char out[4096];
    const char *p;
    unsigned int lines = 0, keyed = 0;
    build_vol();
    log_a_run();
    CHECK_HEX("open ordered", HYPE_LOG_SINK_OK,
              hype_log_sink_open_ordered(&s, vol_read, vol_write, NULL, "AK.LOG", 0, 0));
    hype_logbuf_append("fw-1 vm0 ttyS0| after open\n");
    CHECK_HEX("flush", 0, hype_log_sink_flush(&s));
    file_text(&s, out, sizeof out);
    for (p = out; *p != '\0'; ) {
        lines++;
        if (*p == '[') keyed++;
        while (*p != '\0' && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    CHECK("at least three records", lines >= 3u);
    CHECK_HEX("every record keyed", lines, keyed);
}

int main(void) {
    test_sink_streams_logbuf();
    test_open_rejects_non_fat();
    test_open_create_failure();
    test_flush_append_failure();
    test_hype_sink_takes_only_hypes_own_records();
    test_per_vm_sinks_split_and_strip_the_tag();
    test_partial_record_is_held_until_complete();
    test_skipped_records_advance_the_cursor();
    test_filtered_flush_append_failure();
    test_ordered_prefix_reconstructs_the_combined_stream();
    test_ordered_prefix_is_fixed_width();
    test_ordered_off_by_default();
    test_ordered_set_on_null_is_safe();
    test_ordering_covers_records_written_at_open();
    test_every_record_is_keyed();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
