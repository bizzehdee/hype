#ifndef HYPE_CORE_ISO_STREAM_H
#define HYPE_CORE_ISO_STREAM_H

#include <stdint.h>

/*
 * GLADDER-10 (streaming ISO backend): serves ISO bytes on demand from a raw
 * disk partition instead of a RAM-resident copy. Parallel to
 * core/chunked_iso.c (RAM chunks) -- same "read len bytes at logical offset"
 * contract, so the ATAPI glue can back a guest CD read either way. The ISO
 * lives on its own partition (located via core/gpt.c); logical offset O maps to
 * disk LBA part_start_lba + O/512. Actual sector I/O is an injected callback
 * (core/ahci_host.c's hype_ahci_host_read at runtime; a fake in tests), so the
 * offset/alignment logic is unit-tested without hardware.
 */

#define HYPE_ISO_STREAM_SECTOR 512u

/* Reads `count` 512-byte sectors at `lba` into `dst`; 0 on success, non-zero on
 * error. `ctx` carries whatever the backend needs (e.g. ABAR + port). */
typedef int (*hype_iso_disk_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *dst);

typedef struct {
    hype_iso_disk_read_fn read;
    void *ctx;
    uint64_t part_start_lba; /* first LBA of the raw ISO partition on the disk */
    uint64_t iso_size;       /* logical ISO size in bytes (bounds reads) */
} hype_iso_stream_t;

/*
 * Copies `len` bytes from logical ISO offset `off` into `dst`, fetching the
 * covering disk sectors through `s->read` and handling arbitrary (non-sector-
 * aligned) `off`/`len` via an internal bounce buffer. Returns 0 on success, -1
 * if the range is out of bounds (off+len > iso_size, or overflow), the layout
 * is unset (read==NULL / iso_size==0), or a disk read fails.
 */
int hype_iso_stream_read(hype_iso_stream_t *s, uint64_t off, uint8_t *dst, uint32_t len);

#endif /* HYPE_CORE_ISO_STREAM_H */
