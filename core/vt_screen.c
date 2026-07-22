#include "vt_screen.h"

static void touch(hype_vt_screen_t *s) { s->generation++; }

static void blank_cell(hype_vt_screen_t *s, hype_vt_cell_t *c) {
    c->ch = ' ';
    /* Erased cells keep the current background so a reverse-video fill
     * (common in TUI status bars) survives an erase-to-end. */
    c->attr = (uint8_t)((HYPE_VT_DEFAULT_FG << HYPE_VT_ATTR_FG_SHIFT) |
                        (s->cur_attr & (HYPE_VT_ATTR_COLOR_MASK << HYPE_VT_ATTR_BG_SHIFT)));
}

static void clear_all(hype_vt_screen_t *s) {
    for (unsigned r = 0; r < s->rows; r++)
        for (unsigned c = 0; c < s->cols; c++)
            blank_cell(s, &s->cells[r][c]);
}

void hype_vt_screen_init(hype_vt_screen_t *s, unsigned cols, unsigned rows) {
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;
    if (cols > HYPE_VT_MAX_COLS) cols = HYPE_VT_MAX_COLS;
    if (rows > HYPE_VT_MAX_ROWS) rows = HYPE_VT_MAX_ROWS;
    s->cols = cols;
    s->rows = rows;
    s->cur_col = 0;
    s->cur_row = 0;
    s->cur_attr = HYPE_VT_DEFAULT_ATTR;
    s->state = HYPE_VT_STATE_NORMAL;
    s->n_params = 0;
    s->have_param = 0;
    s->priv = 0;
    s->saved_col = 0;
    s->saved_row = 0;
    s->generation = 0;
    clear_all(s);
}

hype_vt_cell_t hype_vt_screen_cell(const hype_vt_screen_t *s, unsigned col, unsigned row) {
    if (col < s->cols && row < s->rows) return s->cells[row][col];
    hype_vt_cell_t blank = { ' ', HYPE_VT_DEFAULT_ATTR };
    return blank;
}

static void scroll_up(hype_vt_screen_t *s) {
    for (unsigned r = 1; r < s->rows; r++)
        for (unsigned c = 0; c < s->cols; c++)
            s->cells[r - 1][c] = s->cells[r][c];
    for (unsigned c = 0; c < s->cols; c++)
        blank_cell(s, &s->cells[s->rows - 1][c]);
}

static void line_feed(hype_vt_screen_t *s) {
    if (s->cur_row + 1 >= s->rows) scroll_up(s);
    else s->cur_row++;
}

static void put_glyph(hype_vt_screen_t *s, uint8_t ch) {
    if (s->cur_col >= s->cols) { /* deferred wrap */
        s->cur_col = 0;
        line_feed(s);
    }
    hype_vt_cell_t *c = &s->cells[s->cur_row][s->cur_col];
    c->ch = ch;
    c->attr = s->cur_attr;
    s->cur_col++;
}

/* --- CSI helpers ------------------------------------------------------ */

/* param i (1-based semantics vary per command); default d when absent/0. */
static unsigned p(const hype_vt_screen_t *s, unsigned i, unsigned d) {
    if (i >= s->n_params) return d;
    unsigned v = s->params[i];
    return (v == 0) ? d : v;
}
/* raw param (0 is meaningful, e.g. ED/EL/SGR selectors). */
static unsigned p0(const hype_vt_screen_t *s, unsigned i) {
    return (i < s->n_params) ? s->params[i] : 0;
}

static void clamp_cursor(hype_vt_screen_t *s) {
    if (s->cur_col >= s->cols) s->cur_col = s->cols - 1;
    if (s->cur_row >= s->rows) s->cur_row = s->rows - 1;
}

static void erase_cells(hype_vt_screen_t *s, unsigned r, unsigned c0, unsigned c1) {
    for (unsigned c = c0; c < c1 && c < s->cols; c++)
        blank_cell(s, &s->cells[r][c]);
}

static void apply_sgr(hype_vt_screen_t *s) {
    if (s->n_params == 0) { s->cur_attr = HYPE_VT_DEFAULT_ATTR; return; }
    for (unsigned i = 0; i < s->n_params; i++) {
        unsigned v = s->params[i];
        if (v == 0) {
            s->cur_attr = HYPE_VT_DEFAULT_ATTR;
        } else if (v == 1) {
            s->cur_attr |= HYPE_VT_ATTR_BOLD;
        } else if (v == 7) {
            s->cur_attr |= HYPE_VT_ATTR_REVERSE;
        } else if (v == 22) {
            s->cur_attr &= (uint8_t)~HYPE_VT_ATTR_BOLD;
        } else if (v == 27) {
            s->cur_attr &= (uint8_t)~HYPE_VT_ATTR_REVERSE;
        } else if (v >= 30 && v <= 37) {
            s->cur_attr = (uint8_t)((s->cur_attr & ~(HYPE_VT_ATTR_COLOR_MASK << HYPE_VT_ATTR_FG_SHIFT)) |
                                    ((v - 30) << HYPE_VT_ATTR_FG_SHIFT));
        } else if (v == 39) {
            s->cur_attr = (uint8_t)((s->cur_attr & ~(HYPE_VT_ATTR_COLOR_MASK << HYPE_VT_ATTR_FG_SHIFT)) |
                                    (HYPE_VT_DEFAULT_FG << HYPE_VT_ATTR_FG_SHIFT));
        } else if (v >= 40 && v <= 47) {
            s->cur_attr = (uint8_t)((s->cur_attr & ~(HYPE_VT_ATTR_COLOR_MASK << HYPE_VT_ATTR_BG_SHIFT)) |
                                    ((v - 40) << HYPE_VT_ATTR_BG_SHIFT));
        } else if (v == 49) {
            s->cur_attr = (uint8_t)((s->cur_attr & ~(HYPE_VT_ATTR_COLOR_MASK << HYPE_VT_ATTR_BG_SHIFT)) |
                                    (HYPE_VT_DEFAULT_BG << HYPE_VT_ATTR_BG_SHIFT));
        }
        /* bright/aixterm (90-97,100-107) and 256/truecolour extensions
         * are collapsed to nearest-basic silently -- unsupported v's are
         * simply ignored, matching a minimal but well-behaved terminal. */
    }
}

static void dispatch_csi(hype_vt_screen_t *s, uint8_t final) {
    switch (final) {
        case 'A': /* CUU: up */
            s->cur_row -= (p(s, 0, 1) > s->cur_row) ? s->cur_row : p(s, 0, 1);
            break;
        case 'B': /* CUD: down */
            s->cur_row += p(s, 0, 1);
            if (s->cur_row >= s->rows) s->cur_row = s->rows - 1;
            break;
        case 'C': /* CUF: right */
            s->cur_col += p(s, 0, 1);
            if (s->cur_col >= s->cols) s->cur_col = s->cols - 1;
            break;
        case 'D': /* CUB: left */
            s->cur_col -= (p(s, 0, 1) > s->cur_col) ? s->cur_col : p(s, 0, 1);
            break;
        case 'G': /* CHA: absolute column (1-based) */
            s->cur_col = p(s, 0, 1) - 1;
            clamp_cursor(s);
            break;
        case 'd': /* VPA: absolute row (1-based) */
            s->cur_row = p(s, 0, 1) - 1;
            clamp_cursor(s);
            break;
        case 'H': /* CUP */
        case 'f': /* HVP (same) */
            s->cur_row = p(s, 0, 1) - 1;
            s->cur_col = p(s, 1, 1) - 1;
            clamp_cursor(s);
            break;
        case 'J': /* ED: erase display */
            switch (p0(s, 0)) {
                case 0: /* cursor to end */
                    erase_cells(s, s->cur_row, s->cur_col, s->cols);
                    for (unsigned r = s->cur_row + 1; r < s->rows; r++)
                        erase_cells(s, r, 0, s->cols);
                    break;
                case 1: /* start to cursor */
                    for (unsigned r = 0; r < s->cur_row; r++)
                        erase_cells(s, r, 0, s->cols);
                    erase_cells(s, s->cur_row, 0, s->cur_col + 1);
                    break;
                case 2: /* whole screen (cursor unchanged, per xterm) */
                default:
                    for (unsigned r = 0; r < s->rows; r++)
                        erase_cells(s, r, 0, s->cols);
                    break;
            }
            break;
        case 'K': /* EL: erase line */
            switch (p0(s, 0)) {
                case 0: erase_cells(s, s->cur_row, s->cur_col, s->cols); break;
                case 1: erase_cells(s, s->cur_row, 0, s->cur_col + 1); break;
                case 2: default: erase_cells(s, s->cur_row, 0, s->cols); break;
            }
            break;
        case 'm': /* SGR */
            apply_sgr(s);
            break;
        case 's': /* save cursor */
            s->saved_col = s->cur_col;
            s->saved_row = s->cur_row;
            break;
        case 'u': /* restore cursor */
            s->cur_col = s->saved_col;
            s->cur_row = s->saved_row;
            clamp_cursor(s);
            break;
        default:
            /* h/l (mode set/reset incl. DEC private), r (scroll region),
             * and anything else we don't model: consumed, no effect. */
            break;
    }
}

/* --- byte feed -------------------------------------------------------- */

static void reset_csi(hype_vt_screen_t *s) {
    s->n_params = 0;
    s->have_param = 0;
    s->priv = 0;
    for (unsigned i = 0; i < HYPE_VT_MAX_PARAMS; i++) s->params[i] = 0;
}

static void feed_normal(hype_vt_screen_t *s, uint8_t b) {
    switch (b) {
        case 0x1B: s->state = HYPE_VT_STATE_ESC; return; /* ESC */
        case '\r': s->cur_col = 0; return;
        case '\n':
        case 0x0B: /* VT */
        case 0x0C: /* FF */
            line_feed(s);
            return;
        case '\b': if (s->cur_col > 0) s->cur_col--; return;
        case '\t': {
            unsigned next = (s->cur_col + 8u) & ~7u;
            s->cur_col = (next >= s->cols) ? s->cols - 1 : next;
            return;
        }
        case 0x07: return; /* BEL */
        default: break;
    }
    if (b >= 0x20u && b < 0x7Fu) put_glyph(s, b);
    /* other control / high bytes: dropped (no charset translation). */
}

void hype_vt_screen_feed(hype_vt_screen_t *s, uint8_t b) {
    switch (s->state) {
        case HYPE_VT_STATE_ESC:
            if (b == (uint8_t)'[') {
                s->state = HYPE_VT_STATE_CSI;
                reset_csi(s);
            } else {
                if (b == (uint8_t)'c') { /* RIS: full reset */
                    hype_vt_screen_init(s, s->cols, s->rows);
                }
                /* ESC ( / ESC ) charset selectors eat one more byte, but
                 * treating that byte as normal text is harmless in practice
                 * (it's 'B'/'0'); simplest correct-enough behaviour is to
                 * just return to NORMAL here. */
                s->state = HYPE_VT_STATE_NORMAL;
            }
            break;

        case HYPE_VT_STATE_CSI:
            if (b == (uint8_t)'?') {
                s->priv = 1;
            } else if (b >= (uint8_t)'0' && b <= (uint8_t)'9') {
                if (s->n_params == 0) s->n_params = 1;
                unsigned idx = s->n_params - 1;
                if (idx < HYPE_VT_MAX_PARAMS) {
                    s->params[idx] = s->params[idx] * 10u + (unsigned)(b - '0');
                    s->have_param = 1;
                }
            } else if (b == (uint8_t)';') {
                if (s->n_params == 0) s->n_params = 1; /* empty leading param */
                if (s->n_params < HYPE_VT_MAX_PARAMS) {
                    s->params[s->n_params] = 0;
                    s->n_params++;
                }
                s->have_param = 0;
            } else if (b >= 0x40u && b <= 0x7Eu) { /* final byte */
                dispatch_csi(s, b);
                s->state = HYPE_VT_STATE_NORMAL;
            }
            /* 0x20-0x2F intermediates: consumed, ignored. */
            break;

        case HYPE_VT_STATE_NORMAL:
        default:
            feed_normal(s, b);
            break;
    }
    touch(s);
}

void hype_vt_screen_write(hype_vt_screen_t *s, const uint8_t *buf, unsigned len) {
    for (unsigned i = 0; i < len; i++) hype_vt_screen_feed(s, buf[i]);
}
