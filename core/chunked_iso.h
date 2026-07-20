#ifndef HYPE_CORE_CHUNKED_ISO_H
#define HYPE_CORE_CHUNKED_ISO_H

#include <stdint.h>

/*
 * GLADDER-10(a): a multi-GB ISO backed by SEVERAL non-contiguous host buffers
 * instead of one giant contiguous allocation. hype loads an installer ISO into
 * host RAM pre-ExitBootServices (ISO-1) and the emulated ATAPI CD serves the
 * guest's reads from it (ISO-2). A single AllocatePages of a whole server ISO
 * (Fedora ~3.64GB, Ubuntu ~2.72GB) fails -- no contiguous region that large is
 * available in the UEFI memory map (OUT_OF_RESOURCES). Splitting the ISO into
 * N fixed-size chunks, each its own allocation, removes the contiguity
 * requirement while staying entirely RAM-resident (no disk driver needed --
 * booting a live ISO never touches a real disk; only an install would).
 *
 * This module is pure address math + copy: `chunk_base[i]` are host-physical
 * addresses (identity-mapped, so usable as pointers), the logical ISO is the
 * chunks concatenated in order. Unit-testable with ordinary malloc'd buffers.
 */

#define HYPE_ISO_MAX_CHUNKS 48u /* 48 * 256MB = 12GB max ISO */

typedef struct {
    uint64_t chunk_base[HYPE_ISO_MAX_CHUNKS]; /* host-physical base of each chunk */
    uint64_t chunk_bytes;                     /* bytes per chunk (uniform; last chunk may hold < this many valid bytes) */
    uint64_t total_bytes;                     /* logical ISO size */
    unsigned n_chunks;                        /* number of chunks in use */
} hype_chunked_iso_t;

/*
 * Copies `len` bytes from logical ISO offset `off` into `dst`, walking chunk
 * boundaries as needed. Returns 0 on success, -1 if the range is out of bounds
 * (off+len > total_bytes, or an overflow), or if the layout is malformed
 * (chunk_bytes == 0). `dst` must hold `len` bytes.
 */
int hype_chunked_iso_read(const hype_chunked_iso_t *iso, uint64_t off, uint8_t *dst, uint64_t len);

#endif /* HYPE_CORE_CHUNKED_ISO_H */
