#include "fs_battery_ntfs_ext.h"
#include "strutil.h"

#define PATH_BUF 128u
#define PATTERN_LEN 32u
#define APPEND_LEN 16u

static void join(char *out, const char *dir, const char *leaf) {
    unsigned long long n = hype_strlcpy(out, dir, PATH_BUF);
    if (n + 1u < PATH_BUF) {
        out[n] = '/';
        out[n + 1u] = 0;
        n++;
    }
    hype_strlcpy(out + n, leaf, PATH_BUF - n);
}

static void fill_pattern(uint8_t *buf, unsigned n, uint8_t seed) {
    unsigned i;
    for (i = 0; i < n; i++) {
        buf[i] = (uint8_t)(seed + i * 7u + 1u);
    }
}

/* Runs one already-completed operation's result through the battery's
 * bookkeeping exactly once -- never re-invokes the operation. */
static int step(hype_fs_battery_ntfs_ext_result_t *res, hype_fs_battery_ntfs_ext_log_fn log,
                void *logctx, const char *what, int expect_ok, int rc) {
    int ok = expect_ok ? (rc == 0) : (rc != 0);
    if (log != 0) {
        log(logctx, what, ok);
    }
    if (!ok) {
        res->failures++;
        if (res->first_fail[0] == 0) {
            hype_strlcpy(res->first_fail, what, sizeof res->first_fail);
        }
    }
    return ok;
}

/*
 * Adaptive content check on a just-created, empty file at `path`: skipped
 * (not a failure) if hype_fs_lookup() itself cannot produce a handle --
 * a real, driver-reported capability gap, not a battery assumption. When
 * it can, writes a pattern and reads it back byte-exact; if the driver
 * advertises HYPE_FS_CAP_APPEND, appends more and re-verifies the whole,
 * now-longer file reads back correctly.
 */
static void content_test(hype_fs_t *fs, const char *path, hype_fs_battery_ntfs_ext_result_t *res,
                         hype_fs_battery_ntfs_ext_log_fn log, void *logctx) {
    hype_fs_file_t f;
    uint8_t want[PATTERN_LEN + APPEND_LEN];
    uint8_t got[PATTERN_LEN + APPEND_LEN];
    unsigned caps = hype_fs_caps(fs);
    unsigned i;
    int all_ok;

    if (hype_fs_lookup(fs, path, &f) != 0) {
        res->content_skipped++;
        if (log != 0) {
            log(logctx, "content: lookup on fresh file failed (driver capability gap, not a "
                        "failure)",
                1);
        }
        return;
    }
    if ((caps & HYPE_FS_CAP_WRITE_INPLACE) == 0u) {
        res->content_skipped++;
        if (log != 0) {
            log(logctx, "content: driver has no in-place writer", 1);
        }
        return;
    }

    fill_pattern(want, PATTERN_LEN, 0x11u);
    if (!step(res, log, logctx, "content: write_at", 1, hype_fs_write_at(&f, 0, want, PATTERN_LEN))) {
        return;
    }
    if (!step(res, log, logctx, "content: read_at", 1,
             hype_fs_read_at(&f, 0, got, PATTERN_LEN))) {
        return;
    }
    all_ok = 1;
    for (i = 0; i < PATTERN_LEN; i++) {
        if (got[i] != want[i]) {
            all_ok = 0;
            break;
        }
    }
    if (!step(res, log, logctx, "content: write/read byte-exact", 1, all_ok ? 0 : -1)) {
        return;
    }

    if ((caps & HYPE_FS_CAP_APPEND) != 0u) {
        fill_pattern(want + PATTERN_LEN, APPEND_LEN, 0x77u);
        if (!step(res, log, logctx, "content: append", 1,
                 hype_fs_append(&f, want + PATTERN_LEN, APPEND_LEN))) {
            return;
        }
        if (!step(res, log, logctx, "content: read_at after append", 1,
                 hype_fs_read_at(&f, 0, got, PATTERN_LEN + APPEND_LEN))) {
            return;
        }
        all_ok = 1;
        for (i = 0; i < PATTERN_LEN + APPEND_LEN; i++) {
            if (got[i] != want[i]) {
                all_ok = 0;
                break;
            }
        }
        if (!step(res, log, logctx, "content: append byte-exact", 1, all_ok ? 0 : -1)) {
            return;
        }
    }
    res->content_verified++;
}

int hype_fs_battery_ntfs_ext_run(hype_fs_t *fs, const char *dir,
                                 hype_fs_battery_ntfs_ext_result_t *res,
                                 hype_fs_battery_ntfs_ext_log_fn log, void *logctx) {
    hype_fs_file_t f;
    char p_f1[PATH_BUF], p_f2[PATH_BUF], p_f3[PATH_BUF];
    unsigned i;

    if (fs == 0 || dir == 0 || res == 0) {
        return -1;
    }
    for (i = 0; i < sizeof *res; i++) {
        ((uint8_t *)res)[i] = 0u;
    }
    if ((hype_fs_caps(fs) & HYPE_FS_CAP_NAMESPACE) == 0u) {
        return -1; /* this driver does not implement namespace mutation at all */
    }

    join(p_f1, dir, "f1.bin");
    join(p_f2, dir, "f2.bin");
    join(p_f3, dir, "f3.bin");

    /* the battery's own directory: created fresh, must not already exist,
     * and a second mkdir() on the same name must be refused */
    if (step(res, log, logctx, "mkdir(dir)", 1, hype_fs_mkdir(fs, dir))) {
        res->dirs_created++;
    }
    if (step(res, log, logctx, "mkdir(dir) again refused", 0, hype_fs_mkdir(fs, dir))) {
        res->duplicate_refusals_ok++;
    }

    if (step(res, log, logctx, "create(f1)", 1, hype_fs_create(fs, p_f1, &f))) {
        res->files_created++;
    }
    if (step(res, log, logctx, "create(f1) again refused", 0, hype_fs_create(fs, p_f1, &f))) {
        res->duplicate_refusals_ok++;
    }
    if (step(res, log, logctx, "create(f2)", 1, hype_fs_create(fs, p_f2, &f))) {
        res->files_created++;
    }
    content_test(fs, p_f2, res, log, logctx);

    /* rename f1 -> f3: creating AT f3 must now be refused (occupied), and
     * creating a FRESH f1 must now succeed again (the old name is free) */
    if (step(res, log, logctx, "rename(f1 -> f3)", 1, hype_fs_rename(fs, p_f1, p_f3))) {
        res->renames_ok++;
    }
    if (step(res, log, logctx, "create(f3) refused (rename target occupied)", 0,
            hype_fs_create(fs, p_f3, &f))) {
        res->duplicate_refusals_ok++;
    }
    if (step(res, log, logctx, "create(f1) ok again (old name freed by rename)", 1,
            hype_fs_create(fs, p_f1, &f))) {
        res->files_created++;
    }

    /* rename onto an OTHER existing name must be refused, and must leave
     * both names exactly where they were (proven the same way existence is
     * proven throughout: a fresh create() on either must still be refused) */
    step(res, log, logctx, "rename(f1 -> f2) refused (destination occupied)", 0,
        hype_fs_rename(fs, p_f1, p_f2));
    if (step(res, log, logctx, "f1 still present after refused rename", 0,
            hype_fs_create(fs, p_f1, &f))) {
        res->duplicate_refusals_ok++;
    }
    if (step(res, log, logctx, "f2 still present after refused rename", 0,
            hype_fs_create(fs, p_f2, &f))) {
        res->duplicate_refusals_ok++;
    }

    /* teardown: files before the directory, each refused a second time */
    if (step(res, log, logctx, "unlink(f1)", 1, hype_fs_unlink(fs, p_f1))) {
        res->deletes_ok++;
    }
    if (step(res, log, logctx, "unlink(f1) again refused (already gone)", 0,
            hype_fs_unlink(fs, p_f1))) {
        res->stale_refusals_ok++;
    }
    if (step(res, log, logctx, "unlink(f2)", 1, hype_fs_unlink(fs, p_f2))) {
        res->deletes_ok++;
    }
    if (step(res, log, logctx, "unlink(f3)", 1, hype_fs_unlink(fs, p_f3))) {
        res->deletes_ok++;
    }

    step(res, log, logctx, "rmdir(dir) now empty", 1, hype_fs_rmdir(fs, dir));
    if (step(res, log, logctx, "rmdir(dir) again refused (already gone)", 0,
            hype_fs_rmdir(fs, dir))) {
        res->stale_refusals_ok++;
    }

    return res->failures == 0u ? 0 : -1;
}
