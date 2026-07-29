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

    /* --- PERF-2 (#234) part 2: the DIFFING renderer -------------------- */
    {
        unsigned W2 = 80 * 8, H2 = 25 * 8;
        unsigned int *fb2 = calloc((size_t)W2 * H2, sizeof(unsigned int));
        hype_gop_console_t c2;
        hype_vt_render_cache_t *cache = malloc(sizeof(*cache));
        hype_vt_screen_t *s2 = malloc(sizeof(*s2));
        unsigned n;

        hype_gop_console_init(&c2, fb2, W2, H2, W2, 0xAAAAAAu, 0x000000u);
        hype_vt_screen_init(s2, 80, 25);
        hype_vt_render_cache_invalidate(cache);

        /* First render draws every cell (nothing is cached yet). */
        n = hype_vt_render_cached(s2, &c2, 0, cache);
        CHECK_HEX("first cached render draws the whole screen", 80u * 25u, n);

        /* An identical second render draws NOTHING -- the whole point: no
         * dirty rows means core/gop.c pushes no pixels to VRAM at all. */
        n = hype_vt_render_cached(s2, &c2, 0, cache);
        CHECK_HEX("unchanged screen draws 0 cells", 0u, n);

        /* One new character redraws exactly one cell. */
        hype_vt_screen_write(s2, (const uint8_t *)"X", 1);
        n = hype_vt_render_cached(s2, &c2, 0, cache);
        CHECK_HEX("one changed cell draws 1", 1u, n);
        CHECK("the changed cell was actually painted", cell_has_fg(fb2, W2, 0, 0, 0xAAAAAAu));

        /* ... and its content is now cached, so a repeat draws nothing. */
        CHECK_HEX("repeat after a change draws 0", 0u, hype_vt_render_cached(s2, &c2, 0, cache));

        /* Turning the cursor ON repaints just the cursor cell. */
        n = hype_vt_render_cached(s2, &c2, 1, cache);
        CHECK_HEX("cursor appearing draws 1 cell", 1u, n);
        CHECK_HEX("cursor steady draws 0", 0u, hype_vt_render_cached(s2, &c2, 1, cache));

        /* Moving the cursor must repaint BOTH the old and the new cell --
         * the cursor is a colour swap, so the vacated cell needs restoring. */
        hype_vt_screen_write(s2, (const uint8_t *)"Y", 1); /* advances the cursor */
        n = hype_vt_render_cached(s2, &c2, 1, cache);
        CHECK("cursor move + new char repaints at least 2 cells", n >= 2u);
        CHECK_HEX("then steady again", 0u, hype_vt_render_cached(s2, &c2, 1, cache));

        /* Explicit invalidation forces a full repaint (what a view switch does). */
        hype_vt_render_cache_invalidate(cache);
        CHECK_HEX("invalidate forces a full redraw", 80u * 25u,
                  hype_vt_render_cached(s2, &c2, 1, cache));

        /* A geometry change also forces a full redraw rather than diffing
         * against cells that no longer mean the same position. */
        {
            unsigned sw = 40 * 8, sh = 10 * 8;
            unsigned int *small = calloc((size_t)sw * sh, sizeof(unsigned int));
            hype_gop_console_t sc;
            hype_gop_console_init(&sc, small, sw, sh, sw, 0xAAAAAAu, 0x000000u);
            n = hype_vt_render_cached(s2, &sc, 0, cache);
            CHECK_HEX("smaller console redraws all of its own cells", 40u * 10u, n);
            CHECK_HEX("and is then steady", 0u, hype_vt_render_cached(s2, &sc, 0, cache));
            free(small);
        }

        /* A NULL cache degrades to the plain full renderer (no crash). */
        CHECK_HEX("NULL cache renders everything", 80u * 25u,
                  hype_vt_render_cached(s2, &c2, 0, 0));

        /* The diffing renderer's OUTPUT must match the plain one byte-for-byte
         * -- an optimisation that changes what is on screen is a bug. */
        {
            unsigned int *fb_ref = calloc((size_t)W2 * H2, sizeof(unsigned int));
            unsigned int *fb_dif = calloc((size_t)W2 * H2, sizeof(unsigned int));
            hype_gop_console_t cref, cdif;
            hype_vt_render_cache_t *c3 = malloc(sizeof(*c3));
            hype_vt_screen_t *s3 = malloc(sizeof(*s3));
            unsigned step;

            hype_gop_console_init(&cref, fb_ref, W2, H2, W2, 0xAAAAAAu, 0x000000u);
            hype_gop_console_init(&cdif, fb_dif, W2, H2, W2, 0xAAAAAAu, 0x000000u);
            hype_vt_screen_init(s3, 80, 25);
            hype_vt_render_cache_invalidate(c3);
            /* Drive a realistic sequence: text, colours, newlines, scrolling. */
            for (step = 0; step < 60u; step++) {
                char line[64];
                int len = snprintf(line, sizeof line,
                                   "\x1b[3%um step %u: booting the guest\r\n",
                                   (unsigned)(step % 8u), step);
                hype_vt_screen_write(s3, (const uint8_t *)line, (unsigned)len);
                hype_vt_render(s3, &cref, 1);
                hype_vt_render_cached(s3, &cdif, 1, c3);
                if (memcmp(fb_ref, fb_dif, (size_t)W2 * H2 * sizeof(unsigned int)) != 0) {
                    CHECK("diffing output matches the plain renderer", 0);
                    break;
                }
            }
            if (step == 60u) {
                CHECK("diffing output matches the plain renderer over 60 frames", 1);
            }
            free(fb_ref); free(fb_dif); free(c3); free(s3);
        }
        free(fb2); free(cache); free(s2);
    }

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
