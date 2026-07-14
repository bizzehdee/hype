#include "gop_text.h"
#include "font8x8.h"
#include "format.h"

static unsigned long long pixel_index(const hype_gop_console_t *con, unsigned int x, unsigned int y) {
    return (unsigned long long)y * con->stride + x;
}

void hype_gop_console_init(hype_gop_console_t *con, void *fb, unsigned int width,
                            unsigned int height, unsigned int stride,
                            unsigned int fg, unsigned int bg) {
    con->fb = (unsigned int *)fb;
    con->width = width;
    con->height = height;
    con->stride = stride;
    con->cols = width / HYPE_GOP_GLYPH_W;
    con->rows = height / HYPE_GOP_GLYPH_H;
    con->cursor_col = 0;
    con->cursor_row = 0;
    con->fg = fg;
    con->bg = bg;
}

void hype_gop_put_pixel(hype_gop_console_t *con, unsigned int x, unsigned int y, unsigned int color) {
    if (x >= con->width || y >= con->height) {
        return;
    }
    con->fb[pixel_index(con, x, y)] = color;
}

void hype_gop_draw_glyph(hype_gop_console_t *con, unsigned int col, unsigned int row, unsigned char c) {
    unsigned int base_x = col * HYPE_GOP_GLYPH_W;
    unsigned int base_y = row * HYPE_GOP_GLYPH_H;
    const unsigned char *glyph = (c < 128) ? hype_font8x8_basic[c] : hype_font8x8_basic[0];
    unsigned int gy, gx;

    for (gy = 0; gy < HYPE_GOP_GLYPH_H; gy++) {
        unsigned char bits = glyph[gy];
        for (gx = 0; gx < HYPE_GOP_GLYPH_W; gx++) {
            unsigned int color = ((bits >> gx) & 1u) ? con->fg : con->bg;
            hype_gop_put_pixel(con, base_x + gx, base_y + gy, color);
        }
    }
}

void hype_gop_scroll(hype_gop_console_t *con) {
    unsigned int x, y;

    if (con->height <= HYPE_GOP_GLYPH_H) {
        return;
    }

    for (y = 0; y < con->height - HYPE_GOP_GLYPH_H; y++) {
        unsigned int src_row = y + HYPE_GOP_GLYPH_H;
        for (x = 0; x < con->width; x++) {
            con->fb[pixel_index(con, x, y)] = con->fb[pixel_index(con, x, src_row)];
        }
    }
    for (y = con->height - HYPE_GOP_GLYPH_H; y < con->height; y++) {
        for (x = 0; x < con->width; x++) {
            con->fb[pixel_index(con, x, y)] = con->bg;
        }
    }
}

void hype_gop_console_clear(hype_gop_console_t *con) {
    unsigned int x, y;

    for (y = 0; y < con->height; y++) {
        for (x = 0; x < con->width; x++) {
            con->fb[pixel_index(con, x, y)] = con->bg;
        }
    }
    con->cursor_col = 0;
    con->cursor_row = 0;
}

static void gop_newline(hype_gop_console_t *con) {
    con->cursor_col = 0;
    if (con->cursor_row + 1 >= con->rows) {
        hype_gop_scroll(con);
    } else {
        con->cursor_row++;
    }
}

void hype_gop_putc(hype_gop_console_t *con, char c) {
    if (c == '\n') {
        gop_newline(con);
        return;
    }

    hype_gop_draw_glyph(con, con->cursor_col, con->cursor_row, (unsigned char)c);
    con->cursor_col++;
    if (con->cursor_col >= con->cols) {
        gop_newline(con);
    }
}

void hype_gop_write(hype_gop_console_t *con, const char *s) {
    while (*s) {
        hype_gop_putc(con, *s);
        s++;
    }
}

void hype_gop_vprint(hype_gop_console_t *con, const char *fmt, va_list ap) {
    char buf[256];

    hype_vsnprintf(buf, sizeof(buf), fmt, ap);
    hype_gop_write(con, buf);
}

void hype_gop_print(hype_gop_console_t *con, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    hype_gop_vprint(con, fmt, ap);
    va_end(ap);
}
