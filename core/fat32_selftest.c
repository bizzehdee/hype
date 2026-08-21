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
 * -1 on any reopen/size/read/content failure. */
static int verify_one(hype_fs_t *fs, const hype_fat32_selftest_item_t *it,
                      hype_fat32_selftest_result_t *res) {
    hype_fs_file_t f;
    uint8_t buf[CHUNK];
    uint8_t exp[CHUNK];
    uint64_t off = 0;

    if (hype_fs_lookup(fs, it->path, &f) != 0) {
        note_fail(res, "reopen ", it->path);
        return -1;
    }
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

int hype_fat32_selftest_run(hype_fs_t *fs, const hype_rtc_time_t *now,
                            hype_fat32_selftest_result_t *res) {
    hype_fat32_selftest_item_t it;
    unsigned int idx;
    unsigned int i;

    for (i = 0; i < sizeof *res; i++) ((uint8_t *)res)[i] = 0u;

    if (now) {
        hype_fs_set_time(fs, now);
    }
    hype_fs_mkdir(fs, HYPE_FAT32_SELFTEST_DIR); /* ignore "exists": rerun-safe */

    for (idx = 0; hype_fat32_selftest_item(idx, &it); idx++) {
        if (write_one(fs, &it) == 0) {
            res->files_written++;
        } else {
            res->files_refused++;
            note_fail(res, "write ", it.path);
        }
    }
    for (idx = 0; hype_fat32_selftest_item(idx, &it); idx++) {
        if (verify_one(fs, &it, res) != 0) {
            res->selfcheck_fail++;
        }
    }
    hype_fs_sync(fs);
    return res->selfcheck_fail ? -1 : 0;
}
