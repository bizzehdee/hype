#include "vt_render.h"

/* Standard 8-colour ANSI palette as 0x00RRGGBB, plus the bright variants
 * used when the bold attribute is set (the conventional VGA/xterm
 * "bright = bold" mapping). Index order: black, red, green, yellow,
 * blue, magenta, cyan, white. */
static const unsigned int g_pal_normal[8] = {
    0x000000u, 0xAA0000u, 0x00AA00u, 0xAA5500u,
    0x0000AAu, 0xAA00AAu, 0x00AAAAu, 0xAAAAAAu,
};
static const unsigned int g_pal_bright[8] = {
    0x555555u, 0xFF5555u, 0x55FF55u, 0xFFFF55u,
    0x5555FFu, 0xFF55FFu, 0x55FFFFu, 0xFFFFFFu,
};

void hype_vt_render_colors(uint8_t attr, unsigned int *fg, unsigned int *bg) {
    unsigned fg_idx = (attr >> HYPE_VT_ATTR_FG_SHIFT) & HYPE_VT_ATTR_COLOR_MASK;
    unsigned bg_idx = (attr >> HYPE_VT_ATTR_BG_SHIFT) & HYPE_VT_ATTR_COLOR_MASK;
    unsigned int f = (attr & HYPE_VT_ATTR_BOLD) ? g_pal_bright[fg_idx] : g_pal_normal[fg_idx];
    unsigned int b = g_pal_normal[bg_idx];
    if (attr & HYPE_VT_ATTR_REVERSE) {
        unsigned int t = f; f = b; b = t;
    }
    *fg = f;
    *bg = b;
}

void hype_vt_render(const hype_vt_screen_t *s, hype_gop_console_t *con, int show_cursor) {
    unsigned max_cols = (s->cols < con->cols) ? s->cols : con->cols;
    unsigned max_rows = (s->rows < con->rows) ? s->rows : con->rows;

    for (unsigned r = 0; r < max_rows; r++) {
        for (unsigned c = 0; c < max_cols; c++) {
            hype_vt_cell_t cell = hype_vt_screen_cell(s, c, r);
            unsigned int fg, bg;
            hype_vt_render_colors(cell.attr, &fg, &bg);

            /* Block cursor: draw its cell with fg/bg swapped. */
            if (show_cursor && c == s->cur_col && r == s->cur_row) {
                unsigned int t = fg; fg = bg; bg = t;
            }
            con->fg = fg;
            con->bg = bg;
            hype_gop_draw_glyph(con, c, r, (unsigned char)cell.ch);
        }
    }
}

void hype_vt_render_cache_invalidate(hype_vt_render_cache_t *cache) {
    if (cache != 0) {
        cache->valid = 0;
    }
}

/* Same output as hype_vt_render, but only the cells that changed. */
unsigned hype_vt_render_cached(const hype_vt_screen_t *s, hype_gop_console_t *con, int show_cursor,
                               hype_vt_render_cache_t *cache) {
    unsigned max_cols = (s->cols < con->cols) ? s->cols : con->cols;
    unsigned max_rows = (s->rows < con->rows) ? s->rows : con->rows;
    unsigned drawn = 0;
    int full;

    if (cache == 0) {
        hype_vt_render(s, con, show_cursor);
        return max_cols * max_rows;
    }
    if (max_cols > HYPE_VT_MAX_COLS) max_cols = HYPE_VT_MAX_COLS;
    if (max_rows > HYPE_VT_MAX_ROWS) max_rows = HYPE_VT_MAX_ROWS;

    /* A different geometry invalidates every cached cell position. */
    full = (!cache->valid || cache->cols != max_cols || cache->rows != max_rows);

    for (unsigned r = 0; r < max_rows; r++) {
        for (unsigned c = 0; c < max_cols; c++) {
            hype_vt_cell_t cell = hype_vt_screen_cell(s, c, r);
            hype_vt_cell_t was = cache->cells[r][c];
            int is_cursor_now = (show_cursor && c == s->cur_col && r == s->cur_row);
            int was_cursor = (!full && cache->cursor_shown && c == cache->cur_col &&
                             r == cache->cur_row);
            unsigned int fg, bg;

            /* Redraw when the content changed, or when this cell gained or lost
             * the cursor -- the cursor is drawn by swapping fg/bg, so the old
             * position must be repainted even though its character is the same. */
            if (!full && cell.ch == was.ch && cell.attr == was.attr &&
                is_cursor_now == was_cursor) {
                continue;
            }
            hype_vt_render_colors(cell.attr, &fg, &bg);
            if (is_cursor_now) {
                unsigned int t = fg; fg = bg; bg = t;
            }
            con->fg = fg;
            con->bg = bg;
            hype_gop_draw_glyph(con, c, r, (unsigned char)cell.ch);
            cache->cells[r][c] = cell;
            drawn++;
        }
    }
    cache->valid = 1;
    cache->cols = max_cols;
    cache->rows = max_rows;
    cache->cur_col = s->cur_col;
    cache->cur_row = s->cur_row;
    cache->cursor_shown = show_cursor ? 1 : 0;
    return drawn;
}
