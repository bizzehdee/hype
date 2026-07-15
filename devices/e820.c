#include "e820.h"

static void put_le64(uint8_t *p, uint64_t v) {
    unsigned int i;
    for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (i * 8));
    }
}

static void put_le32(uint8_t *p, uint32_t v) {
    unsigned int i;
    for (i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (i * 8));
    }
}

int hype_e820_build(uint8_t *out, uint32_t out_cap, const hype_e820_region_t *regions, unsigned int count) {
    uint32_t total;
    unsigned int i;

    if (count == 0) {
        return -1;
    }
    total = count * HYPE_E820_ENTRY_SIZE;
    if (out_cap < total) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        uint8_t *entry = out + (uint32_t)i * HYPE_E820_ENTRY_SIZE;
        put_le64(entry + 0, regions[i].base);
        put_le64(entry + 8, regions[i].length);
        put_le32(entry + 16, regions[i].type);
    }
    return (int)total;
}
