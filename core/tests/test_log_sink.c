#include <stdio.h>
#include <string.h>
#include "../log_sink.h"
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

    CHECK_HEX("open ok", 0, hype_log_sink_open(&sink, vol_read, vol_write, NULL, "HYPEFULL.LOG"));
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
    CHECK("open fails on bad volume", hype_log_sink_open(&sink, vol_read, vol_write, NULL, "X.LOG") != 0);
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
                                                                   NULL, "C.LOG") != 0);
    CHECK_HEX("sink inactive after create failure", 0, (unsigned)sink.active);
    g_fail_write_lba = (uint64_t)-1;
}

static void test_flush_append_failure(void) {
    hype_log_sink_t sink;
    build_vol();
    hype_logbuf_reset();
    hype_logbuf_append("first\n");
    CHECK_HEX("open ok", 0, hype_log_sink_open(&sink, vol_read, vol_write, NULL, "F.LOG"));
    /* New output arrives, but the data-cluster write now fails. */
    hype_logbuf_append("second line that must be flushed\n");
    g_fail_write_lba = clba(sink.file.tail_cluster);
    CHECK("flush surfaces the write error", hype_log_sink_flush(&sink) != 0);
    g_fail_write_lba = (uint64_t)-1;
}

int main(void) {
    test_sink_streams_logbuf();
    test_open_rejects_non_fat();
    test_open_create_failure();
    test_flush_append_failure();
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
