#ifndef HYPE_VT_SCREEN_H
#define HYPE_VT_SCREEN_H

#include <stdint.h>

/*
 * TERM-1: a VT/ANSI *interpreter* backed by an in-memory character grid.
 *
 * This is the counterpart to devices/vt_filter.c, which merely *strips*
 * escape sequences to make a serial log legible. Stripping is fine for a
 * scrolling log, but useless for the interactive TUIs we actually need to
 * render -- GRUB's menu, subiquity, anaconda -- because those paint the
 * screen by *positioning the cursor and erasing regions* (CUP, ED, EL),
 * so throwing the escapes away collapses the layout into garbage.
 *
 * So instead of dropping escapes we *apply* them to a cols x rows grid of
 * cells (character + colour/attribute), exactly as a real terminal would.
 * gop_text.c then blits that grid cell-by-cell onto the framebuffer; the
 * grid is the model, the GOP layer is the view. Everything here is pure
 * (no I/O, no framebuffer) so it unit-tests against a plain host struct.
 *
 * Supported: printable text with auto-wrap; CR / LF / BS / TAB / BEL;
 * CSI cursor moves (CUU/CUD/CUF/CUB A-D), absolute position (CUP H/f),
 * column/row absolute (CHA G, VPA d), erase display (ED J) and line
 * (EL K), SGR colours/bold/reverse (m), save/restore cursor (s/u), and
 * ESC c (RIS full reset). DEC private toggles (ESC[?..h/l -- cursor
 * visibility, alternate screen, etc.) and charset selection (ESC ( / )),
 * are recognised and consumed but not acted on, which is the correct
 * "ignore gracefully" behaviour for a text grid.
 */

#define HYPE_VT_MAX_COLS 256u
#define HYPE_VT_MAX_ROWS 144u

/* attr bit layout: fg and bg are 3-bit ANSI colour indices (0-7). */
#define HYPE_VT_ATTR_BOLD    0x01u
#define HYPE_VT_ATTR_REVERSE 0x02u
#define HYPE_VT_ATTR_FG_SHIFT 2u
#define HYPE_VT_ATTR_BG_SHIFT 5u
#define HYPE_VT_ATTR_COLOR_MASK 0x7u
#define HYPE_VT_DEFAULT_FG 7u /* light grey / white */
#define HYPE_VT_DEFAULT_BG 0u /* black */
#define HYPE_VT_DEFAULT_ATTR ((HYPE_VT_DEFAULT_FG << HYPE_VT_ATTR_FG_SHIFT) | \
                              (HYPE_VT_DEFAULT_BG << HYPE_VT_ATTR_BG_SHIFT))

typedef struct {
    uint8_t ch;   /* visible character; ' ' when blank */
    uint8_t attr; /* colour + bold/reverse, packed as above */
} hype_vt_cell_t;

#define HYPE_VT_STATE_NORMAL 0
#define HYPE_VT_STATE_ESC    1 /* saw ESC, awaiting '[' or a two-char final */
#define HYPE_VT_STATE_CSI    2 /* inside ESC[ ..., collecting params */

#define HYPE_VT_MAX_PARAMS 8u

typedef struct {
    unsigned cols;
    unsigned rows;
    unsigned cur_col;
    unsigned cur_row;
    uint8_t cur_attr;    /* pen applied to newly written cells */

    hype_vt_cell_t cells[HYPE_VT_MAX_ROWS][HYPE_VT_MAX_COLS];

    /* parser state */
    int state;
    unsigned params[HYPE_VT_MAX_PARAMS];
    unsigned n_params;
    int have_param;      /* a digit has been seen for the current param */
    int priv;            /* CSI private-mode ('?') flag */

    /* saved cursor (ESC[s / ESC[u) */
    unsigned saved_col;
    unsigned saved_row;

    /* bumped on every mutation, so a renderer can cheaply tell "did
     * anything change since I last drew?" without diffing the grid. */
    unsigned generation;
} hype_vt_screen_t;

/* Initialise to a cleared cols x rows grid (clamped to the MAX_* caps). */
void hype_vt_screen_init(hype_vt_screen_t *s, unsigned cols, unsigned rows);

/* Feed one output byte from the guest console. */
void hype_vt_screen_feed(hype_vt_screen_t *s, uint8_t b);

/* Feed a run of bytes. */
void hype_vt_screen_write(hype_vt_screen_t *s, const uint8_t *buf, unsigned len);

/* Read one cell (returns a blank default-attr cell when out of range). */
hype_vt_cell_t hype_vt_screen_cell(const hype_vt_screen_t *s, unsigned col, unsigned row);

#endif /* HYPE_VT_SCREEN_H */
