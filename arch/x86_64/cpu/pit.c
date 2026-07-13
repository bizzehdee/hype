#include "pit.h"

uint16_t hype_pit_divisor_for_frequency(uint32_t hz) {
    uint32_t divisor;

    if (hz == 0) {
        return 0;
    }
    divisor = HYPE_PIT_BASE_FREQUENCY_HZ / hz;
    if (divisor == 0 || divisor > 0xFFFFu) {
        return 0;
    }
    return (uint16_t)divisor;
}
