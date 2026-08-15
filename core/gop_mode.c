#include "gop_mode.h"

int hype_gop_mode_find(const hype_gop_mode_t *modes, unsigned int count, uint32_t width,
                       uint32_t height) {
    unsigned int i;
    for (i = 0; i < count; i++) {
        if (modes[i].width == width && modes[i].height == height) {
            return (int)i;
        }
    }
    return -1;
}
