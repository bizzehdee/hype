#include "fb_blit.h"

uint32_t hype_fb_bytes_per_pixel(hype_fb_pixel_format_t format) {
    switch (format) {
        case HYPE_FB_PIXEL_FORMAT_XRGB8888:
        case HYPE_FB_PIXEL_FORMAT_XBGR8888:
            return 4u;
        case HYPE_FB_PIXEL_FORMAT_RGB565:
            return 2u;
        default:
            return 0u;
    }
}

static void extract_rgb(const uint8_t *pixel, hype_fb_pixel_format_t format, uint8_t *r, uint8_t *g,
                         uint8_t *b) {
    switch (format) {
        case HYPE_FB_PIXEL_FORMAT_XRGB8888:
            *b = pixel[0];
            *g = pixel[1];
            *r = pixel[2];
            return;
        case HYPE_FB_PIXEL_FORMAT_XBGR8888:
            *r = pixel[0];
            *g = pixel[1];
            *b = pixel[2];
            return;
        case HYPE_FB_PIXEL_FORMAT_RGB565: {
            uint16_t value = (uint16_t)(pixel[0] | (pixel[1] << 8));
            uint8_t r5 = (uint8_t)((value >> 11) & 0x1Fu);
            uint8_t g6 = (uint8_t)((value >> 5) & 0x3Fu);
            uint8_t b5 = (uint8_t)(value & 0x1Fu);

            /* Scale up to 8 bits per channel by replicating the high
             * bits into the low bits (the standard, lossless-looking
             * 5/6-bit-to-8-bit expansion; e.g. 5-bit x -> (x<<3)|(x>>2)). */
            *r = (uint8_t)((r5 << 3) | (r5 >> 2));
            *g = (uint8_t)((g6 << 2) | (g6 >> 4));
            *b = (uint8_t)((b5 << 3) | (b5 >> 2));
            return;
        }
    }
}

static void pack_rgb(uint8_t *pixel, hype_fb_pixel_format_t format, uint8_t r, uint8_t g, uint8_t b) {
    switch (format) {
        case HYPE_FB_PIXEL_FORMAT_XRGB8888:
            pixel[0] = b;
            pixel[1] = g;
            pixel[2] = r;
            pixel[3] = 0;
            return;
        case HYPE_FB_PIXEL_FORMAT_XBGR8888:
            pixel[0] = r;
            pixel[1] = g;
            pixel[2] = b;
            pixel[3] = 0;
            return;
        case HYPE_FB_PIXEL_FORMAT_RGB565: {
            uint16_t value = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            pixel[0] = (uint8_t)(value & 0xFFu);
            pixel[1] = (uint8_t)((value >> 8) & 0xFFu);
            return;
        }
    }
}

void hype_fb_blit_copy(const uint8_t *src, uint32_t src_width, uint32_t src_height,
                        uint32_t src_stride_bytes, hype_fb_pixel_format_t src_format,
                        uint8_t *dst, uint32_t dst_width, uint32_t dst_height,
                        uint32_t dst_stride_bytes, hype_fb_pixel_format_t dst_format) {
    uint32_t src_bpp = hype_fb_bytes_per_pixel(src_format);
    uint32_t dst_bpp = hype_fb_bytes_per_pixel(dst_format);
    uint32_t width;
    uint32_t height;
    uint32_t x;
    uint32_t y;

    if (src == 0 || dst == 0 || src_bpp == 0 || dst_bpp == 0 || src_stride_bytes == 0 ||
        dst_stride_bytes == 0) {
        return;
    }

    width = (src_width < dst_width) ? src_width : dst_width;
    height = (src_height < dst_height) ? src_height : dst_height;

    for (y = 0; y < height; y++) {
        const uint8_t *src_row = src + ((uint64_t)y * src_stride_bytes);
        uint8_t *dst_row = dst + ((uint64_t)y * dst_stride_bytes);

        for (x = 0; x < width; x++) {
            uint8_t r, g, b;

            extract_rgb(src_row + ((uint64_t)x * src_bpp), src_format, &r, &g, &b);
            pack_rgb(dst_row + ((uint64_t)x * dst_bpp), dst_format, r, g, b);
        }
    }
}
