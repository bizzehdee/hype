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

/*
 * #327: one contiguous run of the ISO's bytes on disk, in DISK-ABSOLUTE LBAs.
 *
 * A raw ISO partition is one run. An ISO stored as a FILE may be many: hype's resolvers hand back
 * up to HYPE_FAT_MAX_EXTENTS (64) of them, and the ISO path used to accept only the single-extent
 * case -- so a 3-5 GB Windows ISO copied onto a volume that was not freshly formatted simply could
 * not be streamed. On ext it is worse than unlikely: core/ext.h notes large indirect-mapped files
 * are STRUCTURALLY fragmented, so one extent is close to unachievable there.
 */
/*
 * #327/#326 CONTRACT: a hype_iso_stream_t MUST be zero-initialised before use.
 *
 * `extent_count == 0` means "one contiguous run from part_start_lba" -- it cannot be distinguished
 * from "never set", so an uninitialised struct does not fail loudly, it reads the WRONG SECTORS or
 * refuses depending on stack garbage. hype's own streams live in statics (g_vms[]) and are therefore
 * zeroed by construction; a caller building one on the stack must memset it.
 *
 * Learned the hard way: core/tests/test_iso_stream.c declared one on the stack and passed for as long
 * as that stack slot happened to be zero, then started failing four assertions after unrelated code
 * shifted the layout. ASan does not catch uninitialised reads.
 */
typedef struct {
    uint64_t start_lba;    /* disk-absolute first LBA of this run */
    uint64_t sector_count; /* length of the run, in 512-byte sectors */
} hype_iso_extent_t;

#define HYPE_ISO_STREAM_MAX_EXTENTS 64u

/* #352: one bounce buffer per concurrently-readable stream (one per VM). */
#define HYPE_ISO_STREAM_MAX_SLOTS 2u

typedef struct {
    hype_iso_disk_read_fn read;
    void *ctx;
    uint64_t part_start_lba; /* first LBA of the raw ISO partition on the disk */
    uint64_t iso_size;       /* logical ISO size in bytes (bounds reads) */
    /*
     * #327: the extent map, when the ISO is a fragmented file. `extent_count == 0` means "use
     * part_start_lba as one contiguous run", which is exactly what a raw-partition ISO is and what
     * every existing caller already set up -- so that path is unchanged and needs no migration.
     */
    hype_iso_extent_t extents[HYPE_ISO_STREAM_MAX_EXTENTS];
    unsigned extent_count;

    /*
     * #365: what the bounce buffer currently holds, so a sequential stream stops re-fetching
     * bytes it already has.
     *
     * Measured on hardware: a guest 2 KB disc read cost 4.75 separate USB calls, because each
     * fill read up to 64 KiB and then DISCARDED everything past the few bytes asked for -- 6x
     * more read from the device than the guest requested. 97.5% of guest reads start exactly
     * where the previous one ended, so nearly all of that refetching is avoidable.
     *
     * Why this is safe against the #343 class, where two vCPUs sharing one device handed guests
     * corrupt bytes:
     *
     *  - the bounce buffer is ALREADY per-VM (bounce_slot, one slot per guest, #352), so this
     *    retains per-VM state and introduces no new sharing between guests;
     *  - the stream contract is READ-ONLY -- hype_iso_stream_t carries a read callback and no
     *    write callback, so nothing can write through this path and leave the buffer stale;
     *  - a write reaching the underlying device by any OTHER route must still invalidate, which
     *    hype_iso_stream_invalidate() exists for. Media is read-only today; the hook is there so
     *    that stops being an unwritten assumption.
     *
     * cache_sectors == 0 means "nothing held". Zero-initialised by the same contract as
     * extent_count above.
     */
    uint64_t cache_lba;      /* first disk LBA held in the bounce buffer */
    uint32_t cache_sectors;  /* how many sectors, 0 = empty */
    uint64_t cache_hits;     /* served without touching the device */
    /*
     * #365: where the previous fill ended, so read-ahead can be spent only where it pays.
     *
     * A fixed 128-sector read-ahead is a large win on a sequential stream and a large LOSS on a
     * seeking one: every miss fetches 64 KiB to serve a 2 KiB request, then misses again. Measured
     * on real hardware with two guests -- vm1 streaming sequentially hit 95%, vm0 seeking hit 13%,
     * and between them USB moved 55.3 MB to deliver 9.8 MB to the guests. That 5.6x amplification
     * is the cost of reading ahead for a guest that was never going to use it.
     */
    uint64_t next_seq_lba;   /* LBA that would continue the last fill; 0 = no history */
    uint64_t cache_misses;   /* required a device read */
    /*
     * #352: which bounce buffer this stream fills. The buffer used to be one file-global,
     * documented as "single-threaded use (the guest-exit path)" -- true of one guest, false of
     * two: each VM runs its exit loop on its own AP core, so two concurrent streaming reads
     * clobbered each other's covering sectors and both guests were handed the other's bytes.
     * Streams that never set this share slot 0, which is correct as long as only one of them
     * can be read at a time (the single-VM and unit-test cases).
     */
    unsigned bounce_slot;
} hype_iso_stream_t;

/*
 * #327: map a logical ISO offset to the disk LBA holding it, and report how many CONTIGUOUS bytes
 * follow it in the same extent.
 *
 * Pure, and separated from the read path deliberately: the arithmetic is the whole risk here (a
 * read straddling an extent boundary must split, not run off the end of a run), and this way it is
 * unit-testable directly rather than only through a fake disk.
 *
 * Returns 0 and fills *out_lba / *out_head / *out_run on success. *out_head is the byte offset
 * within that sector; *out_run is how many bytes of the request can be served before the next
 * extent must be consulted. Returns -1 if `off` is past the end of the map.
 */
/*
 * #365: drop whatever the bounce buffer holds. Call this if anything writes to the underlying
 * device, so a subsequent read cannot be served from bytes that are no longer on the medium.
 * Safe to call on a zeroed or idle stream.
 */
void hype_iso_stream_invalidate(hype_iso_stream_t *s);

/* #365: read-ahead effectiveness -- hits are guest reads served without a device transfer. */
void hype_iso_stream_cache_stats(const hype_iso_stream_t *s, uint64_t *hits, uint64_t *misses);

int hype_iso_stream_locate(const hype_iso_stream_t *s, uint64_t off, uint64_t *out_lba,
                          uint32_t *out_head, uint64_t *out_run);

/*
 * Copies `len` bytes from logical ISO offset `off` into `dst`, fetching the
 * covering disk sectors through `s->read` and handling arbitrary (non-sector-
 * aligned) `off`/`len` via an internal bounce buffer. Returns 0 on success, -1
 * if the range is out of bounds (off+len > iso_size, or overflow), the layout
 * is unset (read==NULL / iso_size==0), or a disk read fails.
 */
int hype_iso_stream_read(hype_iso_stream_t *s, uint64_t off, uint8_t *dst, uint32_t len);

#endif /* HYPE_CORE_ISO_STREAM_H */
