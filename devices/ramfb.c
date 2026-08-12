#include "ramfb.h"

static uint32_t read_be32(const uint8_t *src) {
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | (uint32_t)src[3];
}

static uint64_t read_be64(const uint8_t *src) {
    uint64_t value = 0;
    int i;
    for (i = 0; i < 8; i++) {
        value = (value << 8) | (uint64_t)src[i];
    }
    return value;
}

void hype_ramfb_decode_config(const uint8_t buf[HYPE_RAMFB_CONFIG_SIZE], hype_ramfb_config_t *out) {
    out->address = read_be64(buf);
    out->fourcc = read_be32(buf + 8);
    out->flags = read_be32(buf + 12);
    out->width = read_be32(buf + 16);
    out->height = read_be32(buf + 20);
    out->stride = read_be32(buf + 24);
}

int hype_ramfb_frame_size(const hype_ramfb_config_t *cfg, uint64_t *out_size) {
    uint64_t visible_row_bytes;
    uint64_t size;

    if (cfg == 0 || out_size == 0 || cfg->address == 0u || cfg->width == 0u ||
        cfg->height == 0u || cfg->stride == 0u || cfg->fourcc != HYPE_RAMFB_FORMAT_XRGB8888 ||
        cfg->flags != 0u) {
        return -1;
    }
    visible_row_bytes = (uint64_t)cfg->width * 4u;
    if (visible_row_bytes > cfg->stride) {
        return -1;
    }
    size = (uint64_t)cfg->stride * (uint64_t)cfg->height;
    if (size == 0u) {
        return -1;
    }
    *out_size = size;
    return 0;
}
