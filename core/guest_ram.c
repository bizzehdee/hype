#include "guest_ram.h"

void hype_guest_ram_zero(void *base, uint64_t size) {
    uint8_t *bytes = (uint8_t *)base;

    for (uint64_t i = 0; i < size; i++) {
        bytes[i] = 0;
    }
}
