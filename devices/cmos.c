#include "cmos.h"

void hype_cmos_reset(hype_cmos_t *cmos) {
    unsigned int i;

    cmos->index = 0;
    for (i = 0; i < HYPE_CMOS_SIZE; i++) {
        cmos->registers[i] = 0;
    }
}

void hype_cmos_set_extended_memory_above_16mb(hype_cmos_t *cmos, uint16_t size_64kb_units) {
    cmos->registers[HYPE_CMOS_REG_EXTMEM_LOW] = (uint8_t)size_64kb_units;
    cmos->registers[HYPE_CMOS_REG_EXTMEM_HIGH] = (uint8_t)(size_64kb_units >> 8);
}

void hype_cmos_index_write(hype_cmos_t *cmos, uint8_t value) {
    cmos->index = value & (uint8_t)HYPE_CMOS_INDEX_MASK;
}

uint8_t hype_cmos_data_read(const hype_cmos_t *cmos) {
    return cmos->registers[cmos->index];
}

void hype_cmos_data_write(hype_cmos_t *cmos, uint8_t value) {
    cmos->registers[cmos->index] = value;
}
