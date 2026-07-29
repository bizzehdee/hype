#ifndef HYPE_VT_RENDER_H
#define HYPE_VT_RENDER_H

#include "vt_screen.h"
#include "gop_text.h"

/*
 * TERM-1 (view half): blit a hype_vt_screen_t character grid onto a GOP
 * framebuffer. vt_screen.c is the model (what the terminal *contains*);
 * this is the view (how it *looks* on the real screen). It reuses
 * gop_text.c's glyph primitive -- for each cell it resolves the cell's
 * packed attribute to concrete fg/bg pixel colours (an 8-colour ANSI
 * palette, bold -> the bright variant, reverse -> swap fg/bg), sets the
 * console pen, and draws the glyph. Nothing here touches UEFI: the
 * console already owns the framebuffer pointer, so this is pure pixel
 * writes and unit-tests against a host-allocated buffer.
 */

/* Resolve a packed vt cell attribute to fg/bg 0x00RRGGBB pixel values. */
void hype_vt_render_colors(uint8_t attr, unsigned int *fg, unsigned int *bg);

/*
 * Draw the whole grid. Cells beyond the console's own cols/rows are
 * clipped. When show_cursor is nonzero and the cursor is on-screen, that
 * cell is drawn with fg/bg swapped (a block cursor).
 */
void hype_vt_render(const hype_vt_screen_t *s, hype_gop_console_t *con, int show_cursor);

/*
 * PERF-2 (#234) part 2: a DIFFING renderer.
 *
 * hype_vt_render above redraws every cell on every call, which marks the whole
 * console dirty, which makes core/gop.c blit the ENTIRE framebuffer -- 8 MB to
 * uncached/WC VRAM -- even when one character changed. Measured on the AMD
 * laptop that dominated the VM-exit loop at 56-140 ms per exit, so OVMF's DXE
 * never reached the installer (#228).
 *
 * hype_vt_render_cached() draws only the cells that DIFFER from the previous
 * frame, so gop.c's dirty range shrinks to the rows that actually changed and
 * the blit shrinks with it. During OVMF DXE, where the screen is mostly static,
 * that is the difference between a full-frame push per exit and nothing at all.
 *
 * The cache is caller-owned rather than a file-global: two VMs' terminals plus
 * the dashboard share one console, and a global would happily diff one view
 * against another's leftovers and paint a mix of the two. Give each view its
 * own cache, or call hype_vt_render_cache_invalidate() when switching.
 *
 * Returns the number of cells actually drawn -- 0 means the screen was
 * identical and nothing was pushed, which is the common case and is worth
 * logging when diagnosing render cost.
 */
typedef struct {
    int valid;         /* 0 = next render draws everything */
    unsigned cols;     /* dimensions the cache was filled at; a change forces a
                        * full redraw, so a resize cannot leave stale cells */
    unsigned rows;
    unsigned cur_col;  /* where the cursor was drawn last frame */
    unsigned cur_row;
    int cursor_shown;
    hype_vt_cell_t cells[HYPE_VT_MAX_ROWS][HYPE_VT_MAX_COLS];
} hype_vt_render_cache_t;

void hype_vt_render_cache_invalidate(hype_vt_render_cache_t *cache);

unsigned hype_vt_render_cached(const hype_vt_screen_t *s, hype_gop_console_t *con, int show_cursor,
                               hype_vt_render_cache_t *cache);

#endif /* HYPE_VT_RENDER_H */
