#include "iso_stream.h"

/* Bounce buffer for one disk read: covering sectors land here, then the exact
 * requested slice is copied out. 64 KiB = 128 sectors -- well within one PRDT
 * entry (hype_ahci_host_read's 4 MiB cap), so each fill is a single command.
 *
 * #352: ONE PER SLOT, not one globally. This was a single buffer whose comment claimed
 * "single-threaded use (the guest-exit path)" -- which held for one guest and broke for two: each
 * VM services its own exits on its own AP core, so two streaming reads in flight overwrote each
 * other's covering sectors. Both guests then read another VM's bytes where their ISO's Primary
 * Volume Descriptor should be, found no filesystem, and OVMF reported the CD-ROM as Not Found. */
#define BOUNCE_SECTORS 128u
#define BOUNCE_BYTES (BOUNCE_SECTORS * HYPE_ISO_STREAM_SECTOR)
static uint8_t g_bounce[HYPE_ISO_STREAM_MAX_SLOTS][BOUNCE_BYTES] __attribute__((aligned(4096)));

int hype_iso_stream_locate(const hype_iso_stream_t *s, uint64_t off, uint64_t *out_lba,
                          uint32_t *out_head, uint64_t *out_run) {
    uint64_t seen = 0;
    unsigned i;

    /*
     * extent_count == 0 is the single contiguous run every existing caller sets up (a raw ISO
     * partition, or a one-extent file): logical offset O is simply part_start_lba + O/512, with no
     * boundary to straddle. Kept as its own case rather than synthesised into a 1-entry map so
     * that path is provably unchanged.
     */
    if (s->extent_count == 0u) {
        *out_lba = s->part_start_lba + off / HYPE_ISO_STREAM_SECTOR;
        *out_head = (uint32_t)(off % HYPE_ISO_STREAM_SECTOR);
        /* One run, so everything left in the ISO is contiguous from here. */
        *out_run = (off < s->iso_size) ? (s->iso_size - off) : 0u;
        return (off < s->iso_size) ? 0 : -1;
    }

    for (i = 0; i < s->extent_count && i < HYPE_ISO_STREAM_MAX_EXTENTS; i++) {
        uint64_t bytes = s->extents[i].sector_count * (uint64_t)HYPE_ISO_STREAM_SECTOR;
        if (off < seen + bytes) {
            uint64_t within = off - seen; /* byte offset into this extent */
            *out_lba = s->extents[i].start_lba + within / HYPE_ISO_STREAM_SECTOR;
            *out_head = (uint32_t)(within % HYPE_ISO_STREAM_SECTOR);
            /* Stop at the extent boundary: the next byte lives at an unrelated LBA, so a caller
             * must come back for it rather than reading straight on. */
            *out_run = bytes - within;
            return 0;
        }
        seen += bytes;
    }
    return -1; /* past the end of the mapped runs */
}

int hype_iso_stream_read(hype_iso_stream_t *s, uint64_t off, uint8_t *dst, uint32_t len) {
    uint64_t cur;
    uint32_t remaining;

    if (s->read == 0 || s->iso_size == 0u) {
        return -1;
    }
    /* Bounds + overflow check (off+len must not wrap or exceed the ISO). */
    if (off > s->iso_size || len > s->iso_size - off) {
        return -1;
    }

    cur = off;
    remaining = len;
    while (remaining != 0u) {
        uint64_t lba;
        uint64_t run;
        uint32_t head;
        uint32_t want;
        uint32_t nsec;
        uint32_t avail;
        uint32_t n;
        uint8_t *bounce;
        uint32_t i;

        /* #327: which disk LBA holds `cur`, and how far the run continues. */
        if (hype_iso_stream_locate(s, cur, &lba, &head, &run) != 0) {
            return -1;
        }
        want = head + remaining; /* bytes from `lba` we still need */
        nsec = (want + HYPE_ISO_STREAM_SECTOR - 1u) / HYPE_ISO_STREAM_SECTOR;
        if (nsec > BOUNCE_SECTORS) {
            nsec = BOUNCE_SECTORS;
        }
        /* Never read past this extent's end: the sectors after it belong to another part of the
         * file (or to another file entirely), so reading them would return the wrong bytes. */
        {
            uint64_t run_sec = (head + run + HYPE_ISO_STREAM_SECTOR - 1u) / HYPE_ISO_STREAM_SECTOR;
            if ((uint64_t)nsec > run_sec) {
                nsec = (uint32_t)run_sec;
            }
        }
        if (nsec == 0u) {
            return -1;
        }
        {
            unsigned slot = (s->bounce_slot < HYPE_ISO_STREAM_MAX_SLOTS) ? s->bounce_slot : 0u;
            bounce = g_bounce[slot];
        }
        if (s->read(s->ctx, lba, nsec, bounce) != 0) {
            return -1;
        }
        avail = nsec * HYPE_ISO_STREAM_SECTOR - head; /* usable bytes this fill */
        if ((uint64_t)avail > run) {
            avail = (uint32_t)run; /* clamp to the extent, not just to the sectors fetched */
        }
        n = (avail < remaining) ? avail : remaining;
        if (n == 0u) {
            return -1; /* no forward progress -- refuse rather than spin */
        }
        for (i = 0; i < n; i++) {
            dst[i] = bounce[head + i];
        }
        dst += n;
        cur += n;
        remaining -= n;
    }
    return 0;
}
