#include "fat32_selftest.h"

/* #597: on-medium FAT32 write battery. See fat32_selftest.h for what this reproduces and why the
 * schedule/content live in the header (shared with the host validator). */

#define CHUNK 4096u

static void gen(unsigned int seed, unsigned int base, uint8_t *buf, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) buf[i] = hype_fat32_selftest_byte(seed, base + i);
}

static void note_fail(hype_fat32_selftest_result_t *res, const char *what, const char *path) {
    unsigned int p = 0, i;
    if (res->first_fail[0] != '\0') return; /* keep only the first */
    for (i = 0; what[i] && p < sizeof res->first_fail - 1u; i++) res->first_fail[p++] = what[i];
    for (i = 0; path[i] && p < sizeof res->first_fail - 1u; i++) res->first_fail[p++] = path[i];
    res->first_fail[p] = '\0';
}

/* Write one battery file with its declared mode. Returns 0 if the writer accepted the whole file,
 * -1 if it refused part way (volume full / I/O). A refusal is not a corruption. */
static int write_one(hype_fs_t *fs, const hype_fat32_selftest_item_t *it) {
    hype_fs_file_t f;
    uint8_t buf[CHUNK];
    unsigned int done = 0;

    hype_fs_unlink(fs, it->path); /* best-effort: make reruns clean */
    if (hype_fs_create(fs, it->path, &f) != 0) return -1;

    while (done < it->len) {
        unsigned int chunk;
        if (it->mode == HYPE_FAT32_SELFTEST_APPEND_N) {
            chunk = 137u + (done & 0x1FFu); /* tiny, cluster-unaligned -- the #596 log pattern */
        } else {
            chunk = CHUNK;
        }
        if (chunk > it->len - done) chunk = it->len - done;
        if (chunk > CHUNK) chunk = CHUNK;
        gen(it->seed, done, buf, chunk);
        if (it->mode == HYPE_FAT32_SELFTEST_WRITEAT) {
            if (hype_fs_write_at(&f, done, buf, chunk) != 0) return -1;
        } else {
            if (hype_fs_append(&f, buf, chunk) != 0) return -1;
        }
        done += chunk;
    }
    return 0;
}

/* Re-open the file and compare every byte to the deterministic content. Returns 0 if byte-exact,
 * -1 on any reopen/size/read/content failure. Writes the file's first cluster to *first_cluster
 * (0 if it could not be opened) so the caller can log the on-volume boundary. */
static int verify_one(hype_fs_t *fs, const hype_fat32_selftest_item_t *it,
                      hype_fat32_selftest_result_t *res, uint32_t *first_cluster) {
    hype_fs_file_t f;
    uint8_t buf[CHUNK];
    uint8_t exp[CHUNK];
    uint64_t off = 0;

    *first_cluster = 0u;
    if (hype_fs_lookup(fs, it->path, &f) != 0) {
        note_fail(res, "reopen ", it->path);
        return -1;
    }
    *first_cluster = f.u.fat32.first_cluster; /* this is a FAT32 volume; the union arm is fat32 */
    if (f.size != (uint64_t)it->len) {
        note_fail(res, "size ", it->path);
        return -1;
    }
    while (off < it->len) {
        unsigned int n = (unsigned int)(it->len - off);
        unsigned int i;
        if (n > CHUNK) n = CHUNK;
        if (hype_fs_read_at(&f, off, buf, n) != 0) {
            note_fail(res, "read ", it->path);
            return -1;
        }
        gen(it->seed, (unsigned int)off, exp, n);
        for (i = 0; i < n; i++) {
            if (buf[i] != exp[i]) {
                note_fail(res, "content ", it->path);
                return -1;
            }
        }
        off += n;
    }
    return 0;
}

/* Interleaved phase: keep several files open at once and round-robin one small append across all of
 * them per pass, so the allocator interleaves their clusters -- the concurrent multi-log workload
 * #584/#596 point at, which the per-item loop (one file to completion) cannot produce. */
static void run_interleaved(hype_fs_t *fs, hype_fat32_selftest_result_t *res,
                            hype_fat32_selftest_log_fn log, void *logctx) {
    static hype_fs_file_t files[HYPE_FAT32_SELFTEST_ILEAVE_N];
    hype_fat32_selftest_item_t items[HYPE_FAT32_SELFTEST_ILEAVE_N];
    unsigned int done[HYPE_FAT32_SELFTEST_ILEAVE_N];
    int active[HYPE_FAT32_SELFTEST_ILEAVE_N];
    uint8_t buf[CHUNK];
    unsigned int n = 0, i;
    int progress;

    while (n < HYPE_FAT32_SELFTEST_ILEAVE_N && hype_fat32_selftest_interleaved_item(n, &items[n])) {
        n++;
    }
    for (i = 0; i < n; i++) {
        done[i] = 0u;
        hype_fs_unlink(fs, items[i].path); /* rerun-safe */
        if (hype_fs_create(fs, items[i].path, &files[i]) != 0) {
            active[i] = 0;
            res->files_refused++;
            note_fail(res, "icreate ", items[i].path);
        } else {
            active[i] = 1;
        }
    }

    do {
        progress = 0;
        for (i = 0; i < n; i++) {
            unsigned int chunk;
            if (!active[i] || done[i] >= items[i].len) continue;
            chunk = 149u + (done[i] & 0x1FFu); /* small, cluster-unaligned -- the log pattern */
            if (chunk > items[i].len - done[i]) chunk = items[i].len - done[i];
            if (chunk > CHUNK) chunk = CHUNK;
            gen(items[i].seed, done[i], buf, chunk);
            if (hype_fs_append(&files[i], buf, chunk) != 0) {
                active[i] = 0;
                res->files_refused++;
                note_fail(res, "iappend ", items[i].path);
                continue;
            }
            done[i] += chunk;
            progress = 1;
        }
    } while (progress);

    for (i = 0; i < n; i++) {
        hype_fat32_selftest_event_t ev;
        uint32_t fc = 0u;
        int ok = 0;
        int complete = (done[i] >= items[i].len);
        if (complete) {
            res->files_written++;
            ok = (verify_one(fs, &items[i], res, &fc) == 0);
            if (!ok) res->selfcheck_fail++;
        }
        ev.idx = 1000u + i; /* 1000+ marks the interleaved phase in the log */
        ev.path = items[i].path;
        ev.seed = items[i].seed;
        ev.len = items[i].len;
        ev.mode = items[i].mode;
        ev.first_cluster = fc;
        ev.refused = !complete;
        ev.selfcheck_ok = ok;
        if (log) log(logctx, &ev);
    }
}

int hype_fat32_logtest_run(hype_fs_t *fs, const hype_rtc_time_t *now,
                           hype_fat32_selftest_result_t *res,
                           hype_fat32_selftest_log_fn log, void *logctx) {
    static hype_fs_file_t files[HYPE_FAT32_LOGTEST_N];
    hype_fat32_selftest_item_t items[HYPE_FAT32_LOGTEST_N];
    unsigned int done[HYPE_FAT32_LOGTEST_N];
    int active[HYPE_FAT32_LOGTEST_N];
    uint8_t buf[CHUNK];
    unsigned int n = 0, i;
    int progress;

    for (i = 0; i < sizeof *res; i++) ((uint8_t *)res)[i] = 0u;
    if (now) hype_fs_set_time(fs, now);
    hype_fs_mkdir(fs, HYPE_FAT32_LOGTEST_DIR); /* rerun-safe */

    while (n < HYPE_FAT32_LOGTEST_N && hype_fat32_logtest_item(n, &items[n])) n++;
    for (i = 0; i < n; i++) {
        done[i] = 0u;
        hype_fs_unlink(fs, items[i].path);
        if (hype_fs_create(fs, items[i].path, &files[i]) != 0) {
            active[i] = 0;
            res->files_refused++;
            note_fail(res, "lcreate ", items[i].path);
        } else {
            active[i] = 1;
        }
    }

    /* Round-robin one ~4 KiB batch to each active file per pass -- the log writer's batch
     * granularity (HYPE_LOG_SINK_BATCH_BYTES), concurrent across files on the shared fs. */
    do {
        progress = 0;
        for (i = 0; i < n; i++) {
            unsigned int chunk;
            if (!active[i] || done[i] >= items[i].len) continue;
            chunk = HYPE_FAT32_LOGTEST_BATCH - ((done[i] >> 5) & 0x1FFu); /* ~3585..4096, varied */
            if (chunk > items[i].len - done[i]) chunk = items[i].len - done[i];
            if (chunk > CHUNK) chunk = CHUNK;
            gen(items[i].seed, done[i], buf, chunk);
            if (hype_fs_append(&files[i], buf, chunk) != 0) {
                active[i] = 0;
                res->files_refused++;
                note_fail(res, "lappend ", items[i].path);
                continue;
            }
            done[i] += chunk;
            progress = 1;
        }
    } while (progress);

    for (i = 0; i < n; i++) {
        hype_fat32_selftest_event_t ev;
        uint32_t fc = 0u;
        int ok = 0;
        int complete = (done[i] >= items[i].len);
        if (complete) {
            res->files_written++;
            ok = (verify_one(fs, &items[i], res, &fc) == 0);
            if (!ok) res->selfcheck_fail++;
        }
        ev.idx = 2000u + i; /* 2000+ marks the log-shaped phase in the log */
        ev.path = items[i].path;
        ev.seed = items[i].seed;
        ev.len = items[i].len;
        ev.mode = items[i].mode;
        ev.first_cluster = fc;
        ev.refused = !complete;
        ev.selfcheck_ok = ok;
        if (log) log(logctx, &ev);
    }
    hype_fs_sync(fs);
    return (res->selfcheck_fail || res->files_refused) ? -1 : 0;
}

int hype_fat32_selftest_run(hype_fs_t *fs, const hype_rtc_time_t *now,
                            hype_fat32_selftest_result_t *res,
                            hype_fat32_selftest_log_fn log, void *logctx) {
    hype_fat32_selftest_item_t it;
    unsigned int idx;
    unsigned int i;

    for (i = 0; i < sizeof *res; i++) ((uint8_t *)res)[i] = 0u;

    if (now) {
        hype_fs_set_time(fs, now);
    }
    hype_fs_mkdir(fs, HYPE_FAT32_SELFTEST_DIR); /* ignore "exists": rerun-safe */

    /* Write, verify, and log every file in one pass, in write order. Each file is logged with its
     * on-volume first cluster and its own self-check result, so a later fsck.vfat or validate_stick
     * failure is attributable to the exact test. Every failure is recorded (not just the first). */
    for (idx = 0; hype_fat32_selftest_item(idx, &it); idx++) {
        hype_fat32_selftest_event_t ev;
        int refused = (write_one(fs, &it) != 0);
        uint32_t fc = 0u;
        int ok = 0;

        if (refused) {
            res->files_refused++;
            note_fail(res, "write ", it.path);
        } else {
            res->files_written++;
            ok = (verify_one(fs, &it, res, &fc) == 0);
            if (!ok) res->selfcheck_fail++;
        }

        ev.idx = idx;
        ev.path = it.path;
        ev.seed = it.seed;
        ev.len = it.len;
        ev.mode = it.mode;
        ev.first_cluster = fc;
        ev.refused = refused;
        ev.selfcheck_ok = ok;
        if (log) log(logctx, &ev);
    }

    /* Second phase: the concurrent multi-file growth that mirrors hype's own log writer. */
    run_interleaved(fs, res, log, logctx);

    hype_fs_sync(fs);
    return (res->selfcheck_fail || res->files_refused) ? -1 : 0;
}
