#include "guest_ram.h"

void hype_guest_ram_zero(void *base, uint64_t size) {
    uint8_t *bytes = (uint8_t *)base;

    for (uint64_t i = 0; i < size; i++) {
        bytes[i] = 0;
    }
}

void hype_guest_ram_copy(void *dst, const void *src, uint64_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    for (uint64_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}
