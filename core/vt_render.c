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
        /*
         * #363: and drop any partial-sweep progress. Without this a view switch could
         * resume mid-screen, leaving every row ABOVE the resume point still showing the
         * previous view -- the exact mixing this cache is per-view to prevent. Caught by
         * the test asserting a restart lands 2 rows in rather than wherever the last
         * sweep happened to stop.
         */
        cache->resume_row = 0u;
    }
}

/* Same output as hype_vt_render, but only the cells that changed. */
unsigned hype_vt_render_cached(const hype_vt_screen_t *s, hype_gop_console_t *con, int show_cursor,
                               hype_vt_render_cache_t *cache) {
    return hype_vt_render_cached_bounded(s, con, show_cursor, cache, 0u, 0);
}

unsigned hype_vt_render_cached_bounded(const hype_vt_screen_t *s, hype_gop_console_t *con,
                                       int show_cursor, hype_vt_render_cache_t *cache,
                                       unsigned max_rows_this_call, int *more) {
    unsigned max_cols = (s->cols < con->cols) ? s->cols : con->cols;
    unsigned max_rows = (s->rows < con->rows) ? s->rows : con->rows;
    unsigned drawn = 0;
    int full;

    unsigned start_row;
    unsigned end_row;

    if (more != 0) *more = 0;
    if (cache == 0) {
        hype_vt_render(s, con, show_cursor);
        return max_cols * max_rows;
    }
    if (max_cols > HYPE_VT_MAX_COLS) max_cols = HYPE_VT_MAX_COLS;
    if (max_rows > HYPE_VT_MAX_ROWS) max_rows = HYPE_VT_MAX_ROWS;

    /* A different geometry invalidates every cached cell position. */
    /*
     * Mid-sweep means a previous BOUNDED call stopped part-way with the same geometry.
     * It must be distinguished from an invalid cache, because the first bounded sweep is
     * BOTH: every cell needs drawing (nothing cached yet) AND progress must be kept
     * across calls. Conflating them made `full` force start_row back to 0 every call, so
     * a bounded render redrew the first few rows forever and never finished -- caught by
     * the test that renders to completion and compares against one unbounded pass.
     */
    {
        int mid_sweep = (cache->resume_row != 0u && cache->resume_row < max_rows &&
                         cache->cols == max_cols && cache->rows == max_rows);
        full = (cache->cols != max_cols || cache->rows != max_rows ||
                (!cache->valid && !mid_sweep));
        start_row = mid_sweep ? cache->resume_row : 0u;
    }
    if (max_rows_this_call == 0u || start_row + max_rows_this_call >= max_rows) {
        end_row = max_rows;
    } else {
        end_row = start_row + max_rows_this_call;
    }

    for (unsigned r = start_row; r < end_row; r++) {
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
    if (end_row >= max_rows) {
        /* A sweep finished: the cache now describes the whole screen, so the cursor
         * bookkeeping is safe to update and the next call starts from the top. */
        cache->valid = 1;
        cache->cols = max_cols;
        cache->rows = max_rows;
        cache->cur_col = s->cur_col;
        cache->cur_row = s->cur_row;
        cache->cursor_shown = show_cursor ? 1 : 0;
        cache->resume_row = 0u;
    } else {
        /*
         * Partial sweep. Deliberately do NOT mark the cache valid or move the cursor
         * bookkeeping: the rows below end_row still hold older content, and claiming
         * otherwise is how a partial render turns into a permanently stale region.
         */
        /* Record the geometry now, so the next call recognises this as a resume rather
         * than a fresh (and therefore restarted) sweep. `valid` stays 0: the rows below
         * end_row still hold older content and the cache does not yet describe the
         * whole screen. */
        cache->cols = max_cols;
        cache->rows = max_rows;
        cache->resume_row = end_row;
        if (more != 0) *more = 1;
    }
    return drawn;
}
