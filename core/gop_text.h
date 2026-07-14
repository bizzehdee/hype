#ifndef HYPE_GOP_TEXT_H
#define HYPE_GOP_TEXT_H

#include <stdarg.h>

/*
 * GOP linear-framebuffer text renderer (M1-6): draws hype_font8x8_basic
 * glyphs directly into a 32-bits-per-pixel linear framebuffer.
 *
 * Unlike console.c (ConOut function pointer) or serial.c (raw port
 * I/O), there is nothing hardware-exempt to split out here at all: a
 * linear framebuffer is just memory the caller already has a pointer
 * to, so every function below is plain pointer arithmetic and writes,
 * fully unit-testable against an ordinary host-allocated buffer with no
 * mocking needed. Only *finding* the real framebuffer (gop.h's
 * LocateProtocol call) touches UEFI.
 */

typedef struct {
    unsigned int *fb;
    unsigned int width;         /* visible pixels per row */
    unsigned int height;        /* visible rows */
    unsigned int stride;        /* pixels per scan line in memory (>= width) */
    unsigned int cols;          /* width / 8 */
    unsigned int rows;          /* height / 8 */
    unsigned int cursor_col;
    unsigned int cursor_row;
    unsigned int fg;
    unsigned int bg;
} hype_gop_console_t;

#define HYPE_GOP_GLYPH_W 8
#define HYPE_GOP_GLYPH_H 8

void hype_gop_console_init(hype_gop_console_t *con, void *fb, unsigned int width,
                            unsigned int height, unsigned int stride,
                            unsigned int fg, unsigned int bg);

/* Bounds-checked (silently drops out-of-range writes, same convention
 * as clamped/rejected-not-crashed elsewhere in this codebase). */
void hype_gop_put_pixel(hype_gop_console_t *con, unsigned int x, unsigned int y, unsigned int color);

/* Draws one 8x8 glyph at the given *cell* (not pixel) coordinates.
 * Characters >= 128 (outside hype_font8x8_basic's range) draw as blank. */
void hype_gop_draw_glyph(hype_gop_console_t *con, unsigned int col, unsigned int row, unsigned char c);

/* Shifts the whole framebuffer up by one glyph row, clearing the new
 * bottom row to bg. No-op if the framebuffer is shorter than one glyph
 * row. */
void hype_gop_scroll(hype_gop_console_t *con);

/* Fills every visible pixel to bg and resets the cursor to (0,0) --
 * firmware's own pre-ExitBootServices console output (or a prior
 * console's leftover text) is still sitting in the same linear
 * framebuffer otherwise, since nothing before this ever clears it. */
void hype_gop_console_clear(hype_gop_console_t *con);

/* Writes one character: '\n' moves to the next line (scrolling if
 * already on the last row); anything else draws a glyph and advances
 * the cursor, wrapping to the next line at the right edge. */
void hype_gop_putc(hype_gop_console_t *con, char c);

void hype_gop_write(hype_gop_console_t *con, const char *s);
void hype_gop_vprint(hype_gop_console_t *con, const char *fmt, va_list ap);
void hype_gop_print(hype_gop_console_t *con, const char *fmt, ...);

#endif /* HYPE_GOP_TEXT_H */
