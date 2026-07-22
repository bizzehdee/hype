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
