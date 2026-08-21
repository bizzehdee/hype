/*
 * #597/#596: reproduce hype's ACTUAL log-writing path against a real mkfs.vfat volume.
 *
 * The FAT32 batteries (tools/fat32-e2e) drive the writer directly and came back clean on real
 * hardware, so #596 is not in the raw writer. This harness drives the layer the real logs go
 * through and the batteries skip: core/log_sink.c on top of the shared capture buffer
 * (core/logbuf.c) and the per-record classifier (core/log_split.c). It reproduces, exactly:
 *
 *   - records fed into ONE shared logbuf, classified per-VM, each sink draining only its own;
 *   - \HYPE.LOG + several per-VM logs sharing ONE hype_fs_t, so their chains grow INTERLEAVED
 *     through one allocator / FAT cache (split_log_setup);
 *   - the 4 KiB batched, budgeted flush cadence with the "[0000000000] " ordered prefix;
 *   - the durability barrier -- and the option to FAIL it the way the stick does (#516/#464);
 *   - the #585 reclaim step (watermark = min flush cursor; slide the residue; fix each cursor).
 *
 * Then an INDEPENDENT judge (fsck.vfat + mtools, in run-log-fsck.sh) decides whether the volume is
 * clean and every log file is readable at the length hype believes it wrote -- the #596 signal.
 *
 * usage: log_fsck <image> <combined|split|longrun> <ok|dead> <manifest>
 */
#include <stdio.h>
#include <string.h>

#include "../../core/blk_io.h"
#include "../../core/fs_ops.h"
#include "../../core/log_sink.h"
#include "../../core/logbuf.h"

#define SECSZ 512u
#define BUDGET 4096u
#define NVM 3u

static FILE *g_img;
static long g_sync_calls;
static int g_sync_flaky; /* 1 => fail every Nth barrier once armed (#516 mostly-works, occasionally rejects) */
static int g_sync_armed;
#define FLAKY_EVERY 7L

static int img_read(void *ctx, uint64_t lba, uint32_t count, void *dst) {
    (void)ctx;
    if (fseek(g_img, (long)(lba * SECSZ), SEEK_SET) != 0) return -1;
    return fread(dst, SECSZ, count, g_img) == count ? 0 : -1;
}
static int img_write(void *ctx, uint64_t lba, uint32_t count, const void *src) {
    (void)ctx;
    if (fseek(g_img, (long)(lba * SECSZ), SEEK_SET) != 0) return -1;
    return fwrite(src, SECSZ, count, g_img) == count ? 0 : -1;
}
static int img_sync(void *ctx) {
    (void)ctx;
    g_sync_calls++;
    /* Intermittent rejection: the write itself succeeds on the device, but the cache-flush barrier
     * is periodically refused (#516). hype must roll back the in-flight growth and stay consistent
     * -- the interesting case is that rollback happening WHILE other sinks are growing on the same
     * fs. Not persistent: a persistently-dead barrier just rolls back every append (empty files,
     * already covered by #464) and does not match #596, where the logs did grow. */
    if (g_sync_armed && g_sync_flaky && (g_sync_calls % FLAKY_EVERY) == 0) return -1;
    fflush(g_img);
    return 0;
}

/* Records, in the exact format the classifier keys on (core/log_split.c). A hype record has no
 * "fw-1 vm<N> " prefix; a guest record does. Deterministic and varied in length. */
static void feed_hype(unsigned int i) {
    char b[192];
    snprintf(b, sizeof b, "hype: diag record %u -- some load-bearing detail %u/%u and padding %u\n",
             i, i * 7u, i % 13u, i * 3u + 1u);
    hype_logbuf_append(b);
}
static void feed_vm(unsigned int vm, unsigned int i) {
    char b[192];
    snprintf(b, sizeof b, "fw-1 vm%u ttyS0| guest %u console line %u payload %u trailing bytes %u\n",
             vm, i, i * 5u, i % 7u, i * 2u);
    hype_logbuf_append(b);
}

/* #585 reclaim step, replicating fw_1_logbuf_reclaim(): drop the prefix every live sink has already
 * written, then fix each sink's cursor by the amount dropped. */
static void reclaim(hype_log_sink_t **sinks, unsigned int n) {
    unsigned int watermark = 0u, dropped, i;
    int live = 0;
    for (i = 0; i < n; i++) {
        unsigned int f;
        if (!sinks[i]->active) continue;
        f = hype_log_sink_flushed(sinks[i]);
        if (!live || f < watermark) watermark = f;
        live = 1;
    }
    if (!live || watermark == 0u) return;
    hype_logbuf_lock();
    dropped = hype_logbuf_reclaim_unlocked(watermark);
    if (dropped != 0u) {
        for (i = 0; i < n; i++) {
            if (sinks[i]->active) sinks[i]->flushed -= dropped;
        }
    }
    hype_logbuf_unlock();
}

static void drain(hype_log_sink_t **sinks, unsigned int n) {
    unsigned int i, guard;
    for (i = 0; i < n; i++) {
        if (!sinks[i]->active) continue;
        /* budgeted, like the real drain loop -- several passes per tick */
        for (guard = 0; guard < 64u; guard++) {
            unsigned int before = hype_log_sink_flushed(sinks[i]);
            hype_log_sink_flush_budget(sinks[i], BUDGET);
            if (hype_log_sink_flushed(sinks[i]) == before) break;
        }
    }
}

/* Record each log file's path and the size hype's own reader reports (or -1 if hype can no longer
 * open it), for the independent mtools/fsck check to compare against. */
static void manifest_file(FILE *mf, hype_fs_t *fs, const char *name) {
    hype_fs_file_t f;
    if (hype_fs_lookup(fs, name, &f) == 0) {
        fprintf(mf, "%s %llu\n", name, (unsigned long long)f.size);
    } else {
        fprintf(mf, "%s -1\n", name);
    }
}

int main(int argc, char **argv) {
    const char *scenario, *policy;
    FILE *mf;
    hype_rtc_time_t now;
    hype_log_sink_t hype_sink;
    hype_log_sink_t vm[NVM];
    hype_log_sink_t *all[1u + NVM];
    static const char *vmname[NVM] = {"VM0.LOG", "VM1.LOG", "VM2.LOG"};
    unsigned int round, i, rounds;
    int longrun, rc = 0;

    if (argc < 5) {
        fprintf(stderr, "usage: %s <image> <combined|split|longrun> <ok|dead> <manifest>\n", argv[0]);
        return 2;
    }
    scenario = argv[2];
    policy = argv[3];
    g_sync_flaky = (strcmp(policy, "flaky") == 0);
    g_img = fopen(argv[1], "r+b");
    if (!g_img) { perror("open image"); return 2; }
    mf = fopen(argv[4], "wb");
    if (!mf) { perror("open manifest"); return 2; }

    memset(&now, 0, sizeof now);
    now.year = 2026; now.month = 8; now.day = 21; now.hour = 13;
    hype_logbuf_reset();

    longrun = (strcmp(scenario, "longrun") == 0);
    rounds = longrun ? 4000u : (strcmp(scenario, "combined") == 0 ? 1200u : 1500u);

    /* \HYPE.LOG mounts the fs and installs the barrier; per-VM logs share that same fs. This is
     * split_log_setup() exactly. */
    if (hype_log_sink_open_ordered_durable(&hype_sink, img_read, img_write, img_sync, 0, "HYPE.LOG",
                                           &now, HYPE_LOG_SINK_HYPE) != HYPE_LOG_SINK_OK) {
        fprintf(stderr, "open HYPE.LOG failed\n");
        return 2;
    }
    all[0] = &hype_sink;

    if (strcmp(scenario, "combined") != 0) {
        for (i = 0; i < NVM; i++) {
            if (hype_log_sink_open_shared_ordered(&vm[i], &hype_sink.fs, vmname[i], &now, (int)i) !=
                HYPE_LOG_SINK_OK) {
                fprintf(stderr, "open %s failed\n", vmname[i]);
                return 2;
            }
            all[1u + i] = &vm[i];
        }
    }

    /* Arm the flaky barrier only now: the sinks are opened at boot, when the barrier works. The
     * intermittent rejection models the device starting to refuse mid-run, which is when #596
     * appeared -- the logs had already grown. */
    g_sync_armed = 1;

    for (round = 0; round < rounds; round++) {
        /* Feed a mixed burst into the shared buffer, then drain every sink -- so the files grow
         * interleaved through the one fs. */
        feed_hype(round);
        feed_hype(round * 2u + 1u);
        if (strcmp(scenario, "combined") != 0) {
            feed_vm(0u, round);
            feed_vm(1u, round);
            if ((round & 1u) == 0u) feed_vm(2u, round); /* vm2 quieter -- uneven cadence */
        }
        drain(all, (strcmp(scenario, "combined") == 0) ? 1u : (1u + NVM));
        if (longrun && (round % 32u) == 0u) {
            reclaim(all, 1u + NVM); /* #585: keep run length bounded by the medium */
        }
    }

    /* Final drain -- the shutdown flush. */
    for (i = 0; i < ((strcmp(scenario, "combined") == 0) ? 1u : (1u + NVM)); i++) {
        hype_log_sink_flush(all[i]);
    }

    manifest_file(mf, &hype_sink.fs, "HYPE.LOG");
    if (strcmp(scenario, "combined") != 0) {
        for (i = 0; i < NVM; i++) manifest_file(mf, &hype_sink.fs, vmname[i]);
    }
    fclose(mf);
    fflush(g_img);
    fclose(g_img);

    printf("scenario=%s policy=%s rounds=%u reclaimed=%llu bytes -- HYPE.LOG flushed=%u\n", scenario,
           policy, rounds, (unsigned long long)hype_logbuf_reclaimed(),
           hype_log_sink_flushed(&hype_sink));
    return rc;
}
