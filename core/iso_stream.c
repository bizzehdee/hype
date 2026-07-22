#include "iso_stream.h"

/* Bounce buffer for one disk read: covering sectors land here, then the exact
 * requested slice is copied out. 64 KiB = 128 sectors -- well within one PRDT
 * entry (hype_ahci_host_read's 4 MiB cap), so each fill is a single command.
 * Single-threaded use (the guest-exit path), like the other device state. */
#define BOUNCE_SECTORS 128u
#define BOUNCE_BYTES (BOUNCE_SECTORS * HYPE_ISO_STREAM_SECTOR)
static uint8_t g_bounce[BOUNCE_BYTES] __attribute__((aligned(4096)));

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
        uint64_t lba = s->part_start_lba + cur / HYPE_ISO_STREAM_SECTOR;
        uint32_t head = (uint32_t)(cur % HYPE_ISO_STREAM_SECTOR); /* offset into the first sector */
        uint32_t want = head + remaining;                        /* bytes from `lba` we still need */
        uint32_t nsec = (want + HYPE_ISO_STREAM_SECTOR - 1u) / HYPE_ISO_STREAM_SECTOR;
        uint32_t avail;
        uint32_t n;
        uint32_t i;

        if (nsec > BOUNCE_SECTORS) {
            nsec = BOUNCE_SECTORS;
        }
        if (s->read(s->ctx, lba, nsec, g_bounce) != 0) {
            return -1;
        }
        avail = nsec * HYPE_ISO_STREAM_SECTOR - head; /* usable bytes this fill */
        n = (avail < remaining) ? avail : remaining;
        for (i = 0; i < n; i++) {
            dst[i] = g_bounce[head + i];
        }
        dst += n;
        cur += n;
        remaining -= n;
    }
    return 0;
}
