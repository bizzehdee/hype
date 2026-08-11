#ifndef HYPE_CORE_LEBYTES_H
#define HYPE_CORE_LEBYTES_H

#include <stdint.h>

/*
 * #292: unaligned little-endian byte accessors, unified from six private
 * copies (fat.c, fat_write.c, fat_exfat.c, fat_exfat_fs.c, ext.c, gpt.c).
 * Every on-disk format hype parses is little-endian, and none of it is
 * naturally aligned, so byte-at-a-time composition is the only portable
 * read -- a cast-and-load would fault on strict alignment and is UB anyway.
 */

static inline uint16_t hype_rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t hype_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t hype_rd64(const uint8_t *p) {
    return (uint64_t)hype_rd32(p) | ((uint64_t)hype_rd32(p + 4) << 32);
}

static inline void hype_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void hype_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline void hype_wr64(uint8_t *p, uint64_t v) {
    hype_wr32(p, (uint32_t)v);
    hype_wr32(p + 4, (uint32_t)(v >> 32));
}

#endif /* HYPE_CORE_LEBYTES_H */
