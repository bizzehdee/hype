#include "chunked_iso.h"

/* Copy n bytes 8 at a time, byte tail last. __builtin_memcpy with a constant
 * size lowers to a single unaligned mov (x86_64 allows unaligned access) -- no
 * libc/memcpy dependency (this is a freestanding build with no libc) and no
 * strict-aliasing UB from a uint64_t* cast. ~8x fewer store ops than the old
 * byte loop for the large PRDT copies a squashfs/installer read drives. */
static void copy_fast(uint8_t *dst, const uint8_t *src, uint64_t n) {
    uint64_t k = 0;
    while (k + 8u <= n) {
        __builtin_memcpy(dst + k, src + k, 8);
        k += 8u;
    }
    while (k < n) {
        dst[k] = src[k];
        k++;
    }
}

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

        if (ci >= iso->n_chunks) {
            return -1; /* defensive: layout says fewer chunks than the offset implies */
        }
        src = (const uint8_t *)(uintptr_t)iso->chunk_base[ci] + within;
        copy_fast(dst + done, src, n);
        done += n;
    }
    return 0;
}
