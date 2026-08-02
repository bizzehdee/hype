#ifndef HYPE_CORE_BLK_BACKEND_H
#define HYPE_CORE_BLK_BACKEND_H

#include <stdint.h>

/*
 * M5-3 (§6d): the block-backend abstraction. A guest's virtio-blk/AHCI disk
 * frontend serves either a host-file-backed virtual disk (`file:` target) or a
 * real physical drive (`physical:` target, M10-3/#123) through ONE vtable, so
 * the frontend never knows or cares which backend it is writing to.
 *
 * The security-critical invariant (AGENTS.md / §6j / VALID-3) lives here: every
 * guest-supplied LBA+count is bounds-checked against the backend's real
 * capacity BEFORE the host dereferences the backing resource. That check is
 * centralised in hype_blk_backend_read/write() so no backend implementation can
 * forget it -- the impls receive only already-validated ranges.
 *
 * Pure/dependency-injected and unit-tested: a backend is a pair of function
 * pointers plus its capacity; the file-backed impl below is a thin memcpy over
 * a host buffer, and tests exercise both the bounds gate and the data path
 * without any real disk.
 */

#define HYPE_BLK_SECTOR_SIZE 512u

/*
 * A block backend: read/write `count` 512-byte sectors at `lba`. The impls are
 * called ONLY with ranges hype_blk_backend_read/write() has already
 * bounds-checked, so they may assume [lba, lba+count) fits `total_sectors`.
 * `write` may be NULL for a read-only backend (e.g. a CD image); a write
 * through a read-only backend is rejected. Return 0 on success, -1 on a
 * backing-store error.
 */
typedef struct hype_blk_backend {
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
    void *ctx;
    uint64_t total_sectors; /* backend capacity, in 512-byte sectors */
} hype_blk_backend_t;

/*
 * True (1) if [lba, lba+count) lies entirely within `total_sectors`, with an
 * overflow guard on lba+count. count==0 is out of bounds (a real read/write is
 * always at least one sector). This is the VALID-3 rule for block I/O.
 */
int hype_blk_range_in_bounds(uint64_t total_sectors, uint64_t lba, uint32_t count);

/*
 * Bounds-check + dispatch a guest read/write. Returns 0 on success; -1 if `be`
 * or its `read`/`write` pointer is NULL, the range is out of bounds, or the
 * backend reports an error. These are the ONLY entry points a frontend should
 * call -- never be->read/be->write directly, which would skip the bounds gate.
 */
/*
 * #265: write-side throughput instrumentation.
 *
 * The read path has reported throughput per batch for a long time; the write path
 * reported NOTHING, so "mkfs.fat takes minutes on real hardware" could be perceived
 * but never quantified -- there was no way to tell from a log whether writes ran at
 * 200 KB/s or 20 MB/s. On a cold-boot-only machine the log is the only telemetry, so
 * that is the blocking issue: this project's PERF-1 discipline is measure-first.
 *
 * The size histogram is the point, not a decoration. The physical write path issues
 * ONE AHCI command at a time and spins for completion, so throughput is 1/latency and
 * is independent of transfer size. That makes the distribution of REQUEST SIZES the
 * thing that decides whether a workload is fast or slow: many 1-sector metadata writes
 * cost one full round trip each, while the same bytes in few large requests do not.
 * The histogram settles which of those is happening without another hardware run.
 *
 * Counters are aggregate across VMs and are updated without locking -- they are
 * diagnostics, and a lost increment under concurrency is preferable to putting a lock
 * on the I/O path.
 */
#define HYPE_BLK_WSTATS_BUCKETS 6u

typedef struct {
    uint64_t writes;    /* completed write requests */
    uint64_t sectors;   /* total sectors written */
    uint64_t first_tsc; /* clock reading at the FIRST write; 0 if no clock installed */
    uint32_t max_count; /* largest single request, in sectors */
    uint32_t hist[HYPE_BLK_WSTATS_BUCKETS];
} hype_blk_wstats_t;

/*
 * Install the clock used to stamp `first_tsc`. Keeps this file free of any platform
 * clock dependency (and lets tests inject a fake). Elapsed must be measured from the
 * first write, not from boot, or the write phase's throughput is diluted by whatever
 * ran before it. With no clock installed, first_tsc stays 0 and callers skip the rate.
 */
void hype_blk_wstats_set_clock(uint64_t (*now)(void));

/*
 * Bucket index for a request of `count` sectors:
 *   0: 1        (the pathological case -- one round trip per 512 bytes)
 *   1: 2-7      2: 8-31     3: 32-127    4: 128-1023    5: >=1024
 * count==0 is not a real transfer and maps to bucket 0. Pure.
 */
unsigned hype_blk_wstats_bucket(uint32_t count);

void hype_blk_wstats_reset(hype_blk_wstats_t *s);

/* Fold one completed write of `count` sectors into `s`. Pure (no clock). */
void hype_blk_wstats_record(hype_blk_wstats_t *s, uint32_t count);

/*
 * Throughput in KB/s over `elapsed_ms`. Returns 0 when elapsed_ms is 0 rather than
 * dividing by zero. Pure, so the rate arithmetic is unit-tested rather than only ever
 * exercised on hardware.
 */
uint64_t hype_blk_wstats_kbps(const hype_blk_wstats_t *s, uint64_t elapsed_ms);

/* The process-wide write stats that hype_blk_backend_write() folds into. */
hype_blk_wstats_t *hype_blk_wstats(void);

int hype_blk_backend_read(const hype_blk_backend_t *be, uint64_t lba, uint32_t count, void *buf);
int hype_blk_backend_write(const hype_blk_backend_t *be, uint64_t lba, uint32_t count,
                           const void *buf);

/*
 * File-backed implementation: a raw disk image resident in a host buffer
 * (`base`, `size_bytes`). `size_bytes` need not be a whole sector multiple --
 * total_sectors is the floor, and any trailing partial sector is unreachable.
 */
typedef struct {
    uint8_t *base;
    uint64_t total_sectors;
} hype_blk_file_t;

/*
 * Initialises `f` over [base, base+size_bytes) and wires `be` to it (read +
 * write, since a file target is writable). After this, drive I/O only through
 * hype_blk_backend_read/write(be, ...).
 */
void hype_blk_file_init(hype_blk_file_t *f, hype_blk_backend_t *be, uint8_t *base,
                        uint64_t size_bytes);

#endif /* HYPE_CORE_BLK_BACKEND_H */
