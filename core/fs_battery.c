#include "fs_battery.h"
#include "strutil.h"

#define PREFIX_LEN 32u
#define APPEND_LEN 16u

static void fill_pattern(uint8_t *buf, unsigned n, uint8_t seed) {
    unsigned i;
    for (i = 0; i < n; i++) {
        buf[i] = (uint8_t)(seed + i * 7u + 1u);
    }
}

static int bytes_eq(const uint8_t *a, const uint8_t *b, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int step(hype_fs_battery_result_t *res, hype_fs_battery_log_fn log, void *logctx,
                const char *what, int ok) {
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

int hype_fs_battery_run(hype_fs_t *fs, const char *path, hype_fs_battery_result_t *res,
                        hype_fs_battery_log_fn log, void *logctx) {
    hype_fs_file_t f;
    uint8_t orig[PREFIX_LEN];   /* the file's real content before this battery touches it */
    uint8_t write_pat[PREFIX_LEN]; /* what write_at() wrote, if it ran */
    uint8_t append_pat[APPEND_LEN];
    uint8_t buf[PREFIX_LEN > APPEND_LEN ? PREFIX_LEN : APPEND_LEN];
    const uint8_t *expect_prefix;
    unsigned caps;
    unsigned prefix_len;
    uint64_t old_size;
    int wrote = 0;

    if (fs == 0 || path == 0 || res == 0) {
        return -1;
    }
    {
        unsigned i;
        for (i = 0; i < sizeof *res; i++) {
            ((uint8_t *)res)[i] = 0u;
        }
    }

    if (!step(res, log, logctx, "lookup(existing file)", hype_fs_lookup(fs, path, &f) == 0)) {
        return -1; /* a bad fixture/path is the caller's bug, not a driver capability gap */
    }
    old_size = f.size;
    prefix_len = (old_size < PREFIX_LEN) ? (unsigned)old_size : PREFIX_LEN;
    if (prefix_len != 0u) {
        if (!step(res, log, logctx, "read_at(0)", hype_fs_read_at(&f, 0, orig, prefix_len) == 0)) {
            return -1;
        }
    }
    res->read_ok++;
    expect_prefix = orig;

    caps = hype_fs_caps(fs);
    if ((caps & HYPE_FS_CAP_WRITE_INPLACE) == 0u || prefix_len == 0u) {
        res->skipped_no_write++;
        if (log != 0) {
            log(logctx, "write_at: driver has no in-place writer, or file is empty", 1);
        }
    } else {
        fill_pattern(write_pat, prefix_len, 0x11u);
        if (!step(res, log, logctx, "write_at(0)",
                 hype_fs_write_at(&f, 0, write_pat, prefix_len) == 0)) {
            return -1;
        }
        if (!step(res, log, logctx, "read_at(0) after write",
                 hype_fs_read_at(&f, 0, buf, prefix_len) == 0)) {
            return -1;
        }
        if (!step(res, log, logctx, "write/read byte-exact", bytes_eq(buf, write_pat, prefix_len))) {
            return -1;
        }
        res->write_verified++;
        wrote = 1;
        expect_prefix = write_pat;
    }

    if ((caps & HYPE_FS_CAP_APPEND) == 0u) {
        res->skipped_no_append++;
        if (log != 0) {
            log(logctx, "append: driver has no HYPE_FS_CAP_APPEND", 1);
        }
        return res->failures == 0u ? 0 : -1;
    }

    fill_pattern(append_pat, APPEND_LEN, 0x77u);
    if (!step(res, log, logctx, "append", hype_fs_append(&f, append_pat, APPEND_LEN) == 0)) {
        return -1;
    }
    if (!step(res, log, logctx, "size grew by exactly the appended length",
             f.size == old_size + APPEND_LEN)) {
        return -1;
    }
    if (!step(res, log, logctx, "read_at(old_size) after append",
             hype_fs_read_at(&f, old_size, buf, APPEND_LEN) == 0)) {
        return -1;
    }
    if (!step(res, log, logctx, "appended region byte-exact", bytes_eq(buf, append_pat, APPEND_LEN))) {
        return -1;
    }
    if (prefix_len != 0u) {
        if (!step(res, log, logctx, "read_at(0) after append",
                 hype_fs_read_at(&f, 0, buf, prefix_len) == 0)) {
            return -1;
        }
        if (!step(res, log, logctx, "prefix untouched by append",
                 bytes_eq(buf, expect_prefix, prefix_len))) {
            return -1;
        }
    }
    (void)wrote;
    res->append_verified++;

    return res->failures == 0u ? 0 : -1;
}
