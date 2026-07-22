#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../vt_render.h"

static int failures = 0;

#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)
#define CHECK_HEX(desc, expected, actual) \
    do { unsigned long long e=(unsigned long long)(expected), a=(unsigned long long)(actual); \
        if (e != a) { printf("FAIL: %s: expected 0x%llx, got 0x%llx\n",(desc),e,a); failures++; } } while (0)

/* pixel at (x,y) in a stride==width framebuffer */
static unsigned int px(const unsigned int *fb, unsigned w, unsigned x, unsigned y) {
    return fb[(size_t)y * w + x];
}

/* Does cell (col,row) contain ANY foreground pixel of colour `fg`? (i.e. a
 * non-blank glyph was drawn in that colour) */
static int cell_has_fg(const unsigned int *fb, unsigned w, unsigned col, unsigned row, unsigned int fg) {
    for (unsigned gy = 0; gy < 8; gy++)
        for (unsigned gx = 0; gx < 8; gx++)
            if (px(fb, w, col * 8 + gx, row * 8 + gy) == fg) return 1;
    return 0;
}

int main(void) {
    /* --- colour resolution --- */
    unsigned int fg, bg;
    hype_vt_render_colors(HYPE_VT_DEFAULT_ATTR, &fg, &bg);
    CHECK_HEX("default fg = grey", 0xAAAAAAu, fg);
    CHECK_HEX("default bg = black", 0x000000u, bg);

    /* fg red (idx1), no bold -> normal red */
    hype_vt_render_colors((uint8_t)(1u << HYPE_VT_ATTR_FG_SHIFT), &fg, &bg);
    CHECK_HEX("fg red normal", 0xAA0000u, fg);

    /* bold red -> bright red */
    hype_vt_render_colors((uint8_t)((1u << HYPE_VT_ATTR_FG_SHIFT) | HYPE_VT_ATTR_BOLD), &fg, &bg);
    CHECK_HEX("fg red bold -> bright", 0xFF5555u, fg);

    /* bg blue (idx4) */
    hype_vt_render_colors((uint8_t)(4u << HYPE_VT_ATTR_BG_SHIFT), &fg, &bg);
    CHECK_HEX("bg blue", 0x0000AAu, bg);

    /* reverse swaps fg/bg: fg=white bg=black reversed -> fg black bg white */
    hype_vt_render_colors((uint8_t)(HYPE_VT_DEFAULT_ATTR | HYPE_VT_ATTR_REVERSE), &fg, &bg);
    CHECK_HEX("reverse: fg becomes bg", 0x000000u, fg);
    CHECK_HEX("reverse: bg becomes fg", 0xAAAAAAu, bg);

    /* --- full render onto a framebuffer --- */
    unsigned W = 80 * 8, H = 25 * 8;
    unsigned int *fb = calloc((size_t)W * H, sizeof(unsigned int));
    hype_gop_console_t con;
    hype_gop_console_init(&con, fb, W, H, W, 0xAAAAAAu, 0x000000u);

    hype_vt_screen_t *s = malloc(sizeof(*s));
    hype_vt_screen_init(s, 80, 25);
    hype_vt_screen_write(s, (const uint8_t *)"\x1b[31mA", 6); /* red 'A' at (0,0) */
    hype_vt_screen_write(s, (const uint8_t *)"\x1b[0m\r\nB", 7); /* default 'B' at (0,1) */

    hype_vt_render(s, &con, 0 /* no cursor */);

    CHECK("red 'A' drawn at cell(0,0)", cell_has_fg(fb, W, 0, 0, 0xAA0000u));
    CHECK("grey 'B' drawn at cell(0,1)", cell_has_fg(fb, W, 0, 1, 0xAAAAAAu));
    /* an untouched cell is pure background (no fg pixels of any glyph colour) */
    CHECK("blank cell has no red", !cell_has_fg(fb, W, 40, 12, 0xAA0000u));

    /* --- block cursor: at cursor cell, colours are swapped --- */
    hype_vt_screen_init(s, 80, 25);
    hype_vt_screen_write(s, (const uint8_t *)"X", 1); /* 'X' at (0,0); cursor now (1,0) */
    memset(fb, 0, (size_t)W * H * sizeof(unsigned int));
    hype_vt_render(s, &con, 1 /* show cursor */);
    /* cursor is at (1,0), an empty cell: swapped -> bg becomes grey, so the
     * whole cell background is grey (block). Check a background pixel there. */
    CHECK_HEX("cursor cell bg is grey block", 0xAAAAAAu, px(fb, W, 1 * 8 + 0, 0 * 8 + 0));
    /* the 'X' cell (0,0) is NOT the cursor -> normal grey-on-black glyph */
    CHECK("X still drawn normally", cell_has_fg(fb, W, 0, 0, 0xAAAAAAu));

    /* --- clipping: a grid larger than the console draws no out-of-range pixels --- */
    {
        unsigned sw = 4 * 8, sh = 2 * 8;
        unsigned int *small = calloc((size_t)sw * sh, sizeof(unsigned int));
        hype_gop_console_t scon;
        hype_gop_console_init(&scon, small, sw, sh, sw, 0xAAAAAAu, 0x000000u);
        hype_vt_screen_t *big = malloc(sizeof(*big));
        hype_vt_screen_init(big, 80, 25);
        hype_vt_screen_write(big, (const uint8_t *)"hello", 5);
        hype_vt_render(big, &scon, 0); /* must not write past small's 4x2 cells */
        /* only cols 0..3 rows 0..1 exist; 'h''e''l''l' fit, 'o' is clipped.
         * Sanity: something got drawn in cell(0,0). */
        CHECK("clipped render drew visible cell(0,0)", cell_has_fg(small, sw, 0, 0, 0xAAAAAAu));
        free(small); free(big);
    }

    /* --- grid smaller than the console: max_* takes the grid dims (True side
     *     of both clamp ternaries), no draw past the grid's own extent --- */
    {
        hype_vt_screen_t *sm = malloc(sizeof(*sm));
        hype_vt_screen_init(sm, 40, 10);            /* < the 80x25 console */
        hype_vt_screen_write(sm, (const uint8_t *)"Z", 1);
        memset(fb, 0, (size_t)W * H * sizeof(unsigned int));
        hype_vt_render(sm, &con, 0);
        CHECK("small grid: Z drawn at (0,0)", cell_has_fg(fb, W, 0, 0, 0xAAAAAAu));
        /* cell (50,20) is outside the 40x10 grid -> never visited -> stays 0 */
        CHECK_HEX("beyond-grid cell untouched", 0x0u, px(fb, W, 50 * 8, 20 * 8));
        free(sm);
    }

    free(fb); free(s);
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
