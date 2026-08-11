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
#define BOUNCE_BYTES (BOUNCE_SECTORS * HYPE_BLK_SECTOR_SIZE)
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
        *out_lba = s->part_start_lba + off / HYPE_BLK_SECTOR_SIZE;
        *out_head = (uint32_t)(off % HYPE_BLK_SECTOR_SIZE);
        /* One run, so everything left in the ISO is contiguous from here. */
        *out_run = (off < s->iso_size) ? (s->iso_size - off) : 0u;
        return (off < s->iso_size) ? 0 : -1;
    }

    for (i = 0; i < s->extent_count && i < HYPE_ISO_STREAM_MAX_EXTENTS; i++) {
        uint64_t bytes = s->extents[i].sector_count * (uint64_t)HYPE_BLK_SECTOR_SIZE;
        if (off < seen + bytes) {
            uint64_t within = off - seen; /* byte offset into this extent */
            *out_lba = s->extents[i].start_lba + within / HYPE_BLK_SECTOR_SIZE;
            *out_head = (uint32_t)(within % HYPE_BLK_SECTOR_SIZE);
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
        uint32_t need;
        uint32_t avail;
        uint32_t n;
        uint8_t *bounce;
        uint32_t i;

        /* #327: which disk LBA holds `cur`, and how far the run continues. */
        if (hype_iso_stream_locate(s, cur, &lba, &head, &run) != 0) {
            return -1;
        }
        want = head + remaining; /* bytes from `lba` we still need */
        need = (want + HYPE_BLK_SECTOR_SIZE - 1u) / HYPE_BLK_SECTOR_SIZE;
        /*
         * #365: fetch a whole bounce buffer, not just what was asked for.
         *
         * The fill used to be sized to exactly the caller's request -- a 2 KB guest read
         * fetched 2 KB -- so nothing was ever left over and retention alone gained nothing.
         * A device read is dominated by a fixed per-request charge (~551 us of USB bus
         * scheduling against ~62 us of wire time for 2 KB), so fetching 64 KiB costs barely
         * more than fetching one sector, and 97.5% of guest reads continue exactly where the
         * previous one ended.
         */
        /*
         * #365: read ahead only when this read continues the last one.
         *
         * Sequential guests get the full bounce buffer, which is where the win is -- a device
         * read is dominated by a fixed per-request charge (~551 us of USB scheduling against
         * ~62 us of wire time for 2 KB), so a big fetch costs barely more than a small one.
         * A seeking guest gets exactly what it asked for, because for it the read-ahead is pure
         * waste: it never returns to those sectors and the next read misses anyway.
         */
        nsec = (s->next_seq_lba != 0u && lba == s->next_seq_lba) ? BOUNCE_SECTORS : need;
        /* Must never exceed the bounce buffer. `need` can: a single 100000-byte guest read is 196
         * sectors against a 128-sector buffer, and taking it verbatim would write past the end of
         * g_bounce. The caller's loop handles a short fill; a buffer overflow it cannot. Caught by
         * an existing test rather than on hardware. */
        if (nsec > BOUNCE_SECTORS) nsec = BOUNCE_SECTORS;

        /* Never read past this extent's end: the sectors after it belong to another part of the
         * file (or to another file entirely), so reading them would return the wrong bytes.
         * Applies to the read-ahead as much as to the requested part. */
        {
            uint64_t run_sec = (head + run + HYPE_BLK_SECTOR_SIZE - 1u) / HYPE_BLK_SECTOR_SIZE;
            if ((uint64_t)nsec > run_sec) {
                nsec = (uint32_t)run_sec;
            }
            if ((uint64_t)need > run_sec) {
                need = (uint32_t)run_sec;
            }
        }
        if (nsec == 0u || need == 0u) {
            return -1;
        }
        {
            unsigned slot = (s->bounce_slot < HYPE_ISO_STREAM_MAX_SLOTS) ? s->bounce_slot : 0u;
            bounce = g_bounce[slot];
        }
        /*
         * #365: are the sectors we NEED already held from a previous fill?
         *
         * The test is on `need`, not on `nsec`. Testing the read-ahead window instead was my
         * first version and it could never hit: lba advances every iteration, so the window
         * asked for is never contained in the window previously fetched. The unit test that
         * asserted a sequential run should mostly hit is what caught it.
         *
         * Only a fully-contained range counts. A partial overlap would need stitching across a
         * refill, and getting that subtly wrong serves stale bytes -- indistinguishable at the
         * guest from #343's corruption. A miss is cheap; a wrong hit is not.
         */
        if (s->cache_sectors != 0u && lba >= s->cache_lba &&
            (lba + need) <= (s->cache_lba + s->cache_sectors)) {
            uint64_t skip = lba - s->cache_lba;
            bounce += skip * HYPE_BLK_SECTOR_SIZE;
            nsec = (uint32_t)(s->cache_sectors - skip); /* usable sectors from `lba` */
            s->cache_hits++;
        } else {
            if (s->read(s->ctx, lba, nsec, bounce) != 0) {
                s->cache_sectors = 0u; /* contents are now unknown, not merely old */
                return -1;
            }
            s->cache_lba = lba;
            s->cache_sectors = nsec;
            s->cache_misses++;
        }
        /* Where a continuing read would start. Updated on hits as well as misses: a run served
         * entirely from cache is still sequential, and forgetting that would drop the stream back
         * to no-read-ahead on the next miss. */
        s->next_seq_lba = lba + need;
        avail = nsec * HYPE_BLK_SECTOR_SIZE - head; /* usable bytes this fill */
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

void hype_iso_stream_invalidate(hype_iso_stream_t *s) {
    if (s != 0) s->next_seq_lba = 0u;
    if (s != 0) {
        s->cache_sectors = 0u;
    }
}

void hype_iso_stream_cache_stats(const hype_iso_stream_t *s, uint64_t *hits, uint64_t *misses) {
    if (s == 0) {
        if (hits != 0) *hits = 0;
        if (misses != 0) *misses = 0;
        return;
    }
    if (hits != 0) *hits = s->cache_hits;
    if (misses != 0) *misses = s->cache_misses;
}
