#include "ext_csum.h"

/*
 * Reflected, table-free bit-at-a-time CRC-32C (Castagnoli, poly 0x1EDC6F41,
 * reflected 0x82F63B78) -- the RAW update, no pre/post complement (see the
 * header). A one-shot, "textbook" CRC-32C of a single buffer is
 * ~hype_ext_crc32c(~0u, buf, len).
 */
uint32_t hype_ext_crc32c(uint32_t seed, const void *data, unsigned int len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = seed;
    unsigned int i, j;
    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (j = 0; j < 8u; j++) {
            uint32_t mask = (uint32_t) - (int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return crc;
}

/*
 * Reflected, table-free bit-at-a-time CRC-16 (poly 0x8005, reflected
 * 0xA001) -- the classic "crc16" GDT_CSUM uses. Same RAW, no-complement
 * convention as hype_ext_crc32c above.
 */
uint16_t hype_ext_crc16(uint16_t seed, const void *data, unsigned int len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = seed;
    unsigned int i, j;
    for (i = 0; i < len; i++) {
        crc = (crc ^ p[i]) & 0xFFFFu;
        for (j = 0; j < 8u; j++) {
            uint32_t mask = (uint32_t) - (int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xA001u & mask);
        }
    }
    return (uint16_t)crc;
}
