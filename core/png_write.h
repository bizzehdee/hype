#ifndef HYPE_CORE_PNG_WRITE_H
#define HYPE_CORE_PNG_WRITE_H

#include <stdint.h>

/*
 * TERM-8 (#445): a minimal PNG encoder, from scratch -- hype is freestanding
 * with no libc/zlib/libpng, so there is nothing to wrap.
 *
 * It started out emitting RFC 1951 section 3.2.4 "stored" (uncompressed)
 * blocks -- spec-legal, read correctly by any viewer, and the simplest thing
 * that works. #463 retired that: a 1920x1080 capture came to 6.2 MB, which
 * took 10-15 seconds to write to a real USB stick (freezing the machine, since
 * the write runs on the BSP through the one USB transfer lock the guests
 * stream their media through) and then failed outright, leaving a 0-byte file.
 *
 * It now emits ONE fixed-Huffman block (BTYPE=01) with run-length matches at
 * distance 1. That is a deliberately small amount of machinery -- no hash
 * chains, no dynamic Huffman tables, no match search -- chosen because the
 * thing being compressed is a text console: long runs of identical background
 * pixels, which RLE alone handles about as well as a full LZ77 would.
 *
 * Output is always 8-bit truecolor-without-alpha (PNG color type 2, 3 bytes
 * per pixel, row-major, no filtering beyond the mandatory per-row "None"
 * filter-type byte PNG itself requires). The caller supplies rows already in
 * that format; converting from whatever hype's own framebuffer pixel format
 * is (BGRA and similar) is the caller's concern, not this module's -- keeps
 * the encoder itself decoupled from any one framebuffer's layout.
 */

/*
 * Encodes `width`x`height` of RGB888 pixel data (3 bytes/pixel, `stride_bytes`
 * apart between the start of each row -- may exceed width*3 if the source has
 * padding) into a complete PNG file, written to `out[0..out_cap)`.
 *
 * Returns the number of bytes written on success, or 0 if the encoding would
 * not fit in `out_cap` (nothing is written in that case -- never a partial,
 * truncated file) or any argument is invalid (0 width/height, null pointers).
 */
uint32_t hype_png_write(const uint8_t *rgb, uint32_t width, uint32_t height,
                        uint32_t stride_bytes, uint8_t *out, uint32_t out_cap);

/*
 * An UPPER BOUND on the bytes hype_png_write() needs for a `width`x`height`
 * image, so a caller can size its output buffer safely up front. 0 for
 * invalid dimensions (0 width or height).
 *
 * #463: this was an exact count while the encoder emitted stored blocks. With
 * compression the real size is data-dependent and typically orders of
 * magnitude smaller; the bound is the worst case (every byte a 9-bit literal),
 * which a real capture never reaches but a buffer must still allow for.
 */
uint32_t hype_png_encoded_size(uint32_t width, uint32_t height);

#endif /* HYPE_CORE_PNG_WRITE_H */
