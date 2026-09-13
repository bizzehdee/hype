#include "stack_watermark.h"

void hype_stack_paint(uint8_t *base, uint64_t len) {
    uint64_t i;
    if (base == 0) {
        return;
    }
    for (i = 0; i < len; i++) {
        base[i] = (uint8_t)HYPE_STACK_PAINT_BYTE;
    }
}

uint64_t hype_stack_used(const uint8_t *base, uint64_t len) {
    uint64_t i;
    if (base == 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (base[i] != (uint8_t)HYPE_STACK_PAINT_BYTE) {
            return len - i;
        }
    }
    return 0;
}

int hype_stack_guard_hit(uint64_t used, uint64_t len) {
    if (len <= HYPE_STACK_GUARD_BYTES) {
        return used != 0u;
    }
    return used > len - HYPE_STACK_GUARD_BYTES;
}

int hype_stack_guard_disturbed(const uint8_t *base, uint64_t len) {
    uint64_t n, i;
    if (base == 0) {
        return 0;
    }
    n = (len < HYPE_STACK_GUARD_BYTES) ? len : HYPE_STACK_GUARD_BYTES;
    for (i = 0; i < n; i++) {
        if (base[i] != (uint8_t)HYPE_STACK_PAINT_BYTE) {
            return 1;
        }
    }
    return 0;
}
