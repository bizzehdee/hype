#include "fs_battery_sparse.h"
#include "strutil.h"

#define WRITE_LEN 16u

static int step(hype_fs_battery_sparse_result_t *res, hype_fs_battery_sparse_log_fn log,
                void *logctx, const char *what, int ok) {
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

int hype_fs_battery_sparse_run(hype_fs_t *fs, const char *path,
                               hype_fs_battery_sparse_result_t *res,
                               hype_fs_battery_sparse_log_fn log, void *logctx) {
    hype_file_rmap_t m;
    hype_fs_file_t f;
    uint64_t hole_off = 0;
    uint64_t hole_len_bytes = 0;
    uint8_t zero_check[64];
    uint8_t pattern[WRITE_LEN];
    uint32_t check_len;
    unsigned i;
    unsigned hi;
    int found = 0;

    if (fs == 0 || path == 0 || res == 0) {
        return -1;
    }
    for (i = 0; i < sizeof *res; i++) {
        ((uint8_t *)res)[i] = 0u;
    }
    if ((hype_fs_caps(fs) & HYPE_FS_CAP_SPARSE) == 0u) {
        return -1; /* this driver cannot represent an internal hole at all */
    }

    if (!step(res, log, logctx, "map_ranges", hype_fs_map_ranges(fs, path, &m) == 0)) {
        return -1;
    }

    {
        uint64_t off = 0;
        for (hi = 0; hi < m.count; hi++) {
            uint64_t run_bytes = m.ranges[hi].sector_count * (uint64_t)512u;
            if (m.ranges[hi].kind == HYPE_RANGE_HOLE || m.ranges[hi].kind == HYPE_RANGE_UNWRITTEN) {
                hole_off = off;
                hole_len_bytes = run_bytes;
                found = 1;
                break;
            }
            off += run_bytes;
        }
    }
    if (!step(res, log, logctx, "fixture has a genuine HOLE/UNWRITTEN range (not a driver gap -- "
                                "the caller's fixture must supply one)",
             found)) {
        return -1;
    }
    res->hole_found++;

    check_len = (hole_len_bytes < sizeof zero_check) ? (uint32_t)hole_len_bytes
                                                     : (uint32_t)sizeof zero_check;
    if (!step(res, log, logctx, "lookup", hype_fs_lookup(fs, path, &f) == 0)) {
        return -1;
    }
    if (!step(res, log, logctx, "read_at spans the hole",
             hype_fs_read_at(&f, hole_off, zero_check, check_len) == 0)) {
        return -1;
    }
    {
        int all_zero = 1;
        for (i = 0; i < check_len; i++) {
            if (zero_check[i] != 0u) {
                all_zero = 0;
                break;
            }
        }
        if (!step(res, log, logctx, "hole reads as genuine zero bytes", all_zero)) {
            return -1;
        }
    }
    res->hole_reads_zero++;

    /* write into the hole: EITHER outcome is legitimate, but exactly one
     * of them must occur. */
    for (i = 0; i < WRITE_LEN && i < sizeof pattern; i++) {
        pattern[i] = (uint8_t)(0x55u + i);
    }
    {
        uint32_t write_len = (WRITE_LEN < hole_len_bytes) ? WRITE_LEN : (uint32_t)hole_len_bytes;
        int write_rc = hype_fs_write_at(&f, hole_off, pattern, write_len);
        if (write_rc == 0) {
            uint8_t back[WRITE_LEN];
            int byte_ok;
            if (!step(res, log, logctx, "hole write accepted: read_at confirms it landed",
                     hype_fs_read_at(&f, hole_off, back, write_len) == 0)) {
                return -1;
            }
            byte_ok = 1;
            for (i = 0; i < write_len; i++) {
                if (back[i] != pattern[i]) {
                    byte_ok = 0;
                    break;
                }
            }
            if (!step(res, log, logctx, "filled hole reads back byte-exact", byte_ok)) {
                return -1;
            }
            res->hole_filled_ok++;
        } else {
            if (log != 0) {
                log(logctx, "hole write refused outright (also a legitimate outcome -- e.g. "
                            "NTFS's decision-30 contract through this generic vtable)",
                    1);
            }
            res->hole_write_refused++;
        }
    }

    return res->failures == 0u ? 0 : -1;
}
