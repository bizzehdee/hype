#ifndef HYPE_FAT32_SELFTEST_H
#define HYPE_FAT32_SELFTEST_H

#include <stdint.h>

#include "fs_ops.h"

/*
 * #597: an on-medium FAT32 write battery that hype runs against the volume it booted from, so the
 * exact host-side writer path that corrupted a log on real hardware (#596) can be reproduced on
 * the device that showed it -- real USB timing, real firmware, real barrier behaviour -- not just
 * against a file-backed image on a workstation.
 *
 * The battery SCHEDULE (names, seeds, sizes, write modes) and the deterministic content lives in
 * THIS header as pure code, so that both:
 *   - hype's on-medium run (core/fat32_selftest.c, driving fs_ops against the live stick), and
 *   - the host validator (tools/fat32-e2e/validate_stick.c, reading the files back off the pulled
 *     stick and judging them against fsck.vfat's clean bill)
 * generate byte-for-byte the SAME expectation from one source. A change in one can never silently
 * diverge from the other.
 *
 * hype's writer is host-side; a guest never runs it. This is therefore a host-side battery, not a
 * tests/micro guest kernel.
 */

/* Trigger: hype runs the battery only when this file exists on the boot volume. An operator drops
 * it (tools/hwstick/stage.sh --fat32); its absence means "do nothing", so a normal stick is
 * untouched. Path is volume-relative, 8.3. */
#define HYPE_FAT32_SELFTEST_MARKER "F32TEST.RUN"

/* All battery files live under this directory so a run is self-contained and easy to sweep. */
#define HYPE_FAT32_SELFTEST_DIR "F32TEST"

#define HYPE_FAT32_SELFTEST_APPEND_1 0 /* one append of the whole file */
#define HYPE_FAT32_SELFTEST_APPEND_N 1 /* many small, cluster-unaligned appends -- the #596 log pattern */
#define HYPE_FAT32_SELFTEST_WRITEAT 2  /* write_at, growing from empty */

/* Deterministic, non-zero-dominated content: identical formula in hype and the validator. A wrong
 * cluster reads back as a detectable mismatch, not a plausible run of zeros. */
static inline uint8_t hype_fat32_selftest_byte(unsigned int seed, unsigned int i) {
    unsigned int v = seed * 2654435761u + i * 40503u + (i >> 8) * 97u + (i >> 16) * 131u;
    uint8_t b = (uint8_t)((v >> 13) ^ v ^ (seed + i));
    if (b == 0u) b = (uint8_t)(0xA5u ^ (i & 0xFFu) ^ (seed & 0xFFu));
    return b;
}

/* Append the decimal value `n` (two digits, zero-padded up to 99; wider values print in full)
 * to `out` at *pos, then a terminating NUL. Freestanding: no libc. */
static inline void hype_fat32_selftest_num(char *out, unsigned int *pos, unsigned int n,
                                           unsigned int min_digits) {
    char tmp[10];
    unsigned int t = 0, i;
    do { tmp[t++] = (char)('0' + (n % 10u)); n /= 10u; } while (n);
    while (t < min_digits && t < sizeof tmp) tmp[t++] = '0';
    for (i = 0; i < t; i++) out[(*pos)++] = tmp[t - 1u - i];
}

/* One battery item: the volume-relative path (under the F32TEST dir), the content seed, the byte
 * length, and the write mode. `name` points at caller-owned storage in `buf`. */
typedef struct {
    char path[32];
    unsigned int seed;
    unsigned int len;
    int mode;
} hype_fat32_selftest_item_t;

/* Fill *out for battery index `idx` (0-based). Returns 1 while `idx` is in range, 0 past the end,
 * so a caller loops `for (i = 0; fill(i, &it); i++)`. The three size classes are laid out in a
 * fixed order; the large APPEND_N files are the ones that reproduce the growing-log workload. */
static inline int hype_fat32_selftest_item(unsigned int idx, hype_fat32_selftest_item_t *out) {
    /* size class boundaries */
    static const unsigned int small_sz[12] = {0u,   1u,   63u,   127u,  511u,   512u,
                                               513u, 1000u, 2000u, 4095u, 4096u,  8000u};
    static const unsigned int med_sz[6] = {4096u, 20000u, 65536u, 4096u, 20000u, 65536u};
    static const unsigned int large_sz[3] = {262144u, 1048576u, 4194304u};
    unsigned int nsmall = 12u, nmed = 6u, nlarge = 3u;
    unsigned int pos = 0u;
    unsigned int i;
    const char *dir = HYPE_FAT32_SELFTEST_DIR;

    for (i = 0; dir[i]; i++) out->path[pos++] = dir[i];
    out->path[pos++] = '/';

    if (idx < nsmall) {
        out->path[pos++] = 'S';
        hype_fat32_selftest_num(out->path, &pos, idx, 2u);
        out->seed = 0x100u + idx;
        out->len = small_sz[idx];
        out->mode = (int)(idx % 3u);
    } else if (idx < nsmall + nmed) {
        unsigned int j = idx - nsmall;
        out->path[pos++] = 'M';
        hype_fat32_selftest_num(out->path, &pos, j, 1u);
        out->seed = 0x300u + j;
        out->len = med_sz[j];
        out->mode = (j < 3u) ? HYPE_FAT32_SELFTEST_APPEND_N : HYPE_FAT32_SELFTEST_WRITEAT;
    } else if (idx < nsmall + nmed + nlarge) {
        unsigned int j = idx - nsmall - nmed;
        out->path[pos++] = 'L';
        hype_fat32_selftest_num(out->path, &pos, j, 1u);
        out->seed = 0x400u + j;
        out->len = large_sz[j];
        out->mode = HYPE_FAT32_SELFTEST_APPEND_N; /* the growing-log pattern, at scale */
    } else {
        return 0;
    }
    out->path[pos++] = '.';
    out->path[pos++] = 'B';
    out->path[pos++] = 'I';
    out->path[pos++] = 'N';
    out->path[pos] = '\0';
    return 1;
}

/*
 * Interleaved group: files grown CONCURRENTLY, round-robin small appends across all of them at
 * once, mimicking hype's own log writer growing \HYPE.LOG + each per-VM log at the same time. This
 * is the workload #584 (a cross-link between two logs from a stale FSInfo hint) and #596 point at
 * -- the sequential battery writes one file to completion and cannot surface it. The allocator
 * hands adjacent clusters to different files, so a stale next_free hint or a backward scan
 * cross-links them. These files are written by hype_fat32_selftest_run's interleaved phase, not the
 * per-item loop; the validator still checks them by iterating this schedule.
 */
#define HYPE_FAT32_SELFTEST_ILEAVE_N 6u

static inline int hype_fat32_selftest_interleaved_item(unsigned int idx,
                                                       hype_fat32_selftest_item_t *out) {
    static const unsigned int sz[HYPE_FAT32_SELFTEST_ILEAVE_N] = {40000u,  96000u,  250000u,
                                                                   32768u,  500000u, 130000u};
    unsigned int pos = 0u, i;
    const char *dir = HYPE_FAT32_SELFTEST_DIR;
    if (idx >= HYPE_FAT32_SELFTEST_ILEAVE_N) return 0;
    for (i = 0; dir[i]; i++) out->path[pos++] = dir[i];
    out->path[pos++] = '/';
    out->path[pos++] = 'I';
    hype_fat32_selftest_num(out->path, &pos, idx, 1u);
    out->path[pos++] = '.';
    out->path[pos++] = 'B';
    out->path[pos++] = 'I';
    out->path[pos++] = 'N';
    out->path[pos] = '\0';
    out->seed = 0x500u + idx;
    out->len = sz[idx];
    out->mode = HYPE_FAT32_SELFTEST_APPEND_N; /* always the small-append log pattern */
    return 1;
}

/*
 * Log-shaped phase: reproduce the FS-level write shape of hype's log writer (core/log_sink.c) on
 * the live medium, which the sequential and interleaved phases do not. The log writer grows
 * \HYPE.LOG + each per-VM log CONCURRENTLY through one shared fs, appending in <= 4 KiB BATCHES
 * (HYPE_LOG_SINK_BATCH_BYTES), with the real device's durability barrier on every commit. The
 * interleaved phase above uses tiny 149 B appends; this one uses ~4 KiB batches, a different
 * allocation granularity, at the proportions of a real run (the combined log biggest).
 *
 * log_sink itself is hardwired to the singleton capture buffer that hype's OWN logging uses, so it
 * cannot be driven cleanly on the live stick; this reproduces the bytes-to-disk shape instead,
 * which is what could corrupt the volume (#596). Gated by its own marker so it can run alone.
 */
#define HYPE_FAT32_LOGTEST_MARKER "LOGTEST.RUN"
#define HYPE_FAT32_LOGTEST_DIR "LOGTEST"
#define HYPE_FAT32_LOGTEST_N 5u
#define HYPE_FAT32_LOGTEST_BATCH 4096u /* == HYPE_LOG_SINK_BATCH_BYTES */

static inline int hype_fat32_logtest_item(unsigned int idx, hype_fat32_selftest_item_t *out) {
    /* Proportions of a real multi-VM run: the combined log dominates, per-VM logs trail. */
    static const unsigned int sz[HYPE_FAT32_LOGTEST_N] = {720000u, 300000u, 300000u, 150000u, 90000u};
    static const char *nm[HYPE_FAT32_LOGTEST_N] = {"LHYPE", "LV0", "LV1", "LV2", "LV3"};
    unsigned int pos = 0u, i;
    const char *dir = HYPE_FAT32_LOGTEST_DIR;
    const char *n;
    if (idx >= HYPE_FAT32_LOGTEST_N) return 0;
    for (i = 0; dir[i]; i++) out->path[pos++] = dir[i];
    out->path[pos++] = '/';
    n = nm[idx];
    for (i = 0; n[i]; i++) out->path[pos++] = n[i];
    out->path[pos++] = '.';
    out->path[pos++] = 'L';
    out->path[pos++] = 'O';
    out->path[pos++] = 'G';
    out->path[pos] = '\0';
    out->seed = 0x600u + idx;
    out->len = sz[idx];
    out->mode = HYPE_FAT32_SELFTEST_APPEND_N;
    return 1;
}

/* Result of an on-medium run. */
typedef struct {
    unsigned int files_written; /* files the writer accepted */
    unsigned int files_refused; /* create/append refused (e.g. volume full) -- not a corruption */
    unsigned int selfcheck_fail; /* files hype read back wrong (writer/reader disagreement) */
    char first_fail[96];         /* first failure detail, for the log line */
} hype_fat32_selftest_result_t;

/*
 * Per-file boundary event. hype_fat32_selftest_run() emits one for EVERY file, in write order,
 * BEFORE the aggregate verdict. Recording the boundary of each file -- its identity and its FIRST
 * CLUSTER on the volume -- is what makes one failure distinguishable from another after the fact:
 * when fsck.vfat later names a bad cluster or a bad file, or validate_stick names a byte offset,
 * this log maps it straight back to the exact test (size class, write mode, seed) that produced
 * it. Without it, a post-hoc fsck failure cannot be attributed to a specific test.
 */
typedef struct {
    unsigned int idx;           /* battery index (write order) */
    const char *path;           /* volume-relative path, e.g. F32TEST/L1.BIN */
    unsigned int seed;          /* content seed */
    unsigned int len;           /* intended byte length */
    int mode;                   /* HYPE_FAT32_SELFTEST_APPEND_1 / _APPEND_N / _WRITEAT */
    uint32_t first_cluster;     /* first data cluster the writer placed the file at (0 if empty/refused) */
    int refused;                /* 1 if the writer refused this file (volume full / I/O) */
    int selfcheck_ok;           /* 1 if hype read it back byte-exact, 0 otherwise */
} hype_fat32_selftest_event_t;

typedef void (*hype_fat32_selftest_log_fn)(void *ctx, const hype_fat32_selftest_event_t *ev);

static inline const char *hype_fat32_selftest_mode_name(int mode) {
    switch (mode) {
        case HYPE_FAT32_SELFTEST_APPEND_1: return "append-1";
        case HYPE_FAT32_SELFTEST_APPEND_N: return "append-N";
        case HYPE_FAT32_SELFTEST_WRITEAT:  return "write_at";
        default: return "?";
    }
}

/*
 * Runs the battery against an already-mounted, writable FAT32 volume `fs`. Creates the F32TEST
 * directory, writes every battery file with deterministic content, and reads each back through
 * fs_ops to self-check byte-exact. Leaves every file in place for the host validator to judge
 * after the stick is pulled. Fills *res. Returns 0 if nothing self-check-failed, -1 otherwise.
 * `now` may be NULL (zero timestamps). Pure of hype globals -- takes the fs it is handed.
 */
int hype_fat32_selftest_run(hype_fs_t *fs, const hype_rtc_time_t *now,
                            hype_fat32_selftest_result_t *res,
                            hype_fat32_selftest_log_fn log, void *logctx);

/*
 * Log-shaped phase (see the HYPE_FAT32_LOGTEST_* block above): grow the LOGTEST files concurrently
 * on `fs` with ~4 KiB batched appends, reproducing hype's log-writer output shape on the live
 * medium. Fills *res, emits a per-file event, returns 0 if nothing self-check-failed. Same
 * signature and result type as hype_fat32_selftest_run. Gated separately by \LOGTEST.RUN.
 */
int hype_fat32_logtest_run(hype_fs_t *fs, const hype_rtc_time_t *now,
                           hype_fat32_selftest_result_t *res,
                           hype_fat32_selftest_log_fn log, void *logctx);

#endif /* HYPE_FAT32_SELFTEST_H */
