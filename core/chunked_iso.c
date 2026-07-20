#include "chunked_iso.h"

int hype_chunked_iso_read(const hype_chunked_iso_t *iso, uint64_t off, uint8_t *dst, uint64_t len) {
    uint64_t done = 0;

    if (iso == 0 || iso->chunk_bytes == 0) {
        return -1;
    }
    /* Bounds + overflow: off + len must not exceed total_bytes and must not wrap. */
    if (off > iso->total_bytes || len > iso->total_bytes - off) {
        return -1;
    }

    while (done < len) {
        uint64_t cur = off + done;
        unsigned ci = (unsigned)(cur / iso->chunk_bytes);
        uint64_t within = cur % iso->chunk_bytes;
        uint64_t avail = iso->chunk_bytes - within; /* bytes left in this chunk */
        uint64_t n = (len - done < avail) ? (len - done) : avail;
        const uint8_t *src;
        uint64_t j;

        if (ci >= iso->n_chunks) {
            return -1; /* defensive: layout says fewer chunks than the offset implies */
        }
        src = (const uint8_t *)(uintptr_t)iso->chunk_base[ci] + within;
        for (j = 0; j < n; j++) {
            dst[done + j] = src[j];
        }
        done += n;
    }
    return 0;
}
