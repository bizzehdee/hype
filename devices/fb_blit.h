#ifndef HYPE_DEVICES_FB_BLIT_H
#define HYPE_DEVICES_FB_BLIT_H

#include <stdint.h>

/*
 * VIDEO-3's other half: actually rendering a guest's own Bochs-VBE
 * framebuffer (devices/bochs_vbe.h) onto the host's real GOP screen,
 * which VIDEO-2's own task.md note explicitly reserved for this
 * milestone rather than VIDEO-2 itself.
 *
 * Pure pixel-format conversion + row-copy logic, no hardware access --
 * both the source (guest framebuffer) and destination (host GOP
 * framebuffer, or a plain test buffer) are just plain memory this
 * function is handed pointers to. Clips to the smaller of the two
 * surfaces' width/height rather than requiring an exact match, since a
 * guest-programmed mode and the host's own real display mode have no
 * reason to agree.
 */

typedef enum {
    /* 32bpp, little-endian 0xXXRRGGBB in memory (byte0=B,1=G,2=R,3=X)
     * -- matches Bochs VBE / QEMU bochs-display's own default 32bpp
     * byte order (PIXMAN_LE_x8r8g8b8). */
    HYPE_FB_PIXEL_FORMAT_XRGB8888 = 0,
    /* 32bpp, little-endian 0xXXBBGGRR in memory (byte0=R,1=G,2=B,3=X)
     * -- UEFI GOP's PixelRedGreenBlueReserved8BitPerColor convention. */
    HYPE_FB_PIXEL_FORMAT_XBGR8888,
    /* 16bpp, little-endian, bits 15:11=R(5) 10:5=G(6) 4:0=B(5) --
     * Bochs VBE's other real supported bpp. */
    HYPE_FB_PIXEL_FORMAT_RGB565
} hype_fb_pixel_format_t;

/* Bytes per pixel for a given format (4 for the two 32bpp formats, 2
 * for RGB565). Returns 0 for an unrecognized format. */
uint32_t hype_fb_bytes_per_pixel(hype_fb_pixel_format_t format);

/*
 * Copies pixels from `src` into `dst`, converting between pixel
 * formats as needed, clipped to width = min(src_width, dst_width) and
 * height = min(src_height, dst_height) -- the destination's own
 * unwritten margin (if the source is smaller) is left untouched, not
 * cleared, matching how a real display adapter's own visible area
 * would simply show old content in any region the guest never wrote.
 * A NULL src or dst, or a zero stride/width/height on either side, is
 * treated as "nothing to copy" (a no-op, not an error) -- the caller
 * is expected to have already checked hype_bochs_vbe_get_mode()'s own
 * `valid` flag before calling this.
 */
void hype_fb_blit_copy(const uint8_t *src, uint32_t src_width, uint32_t src_height,
                        uint32_t src_stride_bytes, hype_fb_pixel_format_t src_format,
                        uint8_t *dst, uint32_t dst_width, uint32_t dst_height,
                        uint32_t dst_stride_bytes, hype_fb_pixel_format_t dst_format);

#endif /* HYPE_DEVICES_FB_BLIT_H */
