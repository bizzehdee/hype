#ifndef HYPE_CORE_PNG_WRITE_H
#define HYPE_CORE_PNG_WRITE_H

#include <stdint.h>

/*
 * TERM-8 (#445): a minimal PNG encoder, from scratch -- hype is freestanding
 * with no libc/zlib/libpng, so there is nothing to wrap.
 *
 * A valid PNG does not require real DEFLATE compression: RFC 1951 section
 * 3.2.4's "stored" (uncompressed) block type is spec-legal and any PNG
 * viewer/decoder reads it correctly, just larger than a compressed file
 * would be. That is the deliberate tradeoff here -- simplicity over file
 * size, since these are one-off operator screenshots, not a video stream.
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
 * The exact byte count hype_png_write() will need for a `width`x`height`
 * image, so a caller can size its output buffer correctly up front rather
 * than guessing. 0 for invalid dimensions (0 width or height).
 */
uint32_t hype_png_encoded_size(uint32_t width, uint32_t height);

#endif /* HYPE_CORE_PNG_WRITE_H */
