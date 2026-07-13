#include <stdio.h>
#include <string.h>
#include "../gop_text.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), (long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define FG 0xFFFFFFu
#define BG 0x000000u

/* 32x16 test framebuffer: 4 cols x 2 rows of 8x8 glyph cells, stride == width. */
#define FB_W 32
#define FB_H 16
static unsigned int g_fb[FB_W * FB_H];

static void reset_fb(hype_gop_console_t *con) {
    memset(g_fb, 0xAA, sizeof(g_fb)); /* sentinel, neither FG nor BG */
    hype_gop_console_init(con, g_fb, FB_W, FB_H, FB_W, FG, BG);
}

static unsigned int px(unsigned int x, unsigned int y) {
    return g_fb[(unsigned long long)y * FB_W + x];
}

static void test_init(void) {
    hype_gop_console_t con;
    reset_fb(&con);
    CHECK_INT("init cols", 4, con.cols);
    CHECK_INT("init rows", 2, con.rows);
    CHECK_INT("init cursor starts at 0,0", 0, con.cursor_col);
    CHECK_INT("init cursor row starts at 0", 0, con.cursor_row);
}

static void test_put_pixel(void) {
    hype_gop_console_t con;
    reset_fb(&con);

    hype_gop_put_pixel(&con, 5, 3, FG);
    CHECK_INT("put_pixel writes the requested pixel", FG, px(5, 3));

    /* Out of bounds: must not crash, must not write anywhere visible. */
    hype_gop_put_pixel(&con, FB_W, 0, FG);
    hype_gop_put_pixel(&con, 0, FB_H, FG);
    CHECK_INT("put_pixel ignores out-of-bounds x", 0xAAAAAAAAu, px(0, 0));
}

static void test_draw_glyph_A(void) {
    hype_gop_console_t con;
    /* hype_font8x8_basic['A'] row 0 is 0x0C (binary 00001100: bits 2,3
     * set, LSB = leftmost column per this project's chosen convention)
     * and row 4 is 0x3F (00111111: bits 0-5 set, 6-7 clear). Checking a
     * handful of specific pixels against these known bytes (rather than
     * the whole 8x8 glyph) catches a wrong bit-extraction order (e.g.
     * MSB-first) without hand-transcribing the entire bitmap. */
    reset_fb(&con);
    hype_gop_draw_glyph(&con, 0, 0, 'A');

    CHECK_INT("'A' row0 col0 is background (bit0 of 0x0C is 0)", BG, px(0, 0));
    CHECK_INT("'A' row0 col2 is foreground (bit2 of 0x0C is 1)", FG, px(2, 0));
    CHECK_INT("'A' row0 col3 is foreground (bit3 of 0x0C is 1)", FG, px(3, 0));
    CHECK_INT("'A' row0 col4 is background (bit4 of 0x0C is 0)", BG, px(4, 0));

    CHECK_INT("'A' row4 col0 is foreground (bit0 of 0x3F is 1)", FG, px(0, 4));
    CHECK_INT("'A' row4 col5 is foreground (bit5 of 0x3F is 1)", FG, px(5, 4));
    CHECK_INT("'A' row4 col6 is background (bit6 of 0x3F is 0)", BG, px(6, 4));
    CHECK_INT("'A' row4 col7 is background (bit7 of 0x3F is 0)", BG, px(7, 4));
}

static void test_draw_glyph_out_of_range_char_is_blank(void) {
    hype_gop_console_t con;
    unsigned int x, y;
    int any_fg = 0;

    reset_fb(&con);
    hype_gop_draw_glyph(&con, 0, 0, (unsigned char)200);

    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            if (px(x, y) == FG) {
                any_fg = 1;
            }
        }
    }
    if (any_fg) {
        printf("FAIL: char >= 128 should draw as fully blank\n");
        failures++;
    }
}

static void test_draw_glyph_at_cell_offset(void) {
    hype_gop_console_t con;
    reset_fb(&con);

    hype_gop_draw_glyph(&con, 1, 0, 'A'); /* second cell, first row */

    CHECK_INT("glyph at col1 lands at pixel x=8+2", FG, px(8 + 2, 0));
    CHECK_INT("glyph at col1 doesn't touch col0's pixels", 0xAAAAAAAAu, px(2, 0));
}

static void test_putc_advances_cursor(void) {
    hype_gop_console_t con;
    reset_fb(&con);

    hype_gop_putc(&con, 'A');
    CHECK_INT("putc advances cursor_col", 1, con.cursor_col);
    CHECK_INT("putc leaves cursor_row alone", 0, con.cursor_row);
}

static void test_putc_wraps_at_line_end(void) {
    hype_gop_console_t con;
    reset_fb(&con);

    hype_gop_putc(&con, 'A');
    hype_gop_putc(&con, 'A');
    hype_gop_putc(&con, 'A');
    hype_gop_putc(&con, 'A'); /* 4th char on a 4-col console: wraps */

    CHECK_INT("putc wraps cursor_col to 0 at line end", 0, con.cursor_col);
    CHECK_INT("putc wrapping advances cursor_row", 1, con.cursor_row);
}

static void test_putc_newline(void) {
    hype_gop_console_t con;
    reset_fb(&con);

    hype_gop_putc(&con, 'A');
    hype_gop_putc(&con, '\n');

    CHECK_INT("\\n resets cursor_col", 0, con.cursor_col);
    CHECK_INT("\\n advances cursor_row", 1, con.cursor_row);
}

static void test_putc_scrolls_when_last_row_wraps(void) {
    hype_gop_console_t con;
    int i;

    reset_fb(&con);
    /* 4 cols x 2 rows = 8 cells; the 9th char wraps past the last row,
     * which must trigger an actual scroll (not just advance cursor_row
     * past con->rows). */
    for (i = 0; i < 8; i++) {
        hype_gop_putc(&con, 'A');
    }
    hype_gop_putc(&con, 'B');

    CHECK_INT("scrolling wrap keeps cursor_row pinned at the last row", 1, con.cursor_row);
    CHECK_INT("scrolling wrap resets cursor_col", 1, con.cursor_col);
}

static void test_scroll(void) {
    hype_gop_console_t con;
    unsigned int x, y;
    int ok = 1;

    reset_fb(&con);
    /* Mark the second glyph row distinctly, then scroll and confirm it
     * moved up to row 0, and the new bottom row is cleared to bg. */
    for (y = 8; y < 16; y++) {
        for (x = 0; x < FB_W; x++) {
            g_fb[(unsigned long long)y * FB_W + x] = FG;
        }
    }

    hype_gop_scroll(&con);

    for (y = 0; y < 8 && ok; y++) {
        for (x = 0; x < FB_W; x++) {
            if (px(x, y) != FG) {
                ok = 0;
                break;
            }
        }
    }
    if (!ok) {
        printf("FAIL: scroll did not move the second row's content up to row 0\n");
        failures++;
    }
    ok = 1;
    for (y = 8; y < 16 && ok; y++) {
        for (x = 0; x < FB_W; x++) {
            if (px(x, y) != BG) {
                ok = 0;
                break;
            }
        }
    }
    if (!ok) {
        printf("FAIL: scroll did not clear the new bottom row to bg\n");
        failures++;
    }
}

static void test_scroll_noop_when_shorter_than_one_glyph_row(void) {
    unsigned int tiny_fb[8 * 4];
    hype_gop_console_t con;

    memset(tiny_fb, 0x55, sizeof(tiny_fb));
    hype_gop_console_init(&con, tiny_fb, 8, 4, 8, FG, BG);

    hype_gop_scroll(&con); /* must not crash / underflow */

    if (tiny_fb[0] != 0x55555555u) {
        printf("FAIL: scroll on a too-short framebuffer should be a no-op\n");
        failures++;
    }
}

static void test_write_and_print(void) {
    hype_gop_console_t con;
    reset_fb(&con);

    hype_gop_write(&con, "AB");
    CHECK_INT("write advances cursor by string length", 2, con.cursor_col);

    reset_fb(&con);
    hype_gop_print(&con, "%s=%d", "x", 7);
    CHECK_INT("print formats then writes (\"x=7\" is 3 chars)", 3, con.cursor_col);
}

int main(void) {
    test_init();
    test_put_pixel();
    test_draw_glyph_A();
    test_draw_glyph_out_of_range_char_is_blank();
    test_draw_glyph_at_cell_offset();
    test_putc_advances_cursor();
    test_putc_wraps_at_line_end();
    test_putc_newline();
    test_putc_scrolls_when_last_row_wraps();
    test_scroll();
    test_scroll_noop_when_shorter_than_one_glyph_row();
    test_write_and_print();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
