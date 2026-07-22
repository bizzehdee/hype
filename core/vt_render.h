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

#endif /* HYPE_VT_RENDER_H */
