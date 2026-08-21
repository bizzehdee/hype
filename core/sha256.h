#ifndef HYPE_CORE_SHA256_H
#define HYPE_CORE_SHA256_H

#include <stdint.h>

/*
 * #433: SHA-256 (FIPS 180-4), freestanding, for the guest TPM's PCR bank. Nothing here is
 * hype-specific -- it exists because the TPM's PCR Extend IS a SHA-256 chain and there was no
 * digest in the tree. Streaming interface so a caller never buffers what it measures. Pure,
 * fully unit-tested against the FIPS vectors.
 */

#define HYPE_SHA256_DIGEST_SIZE 32u

typedef struct {
    uint32_t h[8];
    uint64_t total_len;
    uint8_t block[64];
    unsigned int block_len;
} hype_sha256_t;

void hype_sha256_init(hype_sha256_t *s);
void hype_sha256_update(hype_sha256_t *s, const void *data, unsigned long len);
void hype_sha256_final(hype_sha256_t *s, uint8_t digest[HYPE_SHA256_DIGEST_SIZE]);

/* One-shot convenience. */
void hype_sha256(const void *data, unsigned long len, uint8_t digest[HYPE_SHA256_DIGEST_SIZE]);

#endif /* HYPE_CORE_SHA256_H */
